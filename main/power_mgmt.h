/*
 * power_mgmt.h
 *
 *  Created on: Nov 3, 2025
 *      Author: Thad
 */

#ifndef MAIN_POWER_MGMT_H_
#define MAIN_POWER_MGMT_H_

#include "control_fsm.h"
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "i2c.h"

typedef enum {
	EFUSE_OK = 0,
	EFUSE_OVERCURRENT = 1,
	EFUSE_OVERHEAT = 2,
} efuse_trip_t;

//void efuse_reset_all(void);               // Clear all trip states (manual/programmatic reset)
void         efuse_clear   (bridge_t bridge);
efuse_trip_t efuse_get     (bridge_t bridge);    // Query if bridge is currently faulted
float        efuse_get_heat(bridge_t bridge);
void efuse_set(bridge_t bridge, efuse_trip_t state);

float get_bridge_A(bridge_t bridge);
float get_bridge_raw_A(bridge_t bridge);
float get_battery_V();

// V5 only: hardware overcurrent FAULT line from the ACS37220 (active when true).
// Always false on V4.
bool get_hw_overcurrent_fault(void);

// Raw, unfiltered ADC reads — used by POST. Return 0 on error.
int get_bat_raw_mv(void);
int get_isens_raw_mv(void);  // V5 only — returns 0 on V4
int get_voc_raw_mv(void);    // V5 only — returns 0 on V4

void disable_autozero(bridge_t bridge);
bool get_bridge_overcurrent(bridge_t bridge, float threshold);
bool get_bridge_spike(bridge_t bridge, float threshold);

esp_err_t process_bridge_current(bridge_t bridge);
esp_err_t process_battery_voltage();

esp_err_t adc_init();
esp_err_t adc_post(void);
esp_err_t power_init();
esp_err_t power_stop();


esp_err_t drive_relays(relay_port_t relay_state);

#endif /* MAIN_POWER_MGMT_H_ */