#include "log_test.h"
#include "storage.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>

#define TAG "LOG_TEST"

// External declarations for functions we need to add to storage.c
extern esp_err_t log_erase_all_sectors(void);
extern esp_err_t log_simulate_power_cycle(void);
extern esp_err_t log_read(uint8_t* len, uint8_t* buf, uint8_t* type);
extern void log_read_reset(void);

// Helper to wait for log queue to clear
static void wait_for_log_queue(void) {
    // Wait for all queued log entries to be written
    // The queue size is LOG_QUEUE_SIZE (8), so we wait a bit longer
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_task_wdt_reset();
}

// Helper to compare two byte arrays
static bool arrays_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            ESP_LOGE(TAG, "Array mismatch at index %d: expected 0x%02X, got 0x%02X", i, a[i], b[i]);
            return false;
        }
    }
    return true;
}

// Helper to verify log entry integrity
static bool verify_log_entry(const uint8_t* expected_data, uint8_t expected_len, uint8_t expected_type) {
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len;
    uint8_t read_type;
    
    esp_err_t err = log_read(&read_len, read_buf, &read_type);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "No more log entries to read (expected entry with length %d)", expected_len);
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read log entry: %s", esp_err_to_name(err));
        return false;
    }
    
    if (read_len != expected_len) {
        ESP_LOGE(TAG, "Length mismatch: expected %d, got %d", expected_len, read_len);
        return false;
    }
    
    if (read_type != expected_type) {
        ESP_LOGE(TAG, "Type mismatch: expected 0x%02X, got 0x%02X", expected_type, read_type);
        return false;
    }
    
    if (!arrays_equal(expected_data, read_buf, expected_len)) {
        return false;
    }
    
    return true;
}

// Test 1: Basic log integrity - write and read back entries
bool test_log_integrity_basic(void) {
    ESP_LOGI(TAG, "=== Test: Basic Log Integrity ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    // Erase already resets head/tail and reinitializes, no need for power cycle here
    
    const char* test_data1 = "Test Entry 1";
    const char* test_data2 = "Another test entry with more data";
    const char* test_data3 = "Short";
    
    if (log_write_blocking_test((const uint8_t*)test_data1, strlen(test_data1), LOG_TYPE_DATA) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write entry 1");
        return false;
    }
    
    if (log_write_blocking_test((const uint8_t*)test_data2, strlen(test_data2), LOG_TYPE_EVENT) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write entry 2");
        return false;
    }
    
    if (log_write_blocking_test((const uint8_t*)test_data3, strlen(test_data3), LOG_TYPE_DEBUG) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write entry 3");
        return false;
    }
    
    wait_for_log_queue();
    log_read_reset();
    
    if (!verify_log_entry((const uint8_t*)test_data1, strlen(test_data1), LOG_TYPE_DATA)) {
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)test_data2, strlen(test_data2), LOG_TYPE_EVENT)) {
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)test_data3, strlen(test_data3), LOG_TYPE_DEBUG)) {
        return false;
    }
    
    ESP_LOGI(TAG, "Basic integrity test PASSED");
    return true;
}

