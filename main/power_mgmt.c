/*
 * power_mgmt.c
 *
 * 1 kHz power-management task:
 *  • Samples all three H-bridge current sensors (DRIVE, AUX, JACK)
 *  • Samples battery voltage (BAT)
 *  • Applies EMA filtering on every channel
 *  • Updates shared volatile globals for the control FSM
 *  • Handles over-current spike protection
 *
 * Updated to modern ESP-IDF ADC API (line fitting)
 * All variables now defined locally
 *
 * Created on: Nov 10, 2025
 */

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "board_config.h"
#include "control_fsm.h"
#include "i2c.h"
#include "sensors.h"
#include "soc/rtc_io_reg.h"
#include "power_mgmt.h"

#include "storage.h"
#include "rtc.h"

#define TAG "POWER"

// === GPIO Pin Definitions ===
#ifdef BOARD_V5
// V5: single ACS37220LEZATR-100B3 for all motors.
//   GPIO34 (ADC1_CH6) = VOUT (main current reading)
//   GPIO36 / VP (ADC1_CH0) = VOC (OC-threshold sense, diagnostic)
//   GPIO39 / VN = FAULT (digital, active-low, open-drain — external pull-up on board)
//   GPIO35 (ADC1_CH7) = battery voltage (moved from GPIO39)
#define PIN_V_ISENS_MAIN  ADC_CHANNEL_6   // GPIO34
#define PIN_V_VOC         ADC_CHANNEL_0   // GPIO36 / VP
#define PIN_V_BATTERY     ADC_CHANNEL_7   // GPIO35
#define PIN_FAULT_GPIO    GPIO_NUM_39     // digital input
#else // BOARD_V4
#define PIN_V_ISENS1  ADC_CHANNEL_0   // GPIO36 / VP
#define PIN_V_ISENS2  ADC_CHANNEL_6   // GPIO34
#define PIN_V_ISENS3  ADC_CHANNEL_7   // GPIO35
#define PIN_V_BATTERY ADC_CHANNEL_3   // GPIO39 / VN
#endif
#define PIN_V_SENS_BAT PIN_V_BATTERY

// map from relay number to bridge
/*bridge_t bridge_map[] = {
	-1,
	BRIDGE_AUX,
	BRIDGE_AUX,
	BRIDGE_AUX,
	BRIDGE_JACK,
	BRIDGE_JACK,
	BRIDGE_DRIVE,
	BRIDGE_DRIVE };*/

// update time
#define UPDATE_MS 20
#define UPDATE_S 0.02f

extern int64_t fsm_now; // us

// E-fuse data
typedef struct {
    int64_t  az_enable_time; // Timestamp to enable autozeroing at (negative to disable)
    float    az_offset;      // Accumulated zero offset
    bool     az_initialized; // First valid zero established
    
    float raw_current;

	bool ema_init;
	float ema_current;
	
	float current; // with all the corrections applied
	float current_spike;
	
	float heat;
	efuse_trip_t tripped;
	int64_t trip_time;
	
	int64_t on_us;
	int64_t off_us;
} isens_channel_t;
static isens_channel_t isens[N_BRIDGES] = {0};


/**** DRIVE RELAYS ****/
bool relay_states[8] = {false};

//int64_t bridge_transitions_on[NUM_BRIDGES]  = {-1}; // last time relay turned on  (used to ignore inrush)
//int64_t bridge_transitions_off[NUM_BRIDGES] = {-1}; // last time relay turned off (used to enable autozero)

relay_port_t last_relay_state;

