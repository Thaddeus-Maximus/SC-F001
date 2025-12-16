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


//void efuse_reset_all(void);               // Clear all trip states (manual/programmatic reset)
bool efuse_is_tripped(bridge_t bridge);    // Query if bridge is currently faulted

float get_bridge_A(bridge_t bridge);
float get_battery_V();

void set_autozero(bridge_t bridge);

esp_err_t adc_init();
esp_err_t power_init();
esp_err_t power_stop();

#endif /* MAIN_POWER_MGMT_H_ */