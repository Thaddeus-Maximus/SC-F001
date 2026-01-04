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
#include "control_fsm.h"
#include "soc/rtc_io_reg.h"
#include "power_mgmt.h"

#include "storage.h"
#include "rtc.h"

#define TAG "POWER"

// === GPIO Pin Definitions === 
#define PIN_V_ISENS1  ADC_CHANNEL_0   // GPIO36 / VP
#define PIN_V_ISENS2  ADC_CHANNEL_6   // GPIO34
#define PIN_V_ISENS3  ADC_CHANNEL_7   // GPIO35
#define PIN_V_BATTERY ADC_CHANNEL_3   // GPIO39 / VN
#define PIN_V_SENS_BAT PIN_V_BATTERY


// update time
#define UPDATE_MS 20
#define UPDATE_S 0.02f

int64_t now; // us

typedef struct {
    int64_t  az_enable_time; // Timestamp to enable autozeroing at (negative to disable)
    float    az_offset;      // Accumulated zero offset
    bool     az_initialized; // First valid zero established

	bool ema_init;
	float ema_current;
	
	float current; // with all the corrections applied
	
	float heat;
	bool tripped;
	int64_t trip_time;
	
	// Inrush tolerance tracking
	int64_t inrush_start_time; // When instantaneous overcurrent first detected (0 = not in overcurrent)
} isens_channel_t;
static isens_channel_t isens[N_BRIDGES] = {0};

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

    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_ISENS1, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_ISENS2, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_ISENS3, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, PIN_V_SENS_BAT, &chan_cfg));

    // Line fitting calibration (modern scheme)
    adc_cali_line_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_cali_create_scheme_line_fitting(&cali_cfg, &adc_cali_handle));

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

void set_autozero(bridge_t bridge) {
	// enable autozeroing for this bridge 1 second from now
	isens[bridge].az_enable_time = now+1000000;
	//ESP_LOGI(TAG, "KILLING BRIDGE %d; %lld -> %lld", bridge, (long long int) now, (long long int) isens[bridge].az_enable_time);
}

