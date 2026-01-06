#include <math.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "version.h"

#define TAG "STORAGE"

// ============================================================================
// LOG TYPE DEFINITIONS (Magic values 0xC0-0xCF)
// ============================================================================
#define LOG_TYPE_DATA       0xC0  // Generic data log
#define LOG_TYPE_EVENT      0xC1  // Event marker
#define LOG_TYPE_ERROR      0xC2  // Error log
#define LOG_TYPE_DEBUG      0xC3  // Debug message
#define LOG_TYPE_SENSOR     0xC4  // Sensor reading
#define LOG_TYPE_COMMAND    0xC5  // Command executed
#define LOG_TYPE_STATUS     0xC6  // Status update
#define LOG_TYPE_PADDING    0xCE  // Padding to sector boundary
#define LOG_TYPE_CUSTOM     0xCF  // Custom/user-defined

// Helper macro to check if a byte is a valid log type
#define IS_VALID_LOG_TYPE(x) ((x) >= 0xC0 && (x) <= 0xCF)

// Maximum payload size per log entry (255 max due to 1-byte size field)
#define LOG_MAX_PAYLOAD 255

// ============================================================================
// PARAMETER TABLE GENERATION
// ============================================================================

// Helper macros to construct initializers
#define PARAM_VALUE_INIT(type, val) {.type = val}
#define PARAM_TYPE_ENUM(type) PARAM_TYPE_##type
#define PARAM_NAME_STR(name) #name

// Generate parameter table with live values (initialized to defaults)
#define PARAM_DEF(name, type, default_val, unit) PARAM_VALUE_INIT(type, default_val),
param_value_t parameter_table[NUM_PARAMS] = {
    PARAM_LIST
};
#undef PARAM_DEF

// Generate default values array
#define PARAM_DEF(name, type, default_val, unit) PARAM_VALUE_INIT(type, default_val),
const param_value_t parameter_defaults[NUM_PARAMS] = {
    PARAM_LIST
};
#undef PARAM_DEF

// Generate parameter types array
#define PARAM_DEF(name, type, default_val, unit) PARAM_TYPE_ENUM(type),
const param_type_e parameter_types[NUM_PARAMS] = {
    PARAM_LIST
};
#undef PARAM_DEF

// Generate parameter names array
#define PARAM_DEF(name, type, default_val, unit) PARAM_NAME_STR(name),
const char* parameter_names[NUM_PARAMS] = {
    PARAM_LIST
};
#undef PARAM_DEF

// Generate parameter units array (8 chars max per unit)
#define PARAM_DEF(name, type, default_val, unit) unit,
const char parameter_units[NUM_PARAMS][8] = {
    PARAM_LIST
};
#undef PARAM_DEF

size_t param_type_size(param_type_e x) {
	switch(x) {
		case PARAM_TYPE_u16: return 2;
    	case PARAM_TYPE_i16: return 2;
    	case PARAM_TYPE_u32: return 4;
    	case PARAM_TYPE_i32: return 4;
    	case PARAM_TYPE_f32: return 4;
    	case PARAM_TYPE_f64: return 8;
    	case PARAM_TYPE_str: return PARAM_STR_SIZE;
	}
	return -1;
}

// Partition pointer
static const esp_partition_t *storage_partition = NULL;

// Log head/tail tracking with mutex protection
// These now track byte offsets within the log area, not entry indices
static uint32_t log_head_offset = 0;  // Offset from LOG_START_OFFSET
static uint32_t log_tail_offset = 0;  // Offset from LOG_START_OFFSET
static SemaphoreHandle_t log_mutex = NULL;
static bool log_initialized = false;

uint32_t get_log_head(void) { 
    uint32_t head;
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    head = LOG_START_OFFSET + log_head_offset;
    if (log_mutex) xSemaphoreGive(log_mutex);
    return head;
}

uint32_t get_log_tail(void) { 
    uint32_t tail;
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    tail = LOG_START_OFFSET + log_tail_offset;
    if (log_mutex) xSemaphoreGive(log_mutex);
    return tail;
}

uint32_t get_log_offset(void) { 
    return LOG_START_OFFSET; 
}

// ============================================================================
// PARAMETER FUNCTIONS
// ============================================================================

param_value_t get_param_value_t(param_idx_t id) {
    if (id >= NUM_PARAMS) {
        ESP_LOGE(TAG, "Invalid parameter ID: %d", id);
        param_value_t err = {0};
        return err;
    }
    return parameter_table[id];
}

