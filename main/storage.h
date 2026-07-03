#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>
#include "esp_err.h"

// ============================================================================
// FLASH LAYOUT CONSTANTS
// ============================================================================
#define FLASH_SECTOR_SIZE 4096

/* LOG ENTRY TYPES.
 *
 * Two ranges are emitted by the firmware in practice:
 *   0..13     — FSM state-tagged data entries (see LOG_FSM_NAMES in
 *               webpage.html and send_fsm_log() in control_fsm.c).
 *   100..103  — System events: BAT, CRASH, BOOT, TIME_SET (see
 *               control_fsm.h LOG_TYPE_BAT/CRASH/BOOT/TIME_SET).
 *
 * Constants in the 0xC0-0xCF range are reserved for legacy/future use; the
 * webpage parser does not currently understand them.
 */
#define LOG_TYPE_DATA       0xC0  // reserved
#define LOG_TYPE_EVENT      0xC1
#define LOG_TYPE_ERROR      0xC2
#define LOG_TYPE_DEBUG      0xC3
#define LOG_TYPE_SENSOR     0xC4
#define LOG_TYPE_COMMAND    0xC5
#define LOG_TYPE_STATUS     0xC6
#define LOG_TYPE_CUSTOM_1   0xC7
#define LOG_TYPE_CUSTOM_2   0xC8
#define LOG_TYPE_CUSTOM_3   0xC9
// 0xCA-0xCF reserved for future use

// Maximum payload size per log entry (255 max due to 1-byte size field)
#define LOG_MAX_PAYLOAD 200

/* Helper macro to check if a byte is a valid log type. Includes the
 * 0..15 FSM-state range (must cover the highest fsm_state_t value — currently
 * STATE_MOVE_JACK_SETTLE = 15), the 100..103 system-event range, and the legacy
 * 0xC0-0xCF magic range. Used as a soft sanity check during log_read —
 * unknown types are still surfaced to callers, just with a warning. */
#define IS_VALID_LOG_TYPE(x) (((x) <= 15) || \
                              ((x) >= 100 && (x) <= 103) || \
                              ((x) >= 0xC0 && (x) <= 0xCF))

// ============================================================================
// LOG ENTRY STRUCTURE
// ============================================================================
// Variable-length log entry format:
// [0]:     Type/Magic (0xC0-0xCF)
// [1]:     Payload size (0-255 bytes)
// [2-N]:   Payload data
typedef struct {
    uint8_t size;           // Payload size in bytes (0-255)
    uint8_t data[];         // Flexible array member for payload
} __attribute__((packed)) log_entry_header_t;

#define LOG_HEADER_SIZE (sizeof(log_entry_header_t))  // 2 bytes

// ============================================================================
// PARAMETER SYSTEM
// ============================================================================


// PARAM_DEF(name, type, default, unit, min, max)
// min == max → skip bounds validation (used for keycodes, strings, informational params)
// Division-critical params have min > 0 to prevent div-by-zero
// NaN/Inf floats are always reset to default regardless of bounds
//
// *** FLASH LAYOUT WARNING ***
// Parameters are stored in flash indexed by their enum position (order in this list).
// NEVER insert a new parameter in the middle — it shifts all subsequent indices and
// corrupts every stored value after the insertion point.
// ALWAYS append new parameters at the very end of PARAM_LIST.

