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
//#include "mdns.h"
#include "webpage.h"

#include "esp_partition.h"


#define HOSTNAME "sc.local"
#define SOFT_AP_SSID "sc_main"
#define SOFT_AP_PASSWORD "stockcropper"
#define SERVER_PORT  80

static const char *TAG = "WEBSERVER";


static httpd_handle_t httpServerInstance = NULL;

char httpBuffer[1024];

/* Handler to serve the HTML page */
static esp_err_t root_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "root_get_handler");
    // Send the HTML response
    
    httpd_resp_set_type(req, "text/html"); // Original MIME type
	httpd_resp_set_hdr(req, "Content-Encoding", "gzip"); // Tell browser it's gzipped
	return httpd_resp_send(req, (const char *)html_content, html_content_len);
}

static esp_err_t log_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "log_handler");
    
    
	int32_t tail = -1;
    
    if (req -> method == HTTP_GET) {
		// give the whole log
	}
	if (req -> method == HTTP_POST) {
		int ret = httpd_req_recv(req, httpBuffer, sizeof(httpBuffer));
	    if (ret <= 0) {
	        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
	            httpd_resp_send_408(req);
	        }
	        return ESP_FAIL;
	    }
	    httpBuffer[ret] = '\0'; // Null-terminate the string
			
		ESP_LOGI(TAG, "ST POST %.*s", ret, httpBuffer);
		
		if(sscanf(httpBuffer, "%ld", (long*)&tail) != 1) {
			// if malformed, just send the whole log.
			//httpd_resp_send_err(req, 400, "INVALID TAIL POINTER");
		}
	}

    const esp_partition_t *storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not found");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage partition not found");
    }

    // Figure out the bounds of data
    if (tail < 0)
	    tail = get_log_tail();
    int32_t head = get_log_head();
    int32_t total_size = head - tail + 8; // 8 bytes for the head/tail pointers
    int32_t offset = tail;
    int32_t sent = 0;
    
    if (tail >= head) {
        total_size = storage_partition->size - tail + head - get_log_offset() + 8;
    }

    ESP_LOGI(TAG, "start/end: %ld/%ld -> %ld", (long)tail, (long)head, (long)total_size);

    // Send header
    char len_str[16];
    sprintf(len_str, "%u", (unsigned)total_size);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"sc_storage.bin\"");
    httpd_resp_set_hdr(req, "Content-Length", len_str);


	int32_t htail = htobe32(tail);
	int32_t hhead = htobe32(head);
    // Send head/tail pointers
    memcpy(&httpBuffer[0], &(htail), 4);
    memcpy(&httpBuffer[4], &(hhead), 4);
    if (httpd_resp_send_chunk(req, (const char *)httpBuffer, 8) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send chunk");
        return ESP_FAIL;
    }
    sent += 8;
    
    if (tail != head) {
	    // Send data between tail and end (if wrapped)
	    if (tail >= head) {
	        ESP_LOGI(TAG, "STARTING wrapped section (tail=%ld, head=%ld)", (long)tail, (long)head);
	        while (offset < storage_partition->size) {
	            // FIXED: Don't limit by head in this section - read to end of partition
	            size_t to_read = MIN(sizeof(httpBuffer), storage_partition->size - offset);
	            esp_err_t err = esp_partition_read(storage_partition, offset, httpBuffer, to_read);
	            if (err != ESP_OK) {
	                ESP_LOGE(TAG, "Failed to read partition: %s", esp_err_to_name(err));
	                return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read storage");
	            }
	            ESP_LOGI(TAG, "Sending wrapped chunk: offset=%ld size=%d", (long)offset, to_read);
	            if (httpd_resp_send_chunk(req, (const char *)httpBuffer, to_read) != ESP_OK) {
	                ESP_LOGE(TAG, "Failed to send chunk");
	                return ESP_FAIL;
	            }
	            sent += to_read;
	            offset += to_read;
	        }
	        // Loop back to the beginning
	        offset = get_log_offset();
	        ESP_LOGI(TAG, "Wrapped to beginning, offset now=%ld", (long)offset);
	    }
	
	    // Send data between start (or tail) and head
	    ESP_LOGI(TAG, "FINISHING final section (offset=%ld, head=%ld)", (long)offset, (long)head);
	    while (offset < head) {
	        size_t to_read = MIN(sizeof(httpBuffer), head - offset);
	        esp_err_t err = esp_partition_read(storage_partition, offset, httpBuffer, to_read);
	        if (err != ESP_OK) {
	            ESP_LOGE(TAG, "Failed to read partition: %s", esp_err_to_name(err));
	            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read storage");
	        }
	        ESP_LOGI(TAG, "Sending final chunk: offset=%ld size=%d", (long)offset, to_read);
	        if (httpd_resp_send_chunk(req, (const char *)httpBuffer, to_read) != ESP_OK) {
	            ESP_LOGE(TAG, "Failed to send chunk");
	            return ESP_FAIL;
	        }
	        sent += to_read;
	        offset += to_read;
	    }
	
	    ESP_LOGI(TAG, "Transfer complete: sent %ld bytes (expected %ld)", (long)sent, (long)total_size);
	
	    
    }
    
    // End chunked transfer
    if (httpd_resp_send_chunk(req, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send final empty chunk");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Final empty chunk sent successfully");
    return ESP_OK;
}

