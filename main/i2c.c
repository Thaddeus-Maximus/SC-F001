#include "i2c.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/i2c.h"
#include "esp_rom_sys.h"
#include "sensors.h"

// Static Variables
static bool i2c_initted = false;
//static bool safety_ok = false;  // Safety interlock
static uint8_t last_relay_request = 0;  // Track last relay request
/* Cached last-written values for the two TCA9555 output ports. Used by
 * i2c_get_outputs() so the FSM log can record the full 16-bit output state
 * without paying for an extra I2C read each tick. */
static uint8_t last_output0 = 0;
static uint8_t last_output1 = 0;

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
    //safety_ok = false;  // Start with safety not OK
    last_relay_request = 0;
    
    return ESP_OK;
}

esp_err_t i2c_post(void) {
    // Verify TCA9555 responds by reading input port 0
    uint16_t val = 0;
    esp_err_t err = tca_read_word(TCA_REG_INPUT0, &val);
    if (err != ESP_OK) {
        ESP_LOGE("I2C", "POST: TCA9555 read failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI("I2C", "POST: TCA9555 OK (port0=0x%04X)", val);
    return ESP_OK;
}

esp_err_t i2c_set_relays(relay_port_t states) {
    last_output1 = states.raw;
    return tca_write_word_8(TCA_REG_OUTPUT1, states.raw);
}

esp_err_t i2c_relays_idle(void) {
    return i2c_set_relays((relay_port_t){.bridges = {.SENSORS = 1}});
}

esp_err_t i2c_relays_sleep(void) {
    return i2c_set_relays((relay_port_t){.raw = 0});
}

esp_err_t i2c_set_led1(uint8_t state) {
    /* P05-P07 are LEDs (outputs); P00-P04 are buttons / unused INPUTS
     * (CONFIG0 = 0b00000011 sets P00/P01 as inputs; P02-P04 are unused
     * but also configured as inputs). Writing the whole OUTPUT0 register
     * is therefore safe — the input-bit slots in OUTPUT0 are don't-cares
     * because the pin direction prevents the value from driving the line.
     * If P02-P04 ever become outputs, switch this to read-modify-write. */
    uint8_t v = state << 5;
    last_output0 = v;
    return tca_write_word_8(TCA_REG_OUTPUT0, v);
}

uint16_t i2c_get_outputs(void) {
    /* OUTPUT0 in the high byte (P00..P07), OUTPUT1 in the low byte
     * (P10..P17). Reflects the last value written by this driver. */
    return ((uint16_t)last_output0 << 8) | last_output1;
}

esp_err_t i2c_stop() {
	if (!i2c_initted) return ESP_OK;
	last_output0 = 0;
	last_output1 = 0;
	tca_write_word_8(TCA_REG_OUTPUT0, 0);
	tca_write_word_8(TCA_REG_OUTPUT1, 0);
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
    /* Returns -1 on out-of-range button index (was previously `false` = 0,
     * which conflated error with "no repeat"). 0 means button not pressed
     * or no new repeat this poll. >=1 is a valid repeat count. */
    if (btn >= N_BTNS) return -1;
    if (!i2c_get_button_state(btn)) return 0;

    uint64_t now = esp_timer_get_time() / 1000;
    if (now + DEBOUNCE_MS < last_change_time[btn]) return 0;
    if ((now - last_change_time[btn]) > (REPEAT_START_MS + REPEAT_MS * claimed_repeats[btn])) {
        claimed_repeats[btn]++;
        if (claimed_repeats[btn] > 100)
        	claimed_repeats[btn] = 100;
        ESP_LOGI("BTN", "RPT %d", (uint8_t)(claimed_repeats[btn]+1));
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