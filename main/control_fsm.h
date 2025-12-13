#ifndef MAIN_CONTROL_FSM_H_
#define MAIN_CONTROL_FSM_H_

#include "freertos/FreeRTOS.h"   // Must be FIRST
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/queue.h"


typedef enum { FSM_CMD_STOP, FSM_CMD_UNDO, FSM_CMD_SHUTDOWN} fsm_cmd_t;

typedef enum {
	STATE_IDLE = 0,
	STATE_MOVE_START_DELAY = 1,
	STATE_JACK_UP = 2,
	STATE_DRIVE_START_DELAY = 3,
	STATE_DRIVE = 4,
	STATE_DRIVE_END_DELAY = 5,
	STATE_JACK_DOWN = 6,
	STATE_UNDO_JACK = 7,
	STATE_UNDO_JACK_START = 8,
} fsm_state_t;

typedef enum {
	BRIDGE_AUX   = 0,
	BRIDGE_JACK  = 1,
	BRIDGE_DRIVE = 2,
} bridge_t;

#define N_RELAYS 6
#define N_BRIDGES 3

void pulseOverride(bridge_t bridge, int8_t dir, int64_t pulse);

void start_fsm();

void fsm_request(fsm_cmd_t cmd);

void fsm_begin_auto_move();

int8_t fsm_get_current_progress(int8_t remainder);

fsm_state_t fsm_get_state();

int8_t get_bridge_state(bridge_t bridge);

#endif /* MAIN_CONTROL_FSM_H_ */