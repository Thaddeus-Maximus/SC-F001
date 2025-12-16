
#ifndef RF_H
#define RF_H

#include <inttypes.h>
#include <stdio.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define NUM_RF_BUTTONS 8

int64_t recieveKeycode();

esp_err_t rf_init();
esp_err_t rf_stop();

void rf_set_keycode(uint8_t index, int64_t code);

int8_t rf_get_keycode();
int64_t rf_get_raw_keycode();

void rf_clear_queue();

void rf_learn_keycode(uint8_t index);
void rf_cancel_learn_keycode();

#endif