esp_err_t set_param_value_t(param_idx_t id, param_value_t val) {
    if (id >= NUM_PARAMS) {
        ESP_LOGE(TAG, "Invalid parameter ID: %d", id);
        return ESP_ERR_INVALID_ARG;
    }
    parameter_table[id] = val;
    ESP_LOGI(TAG, "Parameter %d (%s) set (not committed)", id, parameter_names[id]);
    return ESP_OK;
}

esp_err_t set_param_string(param_idx_t id, const char* str) {
    if (id >= NUM_PARAMS) {
        ESP_LOGE(TAG, "Invalid parameter ID: %d", id);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (parameter_types[id] != PARAM_TYPE_str) {
        ESP_LOGE(TAG, "Parameter %d (%s) is not a string type", id, parameter_names[id]);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (str == NULL) {
        parameter_table[id].str[0] = '\0';
    } else {
        strncpy(parameter_table[id].str, str, 15);
        parameter_table[id].str[15] = '\0';  // Ensure null termination
    }
    
    ESP_LOGI(TAG, "String parameter %d (%s) set to '%s' (not committed)", 
             id, parameter_names[id], parameter_table[id].str);
    return ESP_OK;
}

char* get_param_string(param_idx_t id) {
    if (id >= NUM_PARAMS) {
        ESP_LOGE(TAG, "Invalid parameter ID: %d", id);
        return "";
    }
    
    if (parameter_types[id] != PARAM_TYPE_str) {
        ESP_LOGE(TAG, "Parameter %d (%s) is not a string type", id, parameter_names[id]);
        return "";
    }
    
    return parameter_table[id].str;
}

param_type_e get_param_type(param_idx_t id) {
    if (id >= NUM_PARAMS) {
        return PARAM_TYPE_f64; // Default fallback
    }
    return parameter_types[id];
}

// ============================================================================
// JSON-FRIENDLY STRING CONVERSION
// ============================================================================
const char* get_param_json_string(param_idx_t id, char* buffer, size_t buf_size) {
    if (id >= NUM_PARAMS || buffer == NULL || buf_size == 0) {
        if (buffer && buf_size > 0) buffer[0] = '\0';
        return "";
    }
    
    param_type_e type = parameter_types[id];
    param_value_t val = parameter_table[id];
    
    switch(type) {
        case PARAM_TYPE_u16:
            snprintf(buffer, buf_size, "%u", val.u16);
            break;
        case PARAM_TYPE_i16:
            snprintf(buffer, buf_size, "%d", val.i16);
            break;
        case PARAM_TYPE_u32:
            snprintf(buffer, buf_size, "%lu", (unsigned long)val.u32);
            break;
        case PARAM_TYPE_i32:
            snprintf(buffer, buf_size, "%ld", (long)val.i32);
            break;
        case PARAM_TYPE_f32:
            if (isnan(val.f32) || isinf(val.f32)) {
                snprintf(buffer, buf_size, "null");
            } else {
                snprintf(buffer, buf_size, "%.6g", val.f32);
            }
            break;
        case PARAM_TYPE_f64:
            if (isnan(val.f64) || isinf(val.f64)) {
                snprintf(buffer, buf_size, "null");
            } else {
                snprintf(buffer, buf_size, "%.15g", val.f64);
            }
            break;
        case PARAM_TYPE_str:
            // Escape quotes and backslashes for JSON string
            snprintf(buffer, buf_size, "\"%s\"", val.str);
            break;
        default:
            snprintf(buffer, buf_size, "null");
            break;
    }
    
    return buffer;
}

const char* get_param_name(param_idx_t id) {
    if (id >= NUM_PARAMS) {
        return "INVALID";
    }
    return parameter_names[id];
}

param_value_t get_param_default(param_idx_t id) {
    if (id >= NUM_PARAMS) {
        param_value_t err = {0};
        return err;
    }
    return parameter_defaults[id];
}

const char* get_param_unit(param_idx_t id) {
    if (id >= NUM_PARAMS) {
        return "";
    }
    return parameter_units[id];
}

// ============================================================================
// STORAGE HELPER: Pack parameter value into buffer
// ============================================================================
static void pack_param(uint8_t *dest, param_idx_t id) {
    param_type_e type = parameter_types[id];
    
    switch(type) {
        case PARAM_TYPE_u16:
            memcpy(dest, &parameter_table[id].u16, 2);
            break;
        case PARAM_TYPE_i16:
            memcpy(dest, &parameter_table[id].i16, 2);
            break;
        case PARAM_TYPE_u32:
            memcpy(dest, &parameter_table[id].u32, 4);
            break;
        case PARAM_TYPE_i32:
            memcpy(dest, &parameter_table[id].i32, 4);
            break;
        case PARAM_TYPE_f32:
            memcpy(dest, &parameter_table[id].f32, 4);
            break;
        case PARAM_TYPE_f64:
            memcpy(dest, &parameter_table[id].f64, 8);
            break;
        case PARAM_TYPE_str:
            memcpy(dest, parameter_table[id].str, 16);
            break;
        default:
            memset(dest, 0, 16);
            break;
    }
}

// ============================================================================
// STORAGE HELPER: Unpack parameter value from buffer
// ============================================================================
static void unpack_param(const uint8_t *src, param_idx_t id) {
    param_type_e type = parameter_types[id];
    
    switch(type) {
        case PARAM_TYPE_u16:
            memcpy(&parameter_table[id].u16, src, 2);
            break;
        case PARAM_TYPE_i16:
            memcpy(&parameter_table[id].i16, src, 2);
            break;
        case PARAM_TYPE_u32:
            memcpy(&parameter_table[id].u32, src, 4);
            break;
        case PARAM_TYPE_i32:
            memcpy(&parameter_table[id].i32, src, 4);
            break;
        case PARAM_TYPE_f32:
            memcpy(&parameter_table[id].f32, src, 4);
            break;
        case PARAM_TYPE_f64:
            memcpy(&parameter_table[id].f64, src, 8);
            break;
        case PARAM_TYPE_str:
            memcpy(parameter_table[id].str, src, 16);
            parameter_table[id].str[15] = '\0';  // Ensure null termination
            break;
        default:
            break;
    }
}

// ============================================================================
// COMMIT PARAMETERS TO FLASH
// ============================================================================
esp_err_t commit_params(void) {
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Committing %d parameters to flash...", NUM_PARAMS);
    
    // Erase parameter sectors first
    esp_err_t err = esp_partition_erase_range(storage_partition, PARAMS_OFFSET, 
                                               PARAMETER_NUM_SECTORS * FLASH_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase parameter sectors: %s", esp_err_to_name(err));
        return err;
    }
    
    // Write each parameter with CRC
    uint32_t flash_offset = PARAMS_OFFSET;
    for (int i = 0; i < NUM_PARAMS; i++) {
        param_stored_t stored;
        memset(&stored, 0, sizeof(param_stored_t));
        
        // Pack parameter data
        pack_param(stored.data, i);
        
        // Calculate CRC over actual data size
        uint8_t size = param_type_size(parameter_types[i]);
        uint32_t crc_input = PARAM_CRC_SALT;
        stored.crc = esp_crc32_le(crc_input, stored.data, size);
        
        // Write to flash
        err = esp_partition_write(storage_partition, flash_offset, 
                                   &stored, sizeof(param_stored_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write parameter %d (%s): %s", 
                     i, parameter_names[i], esp_err_to_name(err));
            return err;
        }
        
        flash_offset += sizeof(param_stored_t);
    }
    
    ESP_LOGI(TAG, "Successfully committed all parameters to flash");
    return ESP_OK;
}

// ============================================================================
// FACTORY RESET
// ============================================================================
esp_err_t factory_reset(void) {
    ESP_LOGI(TAG, "Performing factory reset...");
    
    // Reset all parameters to defaults
    for (int i = 0; i < NUM_PARAMS; i++) {
        memcpy(&parameter_table[i], &parameter_defaults[i], sizeof(param_value_t));
    }
    
    // Commit defaults to flash
    esp_err_t err = commit_params();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit defaults during factory reset");
        return err;
    }
    
    ESP_LOGI(TAG, "Factory reset complete");
    return ESP_OK;
}

// ============================================================================
// STORAGE INITIALIZATION
// ============================================================================
esp_err_t storage_init(void) {
    ESP_LOGI(TAG, "Initializing storage system...");
    
    storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 
                                                  ESP_PARTITION_SUBTYPE_ANY, 
                                                  "storage");
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Storage partition found: size=%lu bytes", 
             (unsigned long)storage_partition->size);
    
    // Load parameters from flash
    uint32_t flash_offset = PARAMS_OFFSET;
    bool all_valid = true;
    
    for (int i = 0; i < NUM_PARAMS; i++) {
        param_stored_t stored;
        
        esp_err_t err = esp_partition_read(storage_partition, flash_offset, 
                                           &stored, sizeof(param_stored_t));
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read parameter %d (%s), using default", 
                     i, parameter_names[i]);
            memcpy(&parameter_table[i], &parameter_defaults[i], sizeof(param_value_t));
            all_valid = false;
            flash_offset += sizeof(param_stored_t);
            continue;
        }
        
        // Validate CRC over actual data size
        uint8_t size = param_type_size(parameter_types[i]);
        uint32_t crc_input = PARAM_CRC_SALT;
        uint32_t calculated_crc = esp_crc32_le(crc_input, stored.data, size);
        
        if (calculated_crc == stored.crc) {
            unpack_param(stored.data, i);
        } else {
            ESP_LOGW(TAG, "Parameter %d (%s) failed CRC check, using default", 
                     i, parameter_names[i]);
            memcpy(&parameter_table[i], &parameter_defaults[i], sizeof(param_value_t));
            all_valid = false;
        }
        
        flash_offset += sizeof(param_stored_t);
    }
    
    if (all_valid) {
        ESP_LOGI(TAG, "All parameters loaded successfully from flash");
    } else {
        ESP_LOGW(TAG, "Some parameters failed validation, using defaults");
    }
    
    return ESP_OK;
}

