#include <stdio.h>
#include <string.h>
#include <stdlib.h>
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

#include "flash.h"

#define RF_PIN GPIO_NUM_23
#define P_HIGH 1040
#define P_LOW 340
#define P_MARGIN 70
#define P_SKIPMIN 250
#define RF_DEBUG 0
#define NUM_RF_BUTTONS 4

// Struct to hold decoded RF data
typedef struct {
    uint32_t code;
    uint16_t high_avg;
    uint16_t low_avg;
    uint8_t errors;
    size_t num_symbols;
} rf_code_t;

// Global queue for passing decoded codes between tasks
static QueueHandle_t g_code_queue = NULL;

// For rmt_rx_register_event_callbacks
static bool rfrx_done(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *udata) {
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t drx_queue = (QueueHandle_t)udata;
    xQueueSendFromISR(drx_queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

// Task that receives and decodes RF signals
static void rf_receiver_task(void* param) {
	esp_task_wdt_add(NULL);
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
        if (xQueueReceive(rx_queue, &rx_data, pdMS_TO_TICKS(1000)) == pdPASS) {
            
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
                
                // If we got a valid code, send it to processing task
                if (code) {
                    rf_code_t rf_msg = {
                        .code = code,
                        .high_avg = high / 24,
                        .low_avg = low / 24,
                        .errors = err,
                        .num_symbols = len
                    };
                    
                    // Non-blocking send - if queue is full, just drop it
                    xQueueSend(g_code_queue, &rf_msg, 0);
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
            
            esp_task_wdt_reset();
        }
    }
    
    // Cleanup (never reached in this case)
    rmt_disable(rx_channel);
    rmt_del_channel(rx_channel);
    vTaskDelete(NULL);
}

void start_rf() {
    g_code_queue = xQueueCreate(5, sizeof(rf_code_t));
    assert(g_code_queue);
    
    xTaskCreate(rf_receiver_task, "RF", 4096, NULL, 10, NULL);
}

void rf_set_keycode(uint8_t index, int64_t code) {
	char key[] = "keycode0";
	key[7] = 48+index; // ASCII
	
	ESP_LOGI("RF", "SET KEYCODE[%d] = 0x%16llx", index, code);
	
	set_param_i64(key, code);
}

int8_t rf_get_keycode() {
	rf_code_t received_code;
	
	char key[] = "keycode0";
	
	if (xQueueReceive(g_code_queue, &received_code, 0)  == pdPASS) {
		int64_t newcode = ((int64_t)received_code.num_symbols << 56) | received_code.code;
		
		for (uint8_t i = 0; i < NUM_RF_BUTTONS; i++) {
			key[7] = 48+i; // ASCII
            if (newcode == get_param_i64(key))
                return i;
        }
        ESP_LOGI("RF", "Received unknown code 0x%08lx (%d) [0x%16llx]", (unsigned long)received_code.code, received_code.num_symbols, (unsigned long long) newcode);
	}
	return -1;
}

int64_t rf_get_raw_keycode() {
	rf_code_t received_code;
	int64_t code = -1;
    if (xQueueReceive(g_code_queue, &received_code, 0) == pdPASS) {
		code = ((int64_t)received_code.num_symbols << 56) | received_code.code;
		//ESP_LOGI("RF", "Raw Code 0x%08lx (%d) [0x%16llx]", (unsigned long)received_code.code, received_code.num_symbols, (unsigned long long) code);
    }
    return code;
}

void rf_clear_queue() {
	xQueueReset(g_code_queue);
}