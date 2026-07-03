/*
 * sc_errors.h
 *
 *  Created on: Jan 5, 2026
 *      Author: Thad
 */

#ifndef MAIN_SC_ERR_H_
#define MAIN_SC_ERR_H_

// from esp_err.h, from 0 -> 0x100 is clear
// as is 0x10D -> 0x3000

#define SC_ERR_EFUSE_TRIP_1		0x201
#define SC_ERR_EFUSE_TRIP_2		0x202
#define SC_ERR_EFUSE_TRIP_3		0x203

#define SC_ERR_SAFETY_TRIP		0x210

#define SC_ERR_TRAVEL_LIMIT		0x211  /* remaining travel distance exhausted (was SC_ERR_LEASH_HIT) */

#define SC_ERR_RTC_NOT_SET      0x220
#define SC_ERR_LOW_BATTERY      0x230

#include "esp_err.h"

/* Human-readable name for an SC_ERR_* code (or ESP_OK). Used by FSM log
 * messages and the web UI so error strings live in one place. Returns a
 * literal — no allocation, safe to use anywhere. */
const char *sc_err_str(esp_err_t e);


#endif /* MAIN_SC_ERR_H_ */
