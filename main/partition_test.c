#include "partition_test.h"
#include "storage.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "PART_TEST"

// ============================================================================
// Test 1: Params partition read/write
// ============================================================================
bool test_params_partition_rw(void) {
    ESP_LOGI(TAG, "=== Test: params partition read/write ===");

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "params");
    if (part == NULL) {
        ESP_LOGE(TAG, "FAIL: params partition not found");
        return false;
    }

    ESP_LOGI(TAG, "params partition: offset=0x%lx size=%lu",
             (unsigned long)part->address, (unsigned long)part->size);

    // Erase first sector
    esp_err_t err = esp_partition_erase_range(part, 0, FLASH_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: erase failed: %s", esp_err_to_name(err));
        return false;
    }

    // Write test pattern
    uint8_t write_buf[32];
    for (int i = 0; i < 32; i++) write_buf[i] = (uint8_t)(0xAA ^ i);

    err = esp_partition_write(part, 0, write_buf, sizeof(write_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: write failed: %s", esp_err_to_name(err));
        return false;
    }

    // Read back and verify
    uint8_t read_buf[32];
    err = esp_partition_read(part, 0, read_buf, sizeof(read_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: read failed: %s", esp_err_to_name(err));
        return false;
    }

    if (memcmp(write_buf, read_buf, sizeof(write_buf)) != 0) {
        ESP_LOGE(TAG, "FAIL: data mismatch");
        return false;
    }

    // Verify erased area reads 0xFF
    uint8_t erased_check;
    err = esp_partition_read(part, 64, &erased_check, 1);
    if (err != ESP_OK || erased_check != 0xFF) {
        ESP_LOGE(TAG, "FAIL: erased area not 0xFF (got 0x%02X)", erased_check);
        return false;
    }

    // Clean up
    esp_partition_erase_range(part, 0, FLASH_SECTOR_SIZE);

    ESP_LOGI(TAG, "PASS");
    return true;
}

// ============================================================================
// Test 2: Log partition read/write
// ============================================================================
bool test_log_partition_rw(void) {
    ESP_LOGI(TAG, "=== Test: log partition read/write ===");

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "log");
    if (part == NULL) {
        ESP_LOGE(TAG, "FAIL: log partition not found");
        return false;
    }

    ESP_LOGI(TAG, "log partition: offset=0x%lx size=%lu",
             (unsigned long)part->address, (unsigned long)part->size);

    // Verify size matches expectations (108K = 27 sectors)
    uint32_t expected_sectors = part->size / FLASH_SECTOR_SIZE;
    ESP_LOGI(TAG, "log partition has %lu sectors", (unsigned long)expected_sectors);

    // Test write at start of partition
    esp_err_t err = esp_partition_erase_range(part, 0, FLASH_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: erase sector 0 failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t write_buf[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04,
                              0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C};
    err = esp_partition_write(part, 0, write_buf, sizeof(write_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: write failed: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t read_buf[16];
    err = esp_partition_read(part, 0, read_buf, sizeof(read_buf));
    if (err != ESP_OK || memcmp(write_buf, read_buf, sizeof(write_buf)) != 0) {
        ESP_LOGE(TAG, "FAIL: read-back mismatch at offset 0");
        return false;
    }

    // Test write at last sector
    uint32_t last_sector = part->size - FLASH_SECTOR_SIZE;
    err = esp_partition_erase_range(part, last_sector, FLASH_SECTOR_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: erase last sector failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_partition_write(part, last_sector, write_buf, sizeof(write_buf));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: write to last sector failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_partition_read(part, last_sector, read_buf, sizeof(read_buf));
    if (err != ESP_OK || memcmp(write_buf, read_buf, sizeof(write_buf)) != 0) {
        ESP_LOGE(TAG, "FAIL: read-back mismatch at last sector");
        return false;
    }

    // Clean up
    esp_partition_erase_range(part, 0, FLASH_SECTOR_SIZE);
    esp_partition_erase_range(part, last_sector, FLASH_SECTOR_SIZE);

    ESP_LOGI(TAG, "PASS");
    return true;
}

// ============================================================================
// Test 3: POST test partition read/write
// ============================================================================
bool test_post_partition_rw(void) {
    ESP_LOGI(TAG, "=== Test: post_test partition read/write ===");

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "post_test");
    if (part == NULL) {
        ESP_LOGE(TAG, "FAIL: post_test partition not found");
        return false;
    }

    ESP_LOGI(TAG, "post_test partition: offset=0x%lx size=%lu",
             (unsigned long)part->address, (unsigned long)part->size);

    // Verify it's exactly 4K (1 sector)
    if (part->size != FLASH_SECTOR_SIZE) {
        ESP_LOGE(TAG, "FAIL: expected 4096 bytes, got %lu", (unsigned long)part->size);
        return false;
    }

    // Run the actual storage_post() function
    esp_err_t err = storage_post();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: storage_post() returned %s", esp_err_to_name(err));
        return false;
    }

    // Verify the sector is clean after POST (it erases on completion)
    uint8_t check;
    err = esp_partition_read(part, 0, &check, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: read after POST failed");
        return false;
    }
    if (check != 0xFF) {
        ESP_LOGE(TAG, "FAIL: POST didn't clean up (byte 0 = 0x%02X)", check);
        return false;
    }

    ESP_LOGI(TAG, "PASS");
    return true;
}

// ============================================================================
// Test 4: Partitions are independent (writing to one doesn't corrupt another)
// ============================================================================
bool test_partitions_independent(void) {
    ESP_LOGI(TAG, "=== Test: partition independence ===");

    const esp_partition_t *params = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "params");
    const esp_partition_t *log = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "log");
    const esp_partition_t *post = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "post_test");

    if (!params || !log || !post) {
        ESP_LOGE(TAG, "FAIL: one or more partitions not found");
        return false;
    }

    // Verify no overlap
    uint32_t params_end = params->address + params->size;
    uint32_t log_end = log->address + log->size;
    uint32_t post_end = post->address + post->size;

    ESP_LOGI(TAG, "params: 0x%lx - 0x%lx", (unsigned long)params->address, (unsigned long)params_end);
    ESP_LOGI(TAG, "log:    0x%lx - 0x%lx", (unsigned long)log->address, (unsigned long)log_end);
    ESP_LOGI(TAG, "post:   0x%lx - 0x%lx", (unsigned long)post->address, (unsigned long)post_end);

    bool overlap = false;
    if (params->address < log_end && log->address < params_end) overlap = true;
    if (params->address < post_end && post->address < params_end) overlap = true;
    if (log->address < post_end && post->address < log_end) overlap = true;

    if (overlap) {
        ESP_LOGE(TAG, "FAIL: partitions overlap!");
        return false;
    }

    // Write a sentinel to each partition, then verify none were corrupted
    esp_partition_erase_range(params, 0, FLASH_SECTOR_SIZE);
    esp_partition_erase_range(log, 0, FLASH_SECTOR_SIZE);
    esp_partition_erase_range(post, 0, FLASH_SECTOR_SIZE);

    uint8_t pat_params[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t pat_log[4]    = {0x55, 0x66, 0x77, 0x88};
    uint8_t pat_post[4]   = {0x99, 0xAA, 0xBB, 0xCC};

    esp_partition_write(params, 0, pat_params, 4);
    esp_partition_write(log, 0, pat_log, 4);
    esp_partition_write(post, 0, pat_post, 4);

    // Read back all three and verify
    uint8_t rb[4];

    esp_partition_read(params, 0, rb, 4);
    if (memcmp(rb, pat_params, 4) != 0) {
        ESP_LOGE(TAG, "FAIL: params sentinel corrupted");
        return false;
    }

    esp_partition_read(log, 0, rb, 4);
    if (memcmp(rb, pat_log, 4) != 0) {
        ESP_LOGE(TAG, "FAIL: log sentinel corrupted");
        return false;
    }

    esp_partition_read(post, 0, rb, 4);
    if (memcmp(rb, pat_post, 4) != 0) {
        ESP_LOGE(TAG, "FAIL: post sentinel corrupted");
        return false;
    }

    // Clean up
    esp_partition_erase_range(params, 0, FLASH_SECTOR_SIZE);
    esp_partition_erase_range(log, 0, FLASH_SECTOR_SIZE);
    esp_partition_erase_range(post, 0, FLASH_SECTOR_SIZE);

    ESP_LOGI(TAG, "PASS");
    return true;
}

