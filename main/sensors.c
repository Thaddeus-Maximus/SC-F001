#include "sensors.h"
#include "i2c.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "storage.h"

// make the compiler shut up about casting an int to a void
#define INT2VOIDP(i) (void*)(uintptr_t)(i)


static const char* TAG = "SENS";

uint8_t sensor_pins[N_SENSORS] = {GPIO_NUM_27, GPIO_NUM_14};

volatile int32_t sensor_count[N_SENSORS] = {0};
static volatile uint64_t sensor_last_isr_time[N_SENSORS] = {0};
static volatile bool sensor_stable_state[N_SENSORS] = {false};
static QueueHandle_t sensor_event_queue = NULL;

// Safety sensor debouncing
static volatile bool safety_tripped = false;
static volatile uint64_t safety_low_start_time = 0;
static volatile uint64_t safety_high_start_time = 0;
#define SAFETY_TRIP_DEBOUNCE_US   get_param_value_t(PARAM_SAFETY_BREAK_US).u32
#define SAFETY_UNTRIP_DEBOUNCE_US get_param_value_t(PARAM_SAFETY_BREAK_US).u32

#define DEBOUNCE_TIME_US    2000   // 2 ms debounce (adjust per switch)
#define DEBOUNCE_TICKS      pdMS_TO_TICKS(DEBOUNCE_TIME_MS)

typedef struct {
    uint8_t sensor_id;
    bool level;
} sensor_event_t;

// ISR: Minimal work – just record timestamp and forward to queue
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
    esp_task_wdt_add(NULL);
    sensor_event_t evt;
    //static uint64_t last_processed_time[N_SENSORS] = {0};
    static bool last_raw_state[N_SENSORS] = {false};

    // Initialize stable state
    for (uint8_t i = 0; i < N_SENSORS; i++) {
        bool level = !gpio_get_level(sensor_pins[i]);
        sensor_stable_state[i] = level;
        last_raw_state[i] = level;
        //last_processed_time[i] = esp_timer_get_time();
    }
    
    // Initialize safety sensor
    bool safety_current = !gpio_get_level(sensor_pins[SENSOR_SAFETY]);
    if (safety_current) {
        safety_low_start_time = esp_timer_get_time();
        safety_high_start_time = 0;
    } else {
        safety_low_start_time = 0;
        safety_high_start_time = esp_timer_get_time();
    }
    
    uint8_t i = 0;

    while (1) {
        if (xQueueReceive(sensor_event_queue, &evt, pdMS_TO_TICKS(10)) == pdTRUE) {
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
        
        // Handle safety sensor debouncing with asymmetric timing
        bool safety_current = !gpio_get_level(sensor_pins[SENSOR_SAFETY]);
        uint64_t now = esp_timer_get_time();
        
        if (safety_current) {
            // Safety sensor is LOW (active)
            if (safety_low_start_time == 0) {
                // First time going low, start timing
                safety_low_start_time = now;
                safety_high_start_time = 0;
                ESP_LOGI(TAG, "Safety sensor went LOW, starting trip timer");
            } else if (!safety_tripped && (now - safety_low_start_time >= SAFETY_TRIP_DEBOUNCE_US)) {
                // Been low for 200ms, trip the safety
                safety_tripped = true;
                i2c_set_safety_status(false);
                ESP_LOGW(TAG, "SAFETY TRIPPED - Relays disabled");
            }
        } else {
            // Safety sensor is HIGH (inactive)
            if (safety_high_start_time == 0) {
                // First time going high, start timing
                safety_high_start_time = now;
                safety_low_start_time = 0;
                ESP_LOGI(TAG, "Safety sensor went HIGH, starting un-trip timer");
            } else if (safety_tripped && (now - safety_high_start_time >= SAFETY_UNTRIP_DEBOUNCE_US)) {
                // Been high for 300ms, un-trip the safety
                safety_tripped = false;
                i2c_set_safety_status(true);
                ESP_LOGI(TAG, "SAFETY CLEARED - Relays enabled");
            }
        }
        
        esp_task_wdt_reset();
    }
}

esp_err_t sensors_init() {
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
        return ESP_FAIL;
    }

    // Install ISR service
    ESP_ERROR_CHECK(gpio_install_isr_service(0));

    for (uint8_t i = 0; i < N_SENSORS; i++) {
        ESP_ERROR_CHECK(gpio_isr_handler_add(sensor_pins[i], sensor_isr_handler, INT2VOIDP(sensor_pins[i])));
    	sensor_stable_state[i] = !gpio_get_level(sensor_pins[i]);
    }

    xTaskCreate(sensor_debounce_task, "SENSORS", 3072, NULL, 6, NULL);

	return ESP_OK;
}

esp_err_t sensors_stop() {
    for (uint8_t i = 0; i < N_SENSORS; i++) {
        gpio_isr_handler_remove(sensor_pins[i]);
    }
    gpio_uninstall_isr_service();
    vQueueDelete(sensor_event_queue);
    
	return ESP_OK;
}

// Public API
bool get_sensor(sensor_t i) {
    return sensor_stable_state[i];
}

bool get_safety_sensor(void) {
    return !safety_tripped;  // Returns true if safe, false if tripped
}

int32_t get_sensor_counter(sensor_t i) {
    return sensor_count[i];
}

void set_sensor_counter(sensor_t i, int32_t to) {
    sensor_count[i] = to;
}