// ============================================================================
// VARIABLE-LENGTH LOGGING FUNCTIONS
// ============================================================================

/**
 * Find the first valid log entry by scanning for magic bytes.
 * Returns absolute flash offset, or -1 if no valid entry found.
 */
/*static int32_t find_first_valid_entry(uint32_t start_offset, uint32_t end_offset) {
    if (storage_partition == NULL) {
        return -1;
    }
    
    uint8_t buffer[256];
    uint32_t scan_pos = start_offset;
    
    while (scan_pos < end_offset) {
        size_t chunk_size = (end_offset - scan_pos) < sizeof(buffer) ? 
                            (end_offset - scan_pos) : sizeof(buffer);
        
        esp_err_t err = esp_partition_read(storage_partition, scan_pos, buffer, chunk_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read during scan at offset %lu", (unsigned long)scan_pos);
            return -1;
        }
        
        // Scan for valid type byte
        for (size_t i = 0; i < chunk_size; i++) {
            if (IS_VALID_LOG_TYPE(buffer[i])) {
                // Found potential entry - verify we can read size byte
                if (i + 1 < chunk_size) {
                    // Size byte is in buffer
                    return scan_pos + i;
                } else if (scan_pos + i + 1 < end_offset) {
                    // Size byte is in next read
                    return scan_pos + i;
                }
            }
        }
        
        // Move to next chunk, with 1-byte overlap to catch split entries
        scan_pos += chunk_size - 1;
    }
    
    return -1;
}*/

