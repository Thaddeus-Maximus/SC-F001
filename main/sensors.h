/*
 * sensors.h
 *
 *  Created on: Nov 10, 2025
 *      Author: Thad
 */

#ifndef MAIN_SENSORS_H_
#define MAIN_SENSORS_H_

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"   // Must be FIRST

#define SENSOR_SAMPLE_PERIOD_US 100  // 10 kHz → captures 120 Hz easily
#define SENSOR_DEBOUNCE_US      500  // Reduced to 0.5 ms for responsiveness

typedef enum {
	SENSOR_SAFETY = 0, // IO27
	SENSOR_DRIVE  = 1, // IO14
	SENSOR_JACK   = 2, // IO16
	SENSOR_AUX2   = 3, // IO19
	N_SENSORS     = 4
} sensor_t;

void reset_sensor_counter(sensor_t i);
void set_sensor_counter(sensor_t i, int16_t to);
int16_t get_sensor_counter(sensor_t i);

bool get_sensor(sensor_t i);
bool get_is_safe(void);

int8_t pack_sensors();

esp_err_t sensors_init();
void sensors_check();
//esp_err_t sensors_stop();

#endif /* MAIN_SENSORS_H_ */