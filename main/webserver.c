/*  WiFi softAP Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "cJSON.h"
#include "control_fsm.h"
#include "endian.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "power_mgmt.h"
#include "rf_433.h"
#include "rtc.h"
#include "simple_dns_server.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include <math.h>
#include <stdint.h>
#include <sys/param.h>
#include <time.h>
#include "stdio.h"
#include "storage.h"
#include "mdns.h"
#include "version.h"

#include "webpage.h"

#include "esp_partition.h"


#define HOSTNAME "sc.local"
#define SERVER_PORT  80

static const char *TAG = "WEBSERVER";

// HTTPS
/*
#include "esp_https_server.h"
extern const uint8_t servercert_pem_start[] asm("_binary_servercert_pem_start");
extern const uint8_t servercert_pem_end[] asm("_binary_servercert_pem_end");
extern const uint8_t prvtkey_pem_start[] asm("_binary_prvtkey_pem_start");
extern const uint8_t prvtkey_pem_end[] asm("_binary_prvtkey_pem_end");
*/

static httpd_handle_t httpServerInstance = NULL;

char httpBuffer[4096];

/* Handler to serve the HTML page */
static esp_err_t root_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "root_get_handler");
    
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }
    
    // Send the HTML response
    esp_err_t err = httpd_resp_set_type(req, "text/html");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response type: %s", esp_err_to_name(err));
        return err;
    }
    
    err = httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set content encoding header: %s", esp_err_to_name(err));
        return err;
    }
    
    err = httpd_resp_send(req, (const char *)html_content, html_content_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send HTML response: %s", esp_err_to_name(err));
        return err;
    }
    
    err = httpd_resp_set_hdr(req, "Connection", "close");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set connection header: %s", esp_err_to_name(err));
        // Continue anyway
    }
    
    return err;
}

// Cache the storage partition pointer to avoid repeated lookups
static const esp_partition_t *cached_storage_partition = NULL;

