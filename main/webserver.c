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
#include "bringup.h"
#include "power_mgmt.h"
#include "rf_433.h"
#include "rtc.h"
#include "simple_dns_server.h"
#include "string.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "lwip/sockets.h"   // close() for the WS disconnect callback
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
#include "comms_events.h"

#include "esp_partition.h"


/* mDNS label only — the mdns component and esp_netif append ".local"
 * themselves, so this must be "sc", NOT "sc.local". Passing the full
 * "sc.local" advertised the host as "sc.local.local", which is why Apple
 * devices (which resolve .local names exclusively over mDNS) couldn't find
 * "sc.local". */
#define HOSTNAME "sc"
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

/* Shared scratch buffer used by log/post/ota handlers. The httpd default
 * config uses a single worker task, so handlers don't normally race — but
 * with config.lru_purge_enable + 7 sockets some IDF configurations allow
 * concurrent invocations. The mutex below is defense in depth: each
 * handler that touches http_buffer takes it on entry via the wrap_*
 * dispatcher below and releases on exit. */
char http_buffer[4096];
static SemaphoreHandle_t http_buffer_mutex = NULL;

/* Run an httpd call; on non-OK, log a "Failed to <what>" message and
 * return the error code from the enclosing function. Replaces the
 * three-line set_type / log / return triples that dominated handlers. */
#define HTTPD_RET_ON_ERR(expr, what) do { \
    esp_err_t _e = (expr); \
    if (_e != ESP_OK) { \
        ESP_LOGE(TAG, "Failed to %s: %s", what, esp_err_to_name(_e)); \
        return _e; \
    } \
} while (0)

/* Same but only warns (used when the handler can plausibly proceed even
 * if the call failed — typically the trailing Connection: close header). */
#define HTTPD_WARN_ON_ERR(expr, what) do { \
    esp_err_t _e = (expr); \
    if (_e != ESP_OK) { \
        ESP_LOGW(TAG, "Failed to %s: %s", what, esp_err_to_name(_e)); \
    } \
} while (0)

/* Read [from, to) from `part` into http_buffer in chunks and stream each
 * chunk as an HTTP body fragment. The shared http_buffer is already held
 * by the caller via with_http_buffer(). Returns the first error to abort
 * the caller's response (and arranges a 500 if the partition read fails). */
