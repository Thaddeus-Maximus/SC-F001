#include "uart_comms.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "storage.h"
#include <errno.h>
#include <ctype.h>
#include "rf_433.h"

#define TAG "UART"

#define UART_NUM UART_NUM_0
#define BUF_SIZE (1024)
#define CMD_MAX_LEN (256)

static char cmd_buffer[CMD_MAX_LEN];
static int cmd_pos = 0;
static TaskHandle_t uart_task_handle = NULL;

// TODO: Set Time
// TODO: Pair Remote
// TODO: Command Move
// TODO: Show current sensor values

// Parse value as either decimal or hex (0x prefix)
static bool parse_uint64(const char *str, uint64_t *result) {
    char *endptr;
    
    if (str == NULL || result == NULL) {
        return false;
    }
    
    // Check for hex prefix
    if (strlen(str) > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        *result = strtoull(str, &endptr, 16);
    } else {
        *result = strtoull(str, &endptr, 10);
    }
    
    // Check if conversion was successful (endptr should point to null terminator)
    return (*endptr == '\0' && endptr != str);
}

// Format parameter value for display based on its type
static void print_param_value(param_idx_t id, param_value_t val) {
    param_type_e type = get_param_type(id);
    
    char sbuf[9] = {0};
    
    switch (type) {
        case PARAM_TYPE_u16:
            printf("%u (0x%04X)\n",
              val.u16, val.u16);
            break;
            
        case PARAM_TYPE_i16:
            printf("%d (0x%04X)\n",
              val.i16, (uint16_t)val.i16);
            break;
            
        case PARAM_TYPE_u32:
            printf("%lu (0x%08lX)\n",
              (unsigned long)val.u32, (unsigned long)val.u32);
            break;
            
        case PARAM_TYPE_i32:
            printf("%ld (0x%08lX)\n",
              (long)val.i32, (unsigned long)val.i32);
            break;
            
        case PARAM_TYPE_f32:
            printf("%.6f (0x%08lX as bits)\n",
              val.f32, (unsigned long)val.u32);
            break;
            
            
        case PARAM_TYPE_f64:
            printf("%.6f (0x%016llX as bits)\n",
              val.f64, (unsigned long long)val.f64);
            break;
            
        case PARAM_TYPE_str:
        	memcpy(val.str, sbuf, 8);
        	sbuf[8] = '\0';
            printf("\"%s\"", sbuf);
            break;
            
        default:
            printf("UNKNOWN TYPE\n");
            break;
    }
}

static esp_err_t parse_param_value(const char *orig_str, param_type_e type, param_value_t *val) {
    const char *str = orig_str;
    // Skip leading whitespace
    while (isspace((unsigned char)*str)) str++;

    // Check for negative sign on unsigned integer types
    bool is_unsigned_int = (type == PARAM_TYPE_u16 || type == PARAM_TYPE_u32);
    if (is_unsigned_int && *str == '-') {
        return ESP_FAIL;
    }

    char *endptr;
    errno = 0;

    switch (type) {
        case PARAM_TYPE_u16:
            val->u16 = strtoull(str, &endptr, 0);
            break;
        case PARAM_TYPE_u32:
            val->u32 = strtoull(str, &endptr, 0);
            break;

        case PARAM_TYPE_i16:
            val->i16 = strtoll(str, &endptr, 0);
            break;
        case PARAM_TYPE_i32:
            val->i32 = strtoll(str, &endptr, 0);
            break;

        case PARAM_TYPE_f32:
            val->f32 = strtof(str, &endptr);
            break;

        case PARAM_TYPE_f64:
            val->f64 = strtod(str, &endptr);
            break;

        default:
            return ESP_FAIL;
    }

    if (errno == ERANGE || endptr == str || *endptr != '\0') {
        return ESP_FAIL;
    }

    return ESP_OK;
}

// Process set parameter command: sp <id> <value>
static void cmd_set_param(char *args) {
    char *id_str = strtok(args, " \t");
    char *val_str = strtok(NULL, " \t");
    
    if (id_str == NULL || val_str == NULL) {
        printf("ERROR: Usage: sp <id> <value>\n");
        return;
    }
    
    // Parse parameter ID
    uint64_t id_u64;
    if (!parse_uint64(id_str, &id_u64)) {
        printf("ERROR: Invalid parameter ID\n");
        return;
    }
    
    param_idx_t id = (param_idx_t)id_u64;
    if (id >= NUM_PARAMS) {
        printf("ERROR: Parameter ID %u out of range (max %d)\n",
          id, NUM_PARAMS - 1);
        return;
    }
    
    param_value_t param_val = {0};
    param_type_e type = get_param_type(id);
    esp_err_t parse_err = parse_param_value(val_str, type, &param_val);
    if (parse_err != ESP_OK) {
        printf("ERROR: Invalid value\n");
        return;
    }
    
    esp_err_t err = set_param_value_t(id, param_val);
    if (err == ESP_OK) {
        printf("OK: Parameter %u (%s) set to ",
          id, get_param_name(id));
        print_param_value(id, param_val);
        printf("(Not committed - use 'cp' to save)\n");
    } else {
        printf("ERROR: Failed to set parameter\n");
    }
}

