#include <string.h>
#include "esp_partition.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "storage.h"

#define TAG "STORAGE"

// ============================================================================
// PARAMETER TABLE GENERATION
// ============================================================================

// Helper macros to construct initializers
#define PARAM_VALUE_INIT(type, val) {.type = val}
#define PARAM_TYPE_ENUM(type) PARAM_TYPE_##type
#define PARAM_NAME_STR(name) #name

// Generate parameter table with live values (initialized to defaults)
#define PARAM_DEF(name, type, default_val) PARAM_VALUE_INIT(type, default_val),
param_value_t parameter_table[NUM_PARAMS] = {
    PARAM_LIST
};
#undef PARAM_DEF

// Generate default values array
#define PARAM_DEF(name, type, default_val) PARAM_VALUE_INIT(type, default_val),
const param_value_t parameter_defaults[NUM_PARAMS] = {
    PARAM_LIST
};
#undef PARAM_DEF

// Generate parameter types array
#define PARAM_DEF(name, type, default_val) PARAM_TYPE_ENUM(type),
const param_type_e parameter_types[NUM_PARAMS] = {
    PARAM_LIST
};
#undef PARAM_DEF

// Generate parameter names array
#define PARAM_DEF(name, type, default_val) PARAM_NAME_STR(name),
const char* parameter_names[NUM_PARAMS] = {
    PARAM_LIST
};
#undef PARAM_DEF


// Partition pointer
static const esp_partition_t *storage_partition = NULL;

// Log head tracking
static uint32_t log_head_index = 0;
static bool log_initialized = false;

// Calculate offset for log area (after parameters sector)
#define LOG_START_OFFSET FLASH_SECTOR_SIZE

// ============================================================================
// PARAMETER FUNCTIONS
// ============================================================================

param_value_t get_param(param_idx_t id) {
    if (id >= NUM_PARAMS) {
        ESP_LOGE(TAG, "Invalid parameter ID: %d", id);
        param_value_t err = {0};
        return err;
    }
    return parameter_table[id];
}

esp_err_t set_param(param_idx_t id, param_value_t val) {
    if (id >= NUM_PARAMS) {
        ESP_LOGE(TAG, "Invalid parameter ID: %d", id);
        return ESP_ERR_INVALID_ARG;
    }
    parameter_table[id] = val;
    ESP_LOGI(TAG, "Parameter %d (%s) set (not committed)", id, parameter_names[id]);
    return ESP_OK;
}