#define PARAM_LIST \
    PARAM_DEF(BOOT_TIME,         i32, 0,             "us",      0, 0) /* informational, skip */ \
    PARAM_DEF(NUM_MOVES,         u32, 0,             "",        0, 1000) \
    PARAM_DEF(MOVE_START,        u32, 0,             "s",       0, 86400) \
    PARAM_DEF(MOVE_END,          u32, 0,             "s",       0, 86400) \
    PARAM_DEF(DRIVE_DIST,        f32, 4,             "ft",      0.0, 100.0) \
    PARAM_DEF(JACK_DIST,         f32, 2.0,           "in",      0.0, 10.0) /* phase-2 lift after pre-jack */ \
    PARAM_DEF(DRIVE_KE,          f32, 29.2,          "n/ft",    1.0, 1e9) \
    PARAM_DEF(DRIVE_KT,          f32, 1440000,       "us/ft",   1.0, 1e9) /* div-critical */ \
    PARAM_DEF(JACK_KT,           f32, 1725698,       "ms/in",   1.0, 1e9) /* div-critical */ \
    PARAM_DEF(KEYCODE_0,         u32, 0,             "",        0, 0) /* skip */ \
    PARAM_DEF(KEYCODE_1,         u32, 0,             "",        0, 0) \
    PARAM_DEF(KEYCODE_2,         u32, 0,             "",        0, 0) \
    PARAM_DEF(KEYCODE_3,         u32, 0,             "",        0, 0) \
    PARAM_DEF(KEYCODE_4,         u32, 0,             "",        0, 0) \
    PARAM_DEF(KEYCODE_5,         u32, 0,             "",        0, 0) \
    PARAM_DEF(KEYCODE_6,         u32, 0,             "",        0, 0) \
    PARAM_DEF(KEYCODE_7,         u32, 0,             "",        0, 0) \
    PARAM_DEF(ADC_ALPHA_BATTERY, f32, 0.5,           "-",       0.0, 1.0) \
    PARAM_DEF(ADC_ALPHA_ISENS,   f32, 0.6,           "-",       0.0, 1.0) \
    PARAM_DEF(ADC_ALPHA_IAZ,     f32, 0.005,         "-",       0.0, 1.0) \
    PARAM_DEF(ADC_DB_IAZ,        f32, 5.0,           "A",       0.0, 200.0) \
    PARAM_DEF(EFUSE_INOM_1,      f32, 40.0,          "A",       0.0, 200.0) \
    PARAM_DEF(EFUSE_INOM_2,      f32, 14.0,          "A",       0.0, 200.0) \
    PARAM_DEF(EFUSE_INOM_3,      f32, 4.0,           "A",       0.0, 200.0) \
    PARAM_DEF(EFUSE_HEAT_THRESH, f32, 60.0,          "i/i^2-s", 0.0, 1e9) \
    PARAM_DEF(EFUSE_KINST,       f32, 2.0,           "i/i",     0.01, 100.0) /* div-critical */ \
    PARAM_DEF(EFUSE_TAUCOOL,     f32, 0.2,           "i",       0.0, 100.0) \
    PARAM_DEF(EFUSE_TCOOL,       u32, 5000000,       "us",      0, 60000000) \
    PARAM_DEF(LOW_PROTECTION_V,  f32, 10.0,          "V",       0.0, 100.0) \
    PARAM_DEF(LOW_PROTECTION_S,  u32, 10,            "s",       0, 3600) \
    PARAM_DEF(CHG_LOW_V,         f32, 5.0,           "V",       0.0, 100.0) \
    PARAM_DEF(CHG_LOW_S,         u32, 5,             "s",       0, 3600) \
    PARAM_DEF(CHG_BULK_S,        u32, 20,            "s",       0, 3600) \
    PARAM_DEF(RF_PULSE_LENGTH,   u32, 350000,        "us",      0, 10000000) \
    PARAM_DEF(V_SENS_OFFSET,     f32, 0.4,           "V",       -10.0, 10.0) \
    PARAM_DEF(NET_SSID,          str, "",             "",        "", "") \
    PARAM_DEF(NET_PASS,          str, "",             "",        "", "") \
    PARAM_DEF(WIFI_CHANNEL,      u16, 6,             "",        1, 14) \
    PARAM_DEF(WIFI_SSID,         str, "StockCropper","",        "", "") \
    PARAM_DEF(WIFI_PASS,         str, "password",    "",        "", "") \
    PARAM_DEF(EFUSE_INRUSH_US,   u32, 250000,        "us",      0, 10000000) \
    PARAM_DEF(JACK_I_UP,         f32, 8.0,           "A",       0.0, 200.0) \
    PARAM_DEF(JACK_I_DOWN,       f32, 1.6,           "A",       0.0, 200.0) \
    PARAM_DEF(V_SENS_K,          f32, 0.00766666666, "V/mV",    0.0, 1.0) \
    PARAM_DEF(BUILD_VERSION,     str, "undefined",   "",        "", "") \
    PARAM_DEF(SAFETY_BREAK_US,   u32, 300000,        "",        0, 10000000) \
    PARAM_DEF(SAFETY_MAKE_US,    u32, 1000000,       "",        0, 10000000) \
    PARAM_DEF(JACK_IS_DOWN,      f32, 1.6,           "A",       0.0, 200.0) /* deprecated: may duplicate JACK_I_DOWN */ \
    PARAM_DEF(FLUFF_PREDRIVE_MS, u32, 2000,          "ms",      0, 60000) \
    PARAM_DEF(INACTIVITY_TIMEOUT_S, u32, 300,        "s",       10, 86400) \
    /* Tabular schedule: up to 12 daily move times (seconds since local midnight). \
     * A value of -1 marks the slot as disabled — the default. Slots are sorted \
     * by commit_params() so non-negative values appear first in ascending order, \
     * with -1 entries pushed to the end. Range allows the validator to clamp \
     * out-of-band negatives (e.g. -2) back to -1 = disabled. Appended at the \
     * end of PARAM_LIST so existing flash layout / CRC offsets stay stable. \
     * MOVE_START / MOVE_END / NUM_MOVES above are deprecated; the scheduler \
     * no longer reads them and the web UI no longer surfaces them. */ \
    PARAM_DEF(MOVE_TIME_00,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_01,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_02,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_03,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_04,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_05,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_06,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_07,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_08,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_09,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_10,      i32, -1,            "s",       -1, 86399) \
    PARAM_DEF(MOVE_TIME_11,      i32, -1,            "s",       -1, 86399) \
    /* Appended at end to avoid shifting existing flash layout: */ \
    PARAM_DEF(JACK_MAX,          f32, 5.0,            "in",      0.0, 10.0) \
    /* Deadman for web/WebSocket jog only (RF/BT keep RF_PULSE_LENGTH). Longer \
     * than RF's because a browser jog rides a TCP WebSocket, which stalls on \
     * head-of-line blocking during Wi-Fi loss (worst near running motors). \
     * Release + tab-close stop instantly via reliable paths; this only bounds \
     * continued motion on a silent link blackout. Default 1 s. */ \
    PARAM_DEF(WEB_PULSE_LENGTH,  u32, 1000000,        "us",      0, 10000000) \
    /* Two-phase jack extension. Phase 1 ("pre-jack") raises JACK_PRE_DIST inches \
     * OR until the jack up-current threshold (JACK_I_UP) trips, whichever first — \
     * i.e. raise until the jack engages the load. Phase 2 then lifts an additional \
     * JACK_DIST inches. JACK_PRE_DIST + JACK_I_UP are both exposed in the web \
     * DANGER ZONE. Appended at end to keep flash layout stable. */ \
    PARAM_DEF(JACK_PRE_DIST,     f32, 4.0,            "in",      0.0, 10.0)

