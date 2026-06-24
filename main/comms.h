#ifndef COMMS_H
#define COMMS_H

#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

/**
 * Unified communications module
 * 
 * This module provides the core GET/POST logic that can be called from
 * either the webserver (HTTP) or UART (serial) interfaces.
 */

/**
 * Process a GET request - returns complete system status as JSON.
 * Resets the inactivity/shutdown timer (a GET is a real client poll).
 *
 * @return cJSON object containing system status, or NULL on error
 *         Caller is responsible for deleting the returned object with cJSON_Delete()
 */
cJSON* comms_handle_get(void);

/**
 * Build the same system-status JSON as comms_handle_get() but WITHOUT
 * resetting the shutdown timer. Used by the periodic WebSocket status push
 * so an open-but-idle browser tab doesn't prevent the device from sleeping.
 *
 * @return cJSON object containing system status, or NULL on error
 *         Caller is responsible for deleting the returned object with cJSON_Delete()
 */
cJSON* comms_build_status(void);

/**
 * Process a POST request - handles commands, parameter updates, time updates
 * 
 * @param request_json The parsed JSON request object
 * @param response_json Pointer to store the response JSON object
 * @return ESP_OK on success, error code otherwise
 *         Caller is responsible for deleting the response_json with cJSON_Delete()
 */
esp_err_t comms_handle_post(cJSON *request_json, cJSON **response_json);


#endif // COMMS_H