// actually write relay states, taking note of transitions, and debouncing transitions to on.
#define BRIDGE_TRANSITION_LOGIC(BRIDGE_NAME) \
	if (relay_state.bridges.BRIDGE_NAME == last_relay_state.bridges.BRIDGE_NAME) { \
		/* no change; no need to do anything */ \
		if(false) if (BRIDGE_##BRIDGE_NAME == BRIDGE_JACK) ESP_LOGI(TAG, "NO CHANGE"); \
	} \
	else if (last_relay_state.bridges.BRIDGE_NAME != BRIDGE_OFF && relay_state.bridges.BRIDGE_NAME == BRIDGE_OFF) { \
		isens[BRIDGE_##BRIDGE_NAME].off_us = fsm_now; \
		if(false) if (BRIDGE_##BRIDGE_NAME == BRIDGE_JACK) ESP_LOGI(TAG, "ON -> OFF"); \
	} \
	else if (last_relay_state.bridges.BRIDGE_NAME == BRIDGE_OFF && relay_state.bridges.BRIDGE_NAME != BRIDGE_OFF) { \
		if (fsm_now > isens[BRIDGE_##BRIDGE_NAME].off_us + 2*get_param_value_t(PARAM_EFUSE_INRUSH_US).u32) { \
			isens[BRIDGE_##BRIDGE_NAME].on_us = fsm_now; \
		if(false) if (BRIDGE_##BRIDGE_NAME == BRIDGE_JACK) ESP_LOGI(TAG, "OFF -> ON"); \
		} else { \
			relay_state.bridges.BRIDGE_NAME = BRIDGE_OFF; \
		if(false) if (BRIDGE_##BRIDGE_NAME == BRIDGE_JACK) ESP_LOGI(TAG, "NOT YET; -> OFF"); \
		} \
	} \
	else { \
		if(false) if (BRIDGE_##BRIDGE_NAME == BRIDGE_JACK) ESP_LOGE(TAG, "TOO FAST OF TRANSITION"); \
		isens[BRIDGE_##BRIDGE_NAME].off_us = fsm_now; \
		relay_state.bridges.BRIDGE_NAME = BRIDGE_OFF; \
	}

esp_err_t drive_relays(relay_port_t relay_state) {
	// Four types of transitions.
	// Not a transition: this does nothing
	// Anything -> off: always allowed. Record the transition time
	// off -> anything: has debouncing; set & record transition if fsm_now > bridge_transitions_off + debounce, otherwise keep bridge off.
	// fwd/rev/on -> fwd/rev/on: not allowed. Actually go to 0. Record the transition time.
	
	BRIDGE_TRANSITION_LOGIC(DRIVE)
	BRIDGE_TRANSITION_LOGIC(JACK)
	BRIDGE_TRANSITION_LOGIC(AUX)
	
	relay_state.bridges.SENSORS = 1;
	
	if (!get_is_safe())
		relay_state.bridges.DRIVE = 0;
		
	last_relay_state = relay_state;
	
	//ESP_LOGI(TAG, "RELAY STATE: %x", state);
	return i2c_set_relays(relay_state);
}

/**** CURRENT / VOLTAGE MONITORING ****/

// === ADC Handles ===
static adc_oneshot_unit_handle_t adc1_handle = NULL;
static adc_cali_handle_t adc_cali_handle = NULL;

static float ema_battery = 0.0f;
static bool ema_battery_init = false;

esp_err_t adc_init() {
	// ADC1 oneshot mode
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc1_handle));

    // Configure all channels
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };

#ifdef BOARD_V5
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_ISENS_MAIN, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_VOC,        &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_SENS_BAT,   &chan_cfg));

    // FAULT is open-drain on the sensor; ESP32 GPIO39 has no internal pull —
    // V5 board MUST provide an external pull-up to VDD.
    gpio_config_t fault_cfg = {
        .pin_bit_mask = 1ULL << PIN_FAULT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&fault_cfg));
#else
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_ISENS1, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_ISENS2, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_ISENS3, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_SENS_BAT, &chan_cfg));
#endif

    // Line fitting calibration (modern scheme)
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali_cfg, &adc_cali_handle));

#ifdef BOARD_V5
    // Diagnostic: log configured VOC — resistor on board sets OC threshold.
    // Datasheet: VVOC [V] = RL_VOC [Ω] × 1e-5, linear 0.33–0.66 V on 3.3V variant.
    int voc_raw = 0, voc_mv = 0;
    if (adc_oneshot_read(adc1_handle, PIN_V_VOC, &voc_raw) == ESP_OK &&
        adc_cali_raw_to_voltage(adc_cali_handle, voc_raw, &voc_mv) == ESP_OK) {
        ESP_LOGI(TAG, "ACS37220 VOC = %d mV (OC threshold config)", voc_mv);
    }
#endif

	return ESP_OK;
}

