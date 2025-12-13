/*
 * filemgmt.c
 *
 *  Created on: Nov 20, 2025
 *      Author: Thad
 */

#include "filemgmt.h"
#include "endian.h"
#include "esp_log.h"
#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h" // for MACSTR
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "nvs_flash.h"
#include "esp_vfs_fat.h"
#include "esp_littlefs.h"
#include "ftp.h"
#include "control_fsm.h"
#include "power_mgmt.h"
#include "rtc.h"
#include "sensors.h"


const char* partition_label = "storage";
const char *TAG = "MAIN";
const char *mount_point = "/root";

RTC_DATA_ATTR bool find_new_filename = true;

void start_new_log_file() {
	find_new_filename = true;
}

EventGroupHandle_t xEventTask;
int FTP_TASK_FINISH_BIT = BIT2;

esp_err_t start_filesystem() {
	ESP_LOGI(TAG, "Initializing LittleFS on Builtin SPI Flash Memory");

	esp_vfs_littlefs_conf_t conf = {
		.base_path = mount_point,
		.partition_label = partition_label,
		.format_if_mount_failed = true,
		.dont_mount = false,
	};

	// Use settings defined above to initialize and mount LittleFS filesystem.
	// Note: esp_vfs_littlefs_register is an all-in-one convenience function.
	esp_err_t ret = esp_vfs_littlefs_register(&conf);

	if (ret != ESP_OK) {
		if (ret == ESP_FAIL) {
			ESP_LOGE(TAG, "Failed to mount or format filesystem");
		} else if (ret == ESP_ERR_NOT_FOUND) {
			ESP_LOGE(TAG, "Failed to find LittleFS partition");
		} else {
			ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
		}
		return ret;
	}

	size_t total = 0, used = 0;
	ret = esp_littlefs_info(conf.partition_label, &total, &used);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
		//esp_littlefs_format(conf.partition_label);
		return ret;
	}
	ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
	ESP_LOGI(TAG, "Mount LittleFS on %s", mount_point);
	return ret;
}


static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
	if (event_id == WIFI_EVENT_AP_STACONNECTED) {
		wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
		ESP_LOGI(TAG, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
	} else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
		wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
		ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d", MAC2STR(event->mac), event->aid);
	}
} 

extern void ftp_task (void *pvParameters);
// Start WiFi AP and FTP Server
// filemgmt.c
esp_err_t start_ftp_server(void)
{
    ESP_LOGI("FTP", "START");
    
    close_current_log();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // THIS IS THE CORRECT WAY – recreate the default AP netif every time
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_AP_SSID,
            .ssid_len = strlen(ESP_WIFI_AP_SSID),
            .password = ESP_WIFI_AP_PASSWORD,
            .max_connection = ESP_WIFI_AP_N_CONNECTIONS,
            .authmode = strlen(ESP_WIFI_AP_PASSWORD) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP started – IP 192.168.4.1");

    xEventTask = xEventGroupCreate();
    xTaskCreate(ftp_task, "FTP", 1024*6, NULL, 2, NULL);

    return ESP_OK;
}

void stop_ftp_server(void)
{
    ESP_LOGI("FTP", "OFF");

    ftp_terminate();
    vTaskDelay(pdMS_TO_TICKS(500));  // give task time to die

    esp_wifi_stop();
    esp_wifi_deinit();

    // This single call does everything correctly:
    // Destroy default Wi-Fi interface (handles AP)
	esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
	if (ap_netif) {
	    esp_netif_destroy_default_wifi(ap_netif);  // Correct API for AP
	}

    esp_event_loop_delete_default();
    esp_netif_deinit();

    ESP_LOGI("FTP", "WiFi + FTP completely stopped – safe to restart");
}



void stop_filesystem() {
	esp_vfs_littlefs_unregister(partition_label);
	ESP_LOGI(TAG, "LittleFS unmounted");
}

#define LOG_ENTRY_SIZE      32
#define MAX_LOG_FILES       999          // rotate after N files
#define ENTRIES_PER_FILE    8000         // ~256 KB per file → safe