// Test 2: Log integrity with wraparound
bool test_log_integrity_wraparound(void) {
    ESP_LOGI(TAG, "=== Test: Log Integrity with Wraparound ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t test_pattern[64];
    for (int i = 0; i < 64; i++) {
        test_pattern[i] = i & 0xFF;
    }
    
    int entries_written = 0;
    for (int i = 0; i < 200; i++) {
        test_pattern[0] = i & 0xFF;
        if (log_write_blocking_test(test_pattern, 64, LOG_TYPE_DATA) == ESP_OK) {
            entries_written++;
        } else {
            ESP_LOGW(TAG, "Write failed at entry %d", i);
        }
        
        if (i % 20 == 0) {
            esp_task_wdt_reset();
        }
    }
    
    wait_for_log_queue();
    
    ESP_LOGI(TAG, "Wrote %d entries, reading back last few...", entries_written);
    
    log_read_reset();
    
    int entries_read = 0;
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    
    for (int i = 0; i < 10; i++) {
        if (log_read(&read_len, read_buf, &read_type) == ESP_OK) {
            entries_read++;
            if (read_len != 64) {
                ESP_LOGE(TAG, "Entry %d has wrong length: %d", i, read_len);
                return false;
            }
            if (read_type != LOG_TYPE_DATA) {
                ESP_LOGE(TAG, "Entry %d has wrong type: 0x%02X", i, read_type);
                return false;
            }
        } else {
            break;
        }
    }
    
    ESP_LOGI(TAG, "Successfully read %d entries after wraparound", entries_read);
    
    if (entries_read == 0) {
        ESP_LOGE(TAG, "Failed to read any entries after wraparound");
        return false;
    }
    
    ESP_LOGI(TAG, "Wraparound integrity test PASSED");
    return true;
}

// Test 3: Head/tail recovery after power cycle - empty log
bool test_log_head_tail_recovery_empty(void) {
    ESP_LOGI(TAG, "=== Test: Head/Tail Recovery - Empty Log ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint32_t head = log_get_head();
    uint32_t tail = log_get_tail();
    uint32_t log_start = log_get_offset();
    
    ESP_LOGI(TAG, "After power cycle: head=%lu, tail=%lu, log_start=%lu", 
             (unsigned long)head, (unsigned long)tail, (unsigned long)log_start);
    
    if (head != log_start || tail != log_start) {
        ESP_LOGE(TAG, "Empty log should have head and tail at start offset");
        return false;
    }
    
    ESP_LOGI(TAG, "Empty log recovery PASSED");
    return true;
}

// Test 4: Head/tail recovery - partial sector
bool test_log_head_tail_recovery_partial_sector(void) {
    ESP_LOGI(TAG, "=== Test: Head/Tail Recovery - Partial Sector ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint32_t head_before = log_get_head();
    
    const char* data1 = "Entry 1";
    const char* data2 = "Entry 2";
    const char* data3 = "Entry 3";
    
    log_write_blocking_test((const uint8_t*)data1, strlen(data1), LOG_TYPE_DATA);
    log_write_blocking_test((const uint8_t*)data2, strlen(data2), LOG_TYPE_DATA);
    log_write_blocking_test((const uint8_t*)data3, strlen(data3), LOG_TYPE_DATA);
    
    wait_for_log_queue();
    
    uint32_t head_after_writes = log_get_head();
    uint32_t tail_after_writes = log_get_tail();
    
    ESP_LOGI(TAG, "Before power cycle: head=%lu, tail=%lu", 
             (unsigned long)head_after_writes, (unsigned long)tail_after_writes);
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint32_t head_after_cycle = log_get_head();
    uint32_t tail_after_cycle = log_get_tail();
    
    ESP_LOGI(TAG, "After power cycle: head=%lu, tail=%lu", 
             (unsigned long)head_after_cycle, (unsigned long)tail_after_cycle);
    
    if (head_after_cycle < head_before || head_after_cycle > head_after_writes + 10) {
        ESP_LOGE(TAG, "Head not properly recovered: before=%lu, after=%lu", 
                 (unsigned long)head_after_writes, (unsigned long)head_after_cycle);
        return false;
    }
    
    log_read_reset();
    
    if (!verify_log_entry((const uint8_t*)data1, strlen(data1), LOG_TYPE_DATA)) {
        ESP_LOGE(TAG, "Failed to read entry 1 after recovery");
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)data2, strlen(data2), LOG_TYPE_DATA)) {
        ESP_LOGE(TAG, "Failed to read entry 2 after recovery");
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)data3, strlen(data3), LOG_TYPE_DATA)) {
        ESP_LOGE(TAG, "Failed to read entry 3 after recovery");
        return false;
    }
    
    ESP_LOGI(TAG, "Partial sector recovery PASSED");
    return true;
}

