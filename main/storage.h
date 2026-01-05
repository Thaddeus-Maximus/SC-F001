/*
 * storage.h - Simple variable-size parameter storage with per-param CRC
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

#define PARAM_CRC_SALT 0xDEADBEEF  // Salt to prevent all-zero CRC collision

// Union for parameter values - now sized appropriately
typedef union {
    uint16_t u16;
    int16_t  i16;
    uint32_t u32;
    int32_t  i32;
    float    f32;
    double   f64;
    char     str[16];  // 15 chars + null terminator
} param_value_t;

// Enum for parameter types
typedef enum {
    PARAM_TYPE_u16 = 0,
    PARAM_TYPE_i16 = 1,
    PARAM_TYPE_u32 = 2,
    PARAM_TYPE_i32 = 3,
    PARAM_TYPE_f32 = 6,
    PARAM_TYPE_f64 = 7,
    PARAM_TYPE_str = 8
} param_type_e;

// Storage format: each param stored as [data][crc32]
typedef struct __attribute__((packed)) {
    uint8_t data[16];   // Max size needed (for strings)
    uint32_t crc;       // CRC of actual data bytes used
} param_stored_t;

// Get storage size for a given type (data only, not including CRC)
static inline uint8_t param_type_size(param_type_e type) {
    switch(type) {
        case PARAM_TYPE_u16:
        case PARAM_TYPE_i16: return 2;
        case PARAM_TYPE_u32:
        case PARAM_TYPE_i32:
        case PARAM_TYPE_f32: return 4;
        case PARAM_TYPE_f64: return 8;
        case PARAM_TYPE_str: return 16;
        default: return 8;  // Fallback
    }
}

// ============================================================================
// PARAMETER DEFINITION MACRO
// ============================================================================
// Usage: PARAM_DEF(NAME, TYPE, DEFAULT_VALUE, UNIT)
// 
// Examples:
//   PARAM_DEF(NUM_MOVES,    u32, 0,      "")
//   PARAM_DEF(EFUSE_1_AS,   u16, 2400,   "mA")
//   PARAM_DEF(KEYCODE_0,    i64, -1,     "")
//   PARAM_DEF(TEMPERATURE,  f32, 25.5,   "C")
//   PARAM_DEF(DEVICE_NAME,  str, "ESP32", "")
// ============================================================================
// REMEMBER: ORDER IS IMPERATIVE! PARAMETERS ARE ENTERED IN THE TABLE BY INDEX!
// ============================================================================

#define PARAM_LIST \
    PARAM_DEF(BOOT_TIME,    i32, 0, "us") \
    PARAM_DEF(NUM_MOVES,    u32, 0, "") \
    PARAM_DEF(MOVE_START,   u32, 0, "s") \
    PARAM_DEF(MOVE_END,     u32, 0, "s") \
    PARAM_DEF(DRIVE_DIST,   f32, 10, "ft") \
    PARAM_DEF(JACK_DIST,    f32,  5, "in") \
    PARAM_DEF(DRIVE_KE,     f32, 29.2, "n/ft") \
    PARAM_DEF(DRIVE_KT,     f32, 2880000, "us/ft") \
    PARAM_DEF(JACK_KT,      f32, 1428571, "ms/in") \
    PARAM_DEF(KEYCODE_0,    u32, 0, "") \
    PARAM_DEF(KEYCODE_1,    u32, 0, "") \
    PARAM_DEF(KEYCODE_2,    u32, 0, "") \
    PARAM_DEF(KEYCODE_3,    u32, 0, "") \
    PARAM_DEF(KEYCODE_4,    u32, 0, "") \
    PARAM_DEF(KEYCODE_5,    u32, 0, "") \
    PARAM_DEF(KEYCODE_6,    u32, 0, "") \
    PARAM_DEF(KEYCODE_7,    u32, 0, "") \
    PARAM_DEF(ADC_ALPHA_BATTERY, f32, 0.5, "-") \
    PARAM_DEF(ADC_ALPHA_ISENS, f32, 0.6, "-") \
    PARAM_DEF(ADC_ALPHA_IAZ, f32, 0.005, "-") \
    PARAM_DEF(ADC_DB_IAZ, f32, 5.0, "A") \
    PARAM_DEF(EFUSE_INOM_1, f32, 40.0, "A") \
    PARAM_DEF(EFUSE_INOM_2, f32, 6.0, "A") \
    PARAM_DEF(EFUSE_INOM_3, f32, 4.0, "A") \
    PARAM_DEF(EFUSE_HEAT_THRESH, f32, 60.0, "i/i^2-s") \
    PARAM_DEF(EFUSE_KINST, f32, 5.0, "i/i") \
    PARAM_DEF(EFUSE_TAUCOOL, f32, 0.2, "i") \
    PARAM_DEF(EFUSE_TCOOL, u32, 5000000, "us") \
    PARAM_DEF(LOW_PROTECTION_V, f32, 10.0, "V") \
    PARAM_DEF(LOW_PROTECTION_S, u32, 10, "s") \
    PARAM_DEF(CHG_LOW_V,  f32, 5.0, "V") \
    PARAM_DEF(CHG_LOW_S,  u32, 5, "s") \
    PARAM_DEF(CHG_BULK_S, u32, 20, "s") \
    PARAM_DEF(RF_PULSE_LENGTH, u32, 350000, "us") \
    PARAM_DEF(V_SENS_OFFSET, f32, 0.4, "V") \
    PARAM_DEF(WIFI_CHANNEL, u16, 6, "") \
    PARAM_DEF(WIFI_SSID, str, "sc.local", "") \
    PARAM_DEF(WIFI_PASS, str, "password", "") \
    PARAM_DEF(EFUSE_INRUSH_US, u32, 300000, "us") \
    PARAM_DEF(JACK_I_UP,   f32, 5.0, "A") \
    PARAM_DEF(JACK_I_DOWN, f32, 8.0, "A") \
    PARAM_DEF(V_SENS_K, f32, 0.00766666666, "V/mV") \
    PARAM_DEF(BUILD_VERSION, str, "undefined", "") \
    

// Generate enum for parameter indices
#define PARAM_DEF(name, type, default_val, unit) PARAM_##name,
typedef enum {
    PARAM_LIST
    NUM_PARAMS
} param_idx_t;
#undef PARAM_DEF

#define FLASH_SECTOR_SIZE 4096
#define PARAMS_OFFSET 0
#define PARAMETER_NUM_SECTORS 4
#define LOG_START_OFFSET (FLASH_SECTOR_SIZE * PARAMETER_NUM_SECTORS)

// External declarations
extern param_value_t parameter_table[NUM_PARAMS];
extern const param_value_t parameter_defaults[NUM_PARAMS];
extern const param_type_e parameter_types[NUM_PARAMS];
extern const char* parameter_names[NUM_PARAMS];
extern const char parameter_units[NUM_PARAMS][8];

// Core functions
esp_err_t storage_init(void);
esp_err_t log_init(void);
void storage_deinit(void);

// Parameter access functions
param_value_t get_param_value_t(param_idx_t id);
esp_err_t set_param_value_t(param_idx_t id, param_value_t val);
param_type_e get_param_type(param_idx_t id);
const char* get_param_name(param_idx_t id);
param_value_t get_param_default(param_idx_t id);
const char* get_param_unit(param_idx_t id);
const char* get_param_json_string(param_idx_t id, char* buffer, size_t buf_size);

// Helper functions for string parameters
esp_err_t set_param_string(param_idx_t id, const char* str);
char* get_param_string(param_idx_t id);

// Storage operations
esp_err_t commit_params(void);
esp_err_t factory_reset(void);

// Log functions
#define LOG_ENTRY_SIZE 32

uint32_t get_log_head(void);
uint32_t get_log_tail(void);
uint32_t get_log_offset(void);
esp_err_t write_log(char* entry);

// Test functions
esp_err_t write_dummy_log_1(void);
esp_err_t write_dummy_log_2(void);
esp_err_t write_dummy_log_3(void);

#endif /* MAIN_STORAGE_H_ */