// set time: timestamp (unix epoch, seconds)
static esp_err_t st_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "st_post_handler");
    // Send the HTML response
    
	int ret = httpd_req_recv(req, httpBuffer, sizeof(httpBuffer));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    httpBuffer[ret] = '\0'; // Null-terminate the string
		
	ESP_LOGI(TAG, "ST POST %.*s", ret, httpBuffer);
	int64_t tv = -1;
	
	/*if(sscanf(httpBuffer, "%d-%d-%dT%d:%d", &tv) == 1) {
		system_rtc_set_raw_time(tv);
	}*/
	if(sscanf(httpBuffer, "%lld", &tv) == 1) {
		system_rtc_set_raw_time(tv);
	}
	
	
    return httpd_resp_send(req, "200 OK", HTTPD_RESP_USE_STRLEN);
}

// set parameters id & value
// set parameters - accepts multiple parameters with mixed key types
static esp_err_t sp_post_handler(httpd_req_t *req) {
    char content[512]; // Increased buffer for multiple parameters
    size_t recv_size = (req->content_len < sizeof(content)) ? req->content_len : sizeof(content) - 1;

    // 1. Receive the data
    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    content[ret] = '\0'; // Null-terminate the string

    // 2. Parse the JSON
    cJSON *root = cJSON_Parse(content);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", content);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    int params_updated = 0;
    int params_failed = 0;

    // 3. Iterate through all items in the JSON object
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        int param_id = -1;
        const char *key = item->string;
        
        if (key == NULL) {
            ESP_LOGW(TAG, "Skipping item with null key");
            params_failed++;
            continue;
        }

        // Try to parse key as a parameter ID (numeric string like "4")
        char *endptr;
        long parsed_id = strtol(key, &endptr, 10);
        if (*endptr == '\0' && parsed_id >= 0 && parsed_id < NUM_PARAMS) {
            // Key is a valid numeric string
            param_id = (int)parsed_id;
        } else {
            // Key is a string name, search for matching parameter
            for (uint8_t i = 0; i < NUM_PARAMS; i++) {
                if (strcmp(key, get_param_name(i)) == 0) {
                    param_id = i;
                    break;
                }
            }
        }

        // Check if we found a valid parameter
        if (param_id < 0 || param_id >= NUM_PARAMS) {
            ESP_LOGW(TAG, "Unknown parameter key: %s", key);
            params_failed++;
            continue;
        }

        // Get the value
        if (!cJSON_IsNumber(item)) {
            ESP_LOGW(TAG, "Parameter %s has non-numeric value", key);
            params_failed++;
            continue;
        }

        double param_val = item->valuedouble;
        
        ESP_LOGI(TAG, "Updating Param '%s' (ID: %d) to Value: %.2f", key, param_id, param_val);
        
        // Set the parameter based on its type
        switch(get_param_type(param_id)) { 
            case PARAM_TYPE_u8:
                set_param_value_t(param_id, (param_value_t){.u8 = round(param_val)});
                break;
            case PARAM_TYPE_i8:
                set_param_value_t(param_id, (param_value_t){.i8 = round(param_val)});
                break;
            case PARAM_TYPE_u16:
                set_param_value_t(param_id, (param_value_t){.u16 = round(param_val)});
                break;
            case PARAM_TYPE_i16:
                set_param_value_t(param_id, (param_value_t){.i16 = round(param_val)});
                break;
            case PARAM_TYPE_u32:
                set_param_value_t(param_id, (param_value_t){.u32 = round(param_val)});
                break;
            case PARAM_TYPE_i32:
                set_param_value_t(param_id, (param_value_t){.i32 = round(param_val)});
                break;
            case PARAM_TYPE_u64:
                set_param_value_t(param_id, (param_value_t){.u64 = round(param_val)});
                break;
            case PARAM_TYPE_i64:
                set_param_value_t(param_id, (param_value_t){.i64 = round(param_val)});
                break;
            case PARAM_TYPE_f32:
                set_param_value_t(param_id, (param_value_t){.f32 = param_val});
                break;
            case PARAM_TYPE_f64:
                set_param_value_t(param_id, (param_value_t){.f64 = param_val});
                break;
            default:
                ESP_LOGW(TAG, "Unknown parameter type for ID %d", param_id);
                params_failed++;
                continue;
        }
        
        params_updated++;
    }
    
    commit_params();

    cJSON_Delete(root);

    // 5. Send Success Response
   return httpd_resp_send(req, "200 OK", HTTPD_RESP_USE_STRLEN);
}



