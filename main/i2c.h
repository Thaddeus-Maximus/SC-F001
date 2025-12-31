#ifndef I2C_H_
#define I2C_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

// Public Functions
esp_err_t i2c_init(void);
esp_err_t i2c_stop(void);

esp_err_t i2c_set_relays(uint8_t states);
esp_err_t i2c_set_led1(uint8_t state);

esp_err_t i2c_poll_buttons();

bool i2c_get_button_tripped(uint8_t button);
bool i2c_get_button_released(uint8_t button);
bool i2c_get_button_state(uint8_t button);
bool i2c_get_button_repeat(uint8_t btn);
int8_t i2c_get_button_repeats(uint8_t btn);
int64_t i2c_get_button_us(uint8_t btn);
int64_t i2c_get_button_ms(uint8_t btn);

#endif // I2C_H_