static esp_err_t stream_partition_range(httpd_req_t *req,
                                        const esp_partition_t *part,
                                        int32_t from, int32_t to) {
    int32_t offset = from;
    while (offset < to) {
        size_t to_read = MIN(sizeof(http_buffer), (size_t)(to - offset));
        esp_err_t err = esp_partition_read(part, offset, http_buffer, to_read);
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
    return ESP_OK;
}

/* Schedule a one-shot esp_timer to invoke `cb` `delay_us` from now. Used
 * to defer sleep/hibernate/wifi_restart calls until the httpd handler has
 * returned — calling httpd_stop()/webserver_stop() inside a handler dead-
 * locks because httpd_stop waits for all handlers to drain. `*slot` is
 * cached across calls so we don't leak timer handles on repeated requests. */
static void defer_call(esp_timer_handle_t *slot, const char *name,
                       esp_timer_cb_t cb, uint64_t delay_us) {
    if (*slot == NULL) {
        esp_timer_create_args_t ta = { .callback = cb, .name = name };
        esp_timer_create(&ta, slot);
    }
    if (*slot != NULL) esp_timer_start_once(*slot, delay_us);
}

static esp_err_t with_http_buffer(httpd_req_t *req,
                                   esp_err_t (*body)(httpd_req_t *)) {
    if (http_buffer_mutex != NULL) {
        if (xSemaphoreTake(http_buffer_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            /* esp_http_server's httpd_err_code_t enum doesn't include 503,
             * so emit it via the lower-level set_status + send pair. */
            ESP_LOGW(TAG, "http_buffer busy — rejecting request");
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_type(req, "text/plain");
            httpd_resp_send(req, "busy", HTTPD_RESP_USE_STRLEN);
            return ESP_FAIL;
        }
    }
    esp_err_t ret = body(req);
    if (http_buffer_mutex != NULL) xSemaphoreGive(http_buffer_mutex);
    return ret;
}

/* Handler to serve the HTML page */
static esp_err_t root_get_handler(httpd_req_t *req) {
    //ESP_LOGI(TAG, "root_get_handler");

    if (req == NULL) {
        ESP_LOGE(TAG, "Null request pointer");
        return ESP_FAIL;
    }

    bringup_notify_http_request();
    rtc_reset_shutdown_timer();   // serving the page is real activity → wake from soft idle

    HTTPD_RET_ON_ERR(httpd_resp_set_type(req, "text/html"),                 "set response type");
    HTTPD_RET_ON_ERR(httpd_resp_set_hdr(req, "Content-Encoding", "gzip"),   "set content encoding header");
    HTTPD_RET_ON_ERR(httpd_resp_set_hdr(req, "Connection", "close"),        "set connection header");
    HTTPD_RET_ON_ERR(httpd_resp_send(req, (const char *)html_content, html_content_len),
                                                                            "send HTML response");
    return ESP_OK;
}

// Cache the storage partition pointer to avoid repeated lookups
static const esp_partition_t *cached_log_partition = NULL;

// In webserver.c - Replace the log_handler function

static esp_err_t log_handler_locked(httpd_req_t *req);
static esp_err_t log_handler(httpd_req_t *req) {
    return with_http_buffer(req, log_handler_locked);
}
static esp_err_t log_handler_locked(httpd_req_t *req) {
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

    const esp_partition_t *log_part = cached_log_partition;
    if (log_part == NULL) {
        log_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_ANY,
                                            "log");
        if (log_part == NULL) {
            ESP_LOGE(TAG, "Log partition not found");
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                        "Log partition not found");
        }
        cached_log_partition = log_part;
    }

    int32_t head = log_get_head();
    int32_t log_start = log_get_offset();
    
    if (tail < 0) {
        tail = log_get_tail();
    } else {
        if (tail < log_start || tail >= (int32_t)log_part->size) {
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
        log_data_size = (log_part->size - tail) + (head - log_start);
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

    httpd_resp_set_hdr(req, "Connection", "close");

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
    
    // Send log data. Three cases:
    //   tail == head: log is empty, nothing more to stream.
    //   tail <  head: contiguous run [tail, head).
    //   tail >  head: wrapped — stream [tail, partition_end) then [log_start, head).
    if (tail < head) {
        err = stream_partition_range(req, log_part, tail, head);
        if (err != ESP_OK) return err;
    } else if (tail > head) {
        err = stream_partition_range(req, log_part, tail, (int32_t)log_part->size);
        if (err != ESP_OK) return err;
        err = stream_partition_range(req, log_part, log_start, head);
        if (err != ESP_OK) return err;
    }

    // Send empty chunk to signal end
    HTTPD_RET_ON_ERR(httpd_resp_send_chunk(req, NULL, 0), "send final chunk");
    return ESP_OK;
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

    httpd_resp_set_hdr(req, "Connection", "close");
    err = httpd_resp_send(req, json_str, strlen(json_str));
    free(json_str);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send response: %s", esp_err_to_name(err));
        return err;
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
static void hibernate_enter_cb(void *arg)    { hibernate_enter(); }
static void webserver_restart_wifi_cb(void *arg) { webserver_restart_wifi(); }

/**
 * Unified POST handler - handles commands, parameter updates, time updates
 */
static esp_err_t post_handler_locked(httpd_req_t *req);
static esp_err_t post_handler(httpd_req_t *req) {
    return with_http_buffer(req, post_handler_locked);
}
static esp_err_t post_handler_locked(httpd_req_t *req) {
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
    cJSON *reboot_flag         = cJSON_GetObjectItem(response, "reboot");
    cJSON *factory_reset_flag  = cJSON_GetObjectItem(response, "factory_reset");
    cJSON *sleep_flag          = cJSON_GetObjectItem(response, "sleep");
    cJSON *hibernate_flag      = cJSON_GetObjectItem(response, "hibernate");
    cJSON *wifi_restart_flag   = cJSON_GetObjectItem(response, "wifi_restart");
    bool should_reboot         = cJSON_IsTrue(reboot_flag);
    bool should_factory_reset  = cJSON_IsTrue(factory_reset_flag);
    bool should_sleep          = cJSON_IsTrue(sleep_flag);
    bool should_hibernate      = cJSON_IsTrue(hibernate_flag);
    bool should_restart_wifi   = cJSON_IsTrue(wifi_restart_flag);
    
    // Convert response to string
    char *json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    
    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                                    "Failed to serialize response");
    }
    
    // Send response — keep-alive intentional; held-button remote pulses reuse this connection
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

    if (should_factory_reset) {
        ESP_LOGW(TAG, "Factory reset in 2 seconds...");
        vTaskDelay(pdMS_TO_TICKS(2000));
        factory_reset();
        esp_restart();
        return ESP_OK;  // Never reached
    }

    /* All three of these have the same deadlock concern: calling
     * httpd_stop() (directly or via webserver_stop / soft_idle_enter /
     * hibernate_enter) from inside a handler blocks because httpd_stop()
     * waits for all handlers to drain. Defer to a one-shot timer so this
     * handler returns first and the httpd task becomes free. */
    if (should_restart_wifi) {
        static esp_timer_handle_t s_wifi_restart_timer = NULL;
        defer_call(&s_wifi_restart_timer, "wifi_restart", webserver_restart_wifi_cb, 500 * 1000);
        return ESP_OK;
    }
    if (should_sleep) {
        ESP_LOGI(TAG, "Entering soft idle in 2 seconds...");
        static esp_timer_handle_t s_sleep_timer = NULL;
        defer_call(&s_sleep_timer, "soft_idle", soft_idle_enter_cb, 2000 * 1000);
        return ESP_OK;
    }
    if (should_hibernate) {
        ESP_LOGI(TAG, "Hibernating in 2 seconds...");
        static esp_timer_handle_t s_hibernate_timer = NULL;
        defer_call(&s_hibernate_timer, "hibernate", hibernate_enter_cb, 2000 * 1000);
        return ESP_OK;
    }

    return err;
}

static esp_err_t ota_post_handler_locked(httpd_req_t *req);
static esp_err_t ota_post_handler(httpd_req_t *req) {
    return with_http_buffer(req, ota_post_handler_locked);
}
static esp_err_t ota_post_handler_locked(httpd_req_t *req) {
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
        
        // Log progress every 10%. Guard against total_len < 10 (would
        // otherwise divide by zero in the modulo) and total_len == 0
        // (chunked transfer with unknown length).
        if (total_len >= 10 && (received % (total_len / 10)) < recv_len) {
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


/* Captive-portal landing page. Deliberately tiny: it renders inside the OS
 * captive webview (Apple's Captive Network Assistant / Android's sign-in
 * sheet), which can't reliably run the full SPA's WebSocket status push or
 * OTA upload. So we don't serve the app here — we just confirm the connection
 * and point the user at the full UI in a real browser. Palette/title mirror
 * webpage.html (dark theme, tan accent, "Control Panel"). */
static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ClusterCluck Drive</title><style>"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#1a1a1a;color:#f0f0f0;"
    "margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;text-align:center}"
    ".c{padding:2rem;max-width:22rem}h1{font-size:1.4rem;margin:0 0 .4rem}"
    ".ok{color:#38B000;font-weight:600}"
    "a.b{display:inline-block;margin-top:1.6rem;padding:.9rem 1.7rem;background:#ba965b;"
    "color:#1a1a1a;font-weight:600;text-decoration:none;border-radius:8px;font-size:1.1rem}"
    "p{margin-top:1.6rem;font-size:.85rem;color:#a0a0a0;line-height:1.5}b{color:#f0f0f0}</style></head><body>"
    "<div class=\"c\"><h1>ClusterCluck Drive</h1><div class=\"ok\">Connected &#10003;</div>"
    "<a class=\"b\" href=\"http://192.168.4.1/\">Open Control Panel</a>"
    "<p>For live status &amp; firmware updates, open<br><b>http://sc.local</b> or "
    "<b>http://192.168.4.1</b> in your phone's browser.<br><br>"
    "On iPhone, tap <b>Cancel</b> &rarr; <b>Use Without Internet</b>, then open it in Safari.</p>"
    "</div></body></html>";

/* Serve the captive-portal landing page (HTTP 200). Returning real HTML —
 * rather than each OS's expected "success" token — is exactly what makes
 * iOS/Android/Windows decide the network is captive and pop their sign-in
 * sheet. (The previous version returned the success tokens here, which is
 * what suppressed the portal.) */
static esp_err_t send_portal(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Catch-all for every URL that isn't one of our registered endpoints. The
 * wildcard DNS server resolves all names to 192.168.4.1, so the OS
 * connectivity probes (Apple /hotspot-detect.html, Android /generate_204,
 * Windows /connecttest.txt, …) all land here and get the portal page, which
 * triggers the captive sheet. Stray asset requests (e.g. /favicon.ico) get it
 * too, harmlessly. Our real endpoints (/, /get, /post, /log, /ota, /ws) are
 * registered ahead of this wildcard, so they never reach here. */
static esp_err_t catchall_handler(httpd_req_t *req) {
    //ESP_LOGI(TAG, "catchall_handler; %s", req->uri);
    return send_portal(req);
}



/************************************************
**************** URI HANDLER MAP ****************
************************************************/

/* Real-time control + status channel. Defined further down (needs the
 * server_running / http_server_instance globals declared below). */
static esp_err_t ws_handler(httpd_req_t *req);

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
    /* WebSocket: must be registered before the wildcard catchall so the
     * handshake GET to /ws is matched here, not by catchall_handler. */
    .uri       = "/ws",
    .method    = HTTP_GET,
    .handler   = ws_handler,
    .user_ctx  = NULL,
    .is_websocket = true,
    /* Let httpd handle PING/PONG/CLOSE internally. A clean CLOSE still closes
     * the socket, which fires ws_close_fn -> stop_override(). */
    .handle_ws_control_frames = false
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

static esp_netif_t      *s_ap_netif      = NULL;
static bool              s_wifi_initted  = false;

/**********************************************************
**************** WEBSOCKET (REAL-TIME) ********************
**********************************************************/
/* The page opens a WebSocket to /ws for two things:
 *   - client -> server: low-latency remote-control commands (extend, retract,
 *     fwd, rev, aux, stop_override) sent as small JSON text frames. These are
 *     routed through comms_handle_post() so they share the POST command
 *     vocabulary exactly.
 *   - server -> client: a ~1 Hz status push (same JSON as /get) so the page
 *     no longer has to poll over HTTP.
 *
 * Safety: any WS socket close (tab closed, WiFi drop, crash) invokes
 * stop_override() via the httpd close callback, halting remote motion with no
 * dependence on the 350 ms pulse timeout. */

#define WS_MAX_CLIENTS 7   /* mirrors config.max_open_sockets */
#define WS_RX_MAX      256 /* remote-control JSON frames are tiny */

/* Non-blocking writability poll. A live browser drains a ~1 KB status frame
 * instantly, so a WS socket that isn't writable here is effectively dead — the
 * classic case being a phone that dropped off WiFi without sending a TCP FIN.
 * Sending to such a socket would block the httpd task for up to
 * send_wait_timeout (30 s), wedging the server so a reconnect can't even load
 * the page. We skip + reclaim those instead. */
static bool ws_sock_writable(int fd) {
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };  // poll, never block
    int r = select(fd + 1, NULL, &wfds, NULL, &tv);
    return r > 0 && FD_ISSET(fd, &wfds);
}

/* Build the status JSON and push it to every connected WS client. Runs on the
 * httpd task (queued via httpd_queue_work) so all WS sends happen in the
 * server's own context. Dead/stuck sockets are reclaimed rather than sent to,
 * so one vanished client can't block status (or new connections) for everyone. */
static void ws_broadcast_work(void *arg) {
    httpd_handle_t hd = http_server_instance;
    if (hd == NULL) return;

    size_t fds = WS_MAX_CLIENTS;
    int client_fds[WS_MAX_CLIENTS];
    if (httpd_get_client_list(hd, &fds, client_fds) != ESP_OK) return;

    /* Collect just the WS clients first so we skip building the (large) JSON
     * payload entirely when no browser is connected — zero heap churn idle. */
    int ws_fds[WS_MAX_CLIENTS];
    int n_ws = 0;
    for (size_t i = 0; i < fds; i++) {
        if (httpd_ws_get_fd_info(hd, client_fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
            ws_fds[n_ws++] = client_fds[i];
    }
    if (n_ws == 0) return;

    cJSON *root = comms_build_status();   /* does NOT reset shutdown timer */
    if (root == NULL) return;
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) return;

    httpd_ws_frame_t frame = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };
    int sent = 0;
    for (int i = 0; i < n_ws; i++) {
        int fd = ws_fds[i];
        if (!ws_sock_writable(fd)) {
            ESP_LOGW(TAG, "WS fd=%d not writable — reclaiming stale client", fd);
            httpd_sess_trigger_close(hd, fd);
            continue;
        }
        esp_err_t e = httpd_ws_send_frame_async(hd, fd, &frame);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "WS send fd=%d failed (%s) — reclaiming", fd, esp_err_to_name(e));
            httpd_sess_trigger_close(hd, fd);
        } else {
            sent++;
        }
    }
    free(json);

    /* A live, connected browser keeps the device awake — same intent as the
     * old 2 s poll resetting the shutdown timer. Only count genuinely-reachable
     * clients so a pile of stale sockets can't pin the device awake. */
    if (sent > 0) rtc_reset_shutdown_timer();
}

/* 1 Hz timer that queues a broadcast onto the httpd task. */
static esp_timer_handle_t s_ws_push_timer = NULL;
static void ws_push_timer_cb(void *arg) {
    if (server_running && http_server_instance)
        httpd_queue_work(http_server_instance, ws_broadcast_work, NULL);
}
static void ws_push_timer_start(void) {
    if (s_ws_push_timer == NULL) {
        esp_timer_create_args_t ta = { .callback = ws_push_timer_cb, .name = "ws_push" };
        if (esp_timer_create(&ta, &s_ws_push_timer) != ESP_OK) return;
    }
    esp_timer_start_periodic(s_ws_push_timer, 1000 * 1000);  // 1 Hz
}
static void ws_push_timer_stop(void) {
    if (s_ws_push_timer) esp_timer_stop(s_ws_push_timer);
}

/* WebSocket handler. The handshake arrives as HTTP_GET; subsequent frames
 * carry remote-control commands. */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        /* Bound how long a status push can block on this socket. httpd defaults
         * WS sockets to send_wait_timeout (30 s); if this client vanishes, a
         * push must not hang the shared httpd task that long. 2 s is ample for
         * a live client to drain ~1 KB. */
        struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ESP_LOGI(TAG, "WS connect, fd=%d", fd);
        rtc_reset_shutdown_timer();
        return ESP_OK;  // httpd completed the handshake
    }

    /* First recv with max_len=0 fills in frame.len/type without copying. */
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS recv (len query) failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGD(TAG, "WS frame type=%d len=%u fd=%d",
             frame.type, (unsigned)frame.len, httpd_req_to_sockfd(req));

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        stop_override();   // explicit close → halt remote motion
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0) return ESP_OK;
    if (frame.len >= WS_RX_MAX) {
        ESP_LOGW(TAG, "WS frame too large (%u bytes), dropping", (unsigned)frame.len);
        return ESP_OK;
    }

    uint8_t buf[WS_RX_MAX];
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, WS_RX_MAX);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS recv (payload) failed: %s", esp_err_to_name(ret));
        return ret;
    }
    buf[frame.len] = '\0';
    ESP_LOGD(TAG, "WS cmd: %s", (char *)buf);

    /* Route through the shared command dispatcher. Remote-control commands act
     * immediately (pulse_override); request/response commands like reboot just
     * set response flags we don't act on here — those stay on the HTTP path. */
    cJSON *root = cJSON_Parse((char *)buf);
    if (root) {
        cJSON *resp = NULL;
        comms_handle_post(root, &resp);
        cJSON_Delete(root);
        if (resp) cJSON_Delete(resp);
    } else {
        ESP_LOGW(TAG, "WS cmd parse failed");
    }
    return ESP_OK;
}

