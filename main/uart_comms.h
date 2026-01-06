#ifndef UART_COMMS_H
#define UART_COMMS_H

#include "esp_err.h"

/**
 * Initialize UART communication interface
 * Provides JSON-based GET/POST commands over serial
 * 
 * Commands:
 *   GET                    - Returns complete system status as pretty-printed JSON
 *   POST: {json data}      - Sends JSON command/parameter update
 */
esp_err_t uart_init(void);

/**
 * Stop UART communication interface
 */
esp_err_t uart_stop(void);

#endif // UART_COMMS_H