static esp_err_t log_handler(httpd_req_t *req) {
    //ESP_LOGI(TAG, "log_handler");
    
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }
    
    rtc_reset_shutdown_timer();
    
    int32_t tail = -1;
    
    if (req->method == HTTP_GET) {
        // give the whole log
    }
    else if (req->method == HTTP_POST) {
        int ret = httpd_req_recv(req, httpBuffer, sizeof(httpBuffer));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Socket timeout during receive");
                return httpd_resp_send_408(req);
            }
            ESP_LOGE(TAG, "Failed to receive POST data: %d", ret);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        }
        
        if (ret >= (int)sizeof(httpBuffer)) {
            ESP_LOGE(TAG, "POST data exceeds buffer size");
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        }
        
        httpBuffer[ret] = '\0'; // Null-terminate the string
            
        //ESP_LOGI(TAG, "LOG POST %.*s", ret, httpBuffer);
        
        if(sscanf(httpBuffer, "%ld", (long*)&tail) != 1) {
            // if malformed, just send the whole log.
            ESP_LOGW(TAG, "Malformed tail parameter, using default");
            tail = -1;
        }
    }
    else {
        ESP_LOGE(TAG, "Unsupported HTTP method: %d", req->method);
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    }

    // Use cached partition pointer instead of looking it up each time
    const esp_partition_t *storage_partition = cached_storage_partition;
    if (storage_partition == NULL) {
        // Fall back to lookup if cache is empty (shouldn't happen in normal operation)
        storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 
                                                     ESP_PARTITION_SUBTYPE_ANY, 
                                                     "storage");
        if (storage_partition == NULL) {
            ESP_LOGE(TAG, "Storage partition not found");
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                        "Storage partition not found");
        }
        cached_storage_partition = storage_partition;
    }

    // Get head and tail atomically and store in local variables
    // This releases the mutex before we do any partition operations
    int32_t head = get_log_head();
    int32_t log_start = get_log_offset();
    
    if (tail < 0) {
        tail = get_log_tail();
    } else {
        // Validate tail is within log area bounds
        if (tail < log_start || tail >= (int32_t)storage_partition->size) {
            ESP_LOGW(TAG, "Invalid tail pointer %ld, using current tail", (long)tail);
            tail = get_log_tail();
        }
        // Also validate tail is aligned to LOG_ENTRY_SIZE
        if ((tail - log_start) % LOG_ENTRY_SIZE != 0) {
            ESP_LOGW(TAG, "Tail pointer %ld not aligned to entry size, using current tail", 
                     (long)tail);
            tail = get_log_tail();
        }
    }
    
    // Calculate total size to send
    int32_t total_size;
    if (tail == head) {
        // Empty log - just send pointers
        total_size = 8;
    } else if (tail < head) {
        // Normal case: tail before head
        total_size = head - tail + 8;  // +8 for head/tail pointers
    } else {
        // Wrapped case: tail after head
        total_size = (storage_partition->size - tail) + (head - log_start) + 8;
    }

    //ESP_LOGI(TAG, "Log bounds: tail=%ld, head=%ld, total_size=%ld", 
    //         (long)tail, (long)head, (long)total_size);

    // Send HTTP headers
    char len_str[16];
    int written = snprintf(len_str, sizeof(len_str), "%u", (unsigned)total_size);
    if (written < 0 || written >= (int)sizeof(len_str)) {
        ESP_LOGE(TAG, "Failed to format content length");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Internal error formatting response");
    }
    
    esp_err_t err = httpd_resp_set_type(req, "application/octet-stream");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response type: %s", esp_err_to_name(err));
        return err;
    }
    
    err = httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"sc_storage.bin\"");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set content disposition: %s", esp_err_to_name(err));
        return err;
    }
    
    err = httpd_resp_set_hdr(req, "Content-Length", len_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set content length: %s", esp_err_to_name(err));
        return err;
    }

    // Send head/tail pointers in big-endian format
    int32_t htail = htobe32(tail);
    int32_t hhead = htobe32(head);
    memcpy(&httpBuffer[0], &(htail), 4);
    memcpy(&httpBuffer[4], &(hhead), 4);
    
    err = httpd_resp_send_chunk(req, (const char *)httpBuffer, 8);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send head/tail chunk: %s", esp_err_to_name(err));
        return err;
    }
    
    //int32_t sent = 8;
    int32_t offset = tail;
    
    // Only send data if there's something to send
    if (tail != head) {
        // Handle wrapped case: send from tail to end of partition first
        if (tail > head) {
            //ESP_LOGI(TAG, "Wrapped log: sending tail=%ld to partition_end=%lu", 
            //         (long)tail, (unsigned long)storage_partition->size);
            
            while (offset < (int32_t)storage_partition->size) {
                size_t to_read = MIN(sizeof(httpBuffer), storage_partition->size - offset);
                err = esp_partition_read(storage_partition, offset, httpBuffer, to_read);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to read partition at offset %ld: %s", 
                             (long)offset, esp_err_to_name(err));
                    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                                "Failed to read storage");
                }
                
                err = httpd_resp_send_chunk(req, (const char *)httpBuffer, to_read);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to send chunk at offset %ld: %s", 
                             (long)offset, esp_err_to_name(err));
                    return err;
                }
                
                //sent += to_read;
                offset += to_read;
            }
            
            // Wrap to beginning of log area
            offset = log_start;
            //ESP_LOGI(TAG, "Wrapped to log start, offset=%ld", (long)offset);
        }
        
        // Send from current offset to head
        //ESP_LOGI(TAG, "Sending final section: offset=%ld to head=%ld", (long)offset, (long)head);
        
        while (offset < head) {
            size_t to_read = MIN(sizeof(httpBuffer), head - offset);
            err = esp_partition_read(storage_partition, offset, httpBuffer, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read partition at offset %ld: %s", 
                         (long)offset, esp_err_to_name(err));
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                            "Failed to read storage");
            }
            
            err = httpd_resp_send_chunk(req, (const char *)httpBuffer, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send chunk at offset %ld: %s", 
                         (long)offset, esp_err_to_name(err));
                return err;
            }
            
            //sent += to_read;
            offset += to_read;
        }
    }
    
    // Send empty chunk to signal end
    err = httpd_resp_send_chunk(req, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send final chunk: %s", esp_err_to_name(err));
        return err;
    }
    
    //ESP_LOGI(TAG, "Successfully sent %ld bytes", (long)sent);
    
    err = httpd_resp_set_hdr(req, "Connection", "close");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set connection header: %s", esp_err_to_name(err));
        // Continue anyway
    }
    
    return err;
}

/**
 * Unified GET handler - returns complete system status
 * Response format:
 * {
 *   "time": 1234567,
 *   "rtc_valid": true,
 *   "state": 2,
 *   "voltage": 12.45,
 *   "remaining_dist": 12.0,
 *   "msg": "IDLE",
 *   "parameters": {
 *     "values": [12, 45, -3, 45.6, ...],
 *     "names": ["param1", "param2", ...],
 *     "units": ["s", "ms", "inches", ...]
 *   },
 *   ... other parameters as direct key-value pairs
 * }
 */