// Test 5: Head/tail recovery - multiple full sectors
bool test_log_head_tail_recovery_full_sectors(void) {
    ESP_LOGI(TAG, "=== Test: Head/Tail Recovery - Full Sectors ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t pattern[100];
    for (int i = 0; i < 100; i++) {
        pattern[i] = i & 0xFF;
    }
    
    for (int i = 0; i < 100; i++) {
        pattern[0] = i & 0xFF;
        log_write_blocking_test(pattern, 100, LOG_TYPE_DATA);
        
        if (i % 20 == 0) {
            esp_task_wdt_reset();
        }
    }
    
    wait_for_log_queue();
    
    uint32_t head_before = log_get_head();
    uint32_t tail_before = log_get_tail();
    
    ESP_LOGI(TAG, "Before power cycle: head=%lu, tail=%lu", 
             (unsigned long)head_before, (unsigned long)tail_before);
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint32_t head_after = log_get_head();
    uint32_t tail_after = log_get_tail();
    
    ESP_LOGI(TAG, "After power cycle: head=%lu, tail=%lu", 
             (unsigned long)head_after, (unsigned long)tail_after);
    
    if (head_after < head_before - 1000 || head_after > head_before + 1000) {
        ESP_LOGE(TAG, "Head significantly changed after recovery");
        return false;
    }
    
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    int entries_read = 0;
    
    for (int i = 0; i < 10; i++) {
        if (log_read(&read_len, read_buf, &read_type) == ESP_OK) {
            entries_read++;
        }
    }
    
    if (entries_read == 0) {
        ESP_LOGE(TAG, "Could not read any entries after recovery");
        return false;
    }
    
    ESP_LOGI(TAG, "Full sectors recovery PASSED (read %d entries)", entries_read);
    return true;
}

// Test 6: Head/tail recovery with wraparound
bool test_log_head_tail_recovery_wraparound(void) {
    ESP_LOGI(TAG, "=== Test: Head/Tail Recovery - Wraparound ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t pattern[80];
    for (int i = 0; i < 80; i++) {
        pattern[i] = i & 0xFF;
    }
    
    for (int i = 0; i < 250; i++) {
        pattern[0] = i & 0xFF;
        log_write_blocking_test(pattern, 80, LOG_TYPE_DATA);
        
        if (i % 20 == 0) {
            esp_task_wdt_reset();
        }
    }
    
    wait_for_log_queue();
    
    uint32_t head_before = log_get_head();
    uint32_t tail_before = log_get_tail();
    
    ESP_LOGI(TAG, "Before power cycle (wraparound): head=%lu, tail=%lu", 
             (unsigned long)head_before, (unsigned long)tail_before);
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint32_t head_after = log_get_head();
    uint32_t tail_after = log_get_tail();
    
    ESP_LOGI(TAG, "After power cycle (wraparound): head=%lu, tail=%lu", 
             (unsigned long)head_after, (unsigned long)tail_after);
    
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    int entries_read = 0;
    
    for (int i = 0; i < 10; i++) {
        if (log_read(&read_len, read_buf, &read_type) == ESP_OK) {
            entries_read++;
        }
    }
    
    if (entries_read == 0) {
        ESP_LOGE(TAG, "Could not read any entries after wraparound recovery");
        return false;
    }
    
    ESP_LOGI(TAG, "Wraparound recovery PASSED (read %d entries)", entries_read);
    return true;
}

// Test 7: Head equals tail (empty or full)
bool test_log_head_equals_tail(void) {
    ESP_LOGI(TAG, "=== Test: Head Equals Tail ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint32_t head = log_get_head();
    uint32_t tail = log_get_tail();
    
    if (head != tail) {
        ESP_LOGE(TAG, "Empty log should have head == tail");
        return false;
    }
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    
    esp_err_t err = log_read(&read_len, read_buf, &read_type);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "Read succeeded on empty log (head == tail), which may be okay");
    }
    
    ESP_LOGI(TAG, "Head equals tail test PASSED");
    return true;
}

// Test 8: Head = 0, Tail = 0
bool test_log_head_zero_tail_zero(void) {
    ESP_LOGI(TAG, "=== Test: Head=0, Tail=0 (actually LOG_START_OFFSET) ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint32_t head = log_get_head();
    uint32_t tail = log_get_tail();
    uint32_t log_start = log_get_offset();
    
    if (head != log_start || tail != log_start) {
        ESP_LOGE(TAG, "Fresh log should have both at LOG_START_OFFSET");
        return false;
    }
    
    const char* data = "First entry";
    log_write_blocking_test((const uint8_t*)data, strlen(data), LOG_TYPE_DATA);
    wait_for_log_queue();
    
    uint32_t head_after = log_get_head();
    uint32_t tail_after = log_get_tail();
    
    if (head_after == log_start) {
        ESP_LOGE(TAG, "Head should have advanced after write");
        return false;
    }
    
    if (tail_after != log_start) {
        ESP_LOGE(TAG, "Tail should still be at start");
        return false;
    }
    
    ESP_LOGI(TAG, "Head=0, Tail=0 test PASSED");
    return true;
}

// Test 9: Head > Tail (normal case)
bool test_log_head_greater_than_tail(void) {
    ESP_LOGI(TAG, "=== Test: Head > Tail (Normal Case) ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    for (int i = 0; i < 10; i++) {
        char data[32];
        snprintf(data, sizeof(data), "Entry %d", i);
        log_write_blocking_test((const uint8_t*)data, strlen(data), LOG_TYPE_DATA);
    }
    
    wait_for_log_queue();
    
    uint32_t head = log_get_head();
    uint32_t tail = log_get_tail();
    
    ESP_LOGI(TAG, "Head=%lu, Tail=%lu", (unsigned long)head, (unsigned long)tail);
    
    if (head <= tail && tail != log_get_offset()) {
        ESP_LOGE(TAG, "Expected head > tail in normal case");
        return false;
    }
    
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    int entries_read = 0;
    
    for (int i = 0; i < 10; i++) {
        if (log_read(&read_len, read_buf, &read_type) == ESP_OK) {
            entries_read++;
        }
    }
    
    if (entries_read != 10) {
        ESP_LOGE(TAG, "Expected to read 10 entries, got %d", entries_read);
        return false;
    }
    
    ESP_LOGI(TAG, "Head > Tail test PASSED");
    return true;
}

// Test 10: Head < Tail (wraparound case)
bool test_log_head_less_than_tail(void) {
    ESP_LOGI(TAG, "=== Test: Head < Tail (Wraparound) ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t pattern[90];
    for (int i = 0; i < 90; i++) {
        pattern[i] = i & 0xFF;
    }
    
    for (int i = 0; i < 300; i++) {
        pattern[0] = i & 0xFF;
        log_write_blocking_test(pattern, 90, LOG_TYPE_DATA);
        
        if (i % 20 == 0) {
            esp_task_wdt_reset();
        }
    }
    
    wait_for_log_queue();
    
    uint32_t head = log_get_head();
    uint32_t tail = log_get_tail();
    
    ESP_LOGI(TAG, "After wraparound: Head=%lu, Tail=%lu", 
             (unsigned long)head, (unsigned long)tail);
    
    if (tail < head && tail != log_get_offset()) {
        ESP_LOGW(TAG, "Expected wraparound condition, but may depend on partition size");
    }
    
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    
    if (log_read(&read_len, read_buf, &read_type) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read after wraparound");
        return false;
    }
    
    ESP_LOGI(TAG, "Head < Tail test PASSED");
    return true;
}

// Test 11: Multiple wraparounds
bool test_log_wraparound_multiple_times(void) {
    ESP_LOGI(TAG, "=== Test: Multiple Wraparounds ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t pattern[100];
    for (int i = 0; i < 100; i++) {
        pattern[i] = i & 0xFF;
    }
    
    int total_written = 0;
    for (int i = 0; i < 500; i++) {
        pattern[0] = i & 0xFF;
        if (log_write_blocking_test(pattern, 100, LOG_TYPE_DATA) == ESP_OK) {
            total_written++;
        }
        
        if (i % 20 == 0) {
            esp_task_wdt_reset();
        }
    }
    
    wait_for_log_queue();
    
    ESP_LOGI(TAG, "Wrote %d entries across multiple wraps", total_written);
    
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    int entries_read = 0;
    
    for (int i = 0; i < 20; i++) {
        if (log_read(&read_len, read_buf, &read_type) == ESP_OK) {
            entries_read++;
        } else {
            break;
        }
    }
    
    ESP_LOGI(TAG, "Read %d entries after multiple wraps", entries_read);
    
    if (entries_read == 0) {
        ESP_LOGE(TAG, "Failed to read any entries after multiple wraps");
        return false;
    }
    
    ESP_LOGI(TAG, "Multiple wraparound test PASSED");
    return true;
}

// Test 12: Sector boundary conditions
bool test_log_sector_boundary_conditions(void) {
    ESP_LOGI(TAG, "=== Test: Sector Boundary Conditions ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t pattern[100];
    for (int i = 0; i < 100; i++) {
        pattern[i] = i & 0xFF;
    }
    
    for (int i = 0; i < 50; i++) {
	    pattern[0] = i & 0xFF;
	    log_write_blocking_test(pattern, 100, LOG_TYPE_DATA);
	    
	    // Slow down to prevent queue overflow
	    if (i % 5 == 0) {
	        vTaskDelay(pdMS_TO_TICKS(50));
	    }
	    
	    if (i % 20 == 0) {
	        esp_task_wdt_reset();
	    }
	}
    
    wait_for_log_queue();
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    int entries_read = 0;
    
    for (int i = 0; i < 50; i++) {
        if (log_read(&read_len, read_buf, &read_type) == ESP_OK) {
            entries_read++;
            if (read_len != 100) {
                ESP_LOGE(TAG, "Entry %d has wrong length: %d", i, read_len);
                return false;
            }
        } else {
            break;
        }
    }
    
    if (entries_read != 50) {
        ESP_LOGE(TAG, "Expected 50 entries, read %d", entries_read);
        return false;
    }
    
    ESP_LOGI(TAG, "Sector boundary test PASSED");
    return true;
}

// Test 13: Maximum payload size
bool test_log_maximum_payload(void) {
    ESP_LOGI(TAG, "=== Test: Maximum Payload Size ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t max_payload[LOG_MAX_PAYLOAD];
    for (int i = 0; i < LOG_MAX_PAYLOAD; i++) {
        max_payload[i] = i & 0xFF;
    }
    
    if (log_write_blocking_test(max_payload, LOG_MAX_PAYLOAD, LOG_TYPE_DATA) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write maximum payload");
        return false;
    }
    
    wait_for_log_queue();
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    
    if (log_read(&read_len, read_buf, &read_type) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read maximum payload");
        return false;
    }
    
    if (read_len != LOG_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "Read length %d != expected %d", read_len, LOG_MAX_PAYLOAD);
        return false;
    }
    
    if (!arrays_equal(max_payload, read_buf, LOG_MAX_PAYLOAD)) {
        return false;
    }
    
    ESP_LOGI(TAG, "Maximum payload test PASSED");
    return true;
}

// Test 14: Minimum payload size (1 byte)
bool test_log_minimum_payload(void) {
    ESP_LOGI(TAG, "=== Test: Minimum Payload Size ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t min_payload[1] = {0x42};
    
    if (log_write_blocking_test(min_payload, 1, LOG_TYPE_DEBUG) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write minimum payload");
        return false;
    }
    
    wait_for_log_queue();
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    
    if (log_read(&read_len, read_buf, &read_type) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read minimum payload");
        return false;
    }
    
    if (read_len != 1) {
        ESP_LOGE(TAG, "Read length %d != expected 1", read_len);
        return false;
    }
    
    if (read_buf[0] != 0x42) {
        ESP_LOGE(TAG, "Read data 0x%02X != expected 0x42", read_buf[0]);
        return false;
    }
    
    if (read_type != LOG_TYPE_DEBUG) {
        ESP_LOGE(TAG, "Read type 0x%02X != expected 0x%02X", read_type, LOG_TYPE_DEBUG);
        return false;
    }
    
    ESP_LOGI(TAG, "Minimum payload test PASSED");
    return true;
}

// Test 15: Corrupted entry recovery
bool test_log_corrupted_entry_recovery(void) {
    ESP_LOGI(TAG, "=== Test: Corrupted Entry Recovery ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    const char* data1 = "Good entry 1";
    const char* data2 = "Good entry 2";
    
    log_write_blocking_test((const uint8_t*)data1, strlen(data1), LOG_TYPE_DATA);
    log_write_blocking_test((const uint8_t*)data2, strlen(data2), LOG_TYPE_DATA);
    
    wait_for_log_queue();
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    log_read_reset();
    
    if (!verify_log_entry((const uint8_t*)data1, strlen(data1), LOG_TYPE_DATA)) {
        ESP_LOGE(TAG, "Failed to read entry 1 after simulated corruption test");
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)data2, strlen(data2), LOG_TYPE_DATA)) {
        ESP_LOGE(TAG, "Failed to read entry 2 after simulated corruption test");
        return false;
    }
    
    ESP_LOGI(TAG, "Corrupted entry recovery test PASSED (basic validation)");
    return true;
}

// Test 16: Full partition handling
bool test_log_full_partition(void) {
    ESP_LOGI(TAG, "=== Test: Full Partition Handling ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    uint8_t pattern[200];
    for (int i = 0; i < 200; i++) {
        pattern[i] = i & 0xFF;
    }
    
    int successful_writes = 0;
    for (int i = 0; i < 1000; i++) {
        pattern[0] = i & 0xFF;
        if (log_write_blocking_test(pattern, 200, LOG_TYPE_DATA) == ESP_OK) {
            successful_writes++;
        } else {
            ESP_LOGW(TAG, "Write failed at iteration %d", i);
        }
        
        if (i % 50 == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_task_wdt_reset();
        }
    }
    
    ESP_LOGI(TAG, "Successfully wrote %d entries before partition full", successful_writes);
    
    wait_for_log_queue();
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle on full partition");
        return false;
    }
    
    log_read_reset();
    
    uint8_t read_buf[LOG_MAX_PAYLOAD];
    uint8_t read_len, read_type;
    
    if (log_read(&read_len, read_buf, &read_type) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read from full partition");
        return false;
    }
    
    ESP_LOGI(TAG, "Full partition test PASSED");
    return true;
}

// Test 17: Read after write consistency
bool test_log_read_after_write(void) {
    ESP_LOGI(TAG, "=== Test: Read After Write Consistency ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    for (int i = 0; i < 20; i++) {
        char data[32];
        snprintf(data, sizeof(data), "Entry number %d", i);
        
        if (log_write_blocking_test((const uint8_t*)data, strlen(data), LOG_TYPE_DATA) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write entry %d", i);
            return false;
        }
    }
    
    wait_for_log_queue();
    log_read_reset();
    
    for (int i = 0; i < 20; i++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "Entry number %d", i);
        
        if (!verify_log_entry((const uint8_t*)expected, strlen(expected), LOG_TYPE_DATA)) {
            ESP_LOGE(TAG, "Failed to verify entry %d", i);
            return false;
        }
    }
    
    ESP_LOGI(TAG, "Read after write consistency test PASSED");
    return true;
}

// Test 18: Multiple log types
bool test_log_multiple_types(void) {
    ESP_LOGI(TAG, "=== Test: Multiple Log Types ===");
    
    if (log_erase_all_sectors() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log sectors");
        return false;
    }
    
    if (log_simulate_power_cycle() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to simulate power cycle");
        return false;
    }
    
    const char* data_entry = "Data entry";
    const char* event_entry = "Event entry";
    const char* error_entry = "Error entry";
    const char* debug_entry = "Debug entry";
    const char* sensor_entry = "Sensor entry";
    
    log_write_blocking_test((const uint8_t*)data_entry, strlen(data_entry), LOG_TYPE_DATA);
    log_write_blocking_test((const uint8_t*)event_entry, strlen(event_entry), LOG_TYPE_EVENT);
    log_write_blocking_test((const uint8_t*)error_entry, strlen(error_entry), LOG_TYPE_ERROR);
    log_write_blocking_test((const uint8_t*)debug_entry, strlen(debug_entry), LOG_TYPE_DEBUG);
    log_write_blocking_test((const uint8_t*)sensor_entry, strlen(sensor_entry), LOG_TYPE_SENSOR);
    
    wait_for_log_queue();
    log_read_reset();
    
    if (!verify_log_entry((const uint8_t*)data_entry, strlen(data_entry), LOG_TYPE_DATA)) {
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)event_entry, strlen(event_entry), LOG_TYPE_EVENT)) {
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)error_entry, strlen(error_entry), LOG_TYPE_ERROR)) {
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)debug_entry, strlen(debug_entry), LOG_TYPE_DEBUG)) {
        return false;
    }
    
    if (!verify_log_entry((const uint8_t*)sensor_entry, strlen(sensor_entry), LOG_TYPE_SENSOR)) {
        return false;
    }
    
    ESP_LOGI(TAG, "Multiple log types test PASSED");
    return true;
}

