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

// Partition pointer
static const esp_partition_t *storage_partition = NULL;

// Log head tracking with mutex protection
static uint32_t log_head_index = 0;
static uint32_t log_tail_index = 0;
static SemaphoreHandle_t log_mutex = NULL;
static bool log_initialized = false;

uint32_t get_log_head(void) { 
    uint32_t head;
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    head = LOG_START_OFFSET + (log_head_index * LOG_ENTRY_SIZE);
    if (log_mutex) xSemaphoreGive(log_mutex);
    return head;
}

uint32_t get_log_tail(void) { 
    uint32_t tail;
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    tail = LOG_START_OFFSET + (log_tail_index * LOG_ENTRY_SIZE);
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
    }
}

// ============================================================================
// COMMIT PARAMETERS TO FLASH
// ============================================================================
esp_err_t commit_params(void) {
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not initialized");
        return ESP_FAIL;
    }
    
    set_param_string(PARAM_BUILD_VERSION, FIRMWARE_VERSION);
    
    // Calculate flash offset for each parameter
    uint32_t flash_offset = PARAMS_OFFSET;
    
    // Erase the parameter sectors
    esp_err_t err = esp_partition_erase_range(storage_partition, PARAMS_OFFSET, 
                                               LOG_START_OFFSET);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase parameter sectors: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    
    // Write each parameter with its CRC
    for (int i = 0; i < NUM_PARAMS; i++) {
        param_stored_t stored;
        memset(&stored, 0, sizeof(stored));
        
        // Pack the parameter value
        uint8_t size = param_type_size(parameter_types[i]);
        pack_param(stored.data, i);
        
        // Calculate CRC over the actual data size used
        uint32_t crc_input = PARAM_CRC_SALT;
        //uint32_t crc = esp_crc32_le(0, (uint8_t*)&crc_input, sizeof(crc_input));
        stored.crc = esp_crc32_le(crc_input, stored.data, size);
        
        // Write to flash
        err = esp_partition_write(storage_partition, flash_offset, 
                                  &stored, sizeof(param_stored_t));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write parameter %d (%s): %s", 
                     i, parameter_names[i], esp_err_to_name(err));
            return ESP_FAIL;
        }
        
        flash_offset += sizeof(param_stored_t);
    }
    
    ESP_LOGI(TAG, "Parameters committed to flash successfully");
    return ESP_OK;
}

// ============================================================================
// FACTORY RESET
// ============================================================================
esp_err_t factory_reset(void) {
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGW(TAG, "FACTORY RESET: Erasing entire storage partition...");
    
    // Erase the entire storage partition
    esp_err_t err = esp_partition_erase_range(storage_partition, 0, storage_partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase storage partition: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "Storage partition erased successfully");
    
    // Reset all parameters to defaults in RAM
    for (int i = 0; i < NUM_PARAMS; i++) {
        memcpy(&parameter_table[i], &parameter_defaults[i], sizeof(param_value_t));
    }
    
    // Reset log indices
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_head_index = 0;
    log_tail_index = 0;
    if (log_mutex) xSemaphoreGive(log_mutex);
    
    ESP_LOGI(TAG, "Factory reset complete - all data erased");
    
    return ESP_OK;
}

// ============================================================================
// INITIALIZATION FUNCTIONS
// ============================================================================

esp_err_t storage_init(void) {
    // Find the partition labeled "storage"
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
            memcpy(&parameter_table[i], &parameter_defaults[i], sizeof(param_value_t));  // SET DEFAULT HERE
            all_valid = false;
            flash_offset += sizeof(param_stored_t);
            continue;
        }
        
        // Validate CRC over actual data size
        uint8_t size = param_type_size(parameter_types[i]);
        uint32_t crc_input = PARAM_CRC_SALT;
        //uint32_t crc = esp_crc32_le(0, (uint8_t*)&crc_input, sizeof(crc_input));
        uint32_t calculated_crc = esp_crc32_le(crc_input, stored.data, size);
        
        if (calculated_crc == stored.crc) {
            unpack_param(stored.data, i);
        } else {
            ESP_LOGW(TAG, "Parameter %d (%s) failed CRC check, using default", 
                     i, parameter_names[i]);
            memcpy(&parameter_table[i], &parameter_defaults[i], sizeof(param_value_t));  // SET DEFAULT HERE
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
// LOGGING FUNCTIONS (unchanged from original)
// ============================================================================

static esp_err_t find_log_head(void) {
    if (storage_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    uint32_t log_area_size = storage_partition->size - LOG_START_OFFSET;
    uint32_t max_entries = log_area_size / LOG_ENTRY_SIZE;
    
    uint8_t entry[LOG_ENTRY_SIZE];
    uint8_t empty_entry[LOG_ENTRY_SIZE];
    memset(empty_entry, 0xFF, LOG_ENTRY_SIZE);
    
    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t offset = LOG_START_OFFSET + (i * LOG_ENTRY_SIZE);
        
        esp_err_t err = esp_partition_read(storage_partition, offset, entry, LOG_ENTRY_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read log entry at index %lu", (unsigned long)i);
            return err;
        }
        
        if (memcmp(entry, empty_entry, LOG_ENTRY_SIZE) == 0) {
            log_head_index = i;
            ESP_LOGI(TAG, "Log head found at index %lu", (unsigned long)log_head_index);
            return ESP_OK;
        }
    }
    
    log_head_index = 0;
    ESP_LOGI(TAG, "Log is full, wrapping to beginning");
    
    esp_err_t err = esp_partition_erase_range(storage_partition, LOG_START_OFFSET, 
                                               FLASH_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase first log sector");
        return err;
    }
    
    return ESP_OK;
}

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
    
    esp_err_t err = find_log_head();
    if (err != ESP_OK) {
        vSemaphoreDelete(log_mutex);
        log_mutex = NULL;
        return err;
    }
    
    log_initialized = true;
    return ESP_OK;
}