/**
 * Initialize the log system by finding head position
 */
esp_err_t log_init(void) {
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not initialized, call storage_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    
    log_mutex = xSemaphoreCreateMutex();
    if (log_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create log mutex");
        return ESP_ERR_NO_MEM;
    }
    
    //uint32_t log_area_size = storage_partition->size - LOG_START_OFFSET;
    uint32_t log_area_end = storage_partition->size;
    
    // Scan for first empty (0xFF) byte to find head
    uint8_t buffer[256];
    bool found_head = false;
    
    for (uint32_t offset = LOG_START_OFFSET; offset < log_area_end; offset += sizeof(buffer)) {
        size_t to_read = (log_area_end - offset) < sizeof(buffer) ? 
                         (log_area_end - offset) : sizeof(buffer);
        
        esp_err_t err = esp_partition_read(storage_partition, offset, buffer, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read during log init at offset %lu", (unsigned long)offset);
            vSemaphoreDelete(log_mutex);
            log_mutex = NULL;
            return err;
        }
        
        // Look for first 0xFF byte (empty flash)
        for (size_t i = 0; i < to_read; i++) {
            if (buffer[i] == 0xFF) {
                log_head_offset = (offset + i) - LOG_START_OFFSET;
                found_head = true;
                ESP_LOGI(TAG, "Log head found at offset %lu (absolute: %lu)", 
                         (unsigned long)log_head_offset, 
                         (unsigned long)(LOG_START_OFFSET + log_head_offset));
                break;
            }
        }
        
        if (found_head) break;
    }
    
    if (!found_head) {
        // Log is completely full, wrap to beginning
        log_head_offset = 0;
        ESP_LOGI(TAG, "Log is full, wrapping to beginning");
        
        // Erase first sector
        esp_err_t err = esp_partition_erase_range(storage_partition, LOG_START_OFFSET, 
                                                   FLASH_SECTOR_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase first log sector");
            vSemaphoreDelete(log_mutex);
            log_mutex = NULL;
            return err;
        }
    }
    
    // Set tail to start of log area initially (will be updated as sectors are erased)
    log_tail_offset = 0;
    
    log_initialized = true;
    ESP_LOGI(TAG, "Log system initialized. Head offset: %lu, Tail offset: %lu", 
             (unsigned long)log_head_offset, (unsigned long)log_tail_offset);
    
    return ESP_OK;
}