// ============================================================================
// Test 5: Parameter commit and reload
// ============================================================================
bool test_params_persist_after_commit(void) {
    ESP_LOGI(TAG, "=== Test: params persist after commit ===");

    // Save original value
    param_value_t original = get_param_value_t(PARAM_DRIVE_DIST);
    float orig_val = original.f32;

    // Set a distinctive test value
    float test_val = 99.99f;
    param_value_t test = {.f32 = test_val};
    esp_err_t err = set_param_value_t(PARAM_DRIVE_DIST, test);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: set_param_value_t failed: %s", esp_err_to_name(err));
        return false;
    }

    // Commit to flash
    err = commit_params();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: commit_params failed: %s", esp_err_to_name(err));
        return false;
    }

    // Verify the value stuck in RAM
    param_value_t readback = get_param_value_t(PARAM_DRIVE_DIST);
    if (readback.f32 != test_val) {
        ESP_LOGE(TAG, "FAIL: RAM value mismatch (got %.2f, expected %.2f)",
                 readback.f32, test_val);
        return false;
    }

    // Re-init storage to force reload from flash
    err = storage_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: storage_init failed on reload: %s", esp_err_to_name(err));
        return false;
    }

    readback = get_param_value_t(PARAM_DRIVE_DIST);
    if (readback.f32 != test_val) {
        ESP_LOGE(TAG, "FAIL: flash value mismatch after reload (got %.2f, expected %.2f)",
                 readback.f32, test_val);
        // Restore original before returning
        param_value_t orig = {.f32 = orig_val};
        set_param_value_t(PARAM_DRIVE_DIST, orig);
        commit_params();
        return false;
    }

    // Restore original value
    param_value_t orig = {.f32 = orig_val};
    set_param_value_t(PARAM_DRIVE_DIST, orig);
    err = commit_params();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WARN: failed to restore original value");
    }

    ESP_LOGI(TAG, "PASS");
    return true;
}

