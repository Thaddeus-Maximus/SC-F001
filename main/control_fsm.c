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

#define TRANSITION_DELAY_US 1000000

static QueueHandle_t fsm_cmd_queue = NULL;

uint8_t relay_pins[N_RELAYS] = {
	GPIO_NUM_13, // A1
	GPIO_NUM_14, // B1
	GPIO_NUM_15, // A2
	GPIO_NUM_16, // B2
	GPIO_NUM_17, // A3
	GPIO_NUM_18  // B3
	};

bool relay_states[N_RELAYS] = {false};
int64_t override_times[N_RELAYS] = {-1};
bool enabled = false;

void setRelay(int8_t relay, bool state) {
	relay_states[relay] = state;
}

void driveRelays() {
	for (uint8_t i=0; i<N_RELAYS; i++) {
		if (efuse_is_tripped(i/2))
			gpio_set_level(relay_pins[i], 0);
		else
			gpio_set_level(relay_pins[i], relay_states[i]);
	}
}

int8_t bridge_polarities[3] = {
	+1,
	-1,
	-1
};
void setBridge(bridge_t bridge, int8_t dir) {
	dir *= bridge_polarities[bridge];
	setRelay(bridge*2+0, dir<0);
	setRelay(bridge*2+1, dir>0);
}


int8_t get_bridge_state(bridge_t bridge) {
	if (relay_states[bridge*2 + 0]) return +1;
	if (relay_states[bridge*2 + 1]) return -1;
	return 0;
}

volatile fsm_state_t current_state = STATE_IDLE;
volatile int64_t current_time = 0;
volatile bool start_running_request = false;

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

void pulseOverride(bridge_t bridge, int8_t dir, int64_t pulse) {
	if (dir < 0)
		override_times[bridge*2 + (bridge_polarities[bridge]>0?0:1)] = esp_timer_get_time() + pulse;
	if (dir > 0)
		override_times[bridge*2 + (bridge_polarities[bridge]>0?1:0)] = esp_timer_get_time() + pulse;
}

void fsm_begin_auto_move() {
	if (current_state == STATE_IDLE)
		current_state = STATE_MOVE_START_DELAY;
	set_timer(TRANSITION_DELAY_US);
}

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

#define JACK_TIME  (uint64_t)get_param_i8("jack_dist")*get_param_u32("jack_mspi")*1000
#define DRIVE_TIME (uint64_t)get_param_i8("drive_dist")*get_param_u32("drive_mspf")*1000
#define DRIVE_DIST ((int32_t)(get_param_i8("drive_dist")))*((int32_t)get_param_u32("drive_tpdf"))/10

void control_task(void *param) {
	esp_task_wdt_add(NULL);
	
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(50);
    enabled = true;
    
    
    
    for (size_t i = 0; i < N_RELAYS; ++i) {
	    gpio_reset_pin(    relay_pins[i]);
        gpio_set_direction(relay_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(    relay_pins[i], 0);           // Force low
        gpio_hold_dis(     relay_pins[i]);  // CRITICAL: Allow control after wake
    }

    while (enabled) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        current_time = esp_timer_get_time();
        
        fsm_cmd_t cmd;
        while (xQueueReceive(fsm_cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd) {
                case FSM_CMD_STOP:
                    if (current_state != STATE_IDLE &&
                        current_state != STATE_UNDO_JACK_START &&
                        current_state != STATE_UNDO_JACK) {
                        current_state = STATE_IDLE;
                    }
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
        
/*        
		int64_t elapsed_t = (current_time-timer_start);
		int64_t total_t = (timer_end-timer_start);
		int32_t ticks = get_sensor_counter(SENSOR_DRIVE);
		ESP_LOGI("FSM", "[%d] %lld / %lld ms, %ld ticks", current_state, (long long) elapsed_t, (long long) total_t, (long) ticks);
*/
        // Output control
        switch (current_state) {
            case STATE_IDLE:
            	//ESP_LOGI("FSM", "IDLE @ %lld", current_time);
                for (uint8_t i = 0; i < N_RELAYS; ++i) {
					//ESP_LOGI("FSM", "t[%d] %lld", i, override_times[i]);
                    bool active = override_times[i] > current_time;
                    if (active) reset_shutdown_timer();
                    
                    // prohibit movement past jack limit switch
                    if (i == BRIDGE_JACK*2+(bridge_polarities[BRIDGE_JACK]>0?0:1) && get_sensor(SENSOR_JACK))
                    	setRelay(i, false);
                    else
                    	setRelay(i, active);
                    //if (active) ESP_LOGI("FSM", "RUN CHANNEL %d (%lld %c %lld)", i, (long long) override_times[i], active ? '>':'<', (long long) current_time);

                }
                break;
            case STATE_JACK_UP:
                setBridge(BRIDGE_DRIVE, 0);
                setBridge(BRIDGE_JACK, +1);
                setBridge(BRIDGE_AUX,  +1);
                reset_shutdown_timer();
                break;
            case STATE_DRIVE:
                setBridge(BRIDGE_DRIVE, +1);
                setBridge(BRIDGE_JACK,   0);
                setBridge(BRIDGE_AUX,   +1);
                reset_shutdown_timer();
                break;
			case STATE_UNDO_JACK:
            case STATE_JACK_DOWN:
                setBridge(BRIDGE_DRIVE, 0);
                setBridge(BRIDGE_JACK, -1);
                setBridge(BRIDGE_AUX,  +1);
                reset_shutdown_timer();
                break;
			case STATE_UNDO_JACK_START:
            case STATE_DRIVE_START_DELAY:
            case STATE_DRIVE_END_DELAY:
                setBridge(BRIDGE_DRIVE,  0);
                setBridge(BRIDGE_JACK,   0);
                setBridge(BRIDGE_AUX,   +1);
                reset_shutdown_timer();
                break;
            default: break;
        }

        driveRelays();
            
        esp_task_wdt_reset();
    }
    
    for (size_t i = 0; i < N_RELAYS; ++i) {
        gpio_set_level(relay_pins[i], 0);
        gpio_hold_en(relay_pins[i]);
    }
    
    if (fsm_cmd_queue != NULL) {
        vQueueDelete(fsm_cmd_queue);
        fsm_cmd_queue = NULL;
    }
}

void start_fsm() {
    if (fsm_cmd_queue == NULL) {
        fsm_cmd_queue = xQueueCreate(8, sizeof(fsm_cmd_t));
    }
    xTaskCreate(control_task, "FSM", 4096, NULL, 5, NULL);
}