#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "control_fsm.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_rx.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rf_433.h"

#include "storage.h"

#define TAG "RF"

#define RF_PIN GPIO_NUM_25
#define P_HIGH 1040
#define P_LOW 340
#define P_MARGIN 70
#define P_SKIPMIN 250
#define RF_DEBUG 0

// Struct to hold decoded RF data
typedef struct {
    uint32_t code;
    uint16_t high_avg;
    uint16_t low_avg;
    uint8_t errors;
    size_t num_symbols;
} rf_code_t;

int learn_flag = -1;
bool controls_enabled = true;

// Temporary storage for learned keycodes (not committed to params yet)
static int64_t temp_keycodes[NUM_RF_BUTTONS] = {0};

// For rmt_rx_register_event_callbacks
static bool rfrx_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *udata) {
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t drx_queue = (QueueHandle_t)udata;
    xQueueSendFromISR(drx_queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

// Task that receives and decodes RF signals
static void rf_433_receiver_task(void* param) {
    esp_task_wdt_add(NULL);
    esp_log_level_set("rmt", ESP_LOG_NONE); // disable rmt messages about hw buffer too small
    const uint16_t tlow = (P_HIGH - P_LOW - (2 * P_MARGIN));
    const uint16_t thigh = (P_HIGH - P_LOW + (2 * P_MARGIN));
    
    rmt_channel_handle_t rx_channel = NULL;
    rmt_symbol_word_t symbols[64];
    rmt_rx_done_event_data_t rx_data;
    
    rmt_receive_config_t rx_config = {
        .signal_range_min_ns = 2000,
        .signal_range_max_ns = 1250000,
    };
    
    rmt_rx_channel_config_t rx_ch_conf = {
        .gpio_num = (gpio_num_t)RF_PIN,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .mem_block_symbols = 64,
        .flags = {
            .invert_in = false,
            .with_dma = false,
        }
    };
    
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_ch_conf, &rx_channel));
    
    QueueHandle_t rx_queue = xQueueCreate(1, sizeof(rx_data));
    assert(rx_queue);
    
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = rfrx_done,
    };
    
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, rx_queue));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));
    ESP_ERROR_CHECK(rmt_receive(rx_channel, symbols, sizeof(symbols), &rx_config));
    
    ESP_LOGI("RF", "RF receiver task started on core %d", xPortGetCoreID());
    
    for(;;) {
        if (xQueueReceive(rx_queue, &rx_data, pdMS_TO_TICKS(500)) == pdPASS) {
            
            size_t len = rx_data.num_symbols;
            rmt_symbol_word_t *cur = rx_data.received_symbols;
            
            if (len > 23) {
                uint32_t code = 0;
                uint16_t low = 0, high = 0, err = 0;
                
                // Decode the 24-bit code
                for (uint8_t i = 0; i < 24; i++) {
                    uint16_t dur0 = (uint16_t)cur[i].duration0;
                    uint16_t dur1 = (uint16_t)cur[i].duration1;
                    
                    // Validate symbol format
                    if (!(cur[i].level0 && !cur[i].level1 && dur0 >= P_SKIPMIN && dur1 >= P_SKIPMIN)) {
                        code = 0;
                        break;
                    }
                    
                    // Determine bit value based on pulse duration
                    if ((dur0 - dur1) > 0) {
                        code = (code | (1ULL << (23 - i)));
                        high += dur0;
                        low += dur1;
                    } else {
                        high += dur1;
                        low += dur0;
                    }
                    
                    // Check if pulse timing is within expected range
                    int16_t diff = abs(dur0 - dur1);
                    if ((diff < tlow) || (diff > thigh)) {
                        err++;
                    }
                }
                
                // If we got a valid code, process it
                if (code) {
                    ESP_LOGI(TAG, "GOT KEYCODE 0x%lx [%d]", (long) code, len);
                    
                    if (learn_flag >= 0) {
                        // Store to temporary storage, not to params yet
                        temp_keycodes[learn_flag] = (uint32_t)code;
                        ESP_LOGI(TAG, "LEARNED KEYCODE (temp storage)");
                        learn_flag = -1;
                    } else if (controls_enabled) {
                        // Only process RF commands if controls are enabled
                        rf_code_t rf_msg = {
                            .code = code,
                            .high_avg = high / 24,
                            .low_avg = low / 24,
                            .errors = err,
                            .num_symbols = len
                        };
                        
                        for (uint8_t i = 0; i < NUM_RF_BUTTONS; i++) {
                            uint32_t match = get_param_value_t(PARAM_KEYCODE_0+i).u32;
                            // Compare just the code (lower 32 bits)
                            if ((uint32_t)match == code && code!=0) {
                                switch (i) {
                                    case 0: pulseOverride(RELAY_A1); pulseOverride(RELAY_A3); break;
                                    case 1: pulseOverride(RELAY_B1); pulseOverride(RELAY_A3); break;
                                    case 2: pulseOverride(RELAY_A2); break;
                                    case 3: pulseOverride(RELAY_B2); break;
                                    default: break;
                                }
                            }
                        }
                    }
                }
                
                // Debug output - print raw symbols
                #if RF_DEBUG
                    char buf[128];
                    char tbuf[30];
                    snprintf(tbuf, 20, "Rf%zu: ", len);
                    strcpy(buf, tbuf);
                    
                    for (uint8_t i = 0; i < len; i++) {
                        if (strlen(buf) > 100) {
                            printf("%s", buf);
                            buf[0] = '\0';
                        }
                        
                        int d0 = cur[i].duration0;
                        if (!cur[i].level0) {
                            d0 *= -1;
                        }
                        
                        int d1 = cur[i].duration1;
                        if (!cur[i].level1) {
                            d1 *= -1;
                        }
                        
                        snprintf(tbuf, 30, "%d,%d ", d0, d1);
                        strcat(buf, tbuf);
                    }
                    printf("%s\n\n", buf);
                #endif
            }
            
            // Start next receive
            rmt_receive(rx_channel, symbols, sizeof(symbols), &rx_config);
        }
        
        esp_task_wdt_reset();
    }
    
    // Cleanup (never reached in this case)
    rmt_disable(rx_channel);
    rmt_del_channel(rx_channel);
    vTaskDelete(NULL);
}

esp_err_t rf_433_init() {    
    xTaskCreate(rf_433_receiver_task, TAG, 4096, NULL, 10, NULL);
    return ESP_OK;
}

esp_err_t rf_433_stop() { return ESP_OK; }

void rf_433_set_keycode(uint8_t index, uint32_t code) {
    set_param_value_t(PARAM_KEYCODE_0+index, (param_value_t){.u32=code});
}

void rf_433_learn_keycode(uint8_t index) {
    if (index >= 8) return;
    learn_flag = index;
}

void rf_433_cancel_learn_keycode() {
    learn_flag = -1;
}

void rf_433_disable_controls() {
    controls_enabled = false;
}

void rf_433_enable_controls() {
    controls_enabled = true;
}

int32_t rf_433_get_temp_keycode(uint8_t index) {
    if (index >= NUM_RF_BUTTONS) return 0;
    return temp_keycodes[index];
}

void rf_433_set_temp_keycode(uint8_t index, uint32_t code) {
    if (index >= NUM_RF_BUTTONS) return;
    temp_keycodes[index] = code;
}

void rf_433_clear_temp_keycodes() {
    for (uint8_t i = 0; i < NUM_RF_BUTTONS; i++) {
        temp_keycodes[i] = 0;
    }
}