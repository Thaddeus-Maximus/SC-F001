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
	SENSOR_DRIVE = 0,
	SENSOR_JACK  = 1,
	N_SENSORS    = 2
} sensor_t;

void reset_sensor_counter(sensor_t i);
void set_sensor_counter(sensor_t i, int32_t to);
int32_t get_sensor_counter(sensor_t i);

bool get_sensor(sensor_t i);

void start_sensors();
void shutdown_sensors();

#endif /* MAIN_SENSORS_H_ */
