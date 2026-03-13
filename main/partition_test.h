#ifndef PARTITION_TEST_H
#define PARTITION_TEST_H

#include "esp_err.h"
#include <stdbool.h>

// Run all partition verification tests
// Call from app_main() after storage_init() and log_init()
esp_err_t run_partition_tests(void);

// Individual tests
bool test_params_partition_rw(void);
bool test_log_partition_rw(void);
bool test_post_partition_rw(void);
bool test_partitions_independent(void);
bool test_params_persist_after_commit(void);
bool test_log_write_read_cycle(void);

#endif // PARTITION_TEST_H