param_type_e get_param_type(param_idx_t id) {
    if (id >= NUM_PARAMS) {
        return PARAM_TYPE_u64; // Default fallback
    }
    return parameter_types[id];
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

esp_err_t commit_params() {
    if (storage_partition == NULL) {
        ESP_LOGE(TAG, "Storage partition not initialized");
        return ESP_FAIL;
    }
    
    // Prepare storage buffer with parameters and CRCs
    param_stored_t params_to_store[NUM_PARAMS];
    
    for (int i = 0; i < NUM_PARAMS; i++) {
        params_to_store[i].val = parameter_table[i];
        // Calculate CRC32 for each parameter value
        params_to_store[i].crc = esp_crc32_le(0, (uint8_t*)&parameter_table[i], sizeof(param_value_t));
    }
    
    // Erase the first sector (4096 bytes)
    esp_err_t err = esp_partition_erase_range(storage_partition, PARAMS_OFFSET, FLASH_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase parameter sector: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    
    // Write parameters to flash
    err = esp_partition_write(storage_partition, PARAMS_OFFSET, params_to_store, PARAMS_TOTAL_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write parameters: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Parameters committed to flash successfully");
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
    
    ESP_LOGI(TAG, "Storage partition found: size=%lu bytes", (unsigned long)storage_partition->size);
    
    // Load parameters from flash
    param_stored_t params_stored[NUM_PARAMS];
    esp_err_t err = esp_partition_read(storage_partition, PARAMS_OFFSET, 
                                        params_stored, PARAMS_TOTAL_SIZE);
    
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read parameters, using defaults");
        return err;
    }
    
    // Validate and load each parameter
    bool all_valid = true;
    for (int i = 0; i < NUM_PARAMS; i++) {
        uint32_t calculated_crc = esp_crc32_le(0, (uint8_t*)&params_stored[i].val, 
                                                sizeof(param_value_t));
        
        if (calculated_crc == params_stored[i].crc) {
            parameter_table[i] = params_stored[i].val;
        } else {
            ESP_LOGW(TAG, "Parameter %d (%s) failed CRC check, using default", 
                     i, parameter_names[i]);
            all_valid = false;
        }
    }
    
    if (all_valid) {
        ESP_LOGI(TAG, "All parameters loaded successfully from flash");
    } else {
        ESP_LOGW(TAG, "Some parameters failed validation, using defaults");
    }
    
    return ESP_OK;
}

// ============================================================================
// LOGGING FUNCTIONS
// ============================================================================

static esp_err_t find_log_head(void) {
    if (storage_partition == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    
    // Calculate total log area size
    uint32_t log_area_size = storage_partition->size - LOG_START_OFFSET;
    uint32_t max_entries = log_area_size / LOG_ENTRY_SIZE;
    
    // Read through entries to find first uninitialized (all 0xFF)
    uint8_t entry[LOG_ENTRY_SIZE];
    uint8_t empty_entry[LOG_ENTRY_SIZE];
    memset(empty_entry, 0xFF, LOG_ENTRY_SIZE);
    
    // Binary search would be faster, but linear is safer for circular buffer
    for (uint32_t i = 0; i < max_entries; i++) {
        uint32_t offset = LOG_START_OFFSET + (i * LOG_ENTRY_SIZE);
        
        esp_err_t err = esp_partition_read(storage_partition, offset, entry, LOG_ENTRY_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read log entry at index %lu", (unsigned long)i);
            return err;
        }
        
        // Check if this entry is uninitialized
        if (memcmp(entry, empty_entry, LOG_ENTRY_SIZE) == 0) {
            log_head_index = i;
            ESP_LOGI(TAG, "Log head found at index %lu", (unsigned long)log_head_index);
            return ESP_OK;
        }
    }
    
    // If we get here, all entries are full - wrap to beginning
    log_head_index = 0;
    ESP_LOGI(TAG, "Log is full, wrapping to beginning");
    
    // Erase the first log sector to start fresh
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
    
    esp_err_t err = find_log_head();
    if (err != ESP_OK) {
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
    
    uint32_t log_area_end = storage_partition->size;
    uint32_t max_entries = (log_area_end - LOG_START_OFFSET) / LOG_ENTRY_SIZE;
    //uint32_t max_sectors = max_entries / FLASH_SECTOR_SIZE;
    
    // Calculate current offset
    uint32_t current_offset = LOG_START_OFFSET + (log_head_index * LOG_ENTRY_SIZE);
    
    // Check if we need to erase the next sector
    uint32_t current_sector = current_offset / FLASH_SECTOR_SIZE;
    uint32_t next_offset = current_offset + LOG_ENTRY_SIZE;
    if (next_offset >= log_area_end)
    	next_offset = LOG_START_OFFSET;
    uint32_t next_sector = next_offset / FLASH_SECTOR_SIZE;
    
    // If we're crossing into a new sector, check if it needs erasing
    if (next_sector != current_sector) {
        // Check if next sector is uninitialized
        uint8_t check_byte;
        esp_err_t err = esp_partition_read(storage_partition, next_sector * FLASH_SECTOR_SIZE, 
                                            &check_byte, 1);
        
        if (err == ESP_OK && check_byte != 0xFF) {
            // Sector needs erasing
            ESP_LOGI(TAG, "Erasing sector %lu for log", (unsigned long)next_sector);
            err = esp_partition_erase_range(storage_partition, 
                                             next_sector * FLASH_SECTOR_SIZE, 
                                             FLASH_SECTOR_SIZE);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to erase sector: %s", esp_err_to_name(err));
                return ESP_FAIL;
            }
        } else if (err == ESP_OK) {
			ESP_LOGI(TAG, "Next sector %ld clear, no erasing needed", (unsigned long)next_sector);
		} else {
			ESP_LOGE(TAG, "Error checking byte (sector %ld)", (unsigned long)next_sector);
		}
    }
    
    // Write the log entry
    esp_err_t err = esp_partition_write(storage_partition, current_offset, entry, LOG_ENTRY_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write log entry: %s", esp_err_to_name(err));
        return ESP_FAIL;
    }
    
    //ESP_LOGI(TAG, "Log @ sector %lu / index %lu / offset %lu", (unsigned long) current_sector, (unsigned long) log_head_index, (unsigned long)current_offset);
    
    
    log_head_index++;
    if (log_head_index >= max_entries) {
        log_head_index = 0;
        ESP_LOGI(TAG, "Log wrapped to beginning");
    }
    
    return ESP_OK;
}

void storage_deinit(void) {
    storage_partition = NULL;
    log_initialized = false;
}