static esp_err_t cmd_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "cmd_post_handler");
    // Send the HTML response
	int ret = httpd_req_recv(req, httpBuffer, sizeof(httpBuffer));
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    httpBuffer[ret] = '\0'; // Null-terminate the string
		
	cJSON *root = cJSON_Parse(httpBuffer);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", httpBuffer);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");

	if (!cJSON_IsString(cmd) || cmd->valuestring == NULL)
		return httpd_resp_send(req, "400 Bad Request", HTTPD_RESP_USE_STRLEN);
	
	if (strcmp(cmd->valuestring, "stop") == 0) {
		fsm_request(FSM_CMD_STOP);
		ESP_LOGI(TAG, "FSM_CMD_STOP");
	} else if (strcmp(cmd->valuestring, "undo") == 0) {
		fsm_request(FSM_CMD_UNDO);
		ESP_LOGI(TAG, "FSM_CMD_UNDO");
	} else if (strcmp(cmd->valuestring, "start") == 0) {
		fsm_request(FSM_CMD_START);
		ESP_LOGI(TAG, "FSM_CMD_START");
	} else if (strcmp(cmd->valuestring, "rfp") == 0) {
		cJSON *i = cJSON_GetObjectItem(root, "channel");	
		if (cJSON_IsNumber(i) && i->valueint >= 0 && i->valueint < 8) {
			rf_433_learn_keycode(i->valueint);
		} else {
			rf_433_cancel_learn_keycode();
		}
	} else {
		ESP_LOGE(TAG, "Command not valid: %s", httpBuffer);
		return httpd_resp_send(req, "400 Bad Request", HTTPD_RESP_USE_STRLEN);
	}
	
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "200 OK", HTTPD_RESP_USE_STRLEN);

}

