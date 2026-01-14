#include <math.h>
#include <string.h>
#include <sys/param.h>
#include "esp_partition.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "esp_task_wdt.h"
#include "storage.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "version.h"

#define TAG "STORAGE"

// Add these includes at the top
#include "freertos/queue.h"
#include "freertos/task.h"

// Add these defines near the top
#define LOG_QUEUE_SIZE 8  // Number of log entries that can be queued
#define LOG_TASK_STACK_SIZE 4096
#define LOG_TASK_PRIORITY 5

// Add these static variables after the existing log variables (around line 100)
static QueueHandle_t log_queue = NULL;
static TaskHandle_t log_task_handle = NULL;
static bool log_task_running = false;

// Log queue entry structure
typedef struct {
    uint8_t data[LOG_MAX_PAYLOAD + 1];  // +1 for length byte
    uint8_t len;
} log_queue_entry_t;

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
RTC_DATA_ATTR static uint32_t log_head_offset = 0;
RTC_DATA_ATTR static uint32_t log_tail_offset = 0;
RTC_DATA_ATTR static bool log_initialized = false;

static SemaphoreHandle_t log_mutex = NULL;

uint32_t log_get_head(void) { 
    uint32_t head;
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    head = log_head_offset;
    if (log_mutex) xSemaphoreGive(log_mutex);
    return head;
}

uint32_t log_get_tail(void) { 
    uint32_t tail;
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    tail = log_tail_offset;
    if (log_mutex) xSemaphoreGive(log_mutex);
    return tail;
}

uint32_t log_get_offset(void) { 
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
    
    // TODO: WIPE ENTIRE PARTITION
    
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
    
    
    
    log_mutex = xSemaphoreCreateMutex();
    if (log_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create log mutex");
        return ESP_ERR_NO_MEM;
    }
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 
                                                  ESP_PARTITION_SUBTYPE_ANY, 
                                                  "storage");
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not found");
        if (log_mutex) xSemaphoreGive(log_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    
    ESP_LOGI(TAG, "Storage partition found: size=%lu bytes", 
             (unsigned long)storage_partition->size);
    
    // Load parameters from flash
    uint32_t flash_offset = PARAMS_OFFSET;
    //bool all_valid = true;
    
    for (int i = 0; i < NUM_PARAMS; i++) {
        param_stored_t stored;
        
        esp_err_t err = esp_partition_read(storage_partition, flash_offset, 
                                           &stored, sizeof(param_stored_t));
        
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read parameter %d (%s), using default", 
                     i, parameter_names[i]);
            memcpy(&parameter_table[i], &parameter_defaults[i], sizeof(param_value_t));
            //all_valid = false;
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
            //all_valid = false;
        }
        
        flash_offset += sizeof(param_stored_t);
    }
    
    /*if (all_valid) {
        ESP_LOGI(TAG, "All parameters loaded successfully from flash");
    } else {
        ESP_LOGW(TAG, "Some parameters failed validation, using defaults");
    }*/
    
    if (log_mutex) xSemaphoreGive(log_mutex);
    return ESP_OK;
}

static inline uint32_t log_sector_end(uint32_t x) {
	return (x/FLASH_SECTOR_SIZE+1)*FLASH_SECTOR_SIZE;
}

    
// Helper function to check if a sector is erased (all 0xFF)
static bool is_sector_erased(uint32_t sector_offset) {
    uint8_t buf[256];
    esp_err_t err = esp_partition_read(storage_partition, sector_offset, buf, 256);
    if (err != ESP_OK) return false;
    
    for (int i = 0; i < 256; i++) {
        if (buf[i] != 0xFF) return false;
    }
    return true;
}

// Helper function to check if a sector has data (contains non-0xFF, non-0x00 bytes)
static bool sector_has_data(uint32_t sector_offset) {
    uint8_t buf[256];
    esp_err_t err = esp_partition_read(storage_partition, sector_offset, buf, 256);
    if (err != ESP_OK) return false;
    
    for (int i = 0; i < 256; i++) {
        if (buf[i] != 0xFF && buf[i] != 0x00) return true;
    }
    return false;
}