static esp_err_t get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "get_handler");
    
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }
    
    rtc_reset_shutdown_timer();

    int head = 0;

    // Start JSON object
    head += sprintf(httpBuffer+head, "{");
    head += sprintf(httpBuffer+head, "\"build_version\":\"%s\",", FIRMWARE_VERSION);
    head += sprintf(httpBuffer+head, "\"build_date\":\"%s\",", BUILD_DATE);
    head += sprintf(httpBuffer+head, "\"time\":%lld,", (long long)rtc_get_s());
    head += sprintf(httpBuffer+head, "\"rtc_set\":%s,", rtc_is_set() ? "true" : "false");
    head += sprintf(httpBuffer+head, "\"state\":%d,", fsm_get_state());
    head += sprintf(httpBuffer+head, "\"voltage\":%.3f,", get_battery_V());
    head += sprintf(httpBuffer+head, "\"remaining_dist\":%.3f,", fsm_get_remaining_distance());
    head += sprintf(httpBuffer+head, "\"next_alarm\":%lld,", rtc_get_next_alarm_s());
    
    head += sprintf(httpBuffer+head, "\"msg\":\"");
    
    switch(fsm_get_state()) {
		case STATE_IDLE:
			head += sprintf(httpBuffer+head, "IDLE");
			break;
		case STATE_UNDO_JACK:
		case STATE_UNDO_JACK_START:
			head += sprintf(httpBuffer+head, "CANCELLING MOVE");
			break;
		default:
			head += sprintf(httpBuffer+head, "MOVING...");
			break;
	}
	
	if (fsm_get_remaining_distance()<=0) {
		head += sprintf(httpBuffer+head, " | DISTANCE LIMIT HIT");
	}
	if (efuse_is_tripped(BRIDGE_AUX))   head += sprintf(httpBuffer+head, " | AUX EFUSE TRIP");
	if (efuse_is_tripped(BRIDGE_JACK))  head += sprintf(httpBuffer+head, " | JACK EFUSE TRIP");
	if (efuse_is_tripped(BRIDGE_DRIVE)) head += sprintf(httpBuffer+head, " | DRIVE EFUSE TRIP");
    if (!rtc_is_set()) {
		head += sprintf(httpBuffer+head, " | CLOCK NOT SET");
	}
	
    // Add parameters metadata object
    head += sprintf(httpBuffer+head, "\",\"parameters\":{");
    
    // Values array
    //head += sprintf(httpBuffer+head, "\"names\":[");
    for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
        if (i > 0) {
            head += sprintf(httpBuffer+head, ",");
        }
        
        head += sprintf(httpBuffer+head, "\"%s\":", get_param_name(i));
        
        param_value_t value = get_param_value_t(i);
        
        switch (get_param_type(i)) {
            case PARAM_TYPE_f32:
                head += sprintf(httpBuffer+head, "%.4f", value.f32);
                break;
            case PARAM_TYPE_f64:
                head += sprintf(httpBuffer+head, "%.4f", value.f64);
                break;
            case PARAM_TYPE_i32:
                head += sprintf(httpBuffer+head, "%ld", (long)value.i32);
                break;
            case PARAM_TYPE_i16:
                head += sprintf(httpBuffer+head, "%d", value.i16);
                break;
            case PARAM_TYPE_u32:
                head += sprintf(httpBuffer+head, "%lu", (long)value.u32);
                break;
            case PARAM_TYPE_u16:
                head += sprintf(httpBuffer+head, "%u", value.u16);
                break;
            case PARAM_TYPE_str:
                head += sprintf(httpBuffer+head, "\"%s\"", get_param_string(i));
                break;
            default:
                head += sprintf(httpBuffer+head, "null");
                break;
        }
    }
    /*head += sprintf(httpBuffer+head, "],");
    
    // Names array
    head += sprintf(httpBuffer+head, "\"names\":[");
    for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
        if (i > 0) {
            head += sprintf(httpBuffer+head, ",");
        }
        head += sprintf(httpBuffer+head, "\"%s\"", get_param_name(i));
    }
    head += sprintf(httpBuffer+head, "],");
    
    // Units array
    head += sprintf(httpBuffer+head, "\"units\":[");
    for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
        if (i > 0) {
            head += sprintf(httpBuffer+head, ",");
        }
        head += sprintf(httpBuffer+head, "\"%s\"", get_param_unit(i));
    }
    head += sprintf(httpBuffer+head, "]");*/
    
    // Close parameters object
    head += sprintf(httpBuffer+head, "}");
    
    // Close main JSON object
    head += sprintf(httpBuffer+head, "}");
    
    // Check if buffer might overflow
    if (head >= (int)(sizeof(httpBuffer) - 100)) {
        ESP_LOGE(TAG, "GET response buffer near overflow");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Status data too large");
    }

    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response type: %s", esp_err_to_name(err));
        return err;
    }
    
    err = httpd_resp_send(req, httpBuffer, head);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send status response: %s", esp_err_to_name(err));
        return err;
    }
    
    
    err = httpd_resp_set_hdr(req, "Connection", "close");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set connection header: %s", esp_err_to_name(err));
        // Continue anyway
    }
    
    return err;
}

/**
 * Unified POST handler - handles commands, parameter updates, time updates
 * Request format (all fields optional):
 * {
 *   "time": 1234567,           // Update RTC time
 *   "state": 2,                 // Update FSM state
 *   "cmd": "start",             // Execute command
 *   "voltage": 12.45,           // Update individual parameter
 *   "parameters": {             // Batch update parameters
 *     "PARAM_NAME": -15,
 *     "PARAM_NAME2": 12.0
 *   }
 * }
 */