/**
 * Write a variable-length log entry
 * @param type Log entry type (0xC0-0xCF range)
 * @param data Payload data pointer
 * @param size Payload size in bytes (0-255)
 */
/*esp_err_t write_log(char* entry) {
    // Legacy interface for compatibility - treat as raw 32-byte entry
    // Extract type and size from first two bytes if they look valid
    uint8_t type = (uint8_t)entry[0];
    uint8_t size = (uint8_t)entry[1];
    
    if (!IS_VALID_LOG_TYPE(type)) {
        // Old format - use as LOG_TYPE_DATA with 30 bytes
        type = LOG_TYPE_DATA;
        size = 30;  // Assume old 32-byte format minus 2-byte header
    }
    
    return write_log(type, (const uint8_t*)&entry[2], size);
}*/

/**
 * Write a variable-length log entry (new interface)
 */
esp_err_t write_log(uint8_t type, const uint8_t* data, uint8_t size) {
    if (!log_initialized || storage_partition == NULL) {
        ESP_LOGE(TAG, "Logging not initialized");
        return ESP_FAIL;
    }
    
    if (!IS_VALID_LOG_TYPE(type)) {
        ESP_LOGE(TAG, "Invalid log type: 0x%02X", type);
        return ESP_ERR_INVALID_ARG;
    }
    
    /*if (size > LOG_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Log payload too large: %d bytes (max %d)", size, LOG_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }*/
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    uint32_t log_area_size = storage_partition->size - LOG_START_OFFSET;
    uint32_t log_area_end = storage_partition->size;
    uint32_t entry_total_size = LOG_HEADER_SIZE + size;  // 2 + payload
    
    // Calculate absolute offsets
    uint32_t abs_head = LOG_START_OFFSET + log_head_offset;
    
    // Check if entry would cross sector boundary
    uint32_t current_sector = abs_head / FLASH_SECTOR_SIZE;
    uint32_t entry_end = abs_head + entry_total_size;
    uint32_t end_sector = entry_end / FLASH_SECTOR_SIZE;
    
    if (end_sector != current_sector) {
        // Entry would cross sector boundary - write padding
        uint32_t bytes_to_sector_end = FLASH_SECTOR_SIZE - (abs_head % FLASH_SECTOR_SIZE);
        
        ESP_LOGI(TAG, "Entry would cross sector boundary, padding %lu bytes", 
                 (unsigned long)bytes_to_sector_end);
        
        // Write padding entry (type + size = 0)
        uint8_t pad_entry[2] = {LOG_TYPE_PADDING, 0x00};
        esp_err_t err = esp_partition_write(storage_partition, abs_head, pad_entry, 2);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write padding: %s", esp_err_to_name(err));
            if (log_mutex) xSemaphoreGive(log_mutex);
            return ESP_FAIL;
        }
        
        // Advance head to next sector boundary
        log_head_offset += bytes_to_sector_end;
        
        // Handle wrap-around
        if (log_head_offset >= log_area_size) {
            log_head_offset = 0;
        }
        
        // Recalculate abs_head for actual entry write
        abs_head = LOG_START_OFFSET + log_head_offset;
        current_sector = abs_head / FLASH_SECTOR_SIZE;
    }
    
    // Check if we need to erase the next sector before writing
    uint32_t next_offset = abs_head + entry_total_size;
    if (next_offset >= log_area_end) {
        next_offset = LOG_START_OFFSET;  // Wrap
    }
    
    uint32_t next_sector = next_offset / FLASH_SECTOR_SIZE;
    
    if (next_sector != current_sector) {
        // Next write will be in a new sector - check if it needs erasing
        uint8_t check_byte;
        esp_err_t err = esp_partition_read(storage_partition, next_sector * FLASH_SECTOR_SIZE, 
                                            &check_byte, 1);
        
        if (err == ESP_OK && check_byte != 0xFF) {
            ESP_LOGI(TAG, "Erasing sector %lu for log", (unsigned long)next_sector);
            err = esp_partition_erase_range(storage_partition, 
                                             next_sector * FLASH_SECTOR_SIZE, 
                                             FLASH_SECTOR_SIZE);
            
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase sector: %s", esp_err_to_name(err));
                if (log_mutex) xSemaphoreGive(log_mutex);
                return ESP_FAIL;
            }
            
            // Update tail - it's now at the start of the sector we just erased
            uint32_t new_tail_abs = next_sector * FLASH_SECTOR_SIZE;
            if (new_tail_abs < LOG_START_OFFSET) {
                new_tail_abs = LOG_START_OFFSET;
            }
            log_tail_offset = new_tail_abs - LOG_START_OFFSET;
            
            ESP_LOGI(TAG, "Tail/Head are now %lu/%lu", 
                     (unsigned long)log_tail_offset, (unsigned long)log_head_offset);
        }
    }
    
    // Write the actual log entry header + payload
    uint8_t header[LOG_HEADER_SIZE] = {type, size};
    
    esp_err_t err = esp_partition_write(storage_partition, abs_head, header, LOG_HEADER_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write log header: %s", esp_err_to_name(err));
        if (log_mutex) xSemaphoreGive(log_mutex);
        return ESP_FAIL;
    }
    
    if (size > 0) {
        err = esp_partition_write(storage_partition, abs_head + LOG_HEADER_SIZE, data, size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write log payload: %s", esp_err_to_name(err));
            if (log_mutex) xSemaphoreGive(log_mutex);
            return ESP_FAIL;
        }
    }
    
    // Advance head
    log_head_offset += entry_total_size;
    if (log_head_offset >= log_area_size) {
        log_head_offset -= log_area_size;  // Wrap around
    }
    
    if (log_mutex) xSemaphoreGive(log_mutex);
    return ESP_OK;
}