esp_err_t write_log(char* entry) {
    if (!log_initialized || storage_partition == NULL) {
        ESP_LOGE(TAG, "Logging not initialized");
        return ESP_FAIL;
    }
    
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    
    uint32_t log_area_end = storage_partition->size;
    uint32_t max_entries = (log_area_end - LOG_START_OFFSET) / LOG_ENTRY_SIZE;
    
    uint32_t current_offset = LOG_START_OFFSET + (log_head_index * LOG_ENTRY_SIZE);
    uint32_t next_offset = current_offset + LOG_ENTRY_SIZE;
    if (next_offset >= log_area_end) {
        next_offset = LOG_START_OFFSET;
    }
    
    uint32_t current_sector = current_offset / FLASH_SECTOR_SIZE;
    uint32_t next_sector = next_offset / FLASH_SECTOR_SIZE;
    
    if (next_sector != current_sector) {
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
            
            uint32_t tail_offset = next_sector * FLASH_SECTOR_SIZE;
            if (tail_offset < LOG_START_OFFSET) {
                tail_offset = LOG_START_OFFSET;
            }
            log_tail_index = (tail_offset - LOG_START_OFFSET) / LOG_ENTRY_SIZE;
            
            if (log_tail_index >= max_entries) {
                log_tail_index = 0;
            }
            
            ESP_LOGI(TAG, "Tail/Head are now %ld/%ld", (long)log_tail_index, (long)log_head_index);
        }
    }
    
    esp_err_t err = esp_partition_write(storage_partition, current_offset, entry, LOG_ENTRY_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write log entry: %s", esp_err_to_name(err));
        if (log_mutex) xSemaphoreGive(log_mutex);
        return ESP_FAIL;
    }
    
    log_head_index++;
    if (log_head_index >= max_entries) {
        log_head_index = 0;
        ESP_LOGI(TAG, "Log wrapped to beginning");
    }
    
    if (log_mutex) xSemaphoreGive(log_mutex);
    return ESP_OK;
}

esp_err_t write_dummy_log_1(void) {
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_head_index = 0;
    log_tail_index = 0;
    if (log_mutex) xSemaphoreGive(log_mutex);
    
    uint32_t log_area_end = storage_partition->size;
    uint32_t max_entries = (log_area_end - LOG_START_OFFSET) / LOG_ENTRY_SIZE;
    for (uint32_t i=0; i<max_entries*3/2; i++) {
        ESP_LOGI(TAG, "log[%ld]", (long)i);
        char entry[32] = {32, i>>24,i>>16,i>>8,i>>0};
        write_log(entry);
    }
    return ESP_OK;
}

esp_err_t write_dummy_log_2(void) {
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_head_index = 56;
    log_tail_index = 105;
    if (log_mutex) xSemaphoreGive(log_mutex);
    
    uint32_t log_area_end = storage_partition->size;
    uint32_t max_entries = (log_area_end - LOG_START_OFFSET) / LOG_ENTRY_SIZE;
    for (uint32_t i=0; i<max_entries*3/2; i++) {
        ESP_LOGI(TAG, "log[%ld]", (long)i);
        char entry[32] = {32, i>>24,i>>16,i>>8,i>>0};
        write_log(entry);
    }
    return ESP_OK;
}

esp_err_t write_dummy_log_3(void) {
    if (log_mutex) xSemaphoreTake(log_mutex, portMAX_DELAY);
    log_head_index = 105;
    log_tail_index = 34;
    if (log_mutex) xSemaphoreGive(log_mutex);
    
    uint32_t log_area_end = storage_partition->size;
    uint32_t max_entries = (log_area_end - LOG_START_OFFSET) / LOG_ENTRY_SIZE;
    for (uint32_t i=0; i<max_entries*3/2; i++) {
        ESP_LOGI(TAG, "log[%ld]", (long)i);
        char entry[32] = {32, i>>24,i>>16,i>>8,i>>0};
        write_log(entry);
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