/* Global socket-close callback. Fires for every socket the server closes; we
 * only act on WS sockets so a stray HTTP socket close can't stop an active
 * remote hold. Must close the socket ourselves (we replaced the default). */
static void ws_close_fn(httpd_handle_t hd, int sockfd) {
    if (httpd_ws_get_fd_info(hd, sockfd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        ESP_LOGI(TAG, "WS disconnect, fd=%d — stop_override()", sockfd);
        stop_override();
    }
    close(sockfd);
}

static esp_err_t start_http_server(void) {
	if (server_running) return ESP_OK;
	ESP_LOGI(TAG, "STARTING HTTP");
    if (http_buffer_mutex == NULL) {
        http_buffer_mutex = xSemaphoreCreateMutex();
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;  // seconds (default 5)
    config.send_wait_timeout = 30;  // seconds (default 5) — log download needs headroom in STA mode
    config.uri_match_fn = httpd_uri_match_wildcard; // enable wildcarding
    config.close_fn = ws_close_fn;  // halt remote motion on any WS disconnect

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

    ws_push_timer_start();  // begin 1 Hz status push to WS clients

    return ESP_OK;
}

static esp_err_t stop_http_server(void) {
	if (!server_running) return ESP_OK;
	ESP_LOGI(TAG, "STOPPING HTTP");
    if (http_server_instance == NULL) {
        ESP_LOGW(TAG, "HTTP server not running");
        return ESP_ERR_INVALID_STATE;
    }

    ws_push_timer_stop();

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

/* Event handler for WiFi + IP events */

int n_connected = 0;

/* Signaled by the WiFi event task when the AP fully stops. Used by
 * webserver_restart_wifi() to deterministically wait for teardown to
 * complete instead of racing on a fixed delay. */
static SemaphoreHandle_t s_ap_stopped_sem = NULL;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_AP_STOP) {
            ESP_LOGI(TAG, "WIFI_EVENT_AP_STOP");
            if (s_ap_stopped_sem) xSemaphoreGive(s_ap_stopped_sem);
            return;
        }
        if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "Station connected, AID=%d", event->aid);
            rtc_reset_shutdown_timer();   // also wakes from soft idle if needed
            n_connected++;
            /* HTTP lifecycle is no longer tied to client count — the server
             * is started once in webserver_init() and stays up. Tying
             * start/stop to events created races where rapid connect/
             * disconnect bursts could call httpd_start twice or stop on
             * the wrong tick. */
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "Station disconnected, AID=%d", event->aid);
            if (n_connected > 0) n_connected--;
        }
    }
}

