#include "simple_dns_server.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "DNS_SERVER";
static int dns_socket = -1;
static TaskHandle_t dns_task_handle = NULL;
static char dns_ip[16] = {0};

// DNS header structure
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static void dns_server_task(void *pvParameters) {
    char rx_buffer[512];
    char tx_buffer[512];
    struct sockaddr_in dest_addr;
    
    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53);
    
    dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (dns_socket < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    
    int err = bind(dns_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(dns_socket);
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "DNS server started on port 53");
    
    while (1) {
        struct sockaddr_in source_addr;
        socklen_t socklen = sizeof(source_addr);
        
        int len = recvfrom(dns_socket, rx_buffer, sizeof(rx_buffer) - 1, 0,
                          (struct sockaddr *)&source_addr, &socklen);
        
        if (len < 0) {
            ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
            break;
        }
        
        if (len < sizeof(dns_header_t)) {
            continue;
        }
        
        // Parse DNS query
        dns_header_t *header = (dns_header_t *)rx_buffer;
        
        // Build DNS response
        memcpy(tx_buffer, rx_buffer, len);
        dns_header_t *response = (dns_header_t *)tx_buffer;
        
        // Set response flags
        response->flags = htons(0x8180); // Standard query response, no error
        response->ancount = htons(1);    // One answer
        
        // Add answer section (Type A record)
        int pos = len;
        
        // Name pointer to question
        tx_buffer[pos++] = 0xC0;
        tx_buffer[pos++] = 0x0C;
        
        // Type A
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x01;
        
        // Class IN
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x01;
        
        // TTL (60 seconds)
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x3C;
        
        // Data length (4 bytes for IPv4)
        tx_buffer[pos++] = 0x00;
        tx_buffer[pos++] = 0x04;
        
        // IP address
        int a, b, c, d;
        sscanf(dns_ip, "%d.%d.%d.%d", &a, &b, &c, &d);
        tx_buffer[pos++] = a;
        tx_buffer[pos++] = b;
        tx_buffer[pos++] = c;
        tx_buffer[pos++] = d;
        
        // Send response
        sendto(dns_socket, tx_buffer, pos, 0,
               (struct sockaddr *)&source_addr, sizeof(source_addr));
    }
    
    close(dns_socket);
    dns_socket = -1;
    vTaskDelete(NULL);
}

esp_err_t simple_dns_server_start(const char *ap_ip) {
    if (dns_task_handle != NULL) {
        ESP_LOGW(TAG, "DNS server already running");
        return ESP_ERR_INVALID_STATE;
    }
    
    strncpy(dns_ip, ap_ip, sizeof(dns_ip) - 1);
    
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 5, &dns_task_handle);
    
    return ESP_OK;
}

void simple_dns_server_stop(void) {
    if (dns_task_handle != NULL) {
        if (dns_socket >= 0) {
            close(dns_socket);
            dns_socket = -1;
        }
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
        ESP_LOGI(TAG, "DNS server stopped");
    }
}