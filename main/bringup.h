/*
 * bringup.h — manufacturing / bench bring-up protocol over UART.
 *
 * See docs/SC-F001/BRINGUP.md for the line protocol and stage procedure.
 *
 * While bring-up mode is active:
 *   - uart_comms routes every input line to bringup_handle_line() instead of
 *     the JSON parser.
 *   - control_fsm pauses (stays in STATE_IDLE, skips relay writes).
 *   - BU.* commands drive the hardware directly via i2c_set_relays etc.
 *
 * Note: this is distinct from the per-module POST routines (adc_post,
 * i2c_post, storage_post) which run at normal boot and are genuinely
 * power-on self-tests.
 */

#ifndef MAIN_BRINGUP_H_
#define MAIN_BRINGUP_H_

#include <stdbool.h>

void bringup_mode_enter(void);
void bringup_mode_exit(void);
bool bringup_mode_is_active(void);

/* Called by uart_comms when a full line has been received in bring-up mode.
 * `line` is null-terminated, no trailing \r\n. Response is printed via
 * printf(); caller doesn't need to capture it. */
void bringup_handle_line(char *line);

/* Counted by webserver when the root page is served. BU.WIFI.WAIT uses it
 * to confirm that an associated client actually loaded the UI. */
void bringup_notify_http_request(void);

#endif /* MAIN_BRINGUP_H_ */
