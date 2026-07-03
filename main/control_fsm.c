/*
 * control_fsm.c
 *
 *  Created on: Nov 10, 2025
 *      Author: Thad
 */
 
 
// See README.md for FSM documentation (states, guards, timing).

#include "control_fsm.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "i2c.h"
#include "power_mgmt.h"
#include "bringup.h"
#include "rtc_wdt.h"
#include "driver/gpio.h"
#include "sc_err.h"
#include "storage.h"
#include "rtc.h"
#include "sensors.h"
#include "esp_log.h"
#include <string.h>
#include <sys/param.h>

#define TRANSITION_DELAY_US 1000000

#define CALIBRATE_JACK_MAX_TIME  3000000
#define CALIBRATE_DRIVE_MAX_TIME 6000000

#define TAG "FSM"

static QueueHandle_t fsm_cmd_queue = NULL;

// fsm_init() does not zero these — they persist across panics/WDT resets.
// Only cleared by explicit user action (fsm_clear_error, fsm_set_remaining_distance).
RTC_DATA_ATTR esp_err_t fsm_error = ESP_OK;
esp_err_t fsm_get_error() { return fsm_error; }
void fsm_clear_error() { fsm_error = ESP_OK; }


/* override_time + override_cmd are written from RF/BT/comms tasks and read
 * from the control task. int64_t isn't atomic on a 32-bit MCU, so we wrap
 * read/write in a critical section to prevent torn reads (which could land
 * override_time far in the future and run a motor for seconds longer than
 * RF_PULSE_LENGTH). */
static portMUX_TYPE override_spin = portMUX_INITIALIZER_UNLOCKED;
int64_t override_time = -1;
fsm_override_t override_cmd = FSM_OVERRIDE_DRIVE_FWD;
bool enabled = false;

float this_move_dist = 0.0f;
RTC_DATA_ATTR float remaining_distance = 0.0f;
float fsm_get_remaining_distance(void)    { return remaining_distance; }
void  fsm_set_remaining_distance(float x) { remaining_distance = x;}

// Track the starting encoder count for the current move
static int32_t move_start_encoder = 0;

// Track total jack up time to use for jack down duration
static int64_t jack_start_us  = 0;
static int64_t jack_trans_us  = 0;
static int64_t jack_finish_us = 0;

/* Cumulative jack extension estimate in microseconds (0 = fully retracted).
 * Reset to 0 whenever SENSOR_JACK trips (home position). Persists across
 * panics/WDT resets so the guard survives a mid-extension reboot. */
RTC_DATA_ATTR static int64_t jack_pos_us = 0;

volatile fsm_state_t current_state = STATE_IDLE;
volatile int64_t fsm_now = 0;
volatile bool start_running_request = false;


fsm_state_t fsm_get_state() {
	return current_state;
}

bool fsm_is_idle(void) {
	return current_state == STATE_IDLE;
}

int64_t fsm_get_jack_pos_us(void) {
	return jack_pos_us;
}

static int64_t timer_end = 0;
static int64_t timer_start = 0;
static inline void set_timer(uint64_t us) {
	timer_end = fsm_now + us;
	timer_start = fsm_now;
}
static inline bool timer_done() { return fsm_now >= timer_end; }

/* Arm the override deadman for `duration_us` from now. Shared by the RF/BT
 * path (short RF_PULSE_LENGTH) and the web/WS path (longer WEB_PULSE_LENGTH). */
static void arm_override(fsm_override_t cmd, uint32_t duration_us) {
    if (soft_idle_is_active()) return;
    if (current_state == STATE_IDLE) {
        rtc_reset_shutdown_timer();
        int64_t deadline = fsm_now + (int64_t)duration_us;
        portENTER_CRITICAL(&override_spin);
        override_cmd = cmd;
        override_time = deadline;
        portEXIT_CRITICAL(&override_spin);
    }
}

/* RF remote / BT HID jog: short deadman, matched to their fast physical-layer
 * repeat rate (no TCP in between). */
void pulse_override(fsm_override_t cmd) {
    arm_override(cmd, get_param_value_t(PARAM_RF_PULSE_LENGTH).u32);
}

/* Web / WebSocket jog: longer deadman to ride out TCP stalls (head-of-line
 * blocking on Wi-Fi loss). Safe because release/tab-close stop via reliable
 * paths; this only bounds motion on a silent link blackout. See WEB_PULSE_LENGTH. */
void pulse_override_web(fsm_override_t cmd) {
    arm_override(cmd, get_param_value_t(PARAM_WEB_PULSE_LENGTH).u32);
}

void stop_override(void) {
    portENTER_CRITICAL(&override_spin);
    override_time = 0;
    portEXIT_CRITICAL(&override_spin);
}

