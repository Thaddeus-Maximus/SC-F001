/*
 * control_fsm.c
 *
 *  Created on: Nov 10, 2025
 *      Author: Thad
 */

#include "control_fsm.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "power_mgmt.h"
#include "rtc_wdt.h"
#include "driver/gpio.h"
#include "storage.h"
#include "rtc.h"
#include "sensors.h"
#include "esp_log.h"

#define TRANSITION_DELAY_US 1000000

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
bool enabled = false;




volatile fsm_state_t current_state = STATE_IDLE;
volatile int64_t current_time = 0;
volatile bool start_running_request = false;

void setRelay(int8_t relay, bool state) {
	relay_states[relay] = state;
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

static uint64_t timer_end = 0;
static uint64_t timer_start = 0;
static inline void set_timer(uint64_t us) {
	timer_end = current_time + us;
	timer_start = current_time;
}
static inline bool timer_done() { return current_time >= timer_end; }

void pulseOverride(relay_t relay) {
	if (current_state == STATE_IDLE)
		override_times[relay] = current_time + get_param_value_t(PARAM_RF_PULSE_LENGTH).u64;
}

/*void fsm_begin_auto_move() {
	if (current_state == STATE_IDLE)
		current_state = STATE_MOVE_START_DELAY;
	set_timer(TRANSITION_DELAY_US);
}*/

void fsm_request(fsm_cmd_t cmd)
{
	if (fsm_cmd_queue != NULL)
	    xQueueSend(fsm_cmd_queue, &cmd, 0);  // Safe from any context
}

int8_t fsm_get_current_progress(int8_t denominator) {
	int8_t x = 0;
	switch (current_state) {
		case STATE_DRIVE:
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


#define JACK_TIME  get_param_value_t(PARAM_JACK_KT).f32 * get_param_value_t(PARAM_JACK_DIST ).f32
#define DRIVE_TIME get_param_value_t(PARAM_DRIVE_KT).f32 * get_param_value_t(PARAM_DRIVE_DIST).f32
#define DRIVE_DIST get_param_value_t(PARAM_DRIVE_KE).f32 * get_param_value_t(PARAM_DRIVE_DIST).f32

void control_task(void *param) {
	esp_task_wdt_add(NULL);
	
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(50);
    enabled = true;
    

    while (enabled) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        current_time = esp_timer_get_time();
        
        fsm_cmd_t cmd;
        while (xQueueReceive(fsm_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd) {
                case FSM_CMD_START:
                    if (current_state == STATE_IDLE) {
						current_state = STATE_MOVE_START_DELAY;
						set_timer(TRANSITION_DELAY_US);
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
            }
        }
        
        if (!enabled) break;


		
        // State transitions
        switch (current_state) {
            case STATE_IDLE:
                break;
            case STATE_MOVE_START_DELAY:
                if (timer_done()) {
					current_state = STATE_JACK_UP;
					set_timer(JACK_TIME);
				}
                break;
            case STATE_JACK_UP:
                if (timer_done()) {
					current_state = STATE_DRIVE_START_DELAY;
					set_timer(TRANSITION_DELAY_US);
				}
				if (efuse_is_tripped(BRIDGE_JACK)) {
					current_state = STATE_UNDO_JACK_START;
				}
                break;
            case STATE_DRIVE_START_DELAY:
                if (timer_done()) {
					current_state = STATE_DRIVE;
					set_timer(DRIVE_TIME);
					set_sensor_counter(SENSOR_DRIVE, -DRIVE_DIST);
				}
                break;
            case STATE_DRIVE:
                if (timer_done() || get_sensor_counter(SENSOR_DRIVE) > 0) {
					current_state = STATE_DRIVE_END_DELAY;
					set_timer(TRANSITION_DELAY_US);
				}
				if (efuse_is_tripped(BRIDGE_DRIVE)) {
					current_state = STATE_UNDO_JACK_START;
				}
                break;
            case STATE_DRIVE_END_DELAY:
                if (timer_done()) {
					current_state = STATE_JACK_DOWN;
					set_timer(JACK_TIME);
				}
            case STATE_JACK_DOWN:
                if (timer_done() || get_sensor(SENSOR_JACK)) {
					current_state = STATE_IDLE;
				}
				
				// assume we hit something hard and should stop
				if (efuse_is_tripped(BRIDGE_JACK)) {
					current_state = STATE_IDLE;
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
                if (timer_done() || get_sensor(SENSOR_JACK)) {
					current_state = STATE_IDLE;
				}
				
				// assume we are jacked up all the way (e.g. sensor broke) and should stop
				if (efuse_is_tripped(BRIDGE_JACK)) {
					current_state = STATE_IDLE;
				}
				break;
            default: break;
        }
        
       
		int64_t elapsed_t = (current_time-timer_start);
		int64_t total_t   = (timer_end-timer_start);
		int32_t ticks     = get_sensor_counter(SENSOR_DRIVE);
		//ESP_LOGI("FSM", "[%d] %lld / %lld ms, %ld ticks", current_state, (long long) elapsed_t, (long long) total_t, (long) ticks);

        // Output control
        switch (current_state) {
            case STATE_IDLE:
            	//ESP_LOGI("FSM", "IDLE @ %lld", current_time);
                for (uint8_t i = 0; i < N_RELAYS; ++i) {
					//ESP_LOGI("FSM", "t[%d] %lld", i, override_times[i]);
                    bool active = override_times[i] > current_time;
                    if (active) reset_shutdown_timer();
                    
                    // prohibit movement past jack limit switch
                    //if (i == BRIDGE_JACK*2+(bridge_polarities[BRIDGE_JACK]>0?0:1) && get_sensor(SENSOR_JACK))
                    //	setRelay(i, false);
                    //else
                    	setRelay(i, active);
                    //if (active) ESP_LOGI("FSM", "RUN CHANNEL %d (%lld %c %lld)", i, (long long) override_times[i], active ? '>':'<', (long long) current_time);
            	
                }
                break;
            case STATE_JACK_UP:
            	// jack up and fluff
            	setRelay(RELAY_A1, false);
            	setRelay(RELAY_B1, false);
            	
            	setRelay(RELAY_A2, true);
            	setRelay(RELAY_B2, false);
            	
            	setRelay(RELAY_A3, true);
                reset_shutdown_timer();
                break;
            case STATE_DRIVE:
            	// drive and fluff
            	setRelay(RELAY_A1, true);
            	setRelay(RELAY_B1, false);
            	
            	setRelay(RELAY_A2, false);
            	setRelay(RELAY_B2, false);
            	
            	setRelay(RELAY_A3, true);
                reset_shutdown_timer();
                break;
			case STATE_UNDO_JACK:
            case STATE_JACK_DOWN:
            	// jack down and fluffer
            	setRelay(RELAY_A1, false);
            	setRelay(RELAY_B1, false);
            	
            	setRelay(RELAY_A2, false);
            	setRelay(RELAY_B2, true);
            	
            	setRelay(RELAY_A3, true);
                reset_shutdown_timer();
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
                reset_shutdown_timer();
                break;
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