// ============================================================================
// Test 6: Log write/read cycle through the log API
// ============================================================================
bool test_log_write_read_cycle(void) {
    ESP_LOGI(TAG, "=== Test: log write/read cycle ===");

    // Erase and reinit log
    esp_err_t err = log_erase_all_sectors();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: log_erase_all_sectors failed: %s", esp_err_to_name(err));
        return false;
    }

    esp_task_wdt_reset();

    // Write 3 entries with different types
    uint8_t data1[] = {0x01, 0x02, 0x03, 0x04};
    uint8_t data2[] = {0xAA, 0xBB, 0xCC};
    uint8_t data3[] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB};

    err = log_write_blocking_test(data1, sizeof(data1), LOG_TYPE_DATA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: write 1 failed: %s", esp_err_to_name(err));
        return false;
    }

    err = log_write_blocking_test(data2, sizeof(data2), LOG_TYPE_EVENT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: write 2 failed: %s", esp_err_to_name(err));
        return false;
    }

    err = log_write_blocking_test(data3, sizeof(data3), LOG_TYPE_SENSOR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: write 3 failed: %s", esp_err_to_name(err));
        return false;
    }

    // Wait for writes to flush through the queue
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_task_wdt_reset();

    // Read them back
    log_read_reset();
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;

    // Entry 1
    err = log_read(&read_len, read_buf, &read_type);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: read 1 failed: %s", esp_err_to_name(err));
        return false;
    }
    if (read_len != sizeof(data1) || read_type != LOG_TYPE_DATA ||
        memcmp(read_buf, data1, sizeof(data1)) != 0) {
        ESP_LOGE(TAG, "FAIL: entry 1 mismatch (len=%d type=0x%02X)", read_len, read_type);
        return false;
    }

    // Entry 2
    err = log_read(&read_len, read_buf, &read_type);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: read 2 failed: %s", esp_err_to_name(err));
        return false;
    }
    if (read_len != sizeof(data2) || read_type != LOG_TYPE_EVENT ||
        memcmp(read_buf, data2, sizeof(data2)) != 0) {
        ESP_LOGE(TAG, "FAIL: entry 2 mismatch (len=%d type=0x%02X)", read_len, read_type);
        return false;
    }

    // Entry 3
    err = log_read(&read_len, read_buf, &read_type);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FAIL: read 3 failed: %s", esp_err_to_name(err));
        return false;
    }
    if (read_len != sizeof(data3) || read_type != LOG_TYPE_SENSOR ||
        memcmp(read_buf, data3, sizeof(data3)) != 0) {
        ESP_LOGE(TAG, "FAIL: entry 3 mismatch (len=%d type=0x%02X)", read_len, read_type);
        return false;
    }

    // Verify no more entries
    err = log_read(&read_len, read_buf, &read_type);
    if (err != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "FAIL: expected no more entries, got err=%s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "PASS");
    return true;
}

// ============================================================================
// Test runner
// ============================================================================
esp_err_t run_partition_tests(void) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "    PARTITION VERIFICATION TEST SUITE");
    ESP_LOGI(TAG, "=================================================");

    typedef struct {
        const char *name;
        bool (*fn)(void);
    } test_entry_t;

    test_entry_t tests[] = {
        {"params partition r/w",       test_params_partition_rw},
        {"log partition r/w",          test_log_partition_rw},
        {"post_test partition r/w",    test_post_partition_rw},
        {"partition independence",     test_partitions_independent},
        {"params persist after commit", test_params_persist_after_commit},
        {"log write/read cycle",       test_log_write_read_cycle},
    };

    int num_tests = sizeof(tests) / sizeof(tests[0]);
    int passed = 0;

    for (int i = 0; i < num_tests; i++) {
        esp_task_wdt_reset();
        ESP_LOGI(TAG, "");
        bool result = tests[i].fn();
        if (result) passed++;
        else ESP_LOGE(TAG, "FAILED: %s", tests[i].name);
        esp_task_wdt_reset();
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "    RESULTS: %d/%d passed", passed, num_tests);
    ESP_LOGI(TAG, "=================================================");

    return (passed == num_tests) ? ESP_OK : ESP_FAIL;
}
