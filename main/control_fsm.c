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

// AUDIT: fsm_init() does not zero these — they persist across panics/WDT resets.
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

volatile fsm_state_t current_state = STATE_IDLE;
volatile int64_t fsm_now = 0;
volatile bool start_running_request = false;


fsm_state_t fsm_get_state() {
	return current_state;
}

bool fsm_is_idle(void) {
	return current_state == STATE_IDLE;
}

static int64_t timer_end = 0;
static int64_t timer_start = 0;
static inline void set_timer(uint64_t us) {
	timer_end = fsm_now + us;
	timer_start = fsm_now;
}
static inline bool timer_done() { return fsm_now >= timer_end; }

void pulse_override(fsm_override_t cmd) {
    if (soft_idle_is_active()) return;
    if (current_state == STATE_IDLE) {
        rtc_reset_shutdown_timer();
        int64_t deadline = fsm_now + (int64_t)get_param_value_t(PARAM_RF_PULSE_LENGTH).u32;
        portENTER_CRITICAL(&override_spin);
        override_cmd = cmd;
        override_time = deadline;
        portEXIT_CRITICAL(&override_spin);
    }
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

void fsm_request(fsm_cmd_t cmd)
{
    // STOP always goes through (safety). All other commands are blocked during soft idle —
    // the device must be woken by physical button or alarm before remote/RF movement is allowed.
    if (cmd != FSM_CMD_STOP && soft_idle_is_active()) return;

    rtc_reset_shutdown_timer();  // any accepted command extends the wake period
    if (fsm_cmd_queue != NULL)
        xQueueSend(fsm_cmd_queue, &cmd, 0);  // safe from any context
        // TODO: Make sure this is threadsafe
}

int8_t fsm_get_current_progress(int8_t denominator) {
	int8_t x = 0;
	switch (current_state) {
		case STATE_DRIVE:
		case STATE_JACK_UP_START:
		case STATE_JACK_UP:
		case STATE_JACK_DOWN:
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


#define JACK_TIME  get_param_value_t(PARAM_JACK_KT).f32  * get_param_value_t(PARAM_JACK_DIST ).f32

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

    float current_A = get_bridge_raw_A(BRIDGE_DRIVE)
                    + get_bridge_raw_A(BRIDGE_JACK)
                    + get_bridge_raw_A(BRIDGE_AUX);
    memcpy(&entry[12], &current_A, 4);

    int16_t be_counter = get_sensor_counter(SENSOR_DRIVE);
    memcpy(&entry[16], &be_counter, 2);

    entry[18] = pack_sensors();

    float heat = efuse_get_heat(BRIDGE_DRIVE);
    float h2 = efuse_get_heat(BRIDGE_JACK);
    float h3 = efuse_get_heat(BRIDGE_AUX);
    if (h2 > heat) heat = h2;
    if (h3 > heat) heat = h3;
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
						ESP_LOGI(TAG, "FAILED TO START; NO REMAINING DISTANCE");
                		fsm_error = SC_ERR_LEASH_HIT;
                		log = true;
                		continue;
                	}
                	this_move_dist = MIN(get_param_value_t(PARAM_DRIVE_DIST).f32, remaining_distance);
                	goto do_start;
                case FSM_CMD_START_IGNORE_OVERTRAVEL:
                	this_move_dist = get_param_value_t(PARAM_DRIVE_DIST).f32;
                do_start:
                    if (current_state == STATE_IDLE) {
                    	
                    	if (get_battery_V() < get_param_value_t(PARAM_LOW_PROTECTION_V).f32) {
							ESP_LOGI(TAG, "FAILED TO START; INSUFFICIENT VOLTAGE");
							fsm_error = SC_ERR_LOW_BATTERY;
							continue;
						}
                    	if (!get_is_safe()) {
							ESP_LOGI(TAG, "FAILED TO START; SAFETY NOT SET");
							fsm_error = SC_ERR_SAFETY_TRIP;
							continue;
						}
                    	if (efuse_get(BRIDGE_DRIVE)) {
							ESP_LOGI(TAG, "FAILED TO START; EFUSE 1 TRIP");
							fsm_error = SC_ERR_EFUSE_TRIP_1;
							continue;
						}
                    	if (efuse_get(BRIDGE_JACK)) {
							ESP_LOGI(TAG, "FAILED TO START; EFUSE 2 TRIP");
							fsm_error = SC_ERR_EFUSE_TRIP_2;
							continue;
						}
                    	if (efuse_get(BRIDGE_AUX)) {
							ESP_LOGI(TAG, "FAILED TO START; EFUSE 3 TRIP");
							fsm_error = SC_ERR_EFUSE_TRIP_3;
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
                	
                case FSM_CMD_CALIBRATE_JACK_PREP:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_PREP");
					if (current_state == STATE_IDLE
                    		&& get_battery_V() > get_param_value_t(PARAM_LOW_PROTECTION_V).f32) {
						current_state = STATE_CALIBRATE_JACK_DELAY;
                		log = true;
					}
					break;
                	
				case FSM_CMD_CALIBRATE_JACK_START:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_START");
					if (current_state == STATE_CALIBRATE_JACK_DELAY
                    		&& get_battery_V() > get_param_value_t(PARAM_LOW_PROTECTION_V).f32) {
						current_state = STATE_CALIBRATE_JACK_MOVE;
                		log = true;
						set_timer(CALIBRATE_JACK_MAX_TIME);
					}
					break;
				case FSM_CMD_CALIBRATE_JACK_END:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_END");
					if (current_state == STATE_CALIBRATE_JACK_MOVE) {
						fsm_cal_t = fsm_now - timer_start;
						current_state = STATE_IDLE;
                		log = true;
					}
					break;
                case FSM_CMD_CALIBRATE_DRIVE_PREP:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_PREP");
					if (current_state == STATE_IDLE
                    		&& get_battery_V() > get_param_value_t(PARAM_LOW_PROTECTION_V).f32) {
						current_state = STATE_CALIBRATE_DRIVE_DELAY;
                		log = true;
					}
					break;
                	
				case FSM_CMD_CALIBRATE_DRIVE_START:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_START");
					if (current_state == STATE_CALIBRATE_DRIVE_DELAY
                    		&& get_battery_V() > get_param_value_t(PARAM_LOW_PROTECTION_V).f32) {
						current_state = STATE_CALIBRATE_DRIVE_MOVE;
                		log = true;
						set_timer(CALIBRATE_DRIVE_MAX_TIME);
						set_sensor_counter(SENSOR_DRIVE, 0);
					}
					break;
				case FSM_CMD_CALIBRATE_DRIVE_END:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_END");
					if (current_state == STATE_CALIBRATE_DRIVE_MOVE) {
						fsm_cal_t = fsm_now - timer_start;
						fsm_cal_e = get_sensor_counter(SENSOR_DRIVE);
						current_state = STATE_IDLE;
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
            	// 1s pause before raising jack — lets operator abort after pressing start
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_IDLE; // haven't raised jack yet, safe to just stop
                	log = true;
				} else if (timer_done()) {
					current_state = STATE_JACK_UP_START;
					set_timer(JACK_TIME / 2);  // first phase: detect engagement (half of total jack time)
					jack_start_us = fsm_now;
				}
                break;

            case STATE_JACK_UP_START:
            	// Detect when jack engages the load (current spike, efuse, or timeout)
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_UNDO_JACK_START;
					jack_finish_us = fsm_now;
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
            	// Continue raising until timer or efuse — records finish time for symmetric jack-down
            	if (!get_is_safe()) {
					fsm_error = SC_ERR_SAFETY_TRIP;
					current_state = STATE_UNDO_JACK_START;
					jack_finish_us = fsm_now;
					set_timer(JACK_DOWN_TIME);
                	log = true;
				} else {
                	if (timer_done() || efuse_get(BRIDGE_JACK)) {
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
						// Normal completion — deduct planned distance from leash
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
			            	if (efuse_get(BRIDGE_JACK)){
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
            case STATE_JACK_DOWN:
            	drive_relays((relay_port_t){.bridges = {
					.DRIVE=BRIDGE_OFF,
					.JACK=BRIDGE_REV,
					.AUX=BRIDGE_OFF
				}});
                rtc_reset_shutdown_timer();
                log = true;
                break;
			case STATE_DRIVE_START_DELAY:
            	// Quiet 1s after jack-up — all motors off so jack current
            	// settles before the fluffer starts.
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