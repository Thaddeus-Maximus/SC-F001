#include "sensors.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char* TAG = "SENS";

uint8_t sensor_pins[N_SENSORS] = {GPIO_NUM_19, GPIO_NUM_25};

volatile int32_t sensor_count[N_SENSORS] = {0};
static volatile uint64_t sensor_last_isr_time[N_SENSORS] = {0};
static volatile bool sensor_stable_state[N_SENSORS] = {false};
static QueueHandle_t sensor_event_queue = NULL;

#define DEBOUNCE_TIME_US    2000   // 2 ms debounce (adjust per switch)
#define DEBOUNCE_TICKS      pdMS_TO_TICKS(DEBOUNCE_TIME_MS)

typedef struct {
    uint8_t sensor_id;
    bool level;
} sensor_event_t;

// ISR: Minimal work — just record timestamp and forward to queue
static void IRAM_ATTR sensor_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t)arg;
    uint8_t i;
    for (i = 0; i < N_SENSORS; i++) {
        if (sensor_pins[i] == gpio_num) break;
    }
    if (i == N_SENSORS) return;

    uint64_t now = esp_timer_get_time();
    sensor_last_isr_time[i] = now;

    sensor_event_t evt = {.sensor_id = i, .level = !gpio_get_level(gpio_num)};
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(sensor_event_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
}

// Debounce task: Processes queue, updates state & count
static void sensor_debounce_task(void* param) {
    sensor_event_t evt;
    static uint64_t last_processed_time[N_SENSORS] = {0};
    static bool last_raw_state[N_SENSORS] = {false};

    // Initialize stable state
    for (uint8_t i = 0; i < N_SENSORS; i++) {
        bool level = !gpio_get_level(sensor_pins[i]);
        sensor_stable_state[i] = level;
        last_raw_state[i] = level;
        last_processed_time[i] = esp_timer_get_time();
    }

    esp_task_wdt_add(NULL);
    
    
	uint8_t i = 0;
	int64_t now = -1;

    while (1) {
        if (xQueueReceive(sensor_event_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
            i = evt.sensor_id;
            
            ESP_LOGI("SENS", "EVENT %d", i);
            
            bool current_raw = !gpio_get_level(sensor_pins[i]);

            
           sensor_stable_state[i] = current_raw;
           
           if (current_raw && !last_raw_state[i]){
				ESP_LOGI("SENS", "FALLING");
				sensor_count[i]++;
			}
            if (!current_raw && last_raw_state[i]){
				ESP_LOGI("SENS", "RISING");
				sensor_count[i]++;
			}
			
			last_raw_state[i] = current_raw;
        }
        
        
            now = esp_timer_get_time();
        
        /*// Wait for debounce period since last ISR
            if (now - sensor_last_isr_time[i] >= (DEBOUNCE_TIME_US)) {
                bool current_raw = !gpio_get_level(sensor_pins[i]);

                // Only update if stable and different from last stable
                if (current_raw != sensor_stable_state[i]) {
                    bool was_high = sensor_stable_state[i];
                    
                    // Count rising OR falling edges
                    if (current_raw && !sensor_stable_state[i]){
						ESP_LOGI("SENS", "FALLING");
						sensor_count[i]++;
					}
                    if (!current_raw && sensor_stable_state[i]){
						ESP_LOGI("SENS", "RISING");
						sensor_count[i]++;
					}
                    
                    
                    sensor_stable_state[i] = current_raw;

                    last_raw_state[i] = current_raw;
                    last_processed_time[i] = now;
                }
            }*/
            
        esp_task_wdt_reset();
    }
}

void start_sensors() {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << sensor_pins[0]) | (1ULL << sensor_pins[1]),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    sensor_event_queue = xQueueCreate(16, sizeof(sensor_event_t));
    if (!sensor_event_queue) {
        ESP_LOGE(TAG, "Failed to create sensor queue");
        return;
    }

    // Install ISR service
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    for (uint8_t i = 0; i < N_SENSORS; i++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(sensor_pins[i], sensor_isr_handler, (void*)sensor_pins[i]));
    	sensor_stable_state[i] = !gpio_get_level(sensor_pins[i]);
    }

    xTaskCreate(sensor_debounce_task, "SENS_DEBOUNCE", 3072, NULL, 6, NULL);
}

void shutdown_sensors() {
    for (uint8_t i = 0; i < N_SENSORS; i++) {
        gpio_isr_handler_remove(sensor_pins[i]);
    }
    gpio_uninstall_isr_service();
    vQueueDelete(sensor_event_queue);
}

// Public API
bool get_sensor(sensor_t i) {
    return sensor_stable_state[i];
}

int32_t get_sensor_counter(sensor_t i) {
    return sensor_count[i];
}

void set_sensor_counter(sensor_t i, int32_t to) {
    sensor_count[i] = to;
}