// ============================================================================
// TEST FUNCTIONS FOR VARIABLE-LENGTH LOGS
// ============================================================================

esp_err_t write_dummy_log_1(void) {
    ESP_LOGI(TAG, "Writing dummy variable-length log pattern 1");
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_head_offset = 0;
    log_tail_offset = 0;
    if (log_mutex) xSemaphoreGive(log_mutex);
    
    // Write varied-size entries
    for (uint32_t i = 0; i < 100; i++) {
        uint8_t size = (i % 3 == 0) ? 16 : (i % 3 == 1) ? 48 : 32;
        uint8_t data[256];
        
        // Fill with pattern
        for (uint8_t j = 0; j < size; j++) {
            data[j] = (i + j) & 0xFF;
        }
        
        uint8_t type = LOG_TYPE_DATA + (i % 4);  // Vary types
        write_log(type, data, size);
        
        if (i % 10 == 0) {
            ESP_LOGI(TAG, "Wrote entry %lu (type=0x%02X, size=%d)", 
                     (unsigned long)i, type, size);
        }
    }
    
    return ESP_OK;
}

esp_err_t write_dummy_log_2(void) {
    ESP_LOGI(TAG, "Writing dummy variable-length log pattern 2");
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_head_offset = 1000;
    log_tail_offset = 5000;
    if (log_mutex) xSemaphoreGive(log_mutex);
    
    for (uint32_t i = 0; i < 50; i++) {
        uint8_t size = 10 + (i * 3) % 200;  // Varied sizes
        uint8_t data[256];
        memset(data, 0xAA + i, size);
        
        write_log(LOG_TYPE_SENSOR, data, size);
    }
    
    return ESP_OK;
}

esp_err_t write_dummy_log_3(void) {
    ESP_LOGI(TAG, "Writing dummy variable-length log pattern 3");
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_head_offset = 8000;
    log_tail_offset = 2000;
    if (log_mutex) xSemaphoreGive(log_mutex);
    
    // Write maximum-size entries
    for (uint32_t i = 0; i < 20; i++) {
        uint8_t data[LOG_MAX_PAYLOAD];
        for (uint16_t j = 0; j < LOG_MAX_PAYLOAD; j++) {
            data[j] = (i * 17 + j) & 0xFF;
        }
        
        write_log(LOG_TYPE_DEBUG, data, LOG_MAX_PAYLOAD);
        
        ESP_LOGI(TAG, "Wrote max-size entry %lu", (unsigned long)i);
    }
    
    return ESP_OK;
}

void storage_deinit(void) {
    storage_partition = NULL;
    log_initialized = false;
    if (log_mutex) {
        vSemaphoreDelete(log_mutex);
        log_mutex = NULL;
    }
}