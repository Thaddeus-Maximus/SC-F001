#ifndef MAIN_CONTROL_FSM_H_
#define MAIN_CONTROL_FSM_H_

#include "freertos/FreeRTOS.h"   // Must be FIRST
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/queue.h"


typedef enum {
	FSM_CMD_START,
	FSM_CMD_STOP,
	FSM_CMD_UNDO,
	FSM_CMD_SHUTDOWN,
	
	FSM_CMD_CALIBRATE_JACK_PREP,
	FSM_CMD_CALIBRATE_JACK_START,
	FSM_CMD_CALIBRATE_JACK_END,
	FSM_CMD_CALIBRATE_JACK_FINISH,
	
	FSM_CMD_CALIBRATE_DRIVE_PREP,
	FSM_CMD_CALIBRATE_DRIVE_START,
	FSM_CMD_CALIBRATE_DRIVE_END,
	FSM_CMD_CALIBRATE_DRIVE_FINISH
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
	STATE_UNDO_JACK,
	STATE_UNDO_JACK_START,
	
	STATE_CALIBRATE_JACK_DELAY,
	STATE_CALIBRATE_JACK_MOVE,
	
	STATE_CALIBRATE_DRIVE_DELAY,
	STATE_CALIBRATE_DRIVE_MOVE
} fsm_state_t;

typedef enum {
	RELAY_NONE = 0,
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
} bridge_t;

#define N_RELAYS 8
#define N_BRIDGES 3

void pulseOverride(relay_t relay/*, int64_t pulse*/);

esp_err_t fsm_init();
esp_err_t fsm_stop();

bool isRunning();

void fsm_set_cal_val(float v);
int64_t fsm_get_cal_t();
int64_t fsm_get_cal_e();
void fsm_request(fsm_cmd_t cmd);


float fsm_get_remaining_distance(void);
void  fsm_set_remaining_distance(float x);

//void fsm_begin_auto_move();

int8_t fsm_get_current_progress(int8_t remainder);

fsm_state_t fsm_get_state();

int8_t get_bridge_state(bridge_t bridge);

#endif /* MAIN_CONTROL_FSM_H_ */