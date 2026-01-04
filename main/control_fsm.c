/*
 * control_fsm.c
 *
 *  Created on: Nov 10, 2025
 *      Author: Thad
 */

#include "control_fsm.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "i2c.h"
#include "power_mgmt.h"
#include "rtc_wdt.h"
#include "driver/gpio.h"
#include "storage.h"
#include "rtc.h"
#include "sensors.h"
#include "esp_log.h"

#define TRANSITION_DELAY_US 1000000

#define CALIBRATE_JACK_MAX_TIME  3000000
#define CALIBRATE_DRIVE_MAX_TIME 6000000

#define TAG "FSM"

static QueueHandle_t fsm_cmd_queue = NULL;
	
// map from relay number to bridge
bridge_t bridge_map[] = {
	BRIDGE_AUX,
	BRIDGE_AUX,
	BRIDGE_AUX,
	BRIDGE_AUX,
	BRIDGE_JACK,
	BRIDGE_JACK,
	BRIDGE_DRIVE,
	BRIDGE_DRIVE };

bool relay_states[8] = {false};
int64_t override_times[8] = {-1};
int64_t override_cooldown[8] = {-1};
bool enabled = false;


RTC_DATA_ATTR float remaining_distance = 0.0f;
float fsm_get_remaining_distance(void)    { return remaining_distance; }
void  fsm_set_remaining_distance(float x) { remaining_distance = x;}

// Track the starting encoder count for the current move
static int32_t move_start_encoder = 0;

// Track total jack up time to use for jack down duration
static int64_t jack_up_total_time = 0;

volatile fsm_state_t current_state = STATE_IDLE;
volatile int64_t current_time = 0;
volatile bool start_running_request = false;

void setRelay(int8_t relay, bool state) {
	relay_states[relay] = state;
}

bool isRunning() {
	for (int i=0;i<8;i++) {
		if (relay_states[i]) return true;
	}
	return false;
}

void driveRelays() {
	uint8_t state = 0x00;
	//relay_states[0] = (current_time / 1000000) % 2; // for testing purposes
	
	for (uint8_t i=0; i<8; i++) {
		// if we command and efuse permits it set the relay
		if (relay_states[i] && !efuse_is_tripped(bridge_map[i])) {
			state |= 0x01<<i;
			set_autozero(bridge_map[i]);
		}
	}
	
	//ESP_LOGI(TAG, "RELAY STATE: %x", state);
	i2c_set_relays(state);
}



fsm_state_t fsm_get_state() {
	return current_state;
}

static int64_t timer_end = 0;
static int64_t timer_start = 0;
static inline void set_timer(uint64_t us) {
	timer_end = current_time + us;
	timer_start = current_time;
}
static inline bool timer_done() { return current_time >= timer_end; }

void pulseOverride(relay_t relay) {
    if (current_state == STATE_IDLE) {
        // Check if this relay is in cooldown
        if (override_cooldown[relay] > current_time) {
            // Still cooling down, ignore the command
            return;
        }
        override_times[relay] = current_time + get_param_value_t(PARAM_RF_PULSE_LENGTH).u32;
    }
}

/*void fsm_begin_auto_move() {
	if (current_state == STATE_IDLE)
		current_state = STATE_MOVE_START_DELAY;
	set_timer(TRANSITION_DELAY_US);
}*/

int64_t fsm_cal_t, fsm_cal_e;
float fsm_cal_val;
void fsm_set_cal_val(float v) {fsm_cal_val = v;}
int64_t fsm_get_cal_t(){return fsm_cal_t;}
int64_t fsm_get_cal_e(){return fsm_cal_e;}

