/*  WiFi softAP Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "cJSON.h"
#include "comms.h"
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
#include "webserver.h"

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

static httpd_handle_t http_server_instance = NULL;

char http_buffer[4096];

/* Handler to serve the HTML page */
static esp_err_t root_get_handler(httpd_req_t *req) {
    //ESP_LOGI(TAG, "root_get_handler");
    
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

// In webserver.c - Replace the log_handler function

static esp_err_t log_handler(httpd_req_t *req) {
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }
    
    rtc_reset_shutdown_timer();
    
    int32_t tail = -1;
    
    if (req->method == HTTP_GET) {
        // GET without parameters - return JSON + full log
    }
    else if (req->method == HTTP_POST) {
        int ret = httpd_req_recv(req, http_buffer, sizeof(http_buffer));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "Socket timeout during receive");
                return httpd_resp_send_408(req);
            }
            ESP_LOGE(TAG, "Failed to receive POST data: %d", ret);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
        }
        
        if (ret >= (int)sizeof(http_buffer)) {
            ESP_LOGE(TAG, "POST data exceeds buffer size");
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
        }
        
        http_buffer[ret] = '\0';
            
        if(sscanf(http_buffer, "%ld", (long*)&tail) != 1) {
            ESP_LOGW(TAG, "Malformed tail parameter, using default");
            tail = -1;
        }
    }
    else {
        ESP_LOGE(TAG, "Unsupported HTTP method: %d", req->method);
        return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED, "Method not allowed");
    }

    const esp_partition_t *storage_partition = cached_storage_partition;
    if (storage_partition == NULL) {
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

    int32_t head = log_get_head();
    int32_t log_start = log_get_offset();
    
    if (tail < 0) {
        tail = log_get_tail();
    } else {
        if (tail < log_start || tail >= (int32_t)storage_partition->size) {
            ESP_LOGW(TAG, "Invalid tail pointer %ld, using current tail", (long)tail);
            tail = log_get_tail();
        }
    }
    
    // Calculate log data size
    int32_t log_data_size;
    if (tail == head) {
        log_data_size = 0;
    } else if (tail < head) {
        log_data_size = head - tail;
    } else {
        log_data_size = (storage_partition->size - tail) + (head - log_start);
    }

    // Generate JSON header (same as /get endpoint)
    cJSON *json_response = comms_handle_get();
    if (json_response == NULL) {
        ESP_LOGE(TAG, "Failed to generate JSON response");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Failed to generate response");
    }
    
    char *json_str = cJSON_PrintUnformatted(json_response);
    cJSON_Delete(json_response);
    
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Failed to serialize response");
    }
    
    uint32_t json_len = strlen(json_str);
    
    // Total size: 4 (json length) + json + 8 (head/tail) + log_data
    uint32_t total_size = 4 + json_len + 8 + log_data_size;

    //ESP_LOGI(TAG, "Log request: tail=%ld, head=%ld, json_len=%lu, log_size=%ld, total=%lu", 
    //         (long)tail, (long)head, (unsigned long)json_len, (long)log_data_size, 
    //         (unsigned long)total_size);

    // Send HTTP headers
    char len_str[16];
    snprintf(len_str, sizeof(len_str), "%u", (unsigned)total_size);
    
    esp_err_t err = httpd_resp_set_type(req, "application/octet-stream");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response type: %s", esp_err_to_name(err));
        free(json_str);
        return err;
    }
    
    err = httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"sc_log.bin\"");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set content disposition: %s", esp_err_to_name(err));
        free(json_str);
        return err;
    }
    
    err = httpd_resp_set_hdr(req, "Content-Length", len_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set content length: %s", esp_err_to_name(err));
        free(json_str);
        return err;
    }

    // Send JSON length (4 bytes, big-endian)
    uint32_t json_len_be = htobe32(json_len);
    memcpy(&http_buffer[0], &json_len_be, 4);
    err = httpd_resp_send_chunk(req, (const char *)http_buffer, 4);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send JSON length: %s", esp_err_to_name(err));
        free(json_str);
        return err;
    }
    
    // Send JSON string
    err = httpd_resp_send_chunk(req, json_str, json_len);
    free(json_str);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send JSON: %s", esp_err_to_name(err));
        return err;
    }

    // Send head/tail pointers (8 bytes, big-endian)
    int32_t htail = htobe32(tail);
    int32_t hhead = htobe32(head);
    memcpy(&http_buffer[0], &htail, 4);
    memcpy(&http_buffer[4], &hhead, 4);
    
    err = httpd_resp_send_chunk(req, (const char *)http_buffer, 8);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send head/tail chunk: %s", esp_err_to_name(err));
        return err;
    }
    
    // Send log data (same as before)
    int32_t offset = tail;
    
    if (tail == head) {
        // Empty log, nothing more to send
    }
    else if (tail < head) {
        // Normal case: tail before head
        while (offset < head) {
            size_t to_read = MIN(sizeof(http_buffer), head - offset);
            err = esp_partition_read(storage_partition, offset, http_buffer, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read partition at offset %ld: %s", 
                         (long)offset, esp_err_to_name(err));
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                            "Failed to read storage");
            }
            
            err = httpd_resp_send_chunk(req, (const char *)http_buffer, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send chunk at offset %ld: %s", 
                         (long)offset, esp_err_to_name(err));
                return err;
            }
            
            offset += to_read;
        }
    }
    else {
        // Wrapped case: tail after head, read from tail to end, then start to head
        while (offset < (int32_t)storage_partition->size) {
            size_t to_read = MIN(sizeof(http_buffer), storage_partition->size - offset);
            err = esp_partition_read(storage_partition, offset, http_buffer, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read partition at offset %ld: %s", 
                         (long)offset, esp_err_to_name(err));
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                            "Failed to read storage");
            }
            
            err = httpd_resp_send_chunk(req, (const char *)http_buffer, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send chunk at offset %ld: %s", 
                         (long)offset, esp_err_to_name(err));
                return err;
            }
            
            offset += to_read;
        }
        
        // Now read from start to head
        offset = log_start;
        while (offset < head) {
            size_t to_read = MIN(sizeof(http_buffer), head - offset);
            err = esp_partition_read(storage_partition, offset, http_buffer, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read partition at offset %ld: %s", 
                         (long)offset, esp_err_to_name(err));
                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                            "Failed to read storage");
            }
            
            err = httpd_resp_send_chunk(req, (const char *)http_buffer, to_read);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to send chunk at offset %ld: %s", 
                         (long)offset, esp_err_to_name(err));
                return err;
            }
            
            offset += to_read;
        }
    }
    
    // Send empty chunk to signal end
    err = httpd_resp_send_chunk(req, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send final chunk: %s", esp_err_to_name(err));
        return err;
    }
    
    err = httpd_resp_set_hdr(req, "Connection", "close");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set connection header: %s", esp_err_to_name(err));
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
/**
 * Unified GET handler - returns complete system status
 */