/* Atomic snapshot of override_time + override_cmd for the control task. */
static inline void override_snapshot(int64_t *time_out, fsm_override_t *cmd_out) {
    portENTER_CRITICAL(&override_spin);
    *time_out = override_time;
    *cmd_out  = override_cmd;
    portEXIT_CRITICAL(&override_spin);
}

int64_t fsm_cal_t, fsm_cal_e;
int64_t fsm_get_cal_t(){return fsm_cal_t;}
int64_t fsm_get_cal_e(){return fsm_cal_e;}

const char *sc_err_str(esp_err_t e) {
    switch (e) {
        case ESP_OK:              return "OK";
        case SC_ERR_EFUSE_TRIP_1: return "EFUSE 1 TRIP";
        case SC_ERR_EFUSE_TRIP_2: return "EFUSE 2 TRIP";
        case SC_ERR_EFUSE_TRIP_3: return "EFUSE 3 TRIP";
        case SC_ERR_SAFETY_TRIP:  return "SAFETY NOT SET";
        case SC_ERR_TRAVEL_LIMIT: return "TRAVEL LIMIT REACHED";
        case SC_ERR_RTC_NOT_SET:  return "CLOCK NOT SET";
        case SC_ERR_LOW_BATTERY:  return "INSUFFICIENT VOLTAGE";
        default:                  return "UNKNOWN";
    }
}

const char *fsm_state_str(fsm_state_t s) {
    switch (s) {
        case STATE_IDLE:                  return "IDLE";
        case STATE_MOVE_START_DELAY:      return "MOVE_START_DELAY";
        case STATE_JACK_UP_START:         return "JACK_UP_START";
        case STATE_JACK_UP:               return "JACK_UP";
        case STATE_DRIVE_START_DELAY:     return "DRIVE_START_DELAY";
        case STATE_DRIVE_FLUFF_START:     return "DRIVE_FLUFF_START";
        case STATE_DRIVE:                 return "DRIVE";
        case STATE_DRIVE_END_DELAY:       return "DRIVE_END_DELAY";
        case STATE_JACK_DOWN:             return "JACK_DOWN";
        case STATE_MOVE_JACK_RETRACT:     return "MOVE_JACK_RETRACT";
        case STATE_MOVE_JACK_SETTLE:      return "MOVE_JACK_SETTLE";
        case STATE_UNDO_JACK_START:       return "UNDO_JACK_START";
        case STATE_CALIBRATE_JACK_DELAY:  return "CALIBRATE_JACK_DELAY";
        case STATE_CALIBRATE_JACK_MOVE:   return "CALIBRATE_JACK_MOVE";
        case STATE_CALIBRATE_DRIVE_DELAY: return "CALIBRATE_DRIVE_DELAY";
        case STATE_CALIBRATE_DRIVE_MOVE:  return "CALIBRATE_DRIVE_MOVE";
        default:                          return "UNKNOWN";
    }
}

/* Preconditions for accepting a START command. Returns ESP_OK if every gate
 * passes, otherwise the SC_ERR_* code of the first failing gate. Caller is
 * expected to assign the returned code into `fsm_error` and skip the start.
 * Order matters: most-actionable error first (voltage → safety → efuses) so
 * the operator sees the dominant fault when more than one is true. */
static esp_err_t fsm_check_start_preconditions(void) {
    esp_err_t code = ESP_OK;
    if      (get_battery_V() < get_param_value_t(PARAM_LOW_PROTECTION_V).f32) code = SC_ERR_LOW_BATTERY;
    else if (!get_is_safe())                                                  code = SC_ERR_SAFETY_TRIP;
    else if (efuse_get(BRIDGE_DRIVE))                                         code = SC_ERR_EFUSE_TRIP_1;
    else if (efuse_get(BRIDGE_JACK))                                          code = SC_ERR_EFUSE_TRIP_2;
    else if (efuse_get(BRIDGE_AUX))                                           code = SC_ERR_EFUSE_TRIP_3;
    if (code != ESP_OK) ESP_LOGI(TAG, "FAILED TO START; %s", sc_err_str(code));
    return code;
}

/* Gate a calibrate-mode state transition: only accepts the transition from
 * `expected` to `next`, optionally requiring battery above LOW_PROTECTION_V.
 * Returns true if the transition was made; caller then does per-case work
 * (set_timer / save cal data / reset sensor counter) that doesn't fit a
 * uniform helper. Battery gate is on for PREP and START (we are about to
 * energize a motor); off for END (no motor action). */
static bool fsm_calibrate_transition(fsm_state_t expected, fsm_state_t next,
                                     bool require_battery) {
    if (current_state != expected) return false;
    if (require_battery &&
        get_battery_V() <= get_param_value_t(PARAM_LOW_PROTECTION_V).f32) return false;
    current_state = next;
    return true;
}

