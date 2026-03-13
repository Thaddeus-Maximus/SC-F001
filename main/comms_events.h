#ifndef COMMS_EVENTS_H
#define COMMS_EVENTS_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Shared event group for WiFi/BT readiness signaling.
// Set by webserver.c and bt_hid.c; waited on by main.c during alarm wake.

#define WIFI_READY_BIT  BIT0   // Set when STA connected or softAP is up
#define BT_READY_BIT    BIT1   // Set when BT scan task starts
#define COMMS_ALL_BITS  (WIFI_READY_BIT | BT_READY_BIT)

// Must be created once (by main.c) before webserver_init() / bt_hid_init()
extern EventGroupHandle_t comms_event_group;

#endif // COMMS_EVENTS_H
