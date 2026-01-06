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

#define SC_ERR_LEASH_HIT		0x211

#define SC_ERR_RTC_NOT_SET      0x220
#define SC_ERR_LOW_BATTERY      0x230


#endif /* MAIN_SC_ERR_H_ */