/* Tabular schedule width. The enum entries above must remain contiguous so
 * PARAM_MOVE_TIME_00 + i indexes slot i. */
#define NUM_MOVE_TIMES 12

// Generate enum for parameter indices
#define PARAM_DEF(name, type, default_val, unit, min, max) PARAM_##name,
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
esp_err_t storage_post(void);
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
double param_to_double(param_idx_t id);  // numeric params only — see storage.c

// Parameter commit to flash
esp_err_t commit_params(void);

/* In-place sort of MOVE_TIME_0..MOVE_TIME_(NUM_MOVE_TIMES-1) so that
 * non-negative entries come first in ascending order, with -1 (disabled)
 * entries pushed to the end. Called automatically by commit_params() —
 * exposed separately for tests / migrations. */
void sort_move_schedule(void);

// Logging functions
esp_err_t log_init(void);
esp_err_t log_write(const uint8_t* buf, uint8_t len, uint8_t type);
uint32_t log_get_head(void);
uint32_t log_get_tail(void);
uint32_t log_get_offset(void);
uint32_t log_get_size(void);

esp_err_t factory_reset();

// Hardware identity (NVS, survives factory reset)
uint16_t hw_get_board_rev(void);
esp_err_t hw_set_board_rev(uint16_t rev);

// Test/debug functions
esp_err_t write_dummy_log_1(void);
esp_err_t write_dummy_log_2(void);
esp_err_t write_dummy_log_3(void);

esp_err_t log_read(uint8_t* len, uint8_t* buf, uint8_t* type);
esp_err_t log_erase_all_sectors(void);
esp_err_t log_simulate_power_cycle(void);
void log_read_reset(void);
esp_err_t log_write_blocking_test(const uint8_t* buf, uint8_t len, uint8_t type);
#endif // STORAGE_H