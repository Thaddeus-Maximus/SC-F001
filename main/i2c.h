#ifndef I2C_H_
#define I2C_H_

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define I2C_PORT I2C_NUM_0
#define TCA_ADDR_READ  0x21
#define TCA_ADDR_WRITE 0x21
#define I2C_PULLUP GPIO_PULLUP_DISABLE
#define I2C_FREQUENCY 400000

// TCA9555 Registers
#define TCA_REG_INPUT0    0x00
#define TCA_REG_INPUT1    0x01
#define TCA_REG_OUTPUT0   0x02
#define TCA_REG_OUTPUT1   0x03
#define TCA_REG_POLARITY0 0x04
#define TCA_REG_POLARITY1 0x05
#define TCA_REG_CONFIG0   0x06
#define TCA_REG_CONFIG1   0x07

// Debounce & Repeat Settings
#define DEBOUNCE_MS        50
#define REPEAT_MS         200
#define REPEAT_START_MS   700

typedef union {
    uint8_t raw;
    struct {
        uint8_t SENSORS  : 1;  // [0]
        uint8_t C3 : 1;  // [1]
        uint8_t B3 : 1;  // [2]
        uint8_t A3 : 1;  // [3]
        uint8_t B2 : 1;  // [4]
        uint8_t A2 : 1;  // [5]
        uint8_t B1 : 1;  // [6]
        uint8_t A1 : 1;  // [7] MSB
    } bits;
    struct {
		uint8_t SENSORS : 1;
        uint8_t C3      : 1;
        uint8_t AUX     : 2; 
        uint8_t  JACK   : 2;
        uint8_t  DRIVE  : 2;
	} bridges;
} relay_port_t;

// Public Functions
esp_err_t i2c_init(void);
esp_err_t i2c_post(void);
esp_err_t i2c_stop(void);

esp_err_t i2c_set_relays(relay_port_t states);
esp_err_t i2c_set_led1(uint8_t state);

/* Returns the last-written 16-bit TCA9555 output state.
 * High byte: OUTPUT0 (P00..P07, LEDs in P05..P07).
 * Low  byte: OUTPUT1 (P10..P17, relay_port_t.raw). */
uint16_t i2c_get_outputs(void);

/* Normal run state: all bridges off, but P10 (sensor rail) held high.
 * Use whenever "everything off" is really "all motors off, system is still on". */
esp_err_t i2c_relays_idle(void);

/* Sleep state: all bridges off AND P10 low, cutting power to the sensors. */
esp_err_t i2c_relays_sleep(void);

esp_err_t i2c_poll_buttons();

bool i2c_get_button_tripped(uint8_t button);
bool i2c_get_button_released(uint8_t button);
bool i2c_get_button_state(uint8_t button);
bool i2c_get_button_repeat(uint8_t btn);
int8_t i2c_get_button_repeats(uint8_t btn);
int64_t i2c_get_button_us(uint8_t btn);
int64_t i2c_get_button_ms(uint8_t btn);

#endif // I2C_H_