esp_err_t adc_post(void) {
#ifdef BOARD_V5
    const adc_channel_t channels[] = { PIN_V_ISENS_MAIN, PIN_V_SENS_BAT };
    const char *names[] = { "ISENS", "BATTERY" };
    const int n = 2;
#else
    const adc_channel_t channels[] = { PIN_V_ISENS1, PIN_V_ISENS2, PIN_V_ISENS3, PIN_V_SENS_BAT };
    const char *names[] = { "ISENS1", "ISENS2", "ISENS3", "BATTERY" };
    const int n = 4;
#endif
    int first[4], second[4];

    for (int i = 0; i < n; i++) {
        if (adc_oneshot_read(adc1_handle, channels[i], &first[i]) != ESP_OK) {
            ESP_LOGE(TAG, "POST: ADC read failed on %s", names[i]);
            return ESP_FAIL;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(5));

    for (int i = 0; i < n; i++) {
        if (adc_oneshot_read(adc1_handle, channels[i], &second[i]) != ESP_OK) {
            ESP_LOGE(TAG, "POST: ADC read failed on %s (2nd)", names[i]);
            return ESP_FAIL;
        }
    }

    // Frozen-ADC check on current-sense channels only (battery can legitimately be stable)
    for (int i = 0; i < n - 1; i++) {
        if (first[i] == second[i] && first[i] != 0) {
            ESP_LOGW(TAG, "POST: ADC %s may be frozen (both reads = %d)", names[i], first[i]);
        }
    }

#ifdef BOARD_V5
    ESP_LOGI(TAG, "POST: ADC OK (BAT=%d/%d, I=%d/%d) FAULT=%d",
             first[1], second[1], first[0], second[0],
             gpio_get_level(PIN_FAULT_GPIO));
#else
    ESP_LOGI(TAG, "POST: ADC OK (BAT=%d/%d, I1=%d/%d, I2=%d/%d, I3=%d/%d)",
             first[3], second[3], first[0], second[0], first[1], second[1], first[2], second[2]);
#endif
    return ESP_OK;
}

float get_raw_battery_voltage(void) {
	int adc_raw = 0;
    int voltage_mv = 0;

    if (adc_oneshot_read(adc1_handle, PIN_V_SENS_BAT, &adc_raw)
        != ESP_OK) { return NAN; }
    if (adc_cali_raw_to_voltage(adc_cali_handle, adc_raw, &voltage_mv)
        != ESP_OK) { return NAN; }
        
    // Voltage divider: 150kohm to 1Mohm -> gain = 1.15 -> scale = 1150/150
	return voltage_mv * get_param_value_t(PARAM_V_SENS_K).f32 + get_param_value_t(PARAM_V_SENS_OFFSET).f32; // same as / 1000.0 * 1150.0 / 150.0;
}

esp_err_t process_battery_voltage(void)
{
    float raw = get_raw_battery_voltage();
    
    if (!ema_battery_init) {
        ema_battery = (float)raw;
        ema_battery_init = true;
    } else {
		float alpha = get_param_value_t(PARAM_ADC_ALPHA_BATTERY).f32;
		if (isnan(raw)) {
			//ESP_LOGI(TAG, "RAW BATTERY IS NAN");
		} else {
			if (isnan(ema_battery) || isnan(alpha)) {
				ema_battery = raw;
	        } else {
		        ema_battery = alpha * (float)raw + (1.0f - alpha) * ema_battery;
		    }
        }
    }
    
    return ESP_OK;
}

void disable_autozero(bridge_t bridge) {
	// enable autozeroing for this bridge 1 second from now
	isens[bridge].az_enable_time = fsm_now+1000000;
	//ESP_LOGI(TAG, "KILLING BRIDGE %d; %lld -> %lld", bridge, (long long int) now, (long long int) isens[bridge].az_enable_time);
}

bool get_bridge_overcurrent(bridge_t bridge, float threshold) {
	if (bridge < 0 || bridge>=NUM_BRIDGES) return true; // I GUESS?
	if (fsm_now < isens[bridge].on_us + get_param_value_t(PARAM_EFUSE_INRUSH_US).u32) return false;
	if (isens[bridge].raw_current < threshold) return false;
	return true;
}
bool get_bridge_spike(bridge_t bridge, float threshold) {
	if (bridge < 0 || bridge>=NUM_BRIDGES) return true; // I GUESS?
	if (fsm_now < isens[bridge].on_us + get_param_value_t(PARAM_EFUSE_INRUSH_US).u32) return false;
	if (isens[bridge].current_spike < threshold) return false;
	return true;
}

#ifdef BOARD_V5
// V5 has a single current sensor shared by all bridges. Cache the read
// per fsm tick so three process_bridge_current() calls in the same tick
// don't hit the ADC three times.
static int64_t v5_isens_cache_time = INT64_MIN;
static int     v5_isens_mv_cache   = 0;
static bool    v5_isens_cache_ok   = false;

static bool v5_read_isens_mv(int *out_mv) {
    if (v5_isens_cache_time != fsm_now) {
        v5_isens_cache_time = fsm_now;
        int raw = 0;
        int mv  = 0;
        v5_isens_cache_ok = (adc_oneshot_read(adc1_handle, PIN_V_ISENS_MAIN, &raw) == ESP_OK) &&
                            (adc_cali_raw_to_voltage(adc_cali_handle, raw, &mv) == ESP_OK);
        v5_isens_mv_cache = mv;
    }
    *out_mv = v5_isens_mv_cache;
    return v5_isens_cache_ok;
}

static bool v5_bridge_is_active(bridge_t b) {
    switch (b) {
        case BRIDGE_DRIVE: return last_relay_state.bridges.DRIVE != BRIDGE_OFF;
        case BRIDGE_JACK:  return last_relay_state.bridges.JACK  != BRIDGE_OFF;
        case BRIDGE_AUX:   return last_relay_state.bridges.AUX   != BRIDGE_OFF;
        default:           return false;
    }
}

static bool v5_any_bridge_active(void) {
    return v5_bridge_is_active(BRIDGE_DRIVE) ||
           v5_bridge_is_active(BRIDGE_JACK)  ||
           v5_bridge_is_active(BRIDGE_AUX);
}
#endif

esp_err_t process_bridge_current(bridge_t bridge) {
    if (bridge < 0 || bridge >= NUM_BRIDGES) return ESP_ERR_INVALID_ARG;

    isens_channel_t *channel = &isens[bridge];

#ifdef BOARD_V5
    int voltage_mv = 0;
    if (!v5_read_isens_mv(&voltage_mv)) {
        return 0;
    }

    float last_current = channel->raw_current;
    channel->raw_current = NAN;

    // Single ACS37220LEZATR-100B3 for all motors: 13.2 mV/A, Vqvo=1.65 V.
    // Sign convention matches the old V4 DRIVE wiring (ACS37220 oriented such
    // that forward motor current gives negative delta from Vqvo).
    float measured_A = -(voltage_mv - 1650.0f) / 13.2f;

    // Per-bridge attribution:
    //   - bridge active and alone      → it owns the entire reading
    //   - bridge active, others active → attribute full reading to each active
    //       bridge (worst-case; protects hardware). Jack/drive are mutually
    //       exclusive per design, so this only affects drive+aux overlap.
    //   - bridge OFF                   → no current from this bridge
    // TODO(V5): better drive+aux simultaneous attribution (e.g. subtract the
    //           quieter bridge's nominal draw from the total).
    if (v5_bridge_is_active(bridge)) {
        channel->raw_current = measured_A;
    } else {
        channel->raw_current = 0.0f;
    }
#else
	int adc_raw = 0;
    int voltage_mv = 0;

    adc_channel_t pin;
    switch(bridge) {
        case BRIDGE_DRIVE: pin = PIN_V_ISENS1; break;
        case BRIDGE_JACK:  pin = PIN_V_ISENS2; break;
        case BRIDGE_AUX:   pin = PIN_V_ISENS3; break;
        default: return ESP_ERR_INVALID_ARG;
    }

    if (adc_oneshot_read(adc1_handle, pin, &adc_raw) != ESP_OK) {
        return 0;
    }
    if (adc_cali_raw_to_voltage(adc_cali_handle, adc_raw, &voltage_mv) != ESP_OK) {
        return 0;
    }

    float last_current = channel->raw_current;
    channel->raw_current = NAN;

    switch (bridge) {
        case BRIDGE_JACK:
        case BRIDGE_AUX:
        	// ACS37042KLHBLT-030B3 is 30A capable and 44 mV/A
            channel->raw_current = (voltage_mv - 1650.0f) / 44.0f;
            break;
        case BRIDGE_DRIVE:
        	// ACS37220LEZATR-100B3 is 100A capable and 13.2 mV/A
            channel->raw_current = -(voltage_mv - 1650.0f) / 13.2f;
            break;
        default: break;
    }
#endif
    
    if (!channel->ema_init) {
        channel->ema_current = channel->raw_current;
        channel->ema_init = true;
    } else {
        float alpha = get_param_value_t(PARAM_ADC_ALPHA_ISENS).f32;
		if (isnan(channel->raw_current)) {
			//ESP_LOGI(TAG, "RAW BATTERY IS NAN");
			channel->ema_current = NAN;
		} else {
			if (isnan(ema_battery) || isnan(alpha)) {
				channel->ema_current = channel->raw_current;
	        } else {
		        channel->ema_current = alpha * channel->raw_current + (1.0f - alpha) * channel->ema_current;
		    }
        }
    }

    // === AUTO-ZERO LEARNING PHASE ===
    // On V5, the single ADC reads aggregate motor current. A channel's
    // "quiet" periods are when ALL bridges are off — not just this one.
#ifdef BOARD_V5
    bool az_allowed = (fsm_now > channel->az_enable_time) && !v5_any_bridge_active();
#else
    bool az_allowed = (fsm_now > channel->az_enable_time);
#endif
    if (az_allowed) {
		//ESP_LOGI(TAG, "AZING %d", bridge);
		float db = get_param_value_t(PARAM_ADC_DB_IAZ).f32;
        if (isnan(db) || fabsf(channel->ema_current) <= db) {
            // Valid zero sample
            if (!channel->az_initialized) {
                channel->az_offset = channel->ema_current;
                channel->az_initialized = true;
            } else {
                float alpha = get_param_value_t(PARAM_ADC_ALPHA_IAZ).f32;
				if (isnan(channel->raw_current)) {
					//ESP_LOGI(TAG, "RAW BATTERY IS NAN");
				} else {
					if (isnan(ema_battery) || isnan(alpha)) {
						channel->az_offset = channel->ema_current;
			        } else {
				        channel->az_offset = alpha * channel->ema_current +
				            (1.0f - alpha) * channel->az_offset;
				    }
		        }
            }
        }
    }
    
    // Apply the offset
    channel->current = channel->raw_current - channel->az_offset;
    channel->raw_current = channel->raw_current - channel->az_offset;
    channel->current_spike = channel->raw_current - last_current;


	// PARAMETERS FOR E-FUSING ALGORITHM
	// PARAM_EFUSE_KINST : ratio of nominal current that should cause an immediate shutdown
	// PARAM_EFUSE_TCOOL : cooldown timer from trip (in microseconds)
	// PARAM_EFUSE_TAUCOOL : speed of cooldown for heating (units are 1/s; bigger = faster cooldown)
	
	// Monitor E-fusing
	float I_nominal = NAN;
	switch(bridge) {
		case BRIDGE_DRIVE:
			I_nominal = get_param_value_t(PARAM_EFUSE_INOM_1).f32;
			break;
		case BRIDGE_JACK:
			I_nominal = get_param_value_t(PARAM_EFUSE_INOM_2).f32;
			break;
		case BRIDGE_AUX:
			I_nominal = get_param_value_t(PARAM_EFUSE_INOM_3).f32;
			break;
        default: break;
    }

        // Normalize the current as a fraction of rated current
    float I_norm = fabsf(channel->current / I_nominal);

    // Instant trip on extreme overcurrent
    if (fsm_now > channel->on_us + get_param_value_t(PARAM_EFUSE_INRUSH_US).u32
        && I_norm >= get_param_value_t(PARAM_EFUSE_KINST).f32) {
        // Check if overcurrent has persisted long enough
        channel->tripped = true;
        channel->trip_time = fsm_now;
		//ESP_LOGI(TAG, "FUSE TRIP: Inom: %+.5f HEAT:%+2.5f", I_norm, channel->heat);
        return ESP_OK; // no more processing, if we're over, we're over
        // Still in overcurrent but within inrush tolerance window - don't trip yet
    }
    
    // Accumulate heat
    channel->heat += (I_norm * I_norm) * UPDATE_S;

    // Only do cooling when below threshold
    if (I_norm < 1.0f) {
		// if we are hot we radiate more heat
		// (I^2/I^2*t) * (1/t) * t = I^2/I^2*t
        channel->heat -= channel->heat * get_param_value_t(PARAM_EFUSE_TAUCOOL).f32 * UPDATE_S;
        channel->heat = fmaxf(0.0f, channel->heat); // keep it from going negative
        // channel.tripped = false;  // Auto-clear if cooled (WTF why this is insane)
    }
    
    // If built-up heat exceeds the time limit, trip
    // Recall units of heat are (current_actual^2/current_nominal^2)*time
    // Ergo, heat is measured in seconds
    if (channel->heat > get_param_value_t(PARAM_EFUSE_HEAT_THRESH).f32) {
		channel->tripped = true;
		channel->trip_time = fsm_now;
		
	// If we're not overheated
	// And enough time has passed
	// Go ahead and reset the e-fuse
	} else if (channel->tripped &&
		(fsm_now - channel->trip_time) > get_param_value_t(PARAM_EFUSE_TCOOL).u32) {
			channel->tripped = false;
			// channel.heat = 0.0f // I think we should wait for the e-fuse to catch up
	}
	
	//if (bridge == BRIDGE_JACK) ESP_LOGI(TAG, "TIME: %lld", (long long) fsm_now);
	
	//if (bridge == BRIDGE_JACK) ESP_LOGI(TAG, "FUSE: trip [%d] %lld, raw_a: %+.4f cur: %+.4f Inorm: %+.5f HEAT:%+2.5f", channel->tripped, channel->trip_time, channel->raw_current, channel->current, I_norm, channel->heat);
	
	return ESP_OK;
}


// === Public Accessors ===
float get_bridge_A(bridge_t bridge)
{
    if (bridge >= N_BRIDGES) return NAN;
    return isens[bridge].current;
}
float get_bridge_raw_A(bridge_t bridge)
{
    if (bridge >= N_BRIDGES) return NAN;
    return isens[bridge].raw_current;
}

float efuse_get_heat(bridge_t bridge) {
    if (bridge >= N_BRIDGES) return NAN;
    return isens[bridge].heat;	
}

float get_battery_V(void)
{
	if (ema_battery_init)
		return ema_battery;
    return get_raw_battery_voltage();
}

bool get_hw_overcurrent_fault(void)
{
#ifdef BOARD_V5
    // ACS37220 FAULT is active-low, open-drain, not latched.
    return gpio_get_level(PIN_FAULT_GPIO) == 0;
#else
    return false;
#endif
}

static int read_mv_channel(adc_channel_t ch)
{
    int raw = 0, mv = 0;
    if (adc_oneshot_read(adc1_handle, ch, &raw) != ESP_OK) return 0;
    if (adc_cali_raw_to_voltage(adc_cali_handle, raw, &mv) != ESP_OK) return 0;
    return mv;
}

int get_bat_raw_mv(void)
{
    return read_mv_channel(PIN_V_SENS_BAT);
}

int get_isens_raw_mv(void)
{
#ifdef BOARD_V5
    return read_mv_channel(PIN_V_ISENS_MAIN);
#else
    return 0;
#endif
}

int get_voc_raw_mv(void)
{
#ifdef BOARD_V5
    return read_mv_channel(PIN_V_VOC);
#else
    return 0;
#endif
}

efuse_trip_t efuse_get(bridge_t bridge)
{
    if (bridge >= N_BRIDGES) return false;
    return isens[bridge].tripped;
}
void efuse_set(bridge_t bridge, efuse_trip_t state)
{
    if (bridge >= N_BRIDGES) return;
    isens[bridge].tripped = state;
    isens[bridge].trip_time = fsm_now;
}