#ifndef RF_H
#define RF_H

#include <inttypes.h>
#include <stdio.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define NUM_RF_BUTTONS 8

esp_err_t rf_433_init();
esp_err_t rf_433_stop();

/* Consume-once peek of the most recently decoded raw code. Returns 0 if
 * none has arrived since the last call. Used by the bring-up RF.WATCH
 * stage; does not interfere with the normal decode/dispatch path. */
uint32_t rf_433_peek_latest(void);

void rf_433_learn_keycode(uint8_t index);
void rf_433_cancel_learn_keycode();

void rf_433_disable_controls();
void rf_433_enable_controls();

int32_t rf_433_get_temp_keycode(uint8_t index);
void rf_433_set_temp_keycode(uint8_t index, uint32_t code);
void rf_433_clear_temp_keycodes();

#endif