static esp_err_t post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "post_handler");
    
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }
    
    rtc_reset_shutdown_timer();
    
    // Receive POST data
    int ret = httpd_req_recv(req, httpBuffer, sizeof(httpBuffer));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Socket timeout during receive");
            return httpd_resp_send_408(req);
        }
        ESP_LOGE(TAG, "Failed to receive POST data: %d", ret);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
    }
    
    if (ret >= (int)sizeof(httpBuffer)) {
        ESP_LOGE(TAG, "POST data exceeds buffer size");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
    }
    
    httpBuffer[ret] = '\0';
    
    ESP_LOGI(TAG, "POST: %.*s", ret, httpBuffer);
    
    // Parse JSON
    cJSON *root = cJSON_Parse(httpBuffer);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            ESP_LOGE(TAG, "JSON parse error before: %s", error_ptr);
        }
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    
    bool cmd_executed = false;
    bool sleep_requested = false;
    bool reboot_requested = false;
    const char *error_msg = NULL;
    int params_updated = 0;
    int params_failed = 0;
    
    // Process time if present
    cJSON *time = cJSON_GetObjectItem(root, "time");
    if (cJSON_IsNumber(time)) {
        int64_t new_time = (int64_t)cJSON_GetNumberValue(time);
        ESP_LOGI(TAG, "Setting time to %lld", new_time);
        rtc_set_s(new_time);
    }
    
    
    cJSON *remaining_dist = cJSON_GetObjectItem(root, "remaining_dist");
    if (cJSON_IsNumber(remaining_dist)) {
        int64_t new_dist = (int64_t)cJSON_GetNumberValue(remaining_dist);
        ESP_LOGI(TAG, "Setting time to %lld", new_dist);
        fsm_set_remaining_distance(new_dist);
    }
    
    // WE DO NOT PROCESS STATE. STATE IS A READ-ONLY.
    
    // Process command if present
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd)) {
        const char *cmd_str = cmd->valuestring;
        ESP_LOGI(TAG, "Executing command: %s", cmd_str);
        
        /*
        // Claude made this, wtf?
        if (strcmp(cmd_str, "trigger") == 0) {
            // Trigger RF433 transmitter
            rf_433_transmit();
            cmd_executed = true;
        } else */
        if (strcmp(cmd_str, "start") == 0) {
            // Start operation - transition FSM to running state
            //fsm_set_state(FSM_STATE_RUNNING);
            fsm_request(FSM_CMD_START);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "stop") == 0) {
            // Stop operation - transition FSM to idle state
            //fsm_set_state(FSM_STATE_IDLE);
            fsm_request(FSM_CMD_STOP);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "undo") == 0) {
            // Stop operation - transition FSM to idle state
            //fsm_set_state(FSM_STATE_IDLE);
            fsm_request(FSM_CMD_UNDO);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "fwd") == 0) {
            pulseOverride(RELAY_A1); pulseOverride(RELAY_A3);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rev") == 0) {
            pulseOverride(RELAY_B1); pulseOverride(RELAY_A3);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "up") == 0) {
            pulseOverride(RELAY_A2);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "down") == 0) {
            pulseOverride(RELAY_B2);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "aux") == 0) {
            pulseOverride(RELAY_A3);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "reboot") == 0) {
            reboot_requested = true;
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "sleep") == 0) {
            sleep_requested = true;
            cmd_executed = true;
        }
        /*else if (strcmp(cmd_str, "rfp") == 0) {
            // RF programming command - get channel parameter
            cJSON *channel = cJSON_GetObjectItem(root, "channel");
            if (cJSON_IsNumber(channel)) {
                int ch = (int)cJSON_GetNumberValue(channel);
                ESP_LOGI(TAG, "RF programming channel: %d", ch);
                rf_433_learn(ch);
                cmd_executed = true;
            } else {
                ESP_LOGW(TAG, "rfp command missing or invalid channel parameter");
                error_msg = "rfp requires channel parameter";
            }
        }*/
        else if (strcmp(cmd_str, "rf_clear_temp") == 0) {
            rf_433_clear_temp_keycodes();
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rf_disable") == 0) {
            rf_433_disable_controls();
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rf_enable") == 0) {
            rf_433_enable_controls();
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rf_learn") == 0) {
            // Start learning for a specific channel
            cJSON *channel = cJSON_GetObjectItem(root, "channel");
            if (cJSON_IsNumber(channel)) {
                int ch = (int)cJSON_GetNumberValue(channel);
                if (ch >= 0 && ch < NUM_RF_BUTTONS) {
                    rf_433_learn_keycode(ch);
                    cmd_executed = true;
                } else if (ch == -1) {
                    rf_433_cancel_learn_keycode();
                    cmd_executed = true;
                } else {
                    error_msg = "Invalid channel number";
                }
            } else {
                error_msg = "rf_learn requires channel parameter";
            }
        }
        else if (strcmp(cmd_str, "rf_set_temp") == 0) {
            cJSON *index = cJSON_GetObjectItem(root, "index");
            cJSON *code = cJSON_GetObjectItem(root, "code");
            if (cJSON_IsNumber(index) && cJSON_IsNumber(code)) {
                int idx = (int)cJSON_GetNumberValue(index);
                int32_t rf_code = (int32_t)cJSON_GetNumberValue(code);
                rf_433_set_temp_keycode(idx, rf_code);
                cmd_executed = true;
            } else {
                error_msg = "rf_set_temp requires index and code parameters";
            }
        }
        else if (strcmp(cmd_str, "rf_status") == 0) {
            // Return current temp keycodes
            cJSON *response = cJSON_CreateObject();
            cJSON *codes_array = cJSON_CreateArray();
            
            for (int i = 0; i < 4; i++) {  // Only return first 4 for web UI
                int32_t code = rf_433_get_temp_keycode(i);
                cJSON_AddItemToArray(codes_array, cJSON_CreateNumber(code));
            }
            
            cJSON_AddItemToObject(response, "codes", codes_array);
            
            char *json_str = cJSON_Print(response);
            cJSON_Delete(response);
            cJSON_Delete(root);
            
            esp_err_t err = httpd_resp_set_type(req, "application/json");
            if (err == ESP_OK) {
                err = httpd_resp_send(req, json_str, strlen(json_str));
            }
            free(json_str);
            return err;
        }
        
        else if (strcmp(cmd_str, "cal_jack_start") == 0) {
            fsm_request(FSM_CMD_CALIBRATE_JACK_PREP);
            ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_PREP");
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "cal_jack_finish") == 0) {
            cJSON *i = cJSON_GetObjectItem(root, "amt");
            if (cJSON_IsNumber(i) && i->valuedouble >= 0 && i->valuedouble < 8) {
                ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_FINISH");
                fsm_set_cal_val(i->valuedouble);
                fsm_request(FSM_CMD_CALIBRATE_JACK_FINISH);
                cmd_executed = true;
            } else {
                error_msg = "cal_jack_finish requires amt parameter (0-8)";
            }
        }
        else if (strcmp(cmd_str, "cal_drive_start") == 0) {
            fsm_request(FSM_CMD_CALIBRATE_DRIVE_PREP);
            ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_PREP");
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "cal_drive_finish") == 0) {
            cJSON *i = cJSON_GetObjectItem(root, "amt");
            if (cJSON_IsNumber(i) && i->valuedouble >= 0 && i->valuedouble < 8) {
                ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_FINISH");
                fsm_set_cal_val(i->valuedouble);
                fsm_request(FSM_CMD_CALIBRATE_DRIVE_FINISH);
                cmd_executed = true;
            } else {
                error_msg = "cal_drive_finish requires amt parameter (0-8)";
            }
        }
        
        else if (strcmp(cmd_str, "cal_get") == 0) {
            ESP_LOGI(TAG, "CAL_GET");
            
            // Build JSON response with calibration values
            cJSON *response = cJSON_CreateObject();
            cJSON_AddItemToObject(response, "e", cJSON_CreateNumber(fsm_get_cal_e()));
            cJSON_AddItemToObject(response, "t", cJSON_CreateNumber(fsm_get_cal_t()));
            
            char *json_str = cJSON_Print(response);
            cJSON_Delete(response);
            cJSON_Delete(root);
            
            esp_err_t err = httpd_resp_set_type(req, "application/json");
            if (err == ESP_OK) {
                err = httpd_resp_send(req, json_str, strlen(json_str));
            }
            free(json_str);
            return err;
        }
        
        else {
            ESP_LOGW(TAG, "Unknown command: %s", cmd_str);
            error_msg = "Unknown command";
        }
    }
    
    // Process batch parameter updates if present
    cJSON *parameters = cJSON_GetObjectItem(root, "parameters");
    if (cJSON_IsObject(parameters)) {
	    // Process individual parameter updates (direct key-value pairs)
	    cJSON *item = NULL;
	    cJSON_ArrayForEach(item, parameters) {
	        const char *key = item->string;
			ESP_LOGW(TAG, "PARAMETER %s", key);
	        
	        // Find parameter index by name
	        param_idx_t param_idx = NUM_PARAMS;
	        for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
	            if (strcmp(get_param_name(i), key) == 0) {
	                param_idx = i;
	                break;
	            }
	        }
	        
	        if (param_idx == NUM_PARAMS) {
				ESP_LOGW(TAG, "UNKNOWN PARAMETER %s", key);
	            // Not a known parameter, skip silently
	            continue;
	        }
	        
	        cJSON *value_json = cJSON_GetObjectItem(parameters, key);
	        
	        // Set parameter value based on type
	        switch (get_param_type(param_idx)) {
	            case PARAM_TYPE_f32:
	                if (cJSON_IsNumber(value_json)) {
			            set_param_value_t(param_idx,
			            	(param_value_t){.f32=value_json->valueint});
			            params_updated++;
	                } else { ESP_LOGW(TAG, "PARAM TYPE MISMATCH FOR %s", key); }
	                break;
	            case PARAM_TYPE_f64:
	                if (cJSON_IsNumber(value_json)) {
			            set_param_value_t(param_idx,
			            	(param_value_t){.f64=value_json->valueint});
			            params_updated++;
	                } else { ESP_LOGW(TAG, "PARAM TYPE MISMATCH FOR %s", key); }
	                break;
	            case PARAM_TYPE_i32:
	                if (cJSON_IsNumber(value_json)) {
			            set_param_value_t(param_idx,
			            	(param_value_t){.i32=value_json->valueint});
			            params_updated++;
	                } else { ESP_LOGW(TAG, "PARAM TYPE MISMATCH FOR %s", key); }
	                break;
	            case PARAM_TYPE_i16:
	                if (cJSON_IsNumber(value_json)) {
			            set_param_value_t(param_idx,
			            	(param_value_t){.i16=value_json->valueint});
			            params_updated++;
	                } else { ESP_LOGW(TAG, "PARAM TYPE MISMATCH FOR %s", key); }
	                break;
	            case PARAM_TYPE_u32:
	                if (cJSON_IsNumber(value_json)) {
			            set_param_value_t(param_idx,
			            	(param_value_t){.u32=value_json->valueint});
			            params_updated++;
	                } else { ESP_LOGW(TAG, "PARAM TYPE MISMATCH FOR %s", key); }
	                break;
	            case PARAM_TYPE_u16:
	                if (cJSON_IsNumber(value_json)) {
			            set_param_value_t(param_idx,
			            	(param_value_t){.u16=value_json->valueint});
			            params_updated++;
	                } else { ESP_LOGW(TAG, "PARAM TYPE MISMATCH FOR %s", key); }
	                break;
	            case PARAM_TYPE_str:
	                if (cJSON_IsString(value_json)) {
	                    set_param_string(param_idx, value_json->valuestring);
	                    params_updated++;
	                } else { ESP_LOGW(TAG, "PARAM TYPE MISMATCH FOR %s", key); }
	                break;
	            default:
	                break;
	        }
	    }
	    
	    if (params_updated > 0) {
			rtc_schedule_next_alarm();
			commit_params();
		}
    }
    
    cJSON_Delete(root);
    
    // Send response
    esp_err_t err;
    char response[256];
    int response_len;
    
    if (reboot_requested) {
        // Special handling for reboot
        response_len = snprintf(response, sizeof(response), 
                               "{\"status\":\"ok\",\"message\":\"Rebooting...\"}");
        
        if (response_len > 0 && response_len < (int)sizeof(response)) {
            err = httpd_resp_set_type(req, "application/json");
            if (err == ESP_OK) {
                err = httpd_resp_send(req, response, response_len);
            }
        }
        
        ESP_LOGI(TAG, "Rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return ESP_OK;  // Never reached
    }
    
    if (sleep_requested) {
		// Special handling for sleep
        response_len = snprintf(response, sizeof(response), 
                               "{\"status\":\"ok\",\"message\":\"Sleeping...\"}");
        
        if (response_len > 0 && response_len < (int)sizeof(response)) {
            err = httpd_resp_set_type(req, "application/json");
            if (err == ESP_OK) {
                err = httpd_resp_send(req, response, response_len);
            }
        }
        
        ESP_LOGI(TAG, "Sleeping in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        rtc_enter_deep_sleep();
        return ESP_OK;  // Never reached
	}
    
    if (error_msg != NULL) {
        err = httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error_msg);
    } else {
        response_len = snprintf(response, sizeof(response), 
                               "{\"status\":\"ok\",\"params_updated\":%d,\"params_failed\":%d,\"cmd_executed\":%s}", 
                               params_updated, params_failed, cmd_executed ? "true" : "false");
        
        if (response_len < 0 || response_len >= (int)sizeof(response)) {
            ESP_LOGE(TAG, "Failed to format response");
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                        "Internal error");
        }
        
        err = httpd_resp_set_type(req, "application/json");
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set response type: %s", esp_err_to_name(err));
            return err;
        }
        
        err = httpd_resp_send(req, response, response_len);
    }
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(err));
        return err;
    }
    
    
    err = httpd_resp_set_hdr(req, "Connection", "close");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set connection header: %s", esp_err_to_name(err));
        // Continue anyway
    }
    
    return err;
}