void fsm_request(fsm_cmd_t cmd)
{
    // STOP always goes through (safety). All other commands are blocked during soft idle —
    // the device must be woken by physical button or alarm before remote/RF movement is allowed.
    if (cmd != FSM_CMD_STOP && soft_idle_is_active()) return;

    rtc_reset_shutdown_timer();  // any accepted command extends the wake period
    if (fsm_cmd_queue != NULL)
        xQueueSend(fsm_cmd_queue, &cmd, 0);  // safe from any context
}

int8_t fsm_get_current_progress(int8_t denominator) {
	int8_t x = 0;
	switch (current_state) {
		case STATE_DRIVE:
		case STATE_JACK_UP_START:
		case STATE_JACK_UP:
		case STATE_JACK_DOWN:
		case STATE_MOVE_JACK_RETRACT:
		case STATE_MOVE_JACK_SETTLE:
		case STATE_MOVE_START_DELAY:
		case STATE_DRIVE_START_DELAY:
		case STATE_DRIVE_FLUFF_START:
		case STATE_DRIVE_END_DELAY:
			if (timer_end != timer_start)
				x = (fsm_now-timer_start)*denominator/(timer_end-timer_start);
			break;
		case STATE_UNDO_JACK_START:
			x = 0;
			break;
		default:
			break;
	}
	if (x<0) x=0;
	if (x>denominator-1) x=denominator-1;
	return x;
}


#define JACK_TIME      get_param_value_t(PARAM_JACK_KT).f32 * get_param_value_t(PARAM_JACK_DIST).f32
#define JACK_MAX_TIME  get_param_value_t(PARAM_JACK_KT).f32 * get_param_value_t(PARAM_JACK_MAX ).f32
/* Phase-1 ("pre-jack") duration: raise JACK_PRE_DIST inches, but the phase also
 * ends early on the jack up-current threshold (JACK_I_UP) or an e-fuse trip. */
#define JACK_PRE_TIME  get_param_value_t(PARAM_JACK_KT).f32 * get_param_value_t(PARAM_JACK_PRE_DIST).f32

/* Symmetric jack-down duration: how long jack-up actually ran, plus 5%.
 * If jack_start_us / jack_finish_us are zero or negative (panic recovery,
 * or a transition that skipped the normal path) the delta is unsafe — fall
 * back to the parameter-derived JACK_TIME as a floor so we don't either
 * (a) cut the jack-down to ~0 and leave the actuator extended, or (b) run
 * forever. */
static inline int64_t _jack_down_time_us(void) {
    int64_t delta = jack_finish_us - jack_start_us;
    int64_t floor_us = (int64_t)JACK_TIME;
    if (delta < floor_us) delta = floor_us;
    return delta * 105 / 100;
}
#define JACK_DOWN_TIME _jack_down_time_us()
#define DRIVE_TIME get_param_value_t(PARAM_DRIVE_KT).f32 * this_move_dist
#define DRIVE_DIST get_param_value_t(PARAM_DRIVE_KE).f32 * this_move_dist

int64_t last_log_time = 0;
/* FSM log payload (single current channel — V5 has one shared ACS sensor; V4
 * had three but the per-bridge values are redundant since only one bridge is
 * active at a time). Layout:
 *   [0:8]   ts_ms     u64
 *   [8:12]  bat_V     f32
 *   [12:16] current_A f32  — sum of bridge currents (mutually exclusive)
 *   [16:18] counter   i16
 *   [18:19] sensors   u8
 *   [19:23] heat      f32  — max bridge heat
 *   [23:25] i2c_out   u16  — last 16-bit TCA9555 output state
 *                            (high byte = OUTPUT0 / LEDs, low = OUTPUT1 / relays) */
#define LOGSIZE 25
esp_err_t send_fsm_log() {
	if(!rtc_is_set()) return ESP_OK;

	uint8_t entry[LOGSIZE] = {};

    uint64_t be_timestamp = rtc_get_ms();
    memcpy(&entry[0], &be_timestamp, 8);

    float be_voltage = get_battery_V();
    memcpy(&entry[8],  &be_voltage,  4);

    float current_A = 0.0f;
    for (bridge_t b = 0; b < N_BRIDGES; b++) current_A += get_bridge_raw_A(b);
    memcpy(&entry[12], &current_A, 4);

    int16_t be_counter = get_sensor_counter(SENSOR_DRIVE);
    memcpy(&entry[16], &be_counter, 2);

    entry[18] = pack_sensors();

    float heat = max_efuse_heat();
    memcpy(&entry[19], &heat, 4);

    uint16_t i2c_out = i2c_get_outputs();
    memcpy(&entry[23], &i2c_out, 2);

    last_log_time = esp_timer_get_time();

    log_write(entry, LOGSIZE, fsm_get_state());
    
    //ESP_LOGI(TAG, "WROTE LOG; %lld / %ld/%ld; %5.2f %5.2f %5.2f", (long long)rtc_get_ms(), (unsigned long)log_get_tail(), (unsigned long)log_get_head(), heat1, heat2, heat3);
    
    return ESP_OK;
}

