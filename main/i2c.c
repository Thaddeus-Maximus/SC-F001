#include "i2c.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "esp_rom_sys.h"

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

// Static Variables
static bool i2c_initted = false;

// === I2C LOW-LEVEL ===
static esp_err_t tca_write_word_8(uint8_t reg, uint8_t value) {
    uint8_t data[2] = { reg, value };
    return i2c_master_write_to_device(I2C_PORT, TCA_ADDR_WRITE, data, 2, pdMS_TO_TICKS(1000));
}
static esp_err_t tca_read_word(uint8_t reg, uint16_t *value) {
    uint8_t data[2];
    esp_err_t ret = i2c_master_write_read_device(I2C_PORT, TCA_ADDR_READ, &reg, 1, data, 2, pdMS_TO_TICKS(1000));
    if (ret == ESP_OK) {
        *value = data[0] | (data[1] << 8);
    }
    return ret;
}

esp_err_t i2c_init(void) {
    if (i2c_initted) return ESP_OK;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = GPIO_NUM_22,
        .scl_io_num = GPIO_NUM_21,
        .sda_pullup_en = I2C_PULLUP,
        .scl_pullup_en = I2C_PULLUP,
        .master.clk_speed = I2C_FREQUENCY,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, conf.mode, 0, 0, 0));

    
    ESP_ERROR_CHECK(tca_write_word_8(TCA_REG_CONFIG0, 0b00000011));
    ESP_ERROR_CHECK(tca_write_word_8(TCA_REG_CONFIG1, 0b00000000));
    
    i2c_initted = true;
    return ESP_OK;
}

esp_err_t i2c_set_relays(uint8_t states) {
	return tca_write_word_8(TCA_REG_OUTPUT1, states);
}
esp_err_t i2c_set_led1(uint8_t state) {
	// push 3 LSB to top
	return tca_write_word_8(TCA_REG_OUTPUT0, state<<5);
}

esp_err_t i2c_stop() {
	if (!i2c_initted) return ESP_OK;
	i2c_set_relays(0);
	i2c_set_led1(0);
	return ESP_OK;
}

#define N_BTNS 2
static bool debounced_state[N_BTNS] = {false};
static bool last_known_state[N_BTNS] = {false};
static uint64_t last_stable_time[N_BTNS] = {0};
static uint64_t last_change_time[N_BTNS] = {0};
static uint8_t claimed_repeats[N_BTNS] = {0};
esp_err_t i2c_poll_buttons() {
	for (uint8_t btn = 0; btn < N_BTNS; ++btn) {
        last_known_state[btn] = debounced_state[btn];
    }

    uint16_t port_val;
    ESP_ERROR_CHECK(tca_read_word(TCA_REG_INPUT0, &port_val));
    uint8_t raw_buttons = (uint8_t)(port_val & 0x0F);
    uint8_t raw_states = ~raw_buttons & 0x0F;

    uint64_t now = esp_timer_get_time() / 1000;

    for (uint8_t btn = 0; btn < N_BTNS; ++btn) {
        bool raw_pressed = (raw_states & (1 << btn)) != 0;

        if (raw_pressed != debounced_state[btn]) {
            if (now - last_stable_time[btn] >= DEBOUNCE_MS) {
                debounced_state[btn] = raw_pressed;
                last_stable_time[btn] = now;
                last_change_time[btn] = now;
                claimed_repeats[btn] = 0;
            }
        } else {
            last_stable_time[btn] = now;
        }
    }
    return ESP_OK;
}

bool i2c_get_button_tripped(uint8_t button) {
    return (button < N_BTNS) && debounced_state[button] && !last_known_state[button];
}

bool i2c_get_button_released(uint8_t button) {
    return (button < N_BTNS) && !debounced_state[button] && last_known_state[button];
}

bool i2c_get_button_state(uint8_t button) {
    return (button < N_BTNS) && debounced_state[button];
}

bool i2c_get_button_repeat(uint8_t btn) {
    if (btn >= N_BTNS || !debounced_state[btn]) return false;
    uint64_t now = esp_timer_get_time() / 1000;
    if (now + DEBOUNCE_MS < last_change_time[btn]) return false;
    if ((now - last_change_time[btn]) > (REPEAT_START_MS + REPEAT_MS * claimed_repeats[btn])) {
        claimed_repeats[btn]++;
        return true;
    }
    return false;
}

int8_t i2c_get_button_repeats(uint8_t btn) {
	if (!i2c_get_button_state(btn))
		return 0;
		
    if (btn >= N_BTNS || !debounced_state[btn]) return false;
    uint64_t now = esp_timer_get_time() / 1000;
    if (now + DEBOUNCE_MS < last_change_time[btn]) return false;
    if ((now - last_change_time[btn]) > (REPEAT_START_MS + REPEAT_MS * claimed_repeats[btn])) {
        claimed_repeats[btn]++;
        if (claimed_repeats[btn] > 100)
        	claimed_repeats[btn] = 100;
        ESP_LOGI("BTN", "RPT %d", (uint8_t)claimed_repeats[btn]+2);
        return claimed_repeats[btn]+1;
    }
    if (debounced_state[btn] && !last_known_state[btn]) {
		
        ESP_LOGI("BTN", "FST %d", 1);
    	return 1;
    }
    
    return 0;
}

int64_t i2c_get_button_ms(uint8_t btn) {
	if (!i2c_get_button_state(btn))
		return 0;
	
    uint64_t now = esp_timer_get_time() / 1000;
    return now - last_change_time[btn];
}
int64_t i2c_get_button_us(uint8_t btn) {
	return i2c_get_button_ms(btn)*1000;
}