static esp_err_t ota_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "OTA POST request received");
    
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }
    
    rtc_reset_shutdown_timer();

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "No OTA partition available");
    }

    ESP_LOGI(TAG, "Starting OTA update on partition: %s", update_partition->label);

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "OTA begin failed");
    }
    int recv_len;
    int total_len = req->content_len;
    int remaining = total_len;
    int received = 0;

    if (total_len <= 0) {
        ESP_LOGE(TAG, "Invalid content length: %d", total_len);
        esp_ota_abort(update_handle);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 
                                    "Invalid or missing content length");
    }

    ESP_LOGI(TAG, "Expected OTA size: %d bytes", total_len);

    int timeout_count = 0;
    const int MAX_TIMEOUTS = 3;

    while (remaining > 0) {
        recv_len = httpd_req_recv(req, httpBuffer, MIN(remaining, sizeof(httpBuffer)));
        
        if (recv_len < 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                timeout_count++;
                ESP_LOGW(TAG, "Socket timeout (%d/%d), retrying...", 
                         timeout_count, MAX_TIMEOUTS);
                
                if (timeout_count < MAX_TIMEOUTS) {
                    continue;
                } else {
                    ESP_LOGE(TAG, "Too many timeouts, aborting OTA");
                    esp_ota_abort(update_handle);
                    return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, 
                                                "Request timeout");
                }
            } else if (recv_len == HTTPD_SOCK_ERR_FAIL) {
                ESP_LOGE(TAG, "Socket error during receive");
                esp_ota_abort(update_handle);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                            "Socket error during OTA");
            } else {
                ESP_LOGE(TAG, "Unexpected error during receive: %d", recv_len);
                esp_ota_abort(update_handle);
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                            "OTA receive failed");
            }
        }
        
        if (recv_len == 0) {
            ESP_LOGE(TAG, "Connection closed prematurely. Received %d of %d bytes", 
                     received, total_len);
            esp_ota_abort(update_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                        "Connection closed during OTA");
        }

        // Reset timeout counter on successful receive
        timeout_count = 0;

        err = esp_ota_write(update_handle, (const void *)httpBuffer, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                        "OTA write failed");
        }

        remaining -= recv_len;
        received += recv_len;
        
        // Log progress every 10%
        if (total_len > 0 && (received % (total_len / 10)) < recv_len) {
            ESP_LOGI(TAG, "OTA progress: %d%%", (received * 100) / total_len);
        }
    }

    ESP_LOGI(TAG, "OTA data received completely. Total: %d bytes", received);

    if (received != total_len) {
        ESP_LOGE(TAG, "Size mismatch: received %d, expected %d", received, total_len);
        esp_ota_abort(update_handle);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "OTA size mismatch");
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed");
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, 
                                        "OTA image validation failed");
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                        "OTA end failed");
        }
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Failed to set boot partition");
    }

    ESP_LOGI(TAG, "OTA update successful. Rebooting in 2 seconds...");
    
    // Send success response FIRST
    err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response type: %s", esp_err_to_name(err));
        // Continue anyway, try to send response
    }
    
    err = httpd_resp_send(req, "{\"status\":\"ok\",\"message\":\"OTA update successful, rebooting...\"}", 
                         HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(err));
        // Continue with reboot anyway
    }
    
    // Update boot time parameter
    // set_param_value_t(PARAM_BOOT_TIME, (param_value_t){.i32 = system_rtc_get_raw_time()});
    
    // THEN delay and reboot
    vTaskDelay(pdMS_TO_TICKS(2000)); // Give time for TCP to close properly
    esp_restart();

    return ESP_OK;  // Never reached
}


