#ifndef LOG_TEST_H
#define LOG_TEST_H

#include "freertos/FreeRTOS.h"   // Must be FIRST
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Test result structure
typedef struct {
    const char* test_name;
    bool passed;
    const char* error_msg;
} test_result_t;

// Main test suite runner
esp_err_t run_all_log_tests(void);

// Individual test functions
bool test_log_integrity_basic(void);
bool test_log_integrity_wraparound(void);
bool test_log_head_tail_recovery_empty(void);
bool test_log_head_tail_recovery_partial_sector(void);
bool test_log_head_tail_recovery_full_sectors(void);
bool test_log_head_tail_recovery_wraparound(void);
bool test_log_head_equals_tail(void);
bool test_log_head_zero_tail_zero(void);
bool test_log_head_greater_than_tail(void);
bool test_log_head_less_than_tail(void);
bool test_log_wraparound_multiple_times(void);
bool test_log_sector_boundary_conditions(void);
bool test_log_maximum_payload(void);
bool test_log_minimum_payload(void);
bool test_log_corrupted_entry_recovery(void);
bool test_log_full_partition(void);
bool test_log_read_after_write(void);
bool test_log_multiple_types(void);

// Write timing benchmark (not a pass/fail test — prints min/max/avg report)
void test_log_write_timing(void);

// Helper functions for testing
void print_test_results(test_result_t* results, int num_tests);
int count_passed_tests(test_result_t* results, int num_tests);

#endif // LOG_TEST_H