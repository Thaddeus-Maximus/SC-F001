/*
 * power_mgmt.h
 *
 *  Created on: Nov 3, 2025
 *      Author: Thad
 */

#ifndef MAIN_POWER_MGMT_H_
#define MAIN_POWER_MGMT_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	CHG_STATE_OFF = 0,
	CHG_STATE_FLOAT = 1,
	CHG_STATE_BULK = 2
} charge_state_t;
#define N_CHARGE_STATES 3

charge_state_t get_charging_state();

void resetBatTimers();

void efuse_reset_all(void);               // Clear all trip states (manual/programmatic reset)
bool efuse_is_tripped(uint8_t bridge);    // Query if bridge is currently faulted

int32_t get_bridge_mA(uint8_t bridge);
int32_t get_battery_mV();

void start_power();
void shutdown_power();

#endif /* MAIN_POWER_MGMT_H_ */