// Replace log_write with this non-blocking version:
esp_err_t log_write(uint8_t* buf, uint8_t len) {
    if (!log_initialized || storage_partition == NULL) {
        ESP_LOGE(TAG, "Logging not initialized");
        return ESP_FAIL;
    }
    
    if (len > LOG_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Log payload too large: %d bytes (max %d)", len, LOG_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }
    
    if (log_queue == NULL) {
        ESP_LOGE(TAG, "Log queue not initialized");
        return ESP_FAIL;
    }
    
    // Create queue entry
    log_queue_entry_t entry;
    entry.len = len;
    memcpy(entry.data, buf, len);
    
    // Try to send to queue (non-blocking)
    if (xQueueSend(log_queue, &entry, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Log queue full, dropping entry");
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_OK;
}

// The actual blocking write function (called by the task)
static esp_err_t log_write_blocking(uint8_t* buf, uint8_t len) {
    if (!log_initialized || storage_partition == NULL) {
        ESP_LOGE(TAG, "Logging not initialized");
        return ESP_FAIL;
    }
    
    if (len > LOG_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Log payload too large: %d bytes (max %d)", len, LOG_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    // check if we will overrun the sector
    if (log_head_offset + len+1 >= log_sector_end(log_head_offset)) {
        ESP_LOGI(TAG, "WILL OVERRUN (%ld >= %ld)", (long)log_head_offset + len+1, (long)log_sector_end(log_head_offset));
        // zero the rest of sector
        char zeros[256] = {0};
        esp_partition_write(storage_partition, 
        log_head_offset, &zeros,
        log_sector_end(log_head_offset)-log_head_offset);
        
        // set head to next sector, and check for wrap
        log_head_offset = log_sector_end(log_head_offset);
        if (log_head_offset >= storage_partition->size)
            log_head_offset = LOG_START_OFFSET;
            
        // Next write will be in a new sector - check if it needs erasing
        uint8_t check_byte;
        esp_err_t err = esp_partition_read(storage_partition, log_head_offset, 
                                            &check_byte, 1);
        
        // Erase the next sector
        if (err == ESP_OK && check_byte != 0xFF) {
            ESP_LOGI(TAG, "Erasing sector %lu for log", (unsigned long)log_head_offset);
            err = esp_partition_erase_range(storage_partition, 
                                             log_head_offset, 
                                             FLASH_SECTOR_SIZE);
            
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase sector: %s", esp_err_to_name(err));
                if (log_mutex) xSemaphoreGive(log_mutex);
                return ESP_FAIL;
            }
            
        }
        
        // update the tail, if needed
        if (log_tail_offset >= log_head_offset + FLASH_SECTOR_SIZE) log_tail_offset = log_head_offset + FLASH_SECTOR_SIZE;
        if (log_tail_offset >= storage_partition->size)
            log_tail_offset = LOG_START_OFFSET;
        
        ESP_LOGI(TAG, "Tail/Head are now %lu/%lu", 
                 (unsigned long)log_tail_offset, (unsigned long)log_head_offset);
    }
        
        
    esp_partition_write(storage_partition, log_head_offset, &len, 1);
    esp_partition_write(storage_partition, log_head_offset+1, buf, len);
    
    log_head_offset+=len+1;
    ESP_LOGI(TAG, "Wrote; Tail/Head are now %lu/%lu", 
                 (unsigned long)log_tail_offset, (unsigned long)log_head_offset);
    
    if (log_mutex) xSemaphoreGive(log_mutex);
    return ESP_OK;
}

// Log writer task
static void log_writer_task(void *pvParameters) {
    log_queue_entry_t entry;
    
    ESP_LOGI(TAG, "Log writer task started");
    
    while (log_task_running) {
        // Wait for log entry (with timeout to check running flag)
        if (xQueueReceive(log_queue, &entry, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Write the log entry (blocking)
            esp_err_t err = log_write_blocking(entry.data, entry.len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write log entry: %s", esp_err_to_name(err));
            }
        }
        
        // Feed watchdog
        esp_task_wdt_reset();
    }
    
    ESP_LOGI(TAG, "Log writer task stopping");
    vTaskDelete(NULL);
}

// Modified log_init to create queue and task
esp_err_t log_init() {
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not initialized, call storage_init() first");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    uint32_t log_area_size = storage_partition->size - LOG_START_OFFSET;
    uint32_t num_sectors = log_area_size / FLASH_SECTOR_SIZE;
    
    ESP_LOGI(TAG, "Log init: scanning %lu sectors with bisection", (unsigned long)num_sectors);
    
    // Default to empty log
    log_head_offset = LOG_START_OFFSET;
    log_tail_offset = LOG_START_OFFSET;
    
    // ... [INCLUDE THE BISECTION ALGORITHM FROM EARLIER HERE] ...
    // Binary search to find the first erased sector
    int32_t left = 0;
    int32_t right = num_sectors - 1;
    int32_t head_sector = -1;
    
    while (left <= right) {
        int32_t mid = left + (right - left) / 2;
        uint32_t sector_offset = LOG_START_OFFSET + mid * FLASH_SECTOR_SIZE;
        
        esp_task_wdt_reset();
        
        bool erased = is_sector_erased(sector_offset);
        bool prev_has_data = (mid > 0) ? sector_has_data(LOG_START_OFFSET + (mid-1) * FLASH_SECTOR_SIZE) : false;
        
        if (erased && (mid == 0 || prev_has_data)) {
            head_sector = mid;
            break;
        } else if (erased) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }
    
    if (head_sector == -1) {
        if (is_sector_erased(LOG_START_OFFSET)) {
            ESP_LOGI(TAG, "Log is empty");
            log_head_offset = LOG_START_OFFSET;
            log_tail_offset = LOG_START_OFFSET;
        } else {
            head_sector = 0;
            ESP_LOGW(TAG, "Log appears full, searching from start");
        }
    }
    
    // Walk the data structure to find exact head
    uint32_t cursor;
    if (head_sector > 0) {
        cursor = LOG_START_OFFSET + (head_sector - 1) * FLASH_SECTOR_SIZE;
    } else {
        cursor = LOG_START_OFFSET;
    }
    
    uint32_t head_sector_start = LOG_START_OFFSET + head_sector * FLASH_SECTOR_SIZE;
    
    bool found_head = false;
    while (cursor < head_sector_start + FLASH_SECTOR_SIZE && cursor < storage_partition->size) {
        uint8_t buf;
        esp_err_t err = esp_partition_read(storage_partition, cursor, &buf, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read during log init at offset %lu", (unsigned long)cursor);
            if (log_mutex) xSemaphoreGive(log_mutex);
            return err;
        }
        
        if (buf == 0xFF) {
            log_head_offset = cursor;
            found_head = true;
            break;
        } else if (buf == 0x00) {
            cursor = log_sector_end(cursor);
        } else {
            cursor += buf + 1;
        }
        
        esp_task_wdt_reset();
    }
    
    if (!found_head) {
        log_head_offset = cursor;
    }
    
    // Find tail
    int32_t tail_sector = -1;
    
    for (int32_t i = head_sector + 1; i < num_sectors; i++) {
        uint32_t sector_offset = LOG_START_OFFSET + i * FLASH_SECTOR_SIZE;
        esp_task_wdt_reset();
        
        if (sector_has_data(sector_offset)) {
            tail_sector = i;
            break;
        }
    }
    
    if (tail_sector == -1 && head_sector > 0) {
        for (int32_t i = 0; i < head_sector; i++) {
            uint32_t sector_offset = LOG_START_OFFSET + i * FLASH_SECTOR_SIZE;
            esp_task_wdt_reset();
            
            if (sector_has_data(sector_offset)) {
                tail_sector = i;
                break;
            }
        }
    }
    
    if (tail_sector >= 0) {
        log_tail_offset = LOG_START_OFFSET + tail_sector * FLASH_SECTOR_SIZE;
    } else {
        log_tail_offset = LOG_START_OFFSET;
    }
    
    log_initialized = true;
    ESP_LOGI(TAG, "Log system initialized (bisection). Head: %lu, Tail: %lu", 
             (unsigned long)log_head_offset, (unsigned long)log_tail_offset);
    
    // Create the log queue
    log_queue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(log_queue_entry_t));
    if (log_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create log queue");
        if (log_mutex) xSemaphoreGive(log_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    // Create the log writer task
    log_task_running = true;
    BaseType_t task_created = xTaskCreate(
        log_writer_task,
        "log_writer",
        LOG_TASK_STACK_SIZE,
        NULL,
        LOG_TASK_PRIORITY,
        &log_task_handle
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create log writer task");
        vQueueDelete(log_queue);
        log_queue = NULL;
        if (log_mutex) xSemaphoreGive(log_mutex);
        return ESP_ERR_NO_MEM;
    }
    
    // Add the task to watchdog
    esp_task_wdt_add(log_task_handle);
    
    ESP_LOGI(TAG, "Log writer task created successfully");
             
    if (log_mutex) xSemaphoreGive(log_mutex);
    return ESP_OK;
}

// Update storage_deinit to stop the task
void storage_deinit(void) {
    // Stop the log writer task
    if (log_task_running) {
        log_task_running = false;
        
        // Wait a bit for task to finish
        vTaskDelay(pdMS_TO_TICKS(200));
        
        if (log_task_handle != NULL) {
            esp_task_wdt_delete(log_task_handle);
            log_task_handle = NULL;
        }
    }
    
    // Delete the queue
    if (log_queue != NULL) {
        vQueueDelete(log_queue);
        log_queue = NULL;
    }
    
    storage_partition = NULL;
    log_initialized = false;
    if (log_mutex) {
        vSemaphoreDelete(log_mutex);
        log_mutex = NULL;
    }
}