static FILE* logfile = NULL;
RTC_DATA_ATTR char current_log_path[32] = "/root/log000.bin";
static uint32_t current_entry_count = 0;

// Helper: close current file safely
void close_current_log(void)
{
    if (logfile) {
        fflush(logfile);
        fsync(fileno(logfile));   // CRITICAL: forces write to flash
        fclose(logfile);
        logfile = NULL;
        current_entry_count = 0;
    }
}

// Helper: open next log file (rotates 000→009)
bool open_next_log_file(void)
{
    close_current_log();
    
    if (find_new_filename) {
		// Find next filename
	    for (int i = 0; i < MAX_LOG_FILES; i++) {
	        snprintf(current_log_path, sizeof(current_log_path), "/root/log%03d.bin", i);
	        struct stat st;
	        if (stat(current_log_path, &st) != 0) {
	            break;  // file doesn't exist → use this one
	        }
	        if (i == MAX_LOG_FILES-1) {
				ESP_LOGE("FILE", "RAN OUT OF FILES, GOING TO MAX");
	            // All files exist → overwrite oldest (log000.bin)
	            strcpy(current_log_path, "/root/log999.bin");
	        }
	    }
	
	    logfile = fopen(current_log_path, "ab");  // binary append
	    if (!logfile) {
	        ESP_LOGE("FILE", "Failed to open %s", current_log_path);
	        return false;
	    }
	
	    // Optional: truncate if too big (safety)
	    fseek(logfile, 0, SEEK_END);
	    if (ftell(logfile) > ENTRIES_PER_FILE * LOG_ENTRY_SIZE) {
	        fclose(logfile);
	        logfile = fopen(current_log_path, "wb");  // truncate
	    }
	    
		find_new_filename = false;
	} else {
	    logfile = fopen(current_log_path, "ab");
	}
    
    

    ESP_LOGI("FILE", "Logging to %s", current_log_path);
    return true;
}

void file_log() {
	if (!logfile && !open_next_log_file()) {
        return;
    }
	
	char entry[LOG_ENTRY_SIZE] = {0};
	entry[0] = LOG_ENTRY_SIZE;
	
	// pack 64-bit timestamp into bytes 1-8
	uint64_t be_timestamp = htobe64(rtc_time_ms());
	memcpy(&entry[1], &be_timestamp, 8);
	
	// pack 32-bit voltages/currents into bytes 9-24
	int32_t be_voltage = htobe32(get_battery_mV());
	memcpy(&entry[9], &be_voltage, 4);
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
	entry[31] = fsm_get_state();
	
	ESP_LOGI("FILE", "LOGGING TO %s: 0x %02x %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x %02x %02x", current_log_path,
		entry[0],
		entry[1],
		entry[2],
		entry[3],
		entry[4],
		entry[5],
		entry[6],
		entry[7],
		entry[8],
		entry[9],
		entry[10],
		entry[11],
		entry[12],
		entry[13],
		entry[14],
		entry[15],
		entry[16],
		entry[17],
		entry[18],
		entry[19],
		entry[20],
		entry[21],
		entry[22],
		entry[23],
		entry[24],
		entry[25],
		entry[26],
		entry[27],
		entry[28],
		entry[29],
		entry[30],
		entry[31]);
	
	
	/* SEND TO FILE */
	
	size_t written = fwrite(entry, 1, LOG_ENTRY_SIZE, logfile);
    if (written != LOG_ENTRY_SIZE) {
        ESP_LOGE("FILE", "Partial write! Closing file.");
        close_current_log();
        return;
    }

    current_entry_count++;

    // Periodic flush (every 50–100 entries) + full sync every 500
    if (current_entry_count % 100 == 0) {
        fflush(logfile);
    }
    if (current_entry_count % 500 == 0) {
        fsync(fileno(logfile));
    }

    // Rotate if getting large
    if (current_entry_count >= ENTRIES_PER_FILE) {
        ESP_LOGI("FILE", "Rotating log file");
        open_next_log_file();
    }
}