/* One-time WiFi driver + event system init (guarded by s_wifi_initted) */
static esp_err_t wifi_common_init(void) {
    if (s_wifi_initted) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WIFI_EVENT handler: %s", esp_err_to_name(err));
        return err;
    }

    if (s_ap_stopped_sem == NULL) {
        s_ap_stopped_sem = xSemaphoreCreateBinary();
    }

    s_wifi_initted = true;
    return ESP_OK;
}

static esp_err_t launch_soft_ap(void) {
    /* If WiFi is already up, don't re-issue set_mode/set_config/start —
     * those modify driver state on a running AP and can de-associate
     * existing clients. The caller (typically webserver_init or
     * BU.WIFI.START) just gets ESP_OK as if a fresh launch succeeded. */
    if (s_wifi_running) {
        ESP_LOGI(TAG, "AP already running — launch_soft_ap is a no-op");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "AP LAUNCHING");

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "Failed to create default WiFi AP interface");
            return ESP_FAIL;
        }
        esp_netif_set_hostname(s_ap_netif, HOSTNAME);
    }

    esp_err_t err = wifi_common_init();
    if (err != ESP_OK) return err;

    wifi_config_t wifi_config = {
        .ap = {
            .channel = get_param_value_t(PARAM_WIFI_CHANNEL).u16,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    char *ssid_str     = get_param_string(PARAM_WIFI_SSID);
    char *password_str = get_param_string(PARAM_WIFI_PASS);
    if (ssid_str == NULL || password_str == NULL) {
        ESP_LOGE(TAG, "Failed to get WiFi credentials from parameters");
        return ESP_FAIL;
    }

    memcpy(wifi_config.ap.ssid,     ssid_str,     MIN(strlen(ssid_str),     sizeof(wifi_config.ap.ssid)));
    memcpy(wifi_config.ap.password, password_str, MIN(strlen(password_str), sizeof(wifi_config.ap.password)));

    if (strlen(password_str) < 8) {
        ESP_LOGW(TAG, "Password too short, using open authentication");
        wifi_config.ap.password[0] = '\0';
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    if (wifi_config.ap.channel > 11 || wifi_config.ap.channel < 1) {
        ESP_LOGW(TAG, "Invalid WiFi channel %d, using default channel 6", wifi_config.ap.channel);
        wifi_config.ap.channel = 6;
    }

    /* Clamp explicitly: the param store currently caps SSIDs at 16 bytes so
     * the value is always within `wifi_config.ap.ssid` (32-byte) bounds, but
     * if the param size ever grows the WiFi driver would read past the
     * buffer. */
    {
        size_t len = strlen(ssid_str);
        if (len > sizeof(wifi_config.ap.ssid)) len = sizeof(wifi_config.ap.ssid);
        wifi_config.ap.ssid_len = (uint8_t)len;
    }

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_mode AP: %s", esp_err_to_name(err)); return err; }

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set_config AP: %s", esp_err_to_name(err)); return err; }

    err = esp_wifi_start();
    if (err != ESP_OK) { ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err)); return err; }

    s_wifi_running = true;

    err = simple_dns_server_start("192.168.4.1");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start DNS server: %s", esp_err_to_name(err));
        // Non-critical, continue
    }

    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize mDNS: %s", esp_err_to_name(err));
        // Non-critical, continue
    } else {
        mdns_hostname_set(HOSTNAME);
        mdns_instance_name_set("ClusterCommand");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    uint8_t *placeholder = (wifi_config.ap.authmode == WIFI_AUTH_OPEN)
                            ? (uint8_t *)"<open network>"
                            : wifi_config.ap.password;
    ESP_LOGI(TAG, "SoftAP ready. SSID: %s, Channel: %d, Password: %s",
             wifi_config.ap.ssid, wifi_config.ap.channel, placeholder);
    ESP_LOGI(TAG, "Access at: http://%s.local or http://192.168.4.1", HOSTNAME);
    if (comms_event_group) xEventGroupSetBits(comms_event_group, WIFI_READY_BIT);
    return ESP_OK;
}