// Process get parameter command: gp <id>
static void cmd_get_param(char *args) {
    char *id_str = strtok(args, " \t");
    
    if (id_str == NULL) {
        printf("ERROR: Usage: gp <id>\n");
        return;
    }
    
    // Parse parameter ID
    uint64_t id_u64;
    if (!parse_uint64(id_str, &id_u64)) {
        printf("ERROR: Invalid parameter ID\n");
        return;
    }
    
    param_idx_t id = (param_idx_t)id_u64;
    if (id >= NUM_PARAMS) {
        printf("ERROR: Parameter ID %u out of range (max %d)\n",
          id, NUM_PARAMS - 1);
        return;
    }
    
    // Get parameter
    param_value_t val = get_param_value_t(id);
    printf("Parameter %u (%s) = ", id, get_param_name(id));
    print_param_value(id, val);
}

// Process commit parameters command: cp
static void cmd_commit_params(char *args) {
    (void)args; // Unused
    
    printf("Committing parameters to flash...\n");
    commit_params();
    printf("OK: Parameters committed\n");
}

// Process reset parameter command: rp <id>
static void cmd_reset_param(char *args) {
    char *id_str = strtok(args, " \t");
    
    if (id_str == NULL) {
        printf("ERROR: Usage: rp <id>\n");
        return;
    }
    
    // Parse parameter ID
    uint64_t id_u64;
    if (!parse_uint64(id_str, &id_u64)) {
        printf("ERROR: Invalid parameter ID\n");
        return;
    }
    
    param_idx_t id = (param_idx_t)id_u64;
    if (id >= NUM_PARAMS) {
        printf("ERROR: Parameter ID %u out of range (max %d)\n",
          id, NUM_PARAMS - 1);
        return;
    }
    
    // Reset to default
    param_value_t default_val = get_param_default(id);
    esp_err_t err = set_param_value_t(id, default_val);
    
    if (err == ESP_OK) {
        printf("OK: Parameter %u (%s) reset to default: ",
          id, get_param_name(id));
        print_param_value(id, default_val);
        printf("(Not committed - use 'cp' to save)\n");
    } else {
        printf("ERROR: Failed to reset parameter\n");
    }
}

// Process list parameters command: lp
static void cmd_list_params(char *args) {
    (void)args; // Unused
    
    printf("\n=== Parameter List ===\n");
    printf("ID  | Name              | Type | Value\n");
    printf("----+-------------------+------+------------------\n");
    
    const char* type_names[] = {
        "u8 ", "i8 ", "u16", "i16", "u32", "i32", "u64", "i64", "f32", "f64"
    };
    
    for (int i = 0; i < NUM_PARAMS; i++) {
        param_value_t val = get_param_value_t(i);
        param_type_e type = get_param_type(i);
        
        printf("%-3d | %-17s | %-4s | ", i, get_param_name(i), type_names[type]);
        print_param_value(i, val);
    }
    printf("\n");
}

// Process help command
static void cmd_help(char *args) {
    (void)args; // Unused
    
    printf("\n=== Available Commands ===\n");
    printf("sp <id> <value>  - Set parameter (e.g., sp 0 42 or sp 14 0xDEADBEEF)\n");
    printf("gp <id>          - Get parameter (e.g., gp 0)\n");
    printf("rp <id>          - Reset parameter to default (e.g., rp 0)\n");
    printf("cp               - Commit parameters to flash\n");
    printf("lp               - List all parameters\n");
    printf("help             - Show this help\n");
    printf("\nNotes:\n");
    printf("- Values can be decimal (123) or hex (0xABC)\n");
    printf("- Changes are not saved until you run 'cp'\n");
    printf("- Parameter IDs range from 0 to %d\n\n", NUM_PARAMS - 1);
}

