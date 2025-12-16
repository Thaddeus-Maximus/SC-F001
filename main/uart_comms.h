/*
 * uart_comms.h
 *
 *  Created on: Dec 13, 2025
 *      Author: Thad
 */

#ifndef MAIN_UART_COMMS_H_
#define MAIN_UART_COMMS_H_

#include "esp_err.h"

esp_err_t uart_init();
esp_err_t uart_stop();

#endif /* MAIN_UART_COMMS_H_ */