static esp_err_t catchall_handler(httpd_req_t *req) {
	ESP_LOGI(TAG, "catchall_handler; %s", req->uri);
    const char *uri = req->uri;
    
    // Windows NCSI
    if (strcmp(uri, "/connecttest.txt") == 0) {
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Microsoft Connect Test");
        httpd_resp_set_hdr(req, "Connection", "close");
        return ESP_OK;
    }
    
    if (strncmp(uri, "/success.txt", 12) == 0) {  // Handles query params too
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Success");
        httpd_resp_set_hdr(req, "Connection", "close");
        return ESP_OK;
    }
    
    if (strcmp(uri, "/canonical.html") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr(req, "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
        httpd_resp_set_hdr(req, "Connection", "close");
        return ESP_OK;
    }
    
    // Android
    if (strcmp(uri, "/generate_204") == 0 || strcmp(uri, "/gen_204") == 0) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_send(req, NULL, 0);
        httpd_resp_set_hdr(req, "Connection", "close");
        return ESP_OK;
    }
    
    // iOS/macOS
    if (strcmp(uri, "/hotspot-detect.html") == 0 || 
        strcmp(uri, "/library/test/success.html") == 0) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_send(req, NULL, 0);
        httpd_resp_set_hdr(req, "Connection", "close");
        return ESP_OK;
    }
    
    // Default 404
    httpd_resp_send_404(req);
    httpd_resp_set_hdr(req, "Connection", "close");
    
    return ESP_OK;
}



