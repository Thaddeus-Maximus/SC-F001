#include "storage.h"
#include "uart_comms.h"
#include "esp_err.h"
#include "esp_log.h"
#include "endian.h"
#include "control_fsm.h"
#include "power_mgmt.h"
#include "rtc.h"
#include "sensors.h"

#define TAG "MAIN"

esp_err_t send_log() {
	
	char entry[LOG_ENTRY_SIZE] = {0};
    entry[0] = LOG_ENTRY_SIZE;
    
    // Pack 64-bit timestamp into bytes 1-8
    uint64_t be_timestamp = htobe64(rtc_time_ms());
    memcpy(&entry[1], &be_timestamp, 8);
    
    // Pack 32-bit voltages/currents into bytes 9-24
    /*int32_t be_voltage = htobe32(get_battery_mV());
    memcpy(&entry[9],  &be_voltage,  4);
    int32_t be_current1 = htobe32(get_bridge_mA(BRIDGE_DRIVE));
    memcpy(&entry[13], &be_current1, 4);
    int32_t be_current2 = htobe32(get_bridge_mA(BRIDGE_JACK));
    memcpy(&entry[17], &be_current2, 4);
    int32_t be_current3 = htobe32(get_bridge_mA(BRIDGE_AUX));
    memcpy(&entry[21], &be_current3, 4);
    
    int32_t be_counter = htobe32(get_sensor_counter(SENSOR_DRIVE));
    memcpy(&entry[25], &be_counter, 4);
    
    entry[29] = get_sensor(SENSOR_DRIVE);
    entry[30] = get_sensor(SENSOR_JACK);
    entry[31] = fsm_get_state();*/
    
    return write_log(entry);
}

void app_main(void) {
    // Initialize storage and load parameters
    ESP_LOGI(TAG, "Initializing storage...");
    esp_err_t ret = storage_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Storage init failed, using defaults");
    }
    
    // Initialize logging
    ESP_LOGI(TAG, "Initializing logging...");
    ret = log_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Log init failed");
    }
    
    uart_start();
    
	
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(100);

    while(true) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        
		send_log();
	}
}