/* Handler for Status GET request*/
static esp_err_t status_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "status_get_handler");
    size_t head = 0;

    // Set the response type to JSON
    httpd_resp_set_type(req, "application/json");

    // Start building the JSON string with time
    head += sprintf(httpBuffer+head, "{\"time\":%lld,\"battery\":%f,\"rtc_set\":%s,\"values\":[",
    	system_rtc_get_raw_time(),
    	get_battery_V(),
    	rtc_is_set()?"true":"false"
    	);
    
    for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
        if (i > 0) {
            head += sprintf(httpBuffer+head, ",");
        }

        // Retrieve the parameter; assuming get_param(i) returns a param_t struct with union
        param_value_t param = get_param_value_t(i);

        // Append the parameter value based on its type
        switch (get_param_type(i)) {
			case PARAM_TYPE_u8:  head+=sprintf(httpBuffer+head, "%u", param.u8); break;
            case PARAM_TYPE_i8:  head+=sprintf(httpBuffer+head, "%d", param.i8); break;
            case PARAM_TYPE_u16: head+=sprintf(httpBuffer+head, "%u", param.u16); break;
            case PARAM_TYPE_i16: head+=sprintf(httpBuffer+head, "%d", param.i16); break;
            case PARAM_TYPE_u32: head+=sprintf(httpBuffer+head, "%lu", (unsigned long)param.u32); break;
            case PARAM_TYPE_i32: head+=sprintf(httpBuffer+head, "%ld", (long)param.i32); break;
            case PARAM_TYPE_u64: head+=sprintf(httpBuffer+head, "%llu", param.u64); break;
            case PARAM_TYPE_i64: head+=sprintf(httpBuffer+head, "%lld", param.i64); break;
            case PARAM_TYPE_f32: head+=sprintf(httpBuffer+head, "%.8f", param.f32); break;
            case PARAM_TYPE_f64: head+=sprintf(httpBuffer+head, "%.8f", param.f64); break;
        }
    }
    
    head += sprintf(httpBuffer+head, "], \"names\":[");
    for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
        if (i > 0) {
            head += sprintf(httpBuffer+head, ",");
        }
        head += sprintf(httpBuffer+head, "\"%s\"", get_param_name(i));
       }
       
    head += sprintf(httpBuffer+head, "], \"units\":[");
    for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
        if (i > 0) {
            head += sprintf(httpBuffer+head, ",");
        }
        head += sprintf(httpBuffer+head, "\"%s\"", get_param_unit(i));
       }

    // Close the JSON array and object
    head += sprintf(httpBuffer+head, "]}");

    return httpd_resp_send(req, httpBuffer, head);
}
static esp_err_t ota_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "OTA POST request received");

    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
    }

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    }

    char buf[1024];
    int recv_len;
    int total_len = req->content_len;
    int remaining = total_len;

    while (remaining > 0) {
        recv_len = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)));
        if (recv_len <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            esp_ota_abort(update_handle);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
        }

        err = esp_ota_write(update_handle, (const void *)buf, recv_len);
        if (err != ESP_OK) {
            esp_ota_abort(update_handle);
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
        }

        remaining -= recv_len;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
    }

    ESP_LOGI(TAG, "OTA update successful. Rebooting in 2 seconds...");
    
    // Send response FIRST
    httpd_resp_send(req, "OTA update successful, rebooting...", HTTPD_RESP_USE_STRLEN);
    
    set_param_value_t(PARAM_BOOT_TIME, (param_value_t){.i64 = system_rtc_get_raw_time()});
    
    // THEN delay and reboot
    vTaskDelay(pdMS_TO_TICKS(2000)); // Give time for TCP to close properly
    esp_restart();

    return ESP_OK;
}


httpd_uri_t uris[] = {{
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_get_handler,
    .user_ctx  = NULL
},{
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
},{
    .uri       = "/st",
    .method    = HTTP_ANY,
    .handler   = st_post_handler,
    .user_ctx  = NULL
},{
    .uri       = "/log",
    .method    = HTTP_ANY,
    .handler   = log_handler,
    .user_ctx  = NULL
},{
    .uri       = "/sp",
    .method    = HTTP_POST,
    .handler   = sp_post_handler,
    .user_ctx  = NULL
},{
    .uri       = "/cmd",
    .method    = HTTP_POST,
    .handler   = cmd_post_handler,
    .user_ctx  = NULL
},{
    .uri       = "/ota",
    .method    = HTTP_POST,
    .handler   = ota_post_handler,
    .user_ctx  = NULL
}};


static void startHttpServer(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SERVER_PORT;

    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&httpServerInstance, &config) == ESP_OK) {
		for (uint8_t i=0; i<(sizeof(uris)/sizeof(httpd_uri_t)); i++) {
        	httpd_register_uri_handler(httpServerInstance, &uris[i]);
			
		}
    }
}

/* Event handler for WiFi events */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
   if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Station connected.");
        //startHttpServer();
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Station disconnected.");
        //stopHttpServer();
    }
}

void launchSoftAp() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // Create default WiFi AP and get the netif handle
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);  // Optional: check for NULL

    // Set your custom hostname here (max 32 chars, no spaces/special chars recommended)
    ESP_ERROR_CHECK(esp_netif_set_hostname(ap_netif, HOSTNAME));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register the event handler
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
			.channel = 8,
            .ssid = SOFT_AP_SSID,
            .ssid_len = strlen(SOFT_AP_SSID),
            .password = SOFT_AP_PASSWORD,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    //dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE(HOSTNAME, "192.168.4.1");
	//ESP_ERROR_CHECK(dns_server_start(&dns_config));
    
    //ESP_ERROR_CHECK(mdns_init());
	//ESP_ERROR_CHECK(mdns_hostname_set(HOSTNAME));  // Matches the netif hostname
	//ESP_ERROR_CHECK(mdns_instance_name_set("My ESP32 Device"));  // Optional friendly name
	// After mdns_init() and hostname set
	//ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));

    ESP_LOGI(TAG, "SoftAP set up. SSID:%s password:%s", SOFT_AP_SSID, SOFT_AP_PASSWORD);
}

esp_err_t webserver_init(void) {
    launchSoftAp();
    startHttpServer();
    return ESP_OK;
}