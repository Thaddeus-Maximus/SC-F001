/*
 * storage.h
 *
 *  Created on: Nov 5, 2025
 *      Author: Thad
 */

#ifndef MAIN_STORAGE_H_
#define MAIN_STORAGE_H_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include "i2c.h"

typedef union {
    uint8_t  u8;
    int8_t   i8;
    uint16_t u16;
    int16_t  i16;
    uint32_t u32;
    int32_t  i32;
    uint64_t u64;
    int64_t  i64;
    float    f32;
    double   f64;
} param_value_t;

typedef struct {
    param_value_t val;
    uint32_t crc;
} param_stored_t;

typedef enum {
    PARAM_TYPE_u8,
    PARAM_TYPE_i8,
    PARAM_TYPE_u16,
    PARAM_TYPE_i16,
    PARAM_TYPE_u32,
    PARAM_TYPE_i32,
    PARAM_TYPE_u64,
    PARAM_TYPE_i64,
    PARAM_TYPE_f32,
    PARAM_TYPE_f64
} param_type_e;

// ============================================================================
// PARAMETER DEFINITION MACRO
// ============================================================================
// Usage: PARAM_DEF(NAME, TYPE, DEFAULT_VALUE, UNIT)
// 
// Examples:
//   PARAM_DEF(NUM_MOVES,    u32, 0,      "")
//   PARAM_DEF(EFUSE_1_AS,   u16, 2400,   "mA")
//   PARAM_DEF(JACK_DIST,    u8,  5,      "mm")
//   PARAM_DEF(KEYCODE_0,    i64, -1,     "")
//   PARAM_DEF(TEMPERATURE,  f32, 25.5,   "C")
// ============================================================================
// REMEMBER: ORDER IS IMPERATIVE! PARAMETERS ARE ENTERED IN THE TABLE BY INDEX!
// ============================================================================

#define PARAM_LIST \
    PARAM_DEF(BOOT_TIME,    i64, 0, "us") \
    PARAM_DEF(NUM_MOVES,    u32, 0, "") \
    PARAM_DEF(MOVE_START,   u32, 0, "s") \
    PARAM_DEF(MOVE_END,     u32, 0, "s") \
    PARAM_DEF(DRIVE_DIST,   u16, 10, "ft") /*4*/\
    PARAM_DEF(JACK_DIST,    u8,  5, "in") \
    PARAM_DEF(DRIVE_KE,     f32, 4000, "n/ft") \
    PARAM_DEF(DRIVE_KT,     f32, 600, "ms/ft") \
    PARAM_DEF(JACK_KT,      f32, 600, "ms/in") /*8*/\
    PARAM_DEF(KEYCODE_0,    i64, 0x19000000005D0C61, "") \
    PARAM_DEF(KEYCODE_1,    i64, 0x19000000005D0C62, "") \
    PARAM_DEF(KEYCODE_2,    i64, 0x19000000005D0C64, "") \
    PARAM_DEF(KEYCODE_3,    i64, 0x19000000005D0C68, "") /*12*/\
    PARAM_DEF(KEYCODE_4,    i64, -1, "") \
    PARAM_DEF(KEYCODE_5,    i64, -1, "") \
    PARAM_DEF(KEYCODE_6,    i64, -1, "") \
    PARAM_DEF(KEYCODE_7,    i64, -1, "") /*16*/\
    PARAM_DEF(ADC_ALPHA_BATTERY, f32, 0.02, "-") \
    PARAM_DEF(ADC_ALPHA_ISENS, f32, 0.02, "-") \
    PARAM_DEF(ADC_ALPHA_IAZ, f32, 0.005, "-") \
    PARAM_DEF(ADC_DB_IAZ, f32, 5.0, "A") /*20*/\
    PARAM_DEF(EFUSE_INOM_1, f32, 40.0, "A") \
    PARAM_DEF(EFUSE_INOM_2, f32, 6.0, "A") \
    PARAM_DEF(EFUSE_INOM_3, f32, 2.0, "A") \
    PARAM_DEF(EFUSE_HEAT_THRESH, f32, 60.0, "i/i^2-s") /*24*/\
    PARAM_DEF(EFUSE_KINST, f32, 4.0, "i/i") \
    PARAM_DEF(EFUSE_TAUCOOL, f32, 0.2, "i") \
    PARAM_DEF(EFUSE_TCOOL, i64, 5000000, "us") \
    PARAM_DEF(LOW_PROTECTION_V, f32, 10.0, "V") /*28*/\
    PARAM_DEF(LOW_PROTECTION_S, i64, 10, "s") \
    PARAM_DEF(CHG_LOW_V,  f32, 5.0, "V") \
    PARAM_DEF(CHG_LOW_S,  i64, 5.0, "s") \
    PARAM_DEF(CHG_BULK_S, i64, 20, "s") /*32*/\
    PARAM_DEF(RF_PULSE_LENGTH, u64, 350000, "us") \
    PARAM_DEF(V_SENS_OFFSET, f32, 0.4, "V") \
    
    
    

// Generate enum for parameter indices
#define PARAM_DEF(name, type, default_val, unit) PARAM_##name,
typedef enum {
    PARAM_LIST
    NUM_PARAMS
} param_idx_t;
#undef PARAM_DEF

#define PARAMS_SIZE sizeof(param_stored_t)
#define PARAMS_OFFSET 0
#define PARAMS_TOTAL_SIZE (NUM_PARAMS * PARAMS_SIZE)

// External declarations
extern param_value_t parameter_table[NUM_PARAMS];
extern const param_value_t parameter_defaults[NUM_PARAMS];
extern const param_type_e parameter_types[NUM_PARAMS];
extern const char* parameter_names[NUM_PARAMS];
extern const char parameter_units[NUM_PARAMS][8];

esp_err_t storage_init();
esp_err_t log_init();

param_value_t get_param_value_t(param_idx_t id);
esp_err_t set_param_value_t(param_idx_t id, param_value_t val);
param_type_e get_param_type(param_idx_t id);
const char* get_param_name(param_idx_t id);
param_value_t get_param_default(param_idx_t id);
const char* get_param_unit(param_idx_t id);

esp_err_t commit_params();

uint32_t get_log_head();
uint32_t get_log_tail();
uint32_t get_log_offset();

esp_err_t write_dummy_log_1();
esp_err_t write_dummy_log_2();
esp_err_t write_dummy_log_3();


#define LOG_ENTRY_SIZE 32
#define FLASH_SECTOR_SIZE 4096

// Calculate offset for log area (after parameters sector)
#define PARARMETER_NUM_SECTORS 4
#define LOG_START_OFFSET FLASH_SECTOR_SIZE*PARARMETER_NUM_SECTORS

esp_err_t write_log(char* entry);

void storage_deinit();

#endif /* MAIN_STORAGE_H_ */