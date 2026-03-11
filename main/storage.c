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
#include "nvs_flash.h"
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
    uint8_t data[LOG_MAX_PAYLOAD];  // +1 for length byte
    uint8_t len;
    uint8_t type;
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

    // NVS must be initialized before WiFi and BT
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erasing, performing erase...");
        nvs_err = nvs_flash_erase();
        if (nvs_err == ESP_OK) nvs_err = nvs_flash_init();
    }
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(nvs_err));
        return nvs_err;
    }

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

    
// Helper function to check if a sector is erased (starts with 0xFF)
static bool is_sector_erased(uint32_t x) {
    uint8_t buf; //[256];
    esp_err_t err = esp_partition_read(storage_partition, LOG_START_OFFSET + x * FLASH_SECTOR_SIZE, &buf, 1);
    if (err != ESP_OK) return false;
    if (buf == 0xFF) return true;
    
    /*for (int i = 0; i < 256; i++) {
        if (buf[i] != 0xFF) return false;
    }*/
    return false;
}

static bool is_sector_full(uint32_t x) {
    uint8_t buf; //[256];
    esp_err_t err = esp_partition_read(storage_partition, LOG_START_OFFSET + (x+1) * FLASH_SECTOR_SIZE - 1, &buf, 1);
    if (err != ESP_OK) return false;
    if (buf == 0xFF) return false;
    
    /*for (int i = 0; i < 256; i++) {
        if (buf[i] != 0xFF) return false;
    }*/
    return true;
}

static inline void find_head_tail(int32_t num_sectors, int32_t *head, int32_t *tail) {
    
}

// Helper function to check if a sector has data (contains non-0xFF, non-0x00 bytes)
/*static bool sector_has_data(uint32_t sector_offset) {
    uint8_t buf; //[256];
    esp_err_t err = esp_partition_read(storage_partition, sector_offset, &buf, 256);
    if (err != ESP_OK) return false;
    if (buf )
    
    for (int i = 0; i < 256; i++) {
        if (buf[i] != 0xFF && buf[i] != 0x00) return true;
    }
    return false;
}*/

