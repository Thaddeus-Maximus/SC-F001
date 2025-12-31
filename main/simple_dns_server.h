#ifndef SIMPLE_DNS_SERVER_H
#define SIMPLE_DNS_SERVER_H

#include "esp_err.h"

/**
 * @brief Start a simple DNS server that redirects all queries to the AP IP
 * 
 * @param ap_ip The IP address to return for all DNS queries (e.g., "192.168.4.1")
 * @return esp_err_t ESP_OK on success
 */
esp_err_t simple_dns_server_start(const char *ap_ip);

/**
 * @brief Stop the DNS server
 */
void simple_dns_server_stop(void);

#endif // SIMPLE_DNS_SERVER_H