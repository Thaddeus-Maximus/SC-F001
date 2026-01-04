/*
 * solar.h
 *
 *  Created on: Dec 13, 2025
 *      Author: Thad
 */

#ifndef MAIN_SOLAR_H_
#define MAIN_SOLAR_H_

#include "esp_err.h"

esp_err_t solar_run_fsm();
esp_err_t solar_reset_fsm();

#endif /* MAIN_SOLAR_H_ */