/************************************************
**************** URI HANDLER MAP ****************
************************************************/

httpd_uri_t uris[] = {{
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
},{
    .uri       = "/get",
    .method    = HTTP_GET,
    .handler   = get_handler,
    .user_ctx  = NULL
},{
    .uri       = "/post",
    .method    = HTTP_POST,
    .handler   = post_handler,
    .user_ctx  = NULL
},{
    .uri       = "/log",
    .method    = HTTP_ANY,
    .handler   = log_handler,
    .user_ctx  = NULL
},{
    .uri       = "/ota",
    .method    = HTTP_POST,
    .handler   = ota_post_handler,
    .user_ctx  = NULL
},{
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = catchall_handler,
    .user_ctx  = NULL
}};

/**********************************************************
**************** WIFI + WEB SERVER RUNNERS ****************
**********************************************************/

bool server_running = false;

static esp_err_t startHttpServer(void) {
	if (server_running) return ESP_OK;
	ESP_LOGI(TAG, "STARTING HTTP");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard; // enable wildcarding
    
    esp_err_t err = httpd_start(&httpServerInstance, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "HTTP server started successfully");
    
    // Register URI handlers
    for (uint8_t i = 0; i < (sizeof(uris)/sizeof(httpd_uri_t)); i++) {
        err = httpd_register_uri_handler(httpServerInstance, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI handler for %s: %s", 
                     uris[i].uri, esp_err_to_name(err));
            // Continue registering other handlers even if one fails
        } else {
            ESP_LOGI(TAG, "Registered URI handler: %s", uris[i].uri);
        }
    }
    
    server_running = true;
    
    return ESP_OK;
}

