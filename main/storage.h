#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include "esp_err.h"

// ============================================================================
// FLASH LAYOUT CONSTANTS
// ============================================================================
#define FLASH_SECTOR_SIZE 4096
#define PARAMS_OFFSET 0
#define LOG_START_OFFSET 4096  // Start after first sector (parameters)

// ============================================================================
// LOG ENTRY TYPE DEFINITIONS (Magic values 0xC0-0xCF)
// ============================================================================
#define LOG_TYPE_DATA       0xC0  // Generic data log
#define LOG_TYPE_EVENT      0xC1  // Event marker
#define LOG_TYPE_ERROR      0xC2  // Error log
#define LOG_TYPE_DEBUG      0xC3  // Debug message
#define LOG_TYPE_SENSOR     0xC4  // Sensor reading
#define LOG_TYPE_COMMAND    0xC5  // Command executed
#define LOG_TYPE_STATUS     0xC6  // Status update
#define LOG_TYPE_CUSTOM_1   0xC7  // Custom type 1
#define LOG_TYPE_CUSTOM_2   0xC8  // Custom type 2
#define LOG_TYPE_CUSTOM_3   0xC9  // Custom type 3
// 0xCA-0xCF reserved for future use

// Maximum payload size per log entry (255 max due to 1-byte size field)
#define LOG_MAX_PAYLOAD 255

// Helper macro to check if a byte is a valid log type
#define IS_VALID_LOG_TYPE(x) ((x) >= 0xC0 && (x) <= 0xCF)

// ============================================================================
// LOG ENTRY STRUCTURE
// ============================================================================
// Variable-length log entry format:
// [0]:     Type/Magic (0xC0-0xCF)
// [1]:     Payload size (0-255 bytes)
// [2-N]:   Payload data
typedef struct {
    uint8_t type;           // Type/Magic byte (0xC0-0xCF range)
    uint8_t size;           // Payload size in bytes (0-255)
    uint8_t data[];         // Flexible array member for payload
} __attribute__((packed)) log_entry_header_t;

#define LOG_HEADER_SIZE (sizeof(log_entry_header_t))  // 2 bytes

// ============================================================================
// PARAMETER SYSTEM
// ============================================================================

#define PARAMETER_NUM_SECTORS 4

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
    PARAM_DEF(SAFETY_BREAK_US, u32, 200000, "") \

// Generate enum for parameter indices
#define PARAM_DEF(name, type, default_val, unit) PARAM_##name,
typedef enum {
    PARAM_LIST
    NUM_PARAMS
} param_idx_t;
#undef PARAM_DEF

#define PARAM_STR_SIZE 16

// Parameter value union (16 bytes max to fit in storage efficiently)
typedef union {
    uint16_t u16;
    int16_t i16;
    uint32_t u32;
    int32_t i32;
    float f32;
    double f64;
    char str[PARAM_STR_SIZE];
} param_value_t;

// Parameter types
typedef enum {
    PARAM_TYPE_u16,
    PARAM_TYPE_i16,
    PARAM_TYPE_u32,
    PARAM_TYPE_i32,
    PARAM_TYPE_f32,
    PARAM_TYPE_f64,
    PARAM_TYPE_str
} param_type_e;

// Stored parameter format (includes CRC)
typedef struct {
    uint8_t data[16];  // Raw parameter data
    uint32_t crc;      // CRC32 checksum
} __attribute__((packed)) param_stored_t;

#define PARAM_CRC_SALT 0x12345678

// ============================================================================
// FUNCTION DECLARATIONS
// ============================================================================

// Initialization
esp_err_t storage_init(void);
void storage_deinit(void);

// Parameter access
param_value_t get_param_value_t(param_idx_t id);
esp_err_t set_param_value_t(param_idx_t id, param_value_t val);
esp_err_t set_param_string(param_idx_t id, const char* str);
char* get_param_string(param_idx_t id);
param_type_e get_param_type(param_idx_t id);
const char* get_param_name(param_idx_t id);
param_value_t get_param_default(param_idx_t id);
const char* get_param_unit(param_idx_t id);
const char* get_param_json_string(param_idx_t id, char* buffer, size_t buf_size);

// Parameter commit to flash
esp_err_t commit_params(void);

// Logging functions
esp_err_t log_init(void);
esp_err_t write_log(uint8_t type, const uint8_t* data, uint8_t size);
uint32_t get_log_head(void);
uint32_t get_log_tail(void);
uint32_t get_log_offset(void);
uint32_t get_log_size(void);

esp_err_t factory_reset();

// Test/debug functions
esp_err_t write_dummy_log_1(void);
esp_err_t write_dummy_log_2(void);
esp_err_t write_dummy_log_3(void);

#endif // STORAGE_H