/* SoftAP-only startup. STA mode was removed along with try_connect_sta()
 * — revisit if/when STA is reinstated (see git history for the previous
 * implementation). */
static esp_err_t start_wifi(bool reset_wdt) {
    (void)reset_wdt;
    return launch_soft_ap();
}

esp_err_t webserver_stop(void) {
    stop_http_server();
    if (s_wifi_running) {
        esp_wifi_stop();
        s_wifi_running = false;
    }
    if (comms_event_group) xEventGroupClearBits(comms_event_group, WIFI_READY_BIT);
    return ESP_OK;
}

/* Soft-idle entry: stop HTTP server but leave WiFi AP running so clients
 * can still associate and trigger auto-wake via WIFI_EVENT_AP_STACONNECTED. */
esp_err_t webserver_sleep(void) {
    stop_http_server();
    return ESP_OK;
}

/* Soft-idle exit: restart HTTP server (WiFi AP is already up). */
esp_err_t webserver_wake(void) {
    return start_http_server();
}

esp_err_t webserver_restart_wifi(void) {
    ESP_LOGI(TAG, "Restarting WiFi with updated params...");

    stop_http_server();
    if (s_wifi_running) {
        /* Drain any pre-existing token so xSemaphoreTake below blocks for a
         * fresh AP_STOP event rather than returning immediately on stale
         * state. */
        if (s_ap_stopped_sem) xSemaphoreTake(s_ap_stopped_sem, 0);
        esp_wifi_stop();
        s_wifi_running = false;
        /* Wait for the WiFi driver to finish tearing the AP down. The
         * 1-second deadline is generous; on healthy hardware the event
         * arrives within a few tens of ms. */
        if (s_ap_stopped_sem) {
            xSemaphoreTake(s_ap_stopped_sem, pdMS_TO_TICKS(1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    esp_err_t err = start_wifi(false);  // called from esp_timer task, not subscribed to WDT
    if (err != ESP_OK) return err;
    start_http_server();  // no-op if already running
    return ESP_OK;
}

esp_err_t webserver_init(void) {
    ESP_LOGI(TAG, "Initializing webserver...");

    esp_err_t err = start_wifi(true);  // called from app_main, which is subscribed to WDT
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(err));
        return err;
    }

    start_http_server();  // no-op if already running

    ESP_LOGI(TAG, "Webserver initialization complete");
    return ESP_OK;
}