void fsm_request(fsm_cmd_t cmd)
{
	if (fsm_cmd_queue != NULL)
	    xQueueSend(fsm_cmd_queue, &cmd, 0);  // Safe from any context
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
		case STATE_DRIVE_END_DELAY:
		case STATE_UNDO_JACK:
			if (timer_end != timer_start)
				x = (current_time-timer_start)*denominator/(timer_end-timer_start);
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
#define DRIVE_TIME get_param_value_t(PARAM_DRIVE_KT).f32 * get_param_value_t(PARAM_DRIVE_DIST).f32
#define DRIVE_DIST get_param_value_t(PARAM_DRIVE_KE).f32 * get_param_value_t(PARAM_DRIVE_DIST).f32

void control_task(void *param) {
	esp_task_wdt_add(NULL);
	
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(20);
    enabled = true;
    

    while (enabled) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        current_time = esp_timer_get_time();
        
        fsm_cmd_t cmd;
        while (xQueueReceive(fsm_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd) {
                case FSM_CMD_START:
                    if (current_state == STATE_IDLE) {
                    	// Check if we have remaining distance before starting
                    	if (remaining_distance > 0.0f
                    		 && !efuse_is_tripped(BRIDGE_DRIVE)
                    		  && !efuse_is_tripped(BRIDGE_JACK)
                    		   && !efuse_is_tripped(BRIDGE_AUX)) {
							current_state = STATE_MOVE_START_DELAY;
							set_timer(TRANSITION_DELAY_US);
						} else {
							ESP_LOGW(TAG, "Cannot start move: no remaining distance (%.2f)", remaining_distance);
						}
                    }
                    break;
                case FSM_CMD_STOP:
                    current_state = STATE_IDLE;
                    break;
                case FSM_CMD_UNDO:
                    if (current_state != STATE_IDLE &&
                        current_state != STATE_UNDO_JACK_START &&
                        current_state != STATE_UNDO_JACK) {
                        current_state = STATE_UNDO_JACK_START;
                    }
                    break;
                case FSM_CMD_SHUTDOWN:
                	enabled = false;
                	break;
                	
                case FSM_CMD_CALIBRATE_JACK_PREP:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_PREP");
					if (current_state == STATE_IDLE) {
						current_state = STATE_CALIBRATE_JACK_DELAY;
					}
					break;
                	
				case FSM_CMD_CALIBRATE_JACK_START:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_START");
					if (current_state == STATE_CALIBRATE_JACK_DELAY) {
						current_state = STATE_CALIBRATE_JACK_MOVE;
						set_timer(CALIBRATE_JACK_MAX_TIME);
					}
					break;
				case FSM_CMD_CALIBRATE_JACK_END:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_END");
					if (current_state == STATE_CALIBRATE_JACK_MOVE) {
						fsm_cal_t = current_time - timer_start;
						current_state = STATE_IDLE;
					}
					break;
				case FSM_CMD_CALIBRATE_JACK_FINISH:				
					set_param_value_t(PARAM_JACK_KT, 
					(param_value_t){.f32 = fsm_cal_t / fsm_cal_val});
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_FINISH -> %f", get_param_value_t(PARAM_JACK_KT).f32);
					break;
					
					
                	
                case FSM_CMD_CALIBRATE_DRIVE_PREP:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_PREP");
					if (current_state == STATE_IDLE) {
						current_state = STATE_CALIBRATE_DRIVE_DELAY;
					}
					break;
                	
				case FSM_CMD_CALIBRATE_DRIVE_START:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_START");
					if (current_state == STATE_CALIBRATE_DRIVE_DELAY) {
						current_state = STATE_CALIBRATE_DRIVE_MOVE;
						set_timer(CALIBRATE_DRIVE_MAX_TIME);
						set_sensor_counter(SENSOR_DRIVE, 0);
					}
					break;
				case FSM_CMD_CALIBRATE_DRIVE_END:
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_END");
					if (current_state == STATE_CALIBRATE_DRIVE_MOVE) {
						fsm_cal_t = current_time - timer_start;
						fsm_cal_e = get_sensor_counter(SENSOR_DRIVE);
						current_state = STATE_IDLE;
					}
					break;
				case FSM_CMD_CALIBRATE_DRIVE_FINISH:				
					set_param_value_t(PARAM_DRIVE_KT, 
					(param_value_t){.f32 = fsm_cal_t / fsm_cal_val});
					set_param_value_t(PARAM_DRIVE_KE, 
					(param_value_t){.f32 = fsm_cal_e / fsm_cal_val});
					ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_FINISH -> %f / %f",
					  get_param_value_t(PARAM_DRIVE_KT).f32,
					  get_param_value_t(PARAM_DRIVE_KE).f32);
					break;
            }
        }
        
        if (!enabled) break;


		
        // State transitions
        switch (current_state) {
            case STATE_IDLE:
			    //ESP_LOGI("FSM", "IDLE @ %lld", current_time);
			    for (uint8_t i = 0; i < N_RELAYS; ++i) {
			        //ESP_LOGI("FSM", "t[%d] %lld", i, override_times[i]);
			        bool active = override_times[i] > current_time;
			        if (active) rtc_reset_shutdown_timer();
			
			        // Current limiting for manual jack down override (RELAY_B2)
			        if (i == RELAY_B2 && active) {
			            int64_t elapsed = current_time - (override_times[i] - get_param_value_t(PARAM_RF_PULSE_LENGTH).u32);
			            int64_t delay = get_param_value_t(PARAM_EFUSE_INRUSH_US).u32;
			
			            // After inrush delay, check for current spike
			            if (elapsed > delay) {
			                float current = get_bridge_A(BRIDGE_JACK);
			                float threshold = get_param_value_t(PARAM_JACK_I_DOWN).f32;
			
			                if (current > threshold) {
			                    // Current spike detected - stop jacking down and start cooldown
			                    override_times[i] = -1;
			                    override_cooldown[i] = current_time + get_param_value_t(PARAM_EFUSE_TCOOL).u32;
			                    active = false;
			                }
			            }
			        }
			
			        // prohibit movement past jack limit switch
			        //if (i == BRIDGE_JACK*2+(bridge_polarities[BRIDGE_JACK]>0?0:1) && get_sensor(SENSOR_JACK))
			        //	setRelay(i, false);
			        //else
			        	setRelay(i, active);
			        //if (active) ESP_LOGI("FSM", "RUN CHANNEL %d (%lld %c %lld)", i, (long long) override_times[i], active ? '>':'<', (long long) current_time);
			
			    }
			    break;
            case STATE_MOVE_START_DELAY:
                if (timer_done()) {
					current_state = STATE_JACK_UP_START;
					set_timer(JACK_TIME / 2);  // First phase is half of total jack time
					jack_up_total_time = 0;     // Reset jack up time tracker
				}
                break;
            case STATE_JACK_UP_START:
                {
                	// Track elapsed time
                	int64_t elapsed = current_time - timer_start;
                	jack_up_total_time = elapsed;
                	
                	// Get current sensing parameters
                	int64_t delay   = get_param_value_t(PARAM_EFUSE_INRUSH_US).u32;
                	float current   = get_bridge_A(BRIDGE_JACK);
                	float threshold = get_param_value_t(PARAM_JACK_I_UP).f32;
                	
                	// After inrush delay, check for current spike OR half-time timeout
                	if (elapsed > delay) {
                		if (current > threshold || timer_done()) {
							ESP_LOGI(TAG, "START->UP BY CURRENT");
							current_state = STATE_JACK_UP;
							set_timer(JACK_TIME);  // Second phase is also half of total jack time
						}
					}
					
					// E-fuse trip should still cause undo
					if (efuse_is_tripped(BRIDGE_JACK)) {
						ESP_LOGI(TAG, "START->UP BY TIME");
						current_state = STATE_UNDO_JACK_START;
					}
				}
                break;
            case STATE_JACK_UP:
                {                	
                	if (timer_done() || efuse_is_tripped(BRIDGE_JACK)) {
                		// Track total time including first phase
                		jack_up_total_time += current_time - timer_start;
						current_state = STATE_DRIVE_START_DELAY;
						set_timer(TRANSITION_DELAY_US);
					}
					if (efuse_is_tripped(BRIDGE_JACK)) {
						ESP_LOGE(TAG, "JACK TRIPPED EFUSE");
						current_state = STATE_UNDO_JACK_START;
					}
				}
                break;
            case STATE_DRIVE_START_DELAY:
                if (timer_done()) {
					current_state = STATE_DRIVE;
					set_timer(DRIVE_TIME);
					// Set the encoder counter to track remaining distance in this move
					set_sensor_counter(SENSOR_DRIVE, -DRIVE_DIST);
					// Record starting encoder position AFTER setting it
					move_start_encoder = get_sensor_counter(SENSOR_DRIVE);
					ESP_LOGI(TAG, "STATE_DRIVE starting: encoder=%ld, remaining_distance=%.2f, DRIVE_DIST=%.2f", 
					         (long)move_start_encoder, remaining_distance, DRIVE_DIST);
				}
                break;
            case STATE_DRIVE:
            	{
					int32_t current_encoder = get_sensor_counter(SENSOR_DRIVE);
					int32_t ticks_traveled = current_encoder - move_start_encoder;
					float ke = get_param_value_t(PARAM_DRIVE_KE).f32;
					float distance_traveled = ticks_traveled / ke;
					
					ESP_LOGI(TAG, "STATE_DRIVE: current_encoder=%ld, move_start=%ld, ticks=%ld, ke=%.2f, dist_traveled=%.2f, remaining=%.2f",
					         (long)current_encoder, (long)move_start_encoder, (long)ticks_traveled, 
					         ke, distance_traveled, remaining_distance);
					
					// Check if we'll exceed remaining distance with a full move
					bool will_exceed = distance_traveled >= remaining_distance;
					
					// Stop if timer expires OR encoder target reached OR we've used up remaining distance
					if (timer_done() || current_encoder > 0 || will_exceed) {
						ESP_LOGI(TAG, "Drive stopping: timer_done=%d, encoder>0=%d, will_exceed=%d",
						         timer_done(), current_encoder > 0, will_exceed);
						
						// Update remaining distance based on actual travel
						float old_remaining = remaining_distance;
						if (will_exceed) {
							ESP_LOGI(TAG, "Move stopped early - reached remaining distance limit (%.2f)", remaining_distance);
							remaining_distance = 0.0f;
						} else {
							remaining_distance -= distance_traveled;
							if (remaining_distance < 0.0f) remaining_distance = 0.0f;
						}
						ESP_LOGI(TAG, "Drive complete: traveled %.2f, old_remaining %.2f, new_remaining %.2f", 
						         distance_traveled, old_remaining, remaining_distance);
						
						current_state = STATE_DRIVE_END_DELAY;
						set_timer(TRANSITION_DELAY_US);
					}
					
					if (efuse_is_tripped(BRIDGE_DRIVE)) {
						float old_remaining = remaining_distance;
						// Update remaining distance even on fault
						remaining_distance -= distance_traveled;
						if (remaining_distance < 0.0f) remaining_distance = 0.0f;
						ESP_LOGW(TAG, "Drive fault: traveled %.2f, old_remaining %.2f, new_remaining %.2f", 
						         distance_traveled, old_remaining, remaining_distance);
						current_state = STATE_UNDO_JACK_START;
					}
				}
                break;
            case STATE_DRIVE_END_DELAY:
                if (timer_done()) {
					current_state = STATE_JACK_DOWN;
					set_timer(jack_up_total_time);  // Use the tracked jack up time
				}
				break;
            case STATE_JACK_DOWN:
                {
                	// Get current sensing parameters
                	int64_t delay = get_param_value_t(PARAM_EFUSE_INRUSH_US).u32;
                	int64_t elapsed = current_time - timer_start;
                	
                	// After inrush delay, check for current spike
                	if (elapsed > delay) {
                		float current   = get_bridge_A(BRIDGE_JACK);
                		float threshold = get_param_value_t(PARAM_JACK_I_DOWN).f32;
                		
                		if (current > threshold) {
							ESP_LOGI(TAG, "DOWN->IDLE BY CURRENT");
							// Current spike detected - we've hit the ground
							current_state = STATE_IDLE;
							break;
						}
					}
					
					// Timeout - finished jacking down
					if (timer_done()) {
						ESP_LOGI(TAG, "DOWN->IDLE BY TIME");
						current_state = STATE_IDLE;
					}
					
					// E-fuse trip - assume we hit something hard
					if (efuse_is_tripped(BRIDGE_JACK)) {
						current_state = STATE_IDLE;
					}
				}
                break;
                
                
            case STATE_UNDO_JACK_START:
            	// wait for e-fuse to un-trip
            	if (!efuse_is_tripped(BRIDGE_JACK)) {
					current_state = STATE_UNDO_JACK;
					set_timer(JACK_TIME);
				}
				break;
			case STATE_UNDO_JACK:
                if (timer_done()){ // || get_sensor(SENSOR_JACK)) {
					current_state = STATE_IDLE;
				}
				
				// assume we are jacked up all the way (e.g. sensor broke) and should stop
				if (efuse_is_tripped(BRIDGE_JACK)) {
					current_state = STATE_IDLE;
				}
				break;
				
				
			case STATE_CALIBRATE_JACK_DELAY:
				// no way out of this except a command
				break;
			case STATE_CALIBRATE_JACK_MOVE:
				if (timer_done()) {
					ESP_LOGI(TAG, "STATE_CALIBRATE_JACK_END");
					current_state = STATE_IDLE;
					fsm_cal_t = current_time - timer_start;
				}
				break;
				
				
			case STATE_CALIBRATE_DRIVE_DELAY:
				// no way out of this except a command
				break;
			case STATE_CALIBRATE_DRIVE_MOVE:
				if (timer_done()) {
					ESP_LOGI(TAG, "STATE_CALIBRATE_DRIVE_END");
					current_state = STATE_IDLE;
					fsm_cal_t = current_time - timer_start;
					fsm_cal_e = get_sensor_counter(SENSOR_DRIVE);
				}
				break;
				
            default: break;
        }
        
       
		//int64_t elapsed_t = (current_time-timer_start);
		//int64_t total_t   = (timer_end-timer_start);
		//int32_t ticks     = get_sensor_counter(SENSOR_DRIVE);
		//ESP_LOGI("FSM", "[%d] %lld / %lld ms, %ld ticks", current_state, (long long) elapsed_t, (long long) total_t, (long) ticks);

        // Output control
        switch (current_state) {
            case STATE_IDLE:
			    //ESP_LOGI("FSM", "IDLE @ %lld", current_time);
			    for (uint8_t i = 0; i < N_RELAYS; ++i) {
			        //ESP_LOGI("FSM", "t[%d] %lld", i, override_times[i]);
			        bool active = override_times[i] > current_time;
			        if (active) rtc_reset_shutdown_timer();
			        
			        // Current limiting for manual jack down override (RELAY_B2)
			        if (i == RELAY_B2 && active) {
			            int64_t elapsed = current_time - (override_times[i] - get_param_value_t(PARAM_RF_PULSE_LENGTH).u32);
			            int64_t delay = get_param_value_t(PARAM_EFUSE_INRUSH_US).u32;
			            
			            // After inrush delay, check for current spike
			            if (elapsed > delay) {
			                float current = get_bridge_A(BRIDGE_JACK);
			                float threshold = get_param_value_t(PARAM_JACK_I_DOWN).f32;
			                
			                if (current > threshold) {
			                    // Current spike detected - stop jacking down
			                    override_times[i] = -1;
			                    active = false;
			                }
			            }
			        }
			        
			        // prohibit movement past jack limit switch
			        //if (i == BRIDGE_JACK*2+(bridge_polarities[BRIDGE_JACK]>0?0:1) && get_sensor(SENSOR_JACK))
			        //	setRelay(i, false);
			        //else
			        	setRelay(i, active);
			        //if (active) ESP_LOGI("FSM", "RUN CHANNEL %d (%lld %c %lld)", i, (long long) override_times[i], active ? '>':'<', (long long) current_time);
			    
			    }
			    break;
			case STATE_CALIBRATE_JACK_MOVE:
            case STATE_JACK_UP_START:
            case STATE_JACK_UP:
            	// jack up and fluff
            	setRelay(RELAY_A1, false);
            	setRelay(RELAY_B1, false);
            	
            	setRelay(RELAY_A2, true);
            	setRelay(RELAY_B2, false);
            	
            	setRelay(RELAY_A3, true);
                rtc_reset_shutdown_timer();
                break;
            case STATE_CALIBRATE_DRIVE_MOVE:
            case STATE_DRIVE:
            	// drive and fluff
            	setRelay(RELAY_A1, true);
            	setRelay(RELAY_B1, false);
            	
            	setRelay(RELAY_A2, false);
            	setRelay(RELAY_B2, false);
            	
            	setRelay(RELAY_A3, true);
                rtc_reset_shutdown_timer();
                break;
			case STATE_UNDO_JACK:
            case STATE_JACK_DOWN:
            	// jack down and fluffer
            	setRelay(RELAY_A1, false);
            	setRelay(RELAY_B1, false);
            	
            	setRelay(RELAY_A2, false);
            	setRelay(RELAY_B2, true);
            	
            	setRelay(RELAY_A3, true);
                rtc_reset_shutdown_timer();
                break;
			case STATE_UNDO_JACK_START:
            case STATE_DRIVE_START_DELAY:
            case STATE_DRIVE_END_DELAY:
            	// only fluffer
            	setRelay(RELAY_A1, false);
            	setRelay(RELAY_B1, false);
            	
            	setRelay(RELAY_A2, false);
            	setRelay(RELAY_B2, false);
            	
            	setRelay(RELAY_A3, true);
                rtc_reset_shutdown_timer();
                break;
            case STATE_CALIBRATE_JACK_DELAY:
            default:
            	// invalid state; turn all relays off
            	setRelay(RELAY_A1, false);
            	setRelay(RELAY_B1, false);
            	setRelay(RELAY_A2, false);
            	setRelay(RELAY_B2, false);
            	setRelay(RELAY_A3, false);
            	break;
        }

        driveRelays();
            
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
    xTaskCreate(control_task, "FSM", 4096, NULL, 5, NULL);

	return ESP_OK;
}


esp_err_t fsm_stop() { return ESP_OK; }