// Replace log_write with this non-blocking version:
esp_err_t log_write(uint8_t* buf, uint8_t len, uint8_t type) {
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
    
    if (type == 0xFF) {
		ESP_LOGE(TAG, "Attempt to log with type=0xFF; not allowed");
		return ESP_ERR_INVALID_ARG;
	}
    
    // Create queue entry
    log_queue_entry_t entry;
    entry.len = len;
    entry.type = type;
    memcpy(entry.data, buf, len);
    
    // Try to send to queue (non-blocking)
    if (xQueueSend(log_queue, &entry, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Log queue full, dropping entry");
        return ESP_ERR_NO_MEM;
    }
    
    return ESP_OK;
}

// The actual blocking write function (called by the task)
static esp_err_t log_write_blocking(uint8_t* buf, uint8_t len, uint8_t type) {
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
    if (log_head_offset + len+2 >= log_sector_end(log_head_offset)) {
        //ESP_LOGI(TAG, "WILL OVERRUN (%ld >= %ld)", (long)log_head_offset + len+2, (long)log_sector_end(log_head_offset));
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
            //ESP_LOGI(TAG, "Erasing sector %lu for log", (unsigned long)log_head_offset);
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
        
        ESP_LOGI(TAG, "Erased; Tail/Head are now %lu/%lu", 
                 (unsigned long)log_tail_offset, (unsigned long)log_head_offset);
    }
    len++; // account for type bit
        
        
    esp_partition_write(storage_partition, log_head_offset, &len, 1);
    esp_partition_write(storage_partition, log_head_offset+1, buf, len-1);
    esp_partition_write(storage_partition, log_head_offset+len, &type, 1);
    
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
		// Feed watchdog
        //esp_task_wdt_reset();
        // Wait for log entry (with timeout to check running flag)
        if (xQueueReceive(log_queue, &entry, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Write the log entry (blocking)
            esp_err_t err = log_write_blocking(entry.data, entry.len, entry.type);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write log entry: %s", esp_err_to_name(err));
            }
            
            //esp_task_wdt_reset();
        }
        
        
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
    
    // Binary search for the first non-full sector
    int32_t l = 0;
    int32_t r = num_sectors - 1;
    int32_t head_sector = 0;
    int32_t tail_sector = 0;
    
    while (l < r) {
        int32_t m = (l + r) / 2;
        if (is_sector_full(m)) {
            l = m + 1;
        } else {
            r = m;
        }
    }
    
    int32_t first_non_full = l;
    
    // Determine head based on what the first non-full sector is
    if (first_non_full >= num_sectors) {
        // All sectors are full (wraparound case)
        head_sector = 0;
    } else if (is_sector_erased(first_non_full)) {
        // First non-full is erased -> head is this erased sector
        head_sector = first_non_full;
    } else {
        // First non-full is partial -> head is this partial sector
        head_sector = first_non_full;
    }
    
    // Binary search for tail: first full sector after head (with wraparound)
    // Search from head+1 to head+num_sectors-1 (wrapping around)
    l = 1;
    r = num_sectors - 1;
    
    while (l <= r) {
        int32_t m = (l + r) / 2;
        int32_t sector = (head_sector + m) % num_sectors;
        
        if (is_sector_full(sector)) {
            // Found a full sector, but need the FIRST one
            tail_sector = sector;
            r = m - 1;
        } else {
            l = m + 1;
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
    //if (esp_task_wdt_add(log_task_handle) != ESP_OK) {
	//    ESP_LOGW(TAG, "Log task already subscribed to watchdog or failed to add");
	//}
	
    ESP_LOGI(TAG, "Log writer task created successfully");
             
    if (log_mutex) xSemaphoreGive(log_mutex);
    return ESP_OK;
}

// Update storage_deinit to stop the task
void storage_deinit(void) {
    // Stop the log writer task
	if (log_task_running) {
	    log_task_running = false;
	    vTaskDelay(pdMS_TO_TICKS(200));
	    
	    if (log_task_handle != NULL) {
	        // Don't try to delete from watchdog - task was never added
	        // esp_task_wdt_delete(log_task_handle);  // <-- REMOVE THIS LINE
	        vTaskDelay(pdMS_TO_TICKS(100));
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






/*
 * ADDITIONS TO storage.c
 * 
 * Add these functions to storage.c to support the test suite.
 * These provide the ability to erase all log sectors, simulate power cycles,
 * and read back log entries for verification.
 */

// Add these function declarations to storage.h:
/*
esp_err_t log_read(uint8_t* len, uint8_t* buf, uint8_t* type);
void log_read_reset(void);
esp_err_t log_erase_all_sectors(void);
esp_err_t log_simulate_power_cycle(void);
*/

// Add this static variable near the top of storage.c with other log static variables
// (around line 118, after log_tail_offset and log_initialized):

static uint32_t log_read_cursor = 0;


// Add these functions to storage.c (after the existing log functions):

/*
 * ADDITIONS TO storage.c
 * 
 * Add these functions to storage.c to support the test suite.
 * These provide the ability to erase all log sectors, simulate power cycles,
 * and read back log entries for verification.
 */

// Add these function declarations to storage.h:
/*
esp_err_t log_read(uint8_t* len, uint8_t* buf, uint8_t* type);
void log_read_reset(void);
esp_err_t log_erase_all_sectors(void);
esp_err_t log_simulate_power_cycle(void);
*/

// Add this static variable near the top of storage.c with other log static variables
// (around line 118, after log_tail_offset and log_initialized):
/*
static uint32_t log_read_cursor = 0;
*/

// Add these functions to storage.c (after the existing log functions):

/**
 * @brief Read a log entry from the current read cursor position
 * 
 * Reads a log entry from flash starting at the current read cursor and advances
 * the cursor to the next entry. The read cursor is independent of tail - reading
 * does NOT affect the tail pointer (which marks the boundary of valid data).
 * 
 * The tail is only moved by the log write system when sectors need to be erased
 * during wraparound. Reading is completely non-destructive.
 * 
 * This is a test/debug function - the production logging system is write-only.
 * 
 * Log entry format in flash:
 * [1 byte: length] [length-1 bytes: data] [1 byte: type]
 * 
 * @param len Pointer to store the entry length (data length, not including type byte)
 * @param buf Buffer to store the entry data (must be at least LOG_MAX_PAYLOAD bytes)
 * @param type Pointer to store the entry type
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if no more entries, error code otherwise
 */
esp_err_t log_read(uint8_t* len, uint8_t* buf, uint8_t* type) {
    // NOTE: log_read_cursor must be declared as a file-scope static variable
    // Add this declaration near the other log static variables in storage.c:
    // static uint32_t log_read_cursor = 0;
    
    if (!log_initialized || storage_partition == NULL) {
        ESP_LOGE(TAG, "Logging not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (len == NULL || buf == NULL || type == NULL) {
        ESP_LOGE(TAG, "NULL pointer passed to log_read");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    // Initialize read cursor to tail on first read (when cursor is 0)
    if (log_read_cursor == 0) {
        log_read_cursor = log_tail_offset;
    }
    
    // Check if we've caught up to head (no more entries to read)
    if (log_read_cursor == log_head_offset) {
        if (log_mutex) xSemaphoreGive(log_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Read the length byte
    uint8_t entry_len;
    esp_err_t err = esp_partition_read(storage_partition, log_read_cursor, &entry_len, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read entry length at offset %lu", (unsigned long)log_read_cursor);
        if (log_mutex) xSemaphoreGive(log_mutex);
        return err;
    }
    
    // Check for erased flash (0xFF) - means we've reached the end
    if (entry_len == 0xFF) {
        if (log_mutex) xSemaphoreGive(log_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    
    // Check for sector padding (0x00) - skip to next sector
    if (entry_len == 0x00) {
        // Move to next sector
        uint32_t next_sector = ((log_read_cursor / FLASH_SECTOR_SIZE) + 1) * FLASH_SECTOR_SIZE;
        
        // Handle wraparound
        if (next_sector >= storage_partition->size) {
            log_read_cursor = LOG_START_OFFSET;
        } else {
            log_read_cursor = next_sector;
        }
        
        if (log_mutex) xSemaphoreGive(log_mutex);
        return log_read(len, buf, type); // Recursive call to read from next sector
    }
    
    // Validate length
    if (entry_len > LOG_MAX_PAYLOAD + 1) { // +1 for type byte included in length
        ESP_LOGE(TAG, "Invalid entry length %d at offset %lu", entry_len, (unsigned long)log_read_cursor);
        if (log_mutex) xSemaphoreGive(log_mutex);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // Read the data (length-1 bytes, since length includes the type byte)
    uint8_t data_len = entry_len - 1;
    err = esp_partition_read(storage_partition, log_read_cursor + 1, buf, data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read entry data at offset %lu", (unsigned long)(log_read_cursor + 1));
        if (log_mutex) xSemaphoreGive(log_mutex);
        return err;
    }
    
    // Read the type byte
    uint8_t entry_type;
    err = esp_partition_read(storage_partition, log_read_cursor + entry_len, &entry_type, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read entry type at offset %lu", (unsigned long)(log_read_cursor + entry_len));
        if (log_mutex) xSemaphoreGive(log_mutex);
        return err;
    }
    
    // Validate type
    if (!IS_VALID_LOG_TYPE(entry_type)) {
        ESP_LOGW(TAG, "Invalid log type 0x%02X at offset %lu", entry_type, (unsigned long)(log_read_cursor + entry_len));
        // Continue anyway - might be valid data with unknown type
    }
    
    // Set output parameters
    *len = data_len;
    *type = entry_type;
    
    // Advance read cursor (NOT tail - tail is only moved by writes during wraparound)
    log_read_cursor += entry_len + 1; // +1 for the type byte after data
    
    // Handle wraparound
    if (log_read_cursor >= storage_partition->size) {
        log_read_cursor = LOG_START_OFFSET;
    }
    
    if (log_mutex) xSemaphoreGive(log_mutex);
    return ESP_OK;
}

/**
 * @brief Reset the log read cursor to start reading from the beginning
 * 
 * Resets the internal read cursor so that the next call to log_read()
 * will start reading from the tail (oldest valid entry).
 * This is useful for tests that need to re-read the log.
 * 
 * IMPORTANT: This does NOT affect the tail pointer. The tail marks the
 * boundary of valid data and is only moved by the write system.
 */
void log_read_reset(void) {
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_read_cursor = 0;  // Reset to 0 so next read starts from tail
    if (log_mutex) xSemaphoreGive(log_mutex);
}

/**
 * @brief Erase all sectors in the log area
 * 
 * This function erases the entire log area of the flash partition,
 * resetting it to a clean state. Useful for testing.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t log_erase_all_sectors(void) {
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    // Stop the log writer task
	if (log_task_running) {
	    log_task_running = false;
	    vTaskDelay(pdMS_TO_TICKS(200));
	    
	    if (log_task_handle != NULL) {
	        // Don't try to delete from watchdog - task was never added
	        // esp_task_wdt_delete(log_task_handle);  // <-- REMOVE THIS LINE
	        vTaskDelay(pdMS_TO_TICKS(100));
	        log_task_handle = NULL;
	    }
	}
	
	// Clear the queue
	if (log_queue != NULL) {
	    xQueueReset(log_queue);
	}
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    uint32_t log_area_size = storage_partition->size - LOG_START_OFFSET;
    
    ESP_LOGI(TAG, "Erasing all log sectors (%lu bytes)...", (unsigned long)log_area_size);
    
    esp_err_t err = esp_partition_erase_range(storage_partition, 
                                               LOG_START_OFFSET, 
                                               log_area_size);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log area: %s", esp_err_to_name(err));
        if (log_mutex) xSemaphoreGive(log_mutex);
        return err;
    }
    
    ESP_LOGI(TAG, "All log sectors erased successfully");
    
    if (log_mutex) xSemaphoreGive(log_mutex);
    
    // Reinitialize 
    log_initialized = false;
    log_read_cursor = 0;
    
    err = log_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reinitialize log after erase");
        return err;
    }
    
    return ESP_OK;
}

/**
 * @brief Simulate a power cycle by re-running log initialization
 * 
 * This function simulates what happens when the device powers off and back on:
 * - Clears the in-memory head/tail tracking (simulated by marking as uninitialized)
 * - Re-runs log_init() to scan flash and recover head/tail positions
 * 
 * This is crucial for testing that the log system can correctly recover its
 * state from flash after a power loss.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t log_simulate_power_cycle(void) {
    ESP_LOGI(TAG, "Simulating power cycle...");
    // Stop the log writer task
	if (log_task_running) {
	    log_task_running = false;
	    vTaskDelay(pdMS_TO_TICKS(200));
	    
	    if (log_task_handle != NULL) {
	        // Don't try to delete from watchdog - task was never added
	        // esp_task_wdt_delete(log_task_handle);  // <-- REMOVE THIS LINE
	        vTaskDelay(pdMS_TO_TICKS(100));
	        log_task_handle = NULL;
	    }
	}
    
    // Clear the queue
    if (log_queue != NULL) {
        xQueueReset(log_queue);
    }
    
    // Mark log as uninitialized (simulates losing RAM state)
    log_initialized = false;
    
    // Reset read cursor (simulates losing RAM state)
    log_read_cursor = 0;
    
    // Re-run log initialization to scan flash and recover state
    esp_err_t err = log_init();
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reinitialize log after simulated power cycle");
        return err;
    }
    
    ESP_LOGI(TAG, "Power cycle simulation complete. Head=%lu, Tail=%lu", 
             (unsigned long)log_head_offset, (unsigned long)log_tail_offset);
    
    return ESP_OK;
}

esp_err_t log_write_blocking_test(uint8_t* buf, uint8_t len, uint8_t type) {
    // Check queue space - wait if nearly full
    while (uxQueueSpacesAvailable(log_queue) < 2) {
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_task_wdt_reset();
    }
    
    return log_write(buf, len, type);
}