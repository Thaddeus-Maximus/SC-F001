#ifndef MAIN_CONTROL_FSM_H_
#define MAIN_CONTROL_FSM_H_

#include "freertos/FreeRTOS.h"   // Must be FIRST
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/queue.h"


typedef enum {
	FSM_CMD_START,
	FSM_CMD_START_IGNORE_OVERTRAVEL,
	FSM_CMD_STOP,
	FSM_CMD_UNDO,
	FSM_CMD_SHUTDOWN,
	
	FSM_CMD_CALIBRATE_JACK_PREP,
	FSM_CMD_CALIBRATE_JACK_START,
	FSM_CMD_CALIBRATE_JACK_END,

	FSM_CMD_CALIBRATE_DRIVE_PREP,
	FSM_CMD_CALIBRATE_DRIVE_START,
	FSM_CMD_CALIBRATE_DRIVE_END
} fsm_cmd_t;

typedef enum {
	STATE_IDLE = 0,
	STATE_MOVE_START_DELAY,
	STATE_JACK_UP_START,
	STATE_JACK_UP,
	STATE_DRIVE_START_DELAY,
	STATE_DRIVE,
	STATE_DRIVE_END_DELAY,
	STATE_JACK_DOWN,
	STATE_UNDO_JACK_START,
	
	STATE_CALIBRATE_JACK_DELAY,
	STATE_CALIBRATE_JACK_MOVE,
	
	STATE_CALIBRATE_DRIVE_DELAY,
	STATE_CALIBRATE_DRIVE_MOVE
} fsm_state_t;
#define LOG_TYPE_BAT      100
#define LOG_TYPE_CRASH    101
#define LOG_TYPE_BOOT     102  // Every boot: [ts_ms u64][boot_info u8]
                               //   boot_info bits[3:0] = esp_reset_reason_t
                               //   boot_info bits[7:4] = esp_sleep_wakeup_cause_t (non-zero only when reset=DEEPSLEEP)
#define LOG_TYPE_TIME_SET 103  // Time sync:  [ts_ms u64]

typedef enum {
	RELAY_SENSORS = 0,
	RELAY_C3,
	RELAY_B3,
	RELAY_A3,
	RELAY_B2,
	RELAY_A2,
	RELAY_B1,
	RELAY_A1
} relay_t;

typedef enum {
	BRIDGE_AUX   = 2,
	BRIDGE_JACK  = 1,
	BRIDGE_DRIVE = 0,
	NUM_BRIDGES = 3,
} bridge_t;

typedef enum {
	BRIDGE_FWD = 0b10,
	BRIDGE_REV = 0b01,
	BRIDGE_OFF = 0b00,
	BRIDGE_ON  = 0b11
} bridge_dir_t;

typedef enum {
	FSM_OVERRIDE_DRIVE_FWD,
	FSM_OVERRIDE_DRIVE_REV,
	FSM_OVERRIDE_JACK_UP,
	FSM_OVERRIDE_JACK_DOWN,
	FSM_OVERRIDE_AUX
} fsm_override_t;

#define N_RELAYS 8
#define N_BRIDGES 3

void pulse_override(fsm_override_t cmd);

esp_err_t fsm_init();
esp_err_t fsm_stop();

int64_t fsm_get_cal_t();
int64_t fsm_get_cal_e();
void fsm_request(fsm_cmd_t cmd);

esp_err_t fsm_get_error();
void fsm_clear_error();


float fsm_get_remaining_distance(void);
void  fsm_set_remaining_distance(float x);

//void fsm_begin_auto_move();

int8_t fsm_get_current_progress(int8_t remainder);

fsm_state_t fsm_get_state();
bool fsm_is_idle(void);

int8_t get_bridge_state(bridge_t bridge);

#endif /* MAIN_CONTROL_FSM_H_ */