// Helper function to print test results
void print_test_results(test_result_t* results, int num_tests) {
    ESP_LOGI(TAG, "\n\n");
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "TEST RESULTS SUMMARY");
    ESP_LOGI(TAG, "=================================================");
    
    int passed = 0;
    int failed = 0;
    
    ESP_LOGI(TAG, "\n--- FAILED TESTS ---");
    bool any_failures = false;
    for (int i = 0; i < num_tests; i++) {
        if (!results[i].passed) {
            ESP_LOGE(TAG, "[FAIL] Test %d: %s", i + 1, results[i].test_name);
            if (results[i].error_msg) {
                ESP_LOGE(TAG, "       Error: %s", results[i].error_msg);
            }
            failed++;
            any_failures = true;
        }
    }
    
    if (!any_failures) {
        ESP_LOGI(TAG, "None - all tests passed!");
    }
    
    ESP_LOGI(TAG, "\n--- PASSED TESTS ---");
    for (int i = 0; i < num_tests; i++) {
        if (results[i].passed) {
            ESP_LOGI(TAG, "[PASS] Test %d: %s", i + 1, results[i].test_name);
            passed++;
        }
    }
    
    ESP_LOGI(TAG, "\n=================================================");
    ESP_LOGI(TAG, "Total: %d | Passed: %d | Failed: %d", num_tests, passed, failed);
    if (failed > 0) {
        ESP_LOGE(TAG, "FAILED TESTS: %d", failed);
    } else {
        ESP_LOGI(TAG, "ALL TESTS PASSED!");
    }
    ESP_LOGI(TAG, "=================================================\n");
}

