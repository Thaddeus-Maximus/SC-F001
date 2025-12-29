/*  WiFi softAP Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_ota_ops.h"
#include "esp_timer.h"
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

static esp_err_t log_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "log_get_handler");

    const esp_partition_t *storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not found");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Storage partition not found");
    }
    
    int32_t start = get_log_tail();
    int32_t end   = get_log_head();
    int32_t total_size = end - start;
    int32_t offset = start;
    if (start >= end) {
		total_size = storage_partition->size - start + end - get_log_offset();
	}
	
	ESP_LOGI(TAG, "start/end: %ld/%ld -> %ld", (long)start, (long)end, (long)total_size);

    //size_t total_size = storage_partition->size;
    char len_str[16];
    sprintf(len_str, "%u", (unsigned)total_size);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"sc_storage.bin\"");
    httpd_resp_set_hdr(req, "Content-Length", len_str);
    
    if (start >= end) {
		ESP_LOGI(TAG, "STARTING");
	    while (offset < storage_partition->size) {
			// if wrapped around, just go from the start all the way to the end of storage
			// then set start = get_log_offset();
			// and continue
			size_t to_read = MIN(sizeof(httpBuffer), storage_partition->size - offset);
	        esp_err_t err = esp_partition_read(storage_partition, offset, httpBuffer, to_read);
	        if (err != ESP_OK) {
	            ESP_LOGE(TAG, "Failed to read partition: %s", esp_err_to_name(err));
	            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read storage");
	        }
			ESP_LOGI(TAG, "Sending %ld x %d", (long) offset, to_read);
	        if (httpd_resp_send_chunk(req, (const char *)httpBuffer, to_read) != ESP_OK) {
	            ESP_LOGE(TAG, "Failed to send chunk");
	            return ESP_FAIL;
	        }
	        offset += to_read;
		}
		offset = get_log_offset();
	}
	
	while (offset < end) {
		ESP_LOGI(TAG, "FINISHING");
		// if wrapped around, just go from the start all the way to the end of storage
		// then set start = get_log_offset();
		// and continue
		size_t to_read = MIN(sizeof(httpBuffer), end - offset);
        esp_err_t err = esp_partition_read(storage_partition, offset, httpBuffer, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read partition: %s", esp_err_to_name(err));
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read storage");
        }
		ESP_LOGI(TAG, "Sending %ld x %d", (long) offset, to_read);
        if (httpd_resp_send_chunk(req, (const char *)httpBuffer, to_read) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send chunk");
            return ESP_FAIL;
        }
        offset += to_read;
	}

    // End chunked transfer
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// set time: timestamp (unix epoch, seconds)
static esp_err_t st_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "st_post_handler");
    // Send the HTML response
    
	int ret=0;
	int remaining = req -> content_len;
		while (remaining > 0) {
			if ((ret = httpd_req_recv(req, httpBuffer, MIN(remaining, sizeof(httpBuffer))))<= 0) {
				if(ret == HTTPD_SOCK_ERR_TIMEOUT){
					continue;
				}
				return ESP_FAIL;
			}
			
		}
		
		ESP_LOGI(TAG, "ST POST %.*s", ret, httpBuffer);
    return httpd_resp_send(req, "200 OK", HTTPD_RESP_USE_STRLEN);
}

// set parameters id & value
static esp_err_t sp_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "sp_post_handler");
    // Send the HTML response
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "/sp NOT IMPLEMENTED", HTTPD_RESP_USE_STRLEN);
}


static esp_err_t move_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "move_post_handler");
    // Send the HTML response
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "/move NOT IMPLEMENTED", HTTPD_RESP_USE_STRLEN);
}



static esp_err_t stop_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "stop_post_handler");
    // Send the HTML response
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, "/stop NOT IMPLEMENTED", HTTPD_RESP_USE_STRLEN);
}


/* Handler for Status GET request*/
static esp_err_t status_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "status_get_handler");
    size_t head = 0;

    // Set the response type to JSON
    httpd_resp_set_type(req, "application/json");

    // Start building the JSON string with time
    head += sprintf(httpBuffer+head, "{\"time\":%lld,\"params\":[", system_rtc_get_raw_time());

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
    .method    = HTTP_GET,
    .handler   = log_get_handler,
    .user_ctx  = NULL
},{
    .uri       = "/sp",
    .method    = HTTP_POST,
    .handler   = sp_post_handler,
    .user_ctx  = NULL
},{
    .uri       = "/move",
    .method    = HTTP_POST,
    .handler   = move_post_handler,
    .user_ctx  = NULL
},{
    .uri       = "/stop",
    .method    = HTTP_POST,
    .handler   = stop_post_handler,
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