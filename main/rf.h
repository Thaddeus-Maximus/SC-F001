
#ifndef RF_H
#define RF_H

#include <inttypes.h>
#include <stdio.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define NUM_RF_BUTTONS 4

int64_t recieveKeycode();

void start_rf();

void rf_set_keycode(uint8_t index, int64_t code);

int8_t rf_get_keycode();
int64_t rf_get_raw_keycode();

void rf_clear_queue();

#endif