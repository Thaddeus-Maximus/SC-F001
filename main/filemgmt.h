/*
 * filemgmt.h
 *
 *  Created on: Nov 20, 2025
 *      Author: Thad
 */

#ifndef MAIN_FILEMGMT_H_
#define MAIN_FILEMGMT_H_

#include "esp_err.h"


#define ESP_WIFI_AP_SSID "stockcropper"
#define ESP_WIFI_AP_PASSWORD ""
#define ESP_WIFI_AP_N_CONNECTIONS 4

// Open the filesystem (e.g. for logging)
esp_err_t start_filesystem();

// Start WiFi AP and FTP Server
esp_err_t start_ftp_server();

void stop_filesystem();
void stop_ftp_server();

void file_log();

void close_current_log();

void start_new_log_file();

#endif /* MAIN_FILEMGMT_H_ */
