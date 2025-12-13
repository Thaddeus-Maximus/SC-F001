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
// Usage: PARAM_DEF(NAME, TYPE, DEFAULT_VALUE)
// 
// Examples:
//   PARAM_DEF(NUM_MOVES,    u32, 0)
//   PARAM_DEF(EFUSE_1_AS,   u16, 2400)
//   PARAM_DEF(JACK_DIST,    u8,  5)
//   PARAM_DEF(KEYCODE_0,    i64, -1)
//   PARAM_DEF(TEMPERATURE,  f32, 25.5)
// ============================================================================
// REMEMBER: ORDER IS IMPERATIVE! PARAMETERS ARE ENTERED IN THE TABLE BY INDEX!
// ============================================================================

#define PARAM_LIST \
    PARAM_DEF(NUM_MOVES,    u32, 0) \
    PARAM_DEF(MOVE_START,   u32, 0) \
    PARAM_DEF(MOVE_END,     u32, 0) \
    PARAM_DEF(EFUSE_1_A,    u32, 40) \
    PARAM_DEF(EFUSE_2_A,    u32, 10) \
    PARAM_DEF(EFUSE_3_A,    u32, 10) \
    PARAM_DEF(EFUSE_4_A,    u32, 10) \
    PARAM_DEF(EFUSE_1_AS,   u16, 2400) \
    PARAM_DEF(EFUSE_2_AS,   u16, 2400) \
    PARAM_DEF(EFUSE_3_AS,   u16, 600) \
    PARAM_DEF(EFUSE_4_AS,   u16, 600) \
    PARAM_DEF(DRIVE_DIST,   u16, 10) \
    PARAM_DEF(JACK_DIST,    u8,  5) \
    PARAM_DEF(DRIVE_TPDF,   u16, 4000) \
    PARAM_DEF(DRIVE_MSPF,   u16, 600) \
    PARAM_DEF(JACK_MSPI,    u16, 600) \
    PARAM_DEF(KEYCODE_0,    i64, -1) \
    PARAM_DEF(KEYCODE_1,    i64, -1) \
    PARAM_DEF(KEYCODE_2,    i64, -1) \
    PARAM_DEF(KEYCODE_3,    i64, -1)

// Generate enum for parameter indices
#define PARAM_DEF(name, type, default_val) PARAM_##name,
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

esp_err_t storage_init();
esp_err_t log_init();

param_value_t get_param(param_idx_t id);
esp_err_t set_param(param_idx_t id, param_value_t val);
param_type_e get_param_type(param_idx_t id);
const char* get_param_name(param_idx_t id);
param_value_t get_param_default(param_idx_t id);

esp_err_t commit_params();



#define LOG_ENTRY_SIZE 32
#define LOG_NUM_ENTRIES 512
#define FLASH_SECTOR_SIZE 4096

esp_err_t write_log(char* entry);

void storage_deinit();

#endif /* MAIN_STORAGE_H_ */