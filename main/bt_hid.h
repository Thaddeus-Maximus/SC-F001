#ifndef BT_HID_H
#define BT_HID_H

#include "esp_err.h"

/*
 * bt_hid.h
 *
 * BLE HID host module for SC-F001.
 *
 * Scans for and connects to any BLE device advertising the HID service UUID
 * (0x1812). If a previously bonded device address is stored in NVS it is
 * tried first; otherwise the strongest-RSSI device found during a scan is
 * used.
 *
 * Button mapping (hardcoded, no pairing config):
 *   VOL_UP   (0x00E9)  -> FSM_OVERRIDE_JACK_UP
 *   VOL_DOWN (0x00EA)  -> FSM_OVERRIDE_JACK_DOWN
 *   PREV     (0x00B6)  -> FSM_OVERRIDE_DRIVE_REV
 *   NEXT     (0x00B5)  -> FSM_OVERRIDE_DRIVE_FWD
 *
 * While a button is held the matching pulseOverride() is called every
 * BT_HID_REPEAT_MS milliseconds so that the FSM keeps re-arming the pulse.
 */

/* Repeat interval for held buttons (ms). Set to half the FSM poll period. */
#define BT_HID_REPEAT_MS 50

esp_err_t bt_hid_init(void);

#endif /* BT_HID_H */