static esp_err_t get_handler(httpd_req_t *req) {
    //ESP_LOGI(TAG, "get_handler");
    
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }
    
    // Call unified GET handler
    cJSON *response = comms_handle_get();
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to generate GET response");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Failed to generate response");
    }
    
    // Convert to string (not pretty printed for web - save bandwidth)
    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Failed to serialize response");
    }
    
    // Send response
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set response type: %s", esp_err_to_name(err));
        free(json_str);
        return err;
    }
    
    err = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(err));
        return err;
    }
    
    err = httpd_resp_set_hdr(req, "Connection", "close");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set connection header: %s", esp_err_to_name(err));
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
static void soft_idle_enter_cb(void *arg)    { soft_idle_enter(); }
static void webserver_restart_wifi_cb(void *arg) { webserver_restart_wifi(); }

/**
 * Unified POST handler - handles commands, parameter updates, time updates
 */
static esp_err_t post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "post_handler");
    
    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }
    
    // Receive POST data
    int ret = httpd_req_recv(req, http_buffer, sizeof(http_buffer));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Socket timeout during receive");
            return httpd_resp_send_408(req);
        }
        ESP_LOGE(TAG, "Failed to receive POST data: %d", ret);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to receive data");
    }
    
    if (ret >= (int)sizeof(http_buffer)) {
        ESP_LOGE(TAG, "POST data exceeds buffer size");
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request too large");
    }
    
    http_buffer[ret] = '\0';
    
    ESP_LOGI(TAG, "POST: %.*s", ret, http_buffer);
    
    // Parse JSON
    cJSON *root = cJSON_Parse(http_buffer);
    if (root == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            ESP_LOGE(TAG, "JSON parse error before: %s", error_ptr);
        }
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }
    
    // Call unified POST handler
    cJSON *response = NULL;
    esp_err_t err = comms_handle_post(root, &response);
    cJSON_Delete(root);
    
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to generate POST response");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Failed to generate response");
    }
    
    // Check for special response flags
    cJSON *reboot_flag       = cJSON_GetObjectItem(response, "reboot");
    cJSON *sleep_flag        = cJSON_GetObjectItem(response, "sleep");
    cJSON *wifi_restart_flag = cJSON_GetObjectItem(response, "wifi_restart");
    bool should_reboot        = cJSON_IsTrue(reboot_flag);
    bool should_sleep         = cJSON_IsTrue(sleep_flag);
    bool should_restart_wifi  = cJSON_IsTrue(wifi_restart_flag);
    
    // Convert response to string
    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Failed to serialize response");
    }
    
    // Send response
    err = httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        err = httpd_resp_send(req, json_str, strlen(json_str));
    }
    free(json_str);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(err));
        return err;
    }
    
    // Handle special actions after response is sent
    if (should_reboot) {
        ESP_LOGI(TAG, "Rebooting in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return ESP_OK;  // Never reached
    }

    if (should_restart_wifi) {
        /* Same deadlock risk as should_sleep — httpd_stop() inside
         * webserver_restart_wifi() cannot be called from within a handler. */
        static esp_timer_handle_t s_wifi_restart_timer = NULL;
        if (s_wifi_restart_timer == NULL) {
            esp_timer_create_args_t ta = {
                .callback = webserver_restart_wifi_cb,
                .name     = "wifi_restart",
            };
            esp_timer_create(&ta, &s_wifi_restart_timer);
        }
        if (s_wifi_restart_timer != NULL) {
            esp_timer_start_once(s_wifi_restart_timer, 500 * 1000); /* 500 ms in µs */
        }
        return ESP_OK;
    }

    if (should_sleep) {
        ESP_LOGI(TAG, "Entering soft idle in 2 seconds...");
        /* Cannot call soft_idle_enter() (→ httpd_stop()) from within an httpd
         * handler — httpd_stop() waits for all handlers to finish, causing a
         * deadlock.  Schedule via a one-shot timer so this handler returns
         * first and the httpd task is free. */
        static esp_timer_handle_t s_sleep_timer = NULL;
        if (s_sleep_timer == NULL) {
            esp_timer_create_args_t ta = {
                .callback = soft_idle_enter_cb,
                .name     = "soft_idle",
            };
            esp_timer_create(&ta, &s_sleep_timer);
        }
        if (s_sleep_timer != NULL) {
            esp_timer_start_once(s_sleep_timer, 2000 * 1000); /* 2 s in µs */
        }
        return ESP_OK;
    }
    
    err = httpd_resp_set_hdr(req, "Connection", "close");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set connection header: %s", esp_err_to_name(err));
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
        recv_len = httpd_req_recv(req, http_buffer, MIN(remaining, sizeof(http_buffer)));
        
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

        err = esp_ota_write(update_handle, (const void *)http_buffer, recv_len);
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
	//ESP_LOGI(TAG, "catchall_handler; %s", req->uri);
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
static bool s_wifi_running = false;

static esp_err_t start_http_server(void) {
	if (server_running) return ESP_OK;
	ESP_LOGI(TAG, "STARTING HTTP");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard; // enable wildcarding
    
    esp_err_t err = httpd_start(&http_server_instance, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "HTTP server started successfully");
    
    // Register URI handlers
    for (uint8_t i = 0; i < (sizeof(uris)/sizeof(httpd_uri_t)); i++) {
        err = httpd_register_uri_handler(http_server_instance, &uris[i]);
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

static esp_err_t stop_http_server(void) {
	if (!server_running) return ESP_OK;
	ESP_LOGI(TAG, "STOPPING HTTP");
    if (http_server_instance == NULL) {
        ESP_LOGW(TAG, "HTTP server not running");
        return ESP_ERR_INVALID_STATE;
    }
    
    esp_err_t err = httpd_stop(http_server_instance);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    
    http_server_instance = NULL;
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
        	start_http_server();
        
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station disconnected, AID=%d", event->aid);
        n_connected --;
        if (n_connected <= 0)
        	stop_http_server();
    }
}

static esp_err_t launch_soft_ap(void) {
    esp_err_t err;
    
    
    ESP_LOGI(TAG, "AP LAUNCHING");
    
    ESP_LOGI(TAG, "AP LAUNCHING...");

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
    s_wifi_running = true;

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

esp_err_t webserver_stop(void) {
    stop_http_server();
    if (s_wifi_running) {
        esp_wifi_stop();
        s_wifi_running = false;
    }
    return ESP_OK;
}

esp_err_t webserver_restart_wifi(void) {
    ESP_LOGI(TAG, "Restarting WiFi AP with updated params...");

    stop_http_server();
    if (s_wifi_running) {
        esp_wifi_stop();
        s_wifi_running = false;
    }

    wifi_config_t wifi_config = {
        .ap = {
            .channel       = get_param_value_t(PARAM_WIFI_CHANNEL).u16,
            .max_connection = 4,
            .authmode      = WIFI_AUTH_WPA2_PSK,
        },
    };

    char *ssid_str = get_param_string(PARAM_WIFI_SSID);
    char *pass_str = get_param_string(PARAM_WIFI_PASS);
    memcpy(wifi_config.ap.ssid,     ssid_str, MIN(strlen(ssid_str), sizeof(wifi_config.ap.ssid)));
    memcpy(wifi_config.ap.password, pass_str, MIN(strlen(pass_str), sizeof(wifi_config.ap.password)));
    wifi_config.ap.ssid_len = strlen(ssid_str);
    if (strlen(pass_str) < 8) {
        wifi_config.ap.password[0] = '\0';
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    if (wifi_config.ap.channel < 1 || wifi_config.ap.channel > 11)
        wifi_config.ap.channel = 6;

    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();
    s_wifi_running = true;
    start_http_server();

    ESP_LOGI(TAG, "WiFi AP restarted. New SSID: %s, Channel: %d",
             wifi_config.ap.ssid, wifi_config.ap.channel);
    return ESP_OK;
}

esp_err_t webserver_init(void) {
    ESP_LOGI(TAG, "Initializing webserver...");
    
    // Initialize comms module
    esp_err_t err = launch_soft_ap();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to launch SoftAP: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "AP LAUNCHED");
    
    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Webserver initialization complete");
    return ESP_OK;
}