void control_task(void *param) {
	esp_task_wdt_add(NULL);
	
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20);
    enabled = true;
    
    // sensors_init() is called from main.c as a critical init (before FSM starts)

    while (enabled) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        fsm_now = esp_timer_get_time();

        /* Bring-up tool owns the relays and ADCs while active — skip. */
        if (bringup_mode_is_active()) {
            esp_task_wdt_reset();
            continue;
        }

        bool log = false;

        /**** READ INPUTS ****/
        for (uint8_t i = 0; i < N_BRIDGES; i++) {
			process_bridge_current(i);
        }
        process_battery_voltage();
        sensors_check();
        
        /**** LISTEN TO COMMANDS ****/
        fsm_cmd_t cmd;
        while (xQueueReceive(fsm_cmd_queue, &cmd, 0) == pdTRUE) {
            // if (error != ESP_OK) continue; // don't do anything until error is cleared
            
            switch (cmd) {
                case FSM_CMD_START:
                	// Check if we have remaining distance before starting
                	if (remaining_distance <= 0.0f) {
						ESP_LOGI(TAG, "FAILED TO START; %s", sc_err_str(SC_ERR_TRAVEL_LIMIT));
                		fsm_error = SC_ERR_TRAVEL_LIMIT;
                		log = true;
                		continue;
                	}
                	this_move_dist = MIN(get_param_value_t(PARAM_DRIVE_DIST).f32, remaining_distance);
                	goto do_start;
                case FSM_CMD_START_IGNORE_OVERTRAVEL:
                	this_move_dist = get_param_value_t(PARAM_DRIVE_DIST).f32;
                do_start:
                	/* Silently drop START commands received in any non-idle state
                	 * (e.g. duplicate request while already moving). Preconditions
                	 * are checked only once we know the state is acceptable. */
                	if (current_state != STATE_IDLE) break;
                	{
						esp_err_t guard = fsm_check_start_preconditions();
						if (guard != ESP_OK) {
							fsm_error = guard;
							continue;
						}
						ESP_LOGI(TAG, "STARTING");
						fsm_error = ESP_OK; // if everything is OK now, we're OK.
						/* Zero jack timestamps so JACK_DOWN_TIME on this cycle
						 * never inherits a stale value from a prior run. */
						jack_start_us  = 0;
						jack_trans_us  = 0;
						jack_finish_us = 0;
						current_state = STATE_MOVE_START_DELAY;
						log = true;
						set_timer(TRANSITION_DELAY_US);
					}
                    break;
                case FSM_CMD_STOP:
                    current_state = STATE_IDLE;
                    break;
                case FSM_CMD_UNDO:
                    if (current_state != STATE_IDLE &&
                        current_state != STATE_UNDO_JACK_START) {
                        current_state = STATE_UNDO_JACK_START;
                		log = true;
                    }
                    break;
                case FSM_CMD_SHUTDOWN:
                	enabled = false;
                	break;
                	
				/* Calibration sub-FSM: PREP arms (IDLE → DELAY), START energizes
				 * the motor with a hard timeout (DELAY → MOVE), END records
				 * the result and returns to idle (MOVE → IDLE). PREP/START
				 * require battery; END doesn't (no motor action). */
                case FSM_CMD_CALIBRATE_JACK_PREP:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_PREP");
					if (fsm_calibrate_transition(STATE_IDLE, STATE_CALIBRATE_JACK_DELAY, true))
						log = true;
					break;

				case FSM_CMD_CALIBRATE_JACK_START:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_START");
					if (fsm_calibrate_transition(STATE_CALIBRATE_JACK_DELAY,
					                             STATE_CALIBRATE_JACK_MOVE, true)) {
						set_timer(CALIBRATE_JACK_MAX_TIME);
						log = true;
					}
					break;
				case FSM_CMD_CALIBRATE_JACK_END:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_END");
					if (fsm_calibrate_transition(STATE_CALIBRATE_JACK_MOVE,
					                             STATE_IDLE, false)) {
						fsm_cal_t = fsm_now - timer_start;
						log = true;
					}
					break;
                case FSM_CMD_CALIBRATE_DRIVE_PREP:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_PREP");
					if (fsm_calibrate_transition(STATE_IDLE, STATE_CALIBRATE_DRIVE_DELAY, true))
						log = true;
					break;

				case FSM_CMD_CALIBRATE_DRIVE_START:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_START");
					if (fsm_calibrate_transition(STATE_CALIBRATE_DRIVE_DELAY,
					                             STATE_CALIBRATE_DRIVE_MOVE, true)) {
						set_timer(CALIBRATE_DRIVE_MAX_TIME);
						set_sensor_counter(SENSOR_DRIVE, 0);
						log = true;
					}
					break;
				case FSM_CMD_CALIBRATE_DRIVE_END:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_END");
					if (fsm_calibrate_transition(STATE_CALIBRATE_DRIVE_MOVE,
					                             STATE_IDLE, false)) {
						fsm_cal_t = fsm_now - timer_start;
						fsm_cal_e = get_sensor_counter(SENSOR_DRIVE);
						log = true;
					}
					break;
            }
        }
        
        if (!enabled) break;
		
        /**** STATE TRANSITIONS ****/
        // Every active state checks safety first — break triggers UNDO_JACK (emergency lower).
        // Normal cycle: IDLE → DELAY → JACK_UP_START → JACK_UP → DRIVE → JACK_DOWN → IDLE
        switch (current_state) {
            case STATE_IDLE:
			    break;

            case STATE_MOVE_START_DELAY:
            	// 1s pause before moving — lets operator abort after pressing start
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_IDLE; // haven't raised jack yet, safe to just stop
                	log = true;
				} else if (timer_done()) {
					if (get_sensor(SENSOR_JACK)) {
						// Jack already home — skip the retract and go straight up.
						// The jack has been off through this delay, so this is a
						// clean off→forward start with no motor reversal.
						current_state = STATE_JACK_UP_START;
						set_timer((uint64_t)JACK_PRE_TIME);
						jack_start_us = fsm_now;
					} else {
						// Jack left partway up — retract to home first so ride
						// height is correct regardless of where it was left.
						current_state = STATE_MOVE_JACK_RETRACT;
						set_timer((uint64_t)JACK_MAX_TIME);  // generous — home sensor is the real stop
					}
					log = true;
				}
                break;

            case STATE_MOVE_JACK_RETRACT:
            	// Drive the jack down to home before extending. Ends on the home
            	// sensor (normal), a jack efuse trip, or the timeout (safety net),
            	// then an all-off settle so the upcoming jack-up isn't a direct
            	// REV→FWD reversal (which the relay driver blocks).
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_IDLE; // jack is lowering — safe to just stop
                	log = true;
				} else if (get_sensor(SENSOR_JACK) || efuse_get(BRIDGE_JACK) || timer_done()) {
					current_state = STATE_MOVE_JACK_SETTLE;
					set_timer(TRANSITION_DELAY_US);
					log = true;
				}
                break;

            case STATE_MOVE_JACK_SETTLE:
            	// All motors off (~1s) so the jack de-energizes after the retract,
            	// then begin the upward move with a clean off→forward transition.
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_IDLE; // jack is off — safe to just stop
                	log = true;
				} else if (timer_done()) {
					current_state = STATE_JACK_UP_START;
					set_timer((uint64_t)JACK_PRE_TIME);  // phase 1: raise JACK_PRE_DIST, or until current/e-fuse
					jack_start_us = fsm_now;
					log = true;
				}
                break;

            case STATE_JACK_UP_START:
            	// Phase 1 (pre-jack): raise until the jack engages the load — ends on
            	// the up-current threshold (JACK_I_UP), an e-fuse trip, or the
            	// JACK_PRE_DIST timeout. Then phase 2 lifts an additional JACK_DIST.
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_UNDO_JACK_START;
					jack_finish_us = fsm_now;
                	log = true;
				} else if (jack_pos_us >= (int64_t)JACK_MAX_TIME) {
					// Hit the JACK_MAX height ceiling during pre-jack — never lift
					// past JACK_MAX. Skip phase 2 and go drive.
					ESP_LOGI(TAG, "START->DRIVE BY JACK_MAX");
					current_state = STATE_DRIVE_START_DELAY;
					jack_finish_us = fsm_now;
					set_timer(TRANSITION_DELAY_US);
					log = true;
				} else {
					if (efuse_get(BRIDGE_JACK)) {
						ESP_LOGI(TAG, "START->UP BY EFUSE");
						current_state = STATE_JACK_UP;
						jack_trans_us = fsm_now;
	                	log = true;
						set_timer(JACK_TIME);
					}

                	if (get_bridge_overcurrent(BRIDGE_JACK, get_param_value_t(PARAM_JACK_I_UP).f32)) {
						ESP_LOGI(TAG, "START->UP BY CURRENT");
						current_state = STATE_JACK_UP;
						jack_trans_us = fsm_now;
	                	log = true;
						set_timer(JACK_TIME);
					}

					if (timer_done()) {
						ESP_LOGI(TAG, "START->UP BY TIME");
						current_state = STATE_JACK_UP;
						jack_trans_us = fsm_now;
	                	log = true;
						set_timer(JACK_TIME);
					}
				}
                break;

            case STATE_JACK_UP:
            	// Phase 2: lift an additional JACK_DIST (JACK_TIME) — ends on timer,
            	// efuse, or the JACK_MAX height ceiling (never lift past JACK_MAX).
            	// Records finish time for the symmetric jack-down duration.
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_UNDO_JACK_START;
					jack_finish_us = fsm_now;
					set_timer(JACK_DOWN_TIME);
                	log = true;
				} else {
                	if (timer_done() || efuse_get(BRIDGE_JACK) ||
                	    jack_pos_us >= (int64_t)JACK_MAX_TIME) {
						current_state  = STATE_DRIVE_START_DELAY;
						jack_finish_us = fsm_now; // used to calculate symmetric jack-down duration
						log = true;
						set_timer(TRANSITION_DELAY_US);
					}
				}
                break;

            case STATE_DRIVE_START_DELAY:
            	// 1s quiet pause between jack-up and fluffer spin-up.
            	// All motors off here so the jack-up current fully settles
            	// before we energize the fluffer.
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_UNDO_JACK_START;
					set_timer(JACK_DOWN_TIME);
                	log = true;
				} else if (timer_done()) {
					current_state = STATE_DRIVE_FLUFF_START;
            		log = true;
					set_timer((uint64_t)get_param_value_t(PARAM_FLUFF_PREDRIVE_MS).u32 * 1000);
				}
                break;

            case STATE_DRIVE_FLUFF_START:
            	// Fluffer alone for 1s, then drive+fluffer. Splits the old
            	// "jack-up+fluff concurrent" sequence so aux never overlaps
            	// with jack on V5's shared current sensor.
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_UNDO_JACK_START;
					set_timer(JACK_DOWN_TIME);
                	log = true;
				} else if (efuse_get(BRIDGE_AUX)) {
					fsm_error = SC_ERR_EFUSE_TRIP_3;
					current_state = STATE_UNDO_JACK_START;
					set_timer(JACK_DOWN_TIME);
					log = true;
				} else if (timer_done()) {
					current_state = STATE_DRIVE;
            		log = true;
					set_timer(DRIVE_TIME);
					// Encoder counts down from -target to 0 (negative = distance remaining)
					set_sensor_counter(SENSOR_DRIVE, -DRIVE_DIST);
					move_start_encoder = get_sensor_counter(SENSOR_DRIVE);
				}
                break;

            case STATE_DRIVE:
            	// Horizontal travel — stops on timer, encoder target, or efuse trip
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_UNDO_JACK_START;
					set_timer(JACK_DOWN_TIME);
            		log = true;
				} else if (efuse_get(BRIDGE_DRIVE)) {
					// Fault — deduct actual distance traveled (may be partial).
					// Checked before the normal-completion branch so a tick
					// that satisfies both conditions doesn't double-deduct
					// remaining_distance.
					int32_t current_encoder = get_sensor_counter(SENSOR_DRIVE);
					int32_t ticks_traveled = current_encoder - move_start_encoder;
					float ke = get_param_value_t(PARAM_DRIVE_KE).f32;
					float distance_traveled = ticks_traveled / ke;

					remaining_distance -= distance_traveled;
					if (remaining_distance < 0.0f) remaining_distance = 0.0f;

					fsm_error = SC_ERR_EFUSE_TRIP_1;
					current_state = STATE_UNDO_JACK_START;
					set_timer(JACK_DOWN_TIME);
            		log = true;
				} else {
					int32_t current_encoder = get_sensor_counter(SENSOR_DRIVE);
					if (timer_done() || current_encoder > 0) {
						// Normal completion — deduct planned distance from remaining travel
						remaining_distance -= this_move_dist;

						current_state = STATE_DRIVE_END_DELAY;
                		log = true;
						set_timer(TRANSITION_DELAY_US);
					}
				}
                break;

            case STATE_DRIVE_END_DELAY:
            	// 1s pause after drive — then lower jack normally.
            	// Goes straight to STATE_JACK_DOWN so the LED/comms message
            	// reads "MOVING…" rather than "CANCELLING MOVE" on a normal
            	// cycle. STATE_UNDO_JACK_START remains the path for explicit
            	// undo / safety-break / efuse-trip recovery.
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_UNDO_JACK_START;
					set_timer(JACK_DOWN_TIME);
                	log = true;
				} else if (timer_done()) {
					current_state = STATE_JACK_DOWN;
					set_timer(JACK_DOWN_TIME);
            		log = true;
				}
				break;

            case STATE_JACK_DOWN:
            	// Lower jack — stops on efuse (hit ground), position sensor, or timeout
            	if (efuse_get(BRIDGE_JACK)) {
					ESP_LOGI(TAG, "DOWN->IDLE BY EFUSE");
					current_state = STATE_IDLE;
					log = true;
					break;
				}

				if (get_sensor(SENSOR_JACK)) {
					ESP_LOGI(TAG, "DOWN->IDLE BY SENSOR");
					current_state = STATE_IDLE;
					log = true;
					break;
				}

				if (timer_done()) {
					ESP_LOGI(TAG, "DOWN->IDLE BY TIME");
					current_state = STATE_IDLE;
					log = true;
					break;
				}
                break;

            case STATE_UNDO_JACK_START:
            	// Emergency: wait for jack efuse to cool, then lower
            	if (!efuse_get(BRIDGE_JACK)) {
					set_timer(JACK_DOWN_TIME);
					current_state = STATE_JACK_DOWN;
            		log = true;
				}
				break;

			case STATE_CALIBRATE_JACK_DELAY:
				break; // waiting for user command to begin measurement
			case STATE_CALIBRATE_JACK_MOVE:
				if (timer_done()) {
					current_state = STATE_IDLE;
					fsm_cal_t = fsm_now - timer_start;
				}
				break;

			case STATE_CALIBRATE_DRIVE_DELAY:
				break; // waiting for user command to begin measurement
			case STATE_CALIBRATE_DRIVE_MOVE:
				if (!get_is_safe() || timer_done()) {
					current_state = STATE_IDLE;
					fsm_cal_t = fsm_now - timer_start;
					fsm_cal_e = get_sensor_counter(SENSOR_DRIVE);
				}
				break;

            default: break;
        }
        
		/**** SET OUTPUTS ****/
        switch (current_state) {
            case STATE_IDLE: {
            	// In idle we still accept override commands. Snapshot both fields
            	// atomically to defend against the int64 torn read on writers.
            	int64_t local_time;
            	fsm_override_t local_cmd;
            	override_snapshot(&local_time, &local_cmd);
			    if (local_time > fsm_now) {
					switch(local_cmd) {
						case FSM_OVERRIDE_DRIVE_FWD:
	                        if (efuse_get(BRIDGE_DRIVE)){
				            	drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_OFF
								}});
							} else {
								drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_FWD,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_FWD
								}});
							}
							break;

                        case FSM_OVERRIDE_DRIVE_REV:
	                        if (efuse_get(BRIDGE_DRIVE)){
				            	drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_OFF
								}});
							} else {
								drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_REV,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_OFF
								}});
							}
                            break;
                        case FSM_OVERRIDE_JACK_UP:
			            	if (efuse_get(BRIDGE_JACK) || jack_pos_us >= (int64_t)JACK_MAX_TIME) {
				            	drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_OFF
								}});
							} else {
								drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_FWD,
									.AUX=BRIDGE_OFF
								}});
							}
                            break;
                        case FSM_OVERRIDE_JACK_UP_FORCE:
                        	// Force jack up past the JACK_MAX cap — only the efuse stops it.
			            	if (efuse_get(BRIDGE_JACK)) {
				            	drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_OFF
								}});
							} else {
								drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_FWD,
									.AUX=BRIDGE_OFF
								}});
							}
                            break;
                        case FSM_OVERRIDE_JACK_DOWN:
                        	/*if (get_bridge_overcurrent(BRIDGE_JACK, get_param_value_t(PARAM_JACK_I_DOWN).f32) ||
			            	    get_bridge_spike(BRIDGE_JACK, get_param_value_t(PARAM_JACK_IS_DOWN).f32))
			            	    efuse_set(BRIDGE_JACK, EFUSE_OVERCURRENT);
                        	*/
                        	if (get_sensor(SENSOR_JACK) || efuse_get(BRIDGE_JACK)) {
								drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_OFF
								}});
							} else {
								drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_REV,
									.AUX=BRIDGE_OFF
								}});
							}
                            break;
                        case FSM_OVERRIDE_JACK_DOWN_FORCE:
                        	// Force jack down past the home sensor — only the efuse stops it.
			            	if (efuse_get(BRIDGE_JACK)) {
				            	drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_OFF
								}});
							} else {
								drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_REV,
									.AUX=BRIDGE_OFF
								}});
							}
                            break;
                        case FSM_OVERRIDE_AUX:
	                        if (efuse_get(BRIDGE_AUX)){
				            	drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_OFF
								}});
							} else {
								drive_relays((relay_port_t){.bridges = {
									.DRIVE=BRIDGE_OFF,
									.JACK=BRIDGE_OFF,
									.AUX=BRIDGE_FWD
								}});
							}
                            break;
                        default: // should never hit here but just in case...
			            	drive_relays((relay_port_t){.bridges = {
								.DRIVE=BRIDGE_OFF,
								.JACK=BRIDGE_OFF,
								.AUX=BRIDGE_OFF
							}});
							break;
                        }
		            rtc_reset_shutdown_timer();
		            log = true;
        		} else {
	            	drive_relays((relay_port_t){.bridges = {
						.DRIVE=BRIDGE_OFF,
						.JACK=BRIDGE_OFF,
						.AUX=BRIDGE_OFF
					}});
				}
			    break;
			} /* close STATE_IDLE block scope */
			case STATE_CALIBRATE_JACK_MOVE:
            case STATE_JACK_UP_START:
            case STATE_JACK_UP:
            	// jack up only — fluffer is deferred to STATE_DRIVE_FLUFF_START
            	// so aux and jack never energize together.
            	drive_relays((relay_port_t){.bridges = {
					.DRIVE=BRIDGE_OFF,
					.JACK=BRIDGE_FWD,
					.AUX=BRIDGE_OFF
				}});
                rtc_reset_shutdown_timer();
                log = true;
                break;
            case STATE_CALIBRATE_DRIVE_MOVE:
            case STATE_DRIVE:
            	// drive and fluff
            	drive_relays((relay_port_t){.bridges = {
					.DRIVE=BRIDGE_FWD,
					.JACK=BRIDGE_OFF,
					.AUX=BRIDGE_FWD
				}});
                rtc_reset_shutdown_timer();
                log = true;
                break;
            case STATE_MOVE_JACK_RETRACT:
            case STATE_JACK_DOWN:
            	drive_relays((relay_port_t){.bridges = {
					.DRIVE=BRIDGE_OFF,
					.JACK=BRIDGE_REV,
					.AUX=BRIDGE_OFF
				}});
                rtc_reset_shutdown_timer();
                log = true;
                break;
			case STATE_MOVE_JACK_SETTLE:
			case STATE_DRIVE_START_DELAY:
            	// Quiet 1s — all motors off. DRIVE_START_DELAY: let jack-up current
            	// settle before the fluffer starts. MOVE_JACK_SETTLE: let the jack
            	// de-energize after the retract before the clean off→forward jack-up.
            	drive_relays((relay_port_t){.bridges = {
					.DRIVE=BRIDGE_OFF,
					.JACK=BRIDGE_OFF,
					.AUX=BRIDGE_OFF
				}});
                rtc_reset_shutdown_timer();
                log = true;
                break;
            case STATE_DRIVE_FLUFF_START:
            case STATE_UNDO_JACK_START:
            case STATE_DRIVE_END_DELAY:
            	// only fluffer
            	drive_relays((relay_port_t){.bridges = {
					.DRIVE=BRIDGE_OFF,
					.JACK=BRIDGE_OFF,
					.AUX=BRIDGE_FWD
				}});
                rtc_reset_shutdown_timer();
                log = true;
                break;
            case STATE_CALIBRATE_JACK_DELAY:
            default:
            	// invalid state; turn all relays off
            	drive_relays((relay_port_t){.bridges = {
					.DRIVE=BRIDGE_OFF,
					.JACK=BRIDGE_OFF,
					.AUX=BRIDGE_OFF
				}});
            	break;
        }
        
        
        /**** JACK POSITION TRACKING ****/
        /* Update jack_pos_us each tick based on what the relay outputs just did.
         * SENSOR_JACK tripping is the definitive home reset (overrides everything). */
        {
            const int64_t TICK_US = 20000LL;
            bridge_dir_t jack_dir = BRIDGE_OFF;

            switch (current_state) {
                case STATE_JACK_UP_START:
                case STATE_JACK_UP:
                case STATE_CALIBRATE_JACK_MOVE:
                    jack_dir = BRIDGE_FWD;
                    break;
                case STATE_MOVE_JACK_RETRACT:
                case STATE_JACK_DOWN:
                    jack_dir = BRIDGE_REV;
                    break;
                case STATE_IDLE: {
                    int64_t local_time;
                    fsm_override_t local_cmd;
                    override_snapshot(&local_time, &local_cmd);
                    if (local_time > fsm_now && !efuse_get(BRIDGE_JACK)) {
                        if ((local_cmd == FSM_OVERRIDE_JACK_UP && jack_pos_us < (int64_t)JACK_MAX_TIME) ||
                             local_cmd == FSM_OVERRIDE_JACK_UP_FORCE)
                            jack_dir = BRIDGE_FWD;
                        else if (local_cmd == FSM_OVERRIDE_JACK_DOWN ||
                                 local_cmd == FSM_OVERRIDE_JACK_DOWN_FORCE)
                            jack_dir = BRIDGE_REV;
                    }
                    break;
                }
                default: break;
            }

            if (jack_dir == BRIDGE_FWD)
                jack_pos_us += TICK_US;
            else if (jack_dir == BRIDGE_REV) {
                jack_pos_us -= TICK_US;
                if (jack_pos_us < 0LL) jack_pos_us = 0LL;
            }

            if (get_sensor(SENSOR_JACK))
                jack_pos_us = 0LL;
        }

        /**** LOGGING ****/
        if (log) send_fsm_log();

            
        esp_task_wdt_reset();
    }
    
    if (fsm_cmd_queue != NULL) {
        vQueueDelete(fsm_cmd_queue);
        fsm_cmd_queue = NULL;
    }
}

esp_err_t fsm_init() {
    if (fsm_cmd_queue == NULL) {
        fsm_cmd_queue = xQueueCreate(8, sizeof(fsm_cmd_t));
    }
    xTaskCreate(control_task, TAG, 4096, NULL, 10, NULL);

	return ESP_OK;
}


esp_err_t fsm_stop() { return ESP_OK; }