esp_err_t process_bridge_current(bridge_t bridge) {
	int adc_raw = 0;
    int voltage_mv = 0;
    
    isens_channel_t *channel = &isens[bridge];
    
    adc_channel_t pin;
    switch(bridge) {
        case BRIDGE_DRIVE: pin = PIN_V_ISENS1; break;
        case BRIDGE_JACK:  pin = PIN_V_ISENS2; break;
        case BRIDGE_AUX:   pin = PIN_V_ISENS3; break;
        default: return -42069; // lol
    }

    if (adc_oneshot_read(adc1_handle, pin, &adc_raw) != ESP_OK) {
        return 0;
    }
    if (adc_cali_raw_to_voltage(adc_cali_handle, adc_raw, &voltage_mv) != ESP_OK) {
        return 0;
    }
    
    float raw_a = NAN;
    
    switch (bridge) {
        case BRIDGE_JACK:
        case BRIDGE_AUX:
        	// ACS37042KLHBLT-030B3 is 30A capable and 44 mV/A
            raw_a = (voltage_mv - 1650.0f) / 44.0f;
            break;
        case BRIDGE_DRIVE:
        	// ACS37220LEZATR-100B3 is 100A capable and 13.2 mV/A
            raw_a = -(voltage_mv - 1650.0f) / 13.2f;
            break;
    }
    
    if (!channel->ema_init) {
        channel->ema_current = (float)raw_a;
        channel->ema_init = true;
    } else {
        float alpha = get_param_value_t(PARAM_ADC_ALPHA_ISENS).f32;
		if (isnan(raw_a)) {
			//ESP_LOGI(TAG, "RAW BATTERY IS NAN");
			channel->ema_current = NAN;
		} else {
			if (isnan(ema_battery) || isnan(alpha)) {
				channel->ema_current = raw_a;
	        } else {
		        channel->ema_current = alpha * raw_a + (1.0f - alpha) * channel->ema_current;
		    }
        }
    }

    // === AUTO-ZERO LEARNING PHASE ===
    if (now > channel->az_enable_time) {
		//ESP_LOGI(TAG, "AZING %d", bridge);
		float db = get_param_value_t(PARAM_ADC_DB_IAZ).f32;
        if (isnan(db) || fabsf(channel->ema_current) <= db) {
            // Valid zero sample
            if (!channel->az_initialized) {
                channel->az_offset = channel->ema_current;
                channel->az_initialized = true;
            } else {
                float alpha = get_param_value_t(PARAM_ADC_ALPHA_IAZ).f32;
				if (isnan(raw_a)) {
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
    channel->current = channel->ema_current - channel->az_offset;


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
	}
	
	// Normalize the current as a fraction of rated current
    float I_norm = fabsf(channel->current / I_nominal);

    // Instant trip on extreme overcurrent - but with inrush tolerance
    if (I_norm >= get_param_value_t(PARAM_EFUSE_KINST).f32) {
        // Start tracking if this is the first time we've seen overcurrent
        if (channel->inrush_start_time == 0) {
            channel->inrush_start_time = now;
        }
        
        // Check if overcurrent has persisted long enough
        int64_t inrush_duration = now - channel->inrush_start_time;
        if (inrush_duration >= get_param_value_t(PARAM_EFUSE_INRUSH_US).u32) {
            channel->tripped = true;
            channel->trip_time = now;
            channel->inrush_start_time = 0; // Reset for next time
			//ESP_LOGI(TAG, "FUSE TRIP: Inom: %+.5f HEAT:%+2.5f", I_norm, channel->heat);
            return ESP_OK; // no more processing, if we're over, we're over
        }
        // Still in overcurrent but within inrush tolerance window - don't trip yet
    } else {
        // Current dropped below threshold - reset inrush timer
        channel->inrush_start_time = 0;
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
		channel->trip_time = now;
		
	// If we're not overheated
	// And enough time has passed
	// Go ahead and reset the e-fuse
	} else if (channel->tripped &&
		(now - channel->trip_time) > get_param_value_t(PARAM_EFUSE_TCOOL).u32) {
			channel->tripped = false;
			// channel.heat = 0.0f // I think we should wait for the e-fuse to catch up
	}
	
	//if (bridge == BRIDGE_JACK)
		//ESP_LOGI(TAG, "FUSE: raw_a: %+.4f cur: %+.4f Inorm: %+.5f HEAT:%+2.5f", raw_a, channel->current, I_norm, channel->heat);
	
	return ESP_OK;
}


// === Public Accessors ===
float get_bridge_A(bridge_t bridge)
{
    if (bridge >= N_BRIDGES) return NAN;
    return isens[bridge].current;
}

float get_battery_V(void)
{
	if (ema_battery_init)
		return ema_battery;
    return get_raw_battery_voltage();
}

// === Public E-Fuse Controls ===
/*void efuse_reset_all(void)
{
    for (uint8_t i = 0; i < N_BRIDGES; i++) {
        isens[i].heat = 0.0f;
        isens[i].tripped = false;
    }
}*/

bool efuse_is_tripped(bridge_t bridge)
{
    if (bridge >= N_BRIDGES) return false;
    return isens[bridge].tripped;
}

// === Power Management Task ===
void power_mgmt_task(void *param) {
	esp_task_wdt_add(NULL);
	


    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(UPDATE_MS);

    while (1) {
	    vTaskDelayUntil(&xLastWakeTime, xFrequency);
        now = esp_timer_get_time(); // us
        
        /*if (now - last_wake_time < period) {
            uint32_t delay_us = (period - (now - last_wake_time)) / 1000;
            if (delay_us > 0) vTaskDelay(pdMS_TO_TICKS(delay_us));
            continue;
        }
        last_wake_time = now;*/

        // Sample currents
        for (uint8_t i = 0; i < N_BRIDGES; i++) {
			process_bridge_current(i);
        }
        
        process_battery_voltage();
        esp_task_wdt_reset();
    }
}

esp_err_t power_init() {
    xTaskCreate(power_mgmt_task, "PWR", 4096, NULL, 5, NULL);

	return ESP_OK;
}

esp_err_t power_stop() {
	return ESP_OK;
}