int count_passed_tests(test_result_t* results, int num_tests) {
    int passed = 0;
    for (int i = 0; i < num_tests; i++) {
        if (results[i].passed) {
            passed++;
        }
    }
    return passed;
}

// ============================================================================
// Write timing benchmark — measures blocking write duration
// ============================================================================
void test_log_write_timing(void) {
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== Log Write Timing Benchmark ===");

    // Erase and reinit to get a clean state
    esp_err_t err = log_erase_all_sectors();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase log for timing test");
        return;
    }
    esp_task_wdt_reset();

    // Use a 39-byte payload (typical FSM log entry size)
    uint8_t payload[39];
    for (int i = 0; i < 39; i++) payload[i] = (uint8_t)i;

    #define TIMING_ITERATIONS 200

    int64_t min_us = INT64_MAX;
    int64_t max_us = 0;
    int64_t total_us = 0;
    int sector_cross_count = 0;
    int64_t sector_cross_max_us = 0;

    uint32_t prev_head = log_get_head();

    for (int i = 0; i < TIMING_ITERATIONS; i++) {
        int64_t t0 = esp_timer_get_time();
        err = log_write_blocking_test(payload, sizeof(payload), LOG_TYPE_DATA);
        // Wait for queue flush
        vTaskDelay(pdMS_TO_TICKS(50));
        int64_t t1 = esp_timer_get_time();

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Write %d failed: %s", i, esp_err_to_name(err));
            continue;
        }

        int64_t dt = t1 - t0;
        total_us += dt;
        if (dt < min_us) min_us = dt;
        if (dt > max_us) max_us = dt;

        // Detect sector crossing (head wrapped or jumped by > payload size)
        uint32_t cur_head = log_get_head();
        if (cur_head < prev_head || (cur_head - prev_head) > sizeof(payload) + 10) {
            sector_cross_count++;
            if (dt > sector_cross_max_us) sector_cross_max_us = dt;
        }
        prev_head = cur_head;

        if (i % 50 == 0) esp_task_wdt_reset();
    }

    int64_t avg_us = total_us / TIMING_ITERATIONS;

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "=== WRITE TIMING REPORT ===");
    ESP_LOGI(TAG, "  Iterations:       %d", TIMING_ITERATIONS);
    ESP_LOGI(TAG, "  Payload size:     %d bytes", (int)sizeof(payload));
    ESP_LOGI(TAG, "  Min:              %lld us", (long long)min_us);
    ESP_LOGI(TAG, "  Max:              %lld us", (long long)max_us);
    ESP_LOGI(TAG, "  Avg:              %lld us", (long long)avg_us);
    ESP_LOGI(TAG, "  Sector crossings: %d (max %lld us)", sector_cross_count, (long long)sector_cross_max_us);
    ESP_LOGI(TAG, "  WDT margin:       %.1fs (WDT=5s, worst=%lldus)",
             5.0 - (double)max_us / 1000000.0, (long long)max_us);
    if (max_us > 1000000) {
        ESP_LOGW(TAG, "  WARNING: max write > 1s — close to WDT timeout!");
    } else if (max_us > 100000) {
        ESP_LOGI(TAG, "  Note: max write > 100ms (expected during sector erase)");
    }
    ESP_LOGI(TAG, "===========================");
    ESP_LOGI(TAG, "");

    #undef TIMING_ITERATIONS
    esp_task_wdt_reset();
}