static esp_err_t stopHttpServer(void) {
	if (!server_running) return ESP_OK;
	ESP_LOGI(TAG, "STOPPING HTTP");
    if (httpServerInstance == NULL) {
        ESP_LOGW(TAG, "HTTP server not running");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t err = httpd_stop(httpServerInstance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    
    httpServerInstance = NULL;
    ESP_LOGI(TAG, "HTTP server stopped");
    server_running = false;
    return ESP_OK;
}

/* Event handler for WiFi events */

int n_connected = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    
    
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station connected, AID=%d", event->aid);
        rtc_reset_shutdown_timer();
        n_connected ++;
        if (n_connected > 0)
        	startHttpServer();
        
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station disconnected, AID=%d", event->aid);
        n_connected --;
        if (n_connected <= 0)
        	stopHttpServer();
    }
}

static esp_err_t launchSoftAp(void) {
    esp_err_t err;
    
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // NVS partition was truncated and needs to be erased
        ESP_LOGW(TAG, "NVS partition needs erasing, performing erase...");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
            return err;
        }
        // Retry init after erase
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize netif: %s", esp_err_to_name(err));
        return err;
    }
    
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default WiFi AP interface");
        return ESP_FAIL;
    }

    err = esp_netif_set_hostname(ap_netif, HOSTNAME);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set hostname: %s", esp_err_to_name(err));
        // Non-critical, continue
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &wifi_event_handler,
                                              NULL,
                                              NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register event handler: %s", esp_err_to_name(err));
        return err;
    }

    wifi_config_t wifi_config = {
        .ap = {
            .channel = get_param_value_t(PARAM_WIFI_CHANNEL).i16,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    
    // Get the strings from your parameter system
    char* ssid_str = get_param_string(PARAM_WIFI_SSID);
    char* password_str = get_param_string(PARAM_WIFI_PASS);
    
    if (ssid_str == NULL || password_str == NULL) {
        ESP_LOGE(TAG, "Failed to get WiFi credentials from parameters");
        return ESP_FAIL;
    }
    
    // Allocate and set the SSID and password
    memcpy(wifi_config.ap.ssid, ssid_str, MIN(strlen(ssid_str), sizeof(wifi_config.ap.ssid)));
    memcpy(wifi_config.ap.password, password_str, MIN(strlen(password_str), sizeof(wifi_config.ap.password)));
    
    // password minimum length of 8
    if (strlen(password_str) < 8) {
        ESP_LOGW(TAG, "Password too short, using open authentication");
        wifi_config.ap.password[0] = '\0';
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    
    // Validate WiFi channel
    if (wifi_config.ap.channel > 11 || wifi_config.ap.channel < 1) {
        ESP_LOGW(TAG, "Invalid WiFi channel %d, using default channel 6", 
                 wifi_config.ap.channel);
        wifi_config.ap.channel = 6;
    }
    
    // Set the length of SSID
    wifi_config.ap.ssid_len = strlen(ssid_str);

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(err));
        return err;
    }
    
    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
        return err;
    }
    
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        return err;
    }

    // Start DNS server with your specific hostname
    err = simple_dns_server_start("192.168.4.1");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start DNS server: %s", esp_err_to_name(err));
        // Non-critical, continue
    }

    // Start mDNS for .local domain
    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mDNS: %s", esp_err_to_name(err));
        // Non-critical, continue
    } else {
        err = mdns_hostname_set(HOSTNAME);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set mDNS hostname: %s", esp_err_to_name(err));
        }
        
        err = mdns_instance_name_set("ClusterCommand");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set mDNS instance name: %s", esp_err_to_name(err));
        }
        
        err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to add mDNS service: %s", esp_err_to_name(err));
        }
    }

    uint8_t *placeholder = (wifi_config.ap.authmode == WIFI_AUTH_OPEN)
                            ? (uint8_t*)"<open network>"
                            : wifi_config.ap.password;
    ESP_LOGI(TAG, "SoftAP ready. SSID: %s, Channel: %d, Password: %s",
             wifi_config.ap.ssid, wifi_config.ap.channel, placeholder);
    ESP_LOGI(TAG, "Access at: http://%s or http://%s.local or http://192.168.4.1", 
             HOSTNAME, HOSTNAME);
    
    return ESP_OK;
}

esp_err_t webserver_init(void) {
    ESP_LOGI(TAG, "Initializing webserver...");
    
    esp_err_t err = launchSoftAp();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to launch SoftAP: %s", esp_err_to_name(err));
        return err;
    }
    
    err = startHttpServer();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Webserver initialization complete");
    return ESP_OK;
}