static void cmd_rf_learn(char *args) {
	char *id_str = strtok(args, " \t");
    
    if (id_str == NULL) {
        rf_433_cancel_learn_keycode();
        return;
    }
    
    // Parse parameter ID
    uint64_t id_u64;
    if (!parse_uint64(id_str, &id_u64)) {
        printf("ERROR: Invalid parameter ID\n");
        return;
    }
    
    param_idx_t id = (param_idx_t)id_u64;
    if (id < 8) {
		printf("Listening for keycode for slot %d\n", id);
        rf_433_learn_keycode(id);
        return;
    }
	printf("ERROR: Keycode slot index out of bounds.\n");
}

// Parse and execute command
static void process_command(char *cmd) {
    // Trim leading whitespace
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    
    // Ignore empty commands
    if (*cmd == '\0') {
        return;
    }
    
    // Extract command
    char *space = strchr(cmd, ' ');
    char *tab = strchr(cmd, '\t');
    char *delim = NULL;
    
    if (space && tab) {
        delim = (space < tab) ? space : tab;
    } else if (space) {
        delim = space;
    } else if (tab) {
        delim = tab;
    }
    
    char command[16] = {0};
    if (delim) {
        int cmd_len = delim - cmd;
        if (cmd_len >= sizeof(command)) {
            cmd_len = sizeof(command) - 1;
        }
        strncpy(command, cmd, cmd_len);
        cmd = delim + 1; // Point to arguments
    } else {
        strncpy(command, cmd, sizeof(command) - 1);
        cmd = cmd + strlen(cmd); // Point to empty string
    }
    
    // Execute command
    if (strcmp(command, "sp") == 0) {
        cmd_set_param(cmd);
    } else if (strcmp(command, "gp") == 0) {
        cmd_get_param(cmd);
    } else if (strcmp(command, "rp") == 0) {
        cmd_reset_param(cmd);
    } else if (strcmp(command, "cp") == 0) {
        cmd_commit_params(cmd);
    } else if (strcmp(command, "lp") == 0) {
        cmd_list_params(cmd);
    } else if (strcmp(command, "help") == 0) {
        cmd_help(cmd);
    } else if (strcmp(command, "rfl") == 0) {
        cmd_rf_learn(cmd);
    } else {
        printf("ERROR: Unknown command '%s' (type 'help' for commands)\n", command);
    }
}

// UART event task
void uart_event_task(void *pvParameters) {
    esp_task_wdt_add(NULL);
    uint8_t data[BUF_SIZE];
    
    while (1) {
		esp_task_wdt_reset();
        int len = uart_read_bytes(UART_NUM, data, BUF_SIZE - 1, 20 / portTICK_PERIOD_MS);
        
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)data[i];
                
                // Echo character
                printf("%c", c);
                fflush(stdout);
                
                // Handle backspace
                if (c == '\b' || c == 127) {
                    if (cmd_pos > 0) {
                        cmd_pos--;
                        printf(" \b"); // Clear character on screen
                        fflush(stdout);
                    }
                    continue;
                }
                
                // Handle newline/carriage return
                if (c == '\n' || c == '\r') {
                    cmd_buffer[cmd_pos] = '\0';
                    printf("\n");
                    
                    if (cmd_pos > 0) {
                        process_command(cmd_buffer);
                    }
                    
                    cmd_pos = 0;
                    printf("> ");
                    fflush(stdout);
                    continue;
                }
                
                // Add to buffer if not full
                if (cmd_pos < CMD_MAX_LEN - 1) {
                    cmd_buffer[cmd_pos++] = c;
                }
            }
        }
    }
}

esp_err_t uart_init() {
    // Configure UART
    uart_config_t uart_config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    
    // Print startup message
    /*printf("\n\n");
    printf("=================================\n");
    printf("  ESP32 Parameter Manager\n");
    printf("=================================\n");
    printf("Type 'help' for available commands\n\n");
    printf("> ");
    fflush(stdout);*/
    
    // Create UART task    
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 12, &uart_task_handle);
    
    ESP_LOGI(TAG, "UART interface started");
    return ESP_OK;
}

esp_err_t uart_stop() {
    if (uart_task_handle == NULL) {
        ESP_LOGW(TAG, "UART task not running");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Shutting down UART...");
    
    // Wait for UART TX to finish
    uart_wait_tx_done(UART_NUM, pdMS_TO_TICKS(100));
    
    // Delete the UART task
    vTaskDelete(uart_task_handle);
    uart_task_handle = NULL;
    
    // Small delay to ensure task deletion completes
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // Disable UART driver
    uart_driver_delete(UART_NUM);
    
    ESP_LOGI(TAG, "UART shutdown complete");
    
    return ESP_OK;
}