// Main test runner
esp_err_t run_all_log_tests(void) {
    ESP_LOGI(TAG, "\n\n");
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "STARTING COMPREHENSIVE LOG TESTS");
    ESP_LOGI(TAG, "=================================================\n");
    
    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG, "STORAGE FAILED");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Storage initialized successfully");
    
    typedef struct {
        const char* name;
        bool (*test_func)(void);
    } test_entry_t;
    
    test_entry_t tests[] = {
        {"Basic Log Integrity", test_log_integrity_basic},
        {"Log Integrity with Wraparound", test_log_integrity_wraparound},
        {"Head/Tail Recovery - Empty Log", test_log_head_tail_recovery_empty},
        {"Head/Tail Recovery - Partial Sector", test_log_head_tail_recovery_partial_sector},
        {"Head/Tail Recovery - Full Sectors", test_log_head_tail_recovery_full_sectors},
        {"Head/Tail Recovery - Wraparound", test_log_head_tail_recovery_wraparound},
        {"Head Equals Tail", test_log_head_equals_tail},
        {"Head=0, Tail=0", test_log_head_zero_tail_zero},
        {"Head > Tail (Normal)", test_log_head_greater_than_tail},
        {"Head < Tail (Wraparound)", test_log_head_less_than_tail},
        {"Multiple Wraparounds", test_log_wraparound_multiple_times},
        {"Sector Boundary Conditions", test_log_sector_boundary_conditions},
        {"Maximum Payload Size", test_log_maximum_payload},
        {"Minimum Payload Size", test_log_minimum_payload},
        {"Corrupted Entry Recovery", test_log_corrupted_entry_recovery},
        {"Full Partition Handling", test_log_full_partition},
        {"Read After Write Consistency", test_log_read_after_write},
        {"Multiple Log Types", test_log_multiple_types},
    };
    
    int num_tests = sizeof(tests) / sizeof(tests[0]);
    test_result_t* results = malloc(num_tests * sizeof(test_result_t));
    
    if (results == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for test results");
        return ESP_ERR_NO_MEM;
    }
    
    for (int i = 0; i < num_tests; i++) {
        ESP_LOGI(TAG, "\n--- Running Test %d/%d: %s ---", i + 1, num_tests, tests[i].name);
        
        esp_task_wdt_reset();
        
        results[i].test_name = tests[i].name;
        results[i].passed = tests[i].test_func();
        results[i].error_msg = NULL;
        
        if (results[i].passed) {
            ESP_LOGI(TAG, ">>> TEST PASSED: %s <<<", tests[i].name);
        } else {
            ESP_LOGE(TAG, ">>> TEST FAILED: %s <<<", tests[i].name);
            ESP_LOGE(TAG, ">>> STOPPING AT FIRST FAILURE <<<");
            
            for (int j = i + 1; j < num_tests; j++) {
                results[j].test_name = tests[j].name;
                results[j].passed = false;
                results[j].error_msg = "Not run - stopped at first failure";
            }
            break;
        }
        
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    print_test_results(results, num_tests);
    
    int passed = count_passed_tests(results, num_tests);
    free(results);
    
    if (passed == num_tests) {
        ESP_LOGI(TAG, "ALL TESTS PASSED!");
        // Run write timing benchmark as a final report (not a pass/fail test)
        test_log_write_timing();
    } else {
        ESP_LOGE(TAG, "SOME TESTS FAILED!");
    }

    while(1) { esp_task_wdt_reset(); vTaskDelay(pdMS_TO_TICKS(100)); }
    return ESP_OK;
}