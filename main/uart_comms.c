#include "uart_comms.h"
#include "comms.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtc.h"
#include <stdio.h>
#include <string.h>

#define TAG "UART"

#define UART_NUM UART_NUM_0
#define BUF_SIZE (1024)
#define CMD_MAX_LEN (2048)

static char cmd_buffer[CMD_MAX_LEN];
static int cmd_pos = 0;
static TaskHandle_t uart_task_handle = NULL;

// Process GET command
static void cmd_get(void) {
    printf("\n");
    
    cJSON *response = comms_handle_get();
    if (response == NULL) {
        printf("ERROR: Failed to generate GET response\n");
        return;
    }
    
    // Pretty print the JSON
    char *json_str = cJSON_Print(response);
    cJSON_Delete(response);
    
    if (json_str == NULL) {
        printf("ERROR: Failed to serialize JSON\n");
        return;
    }
    
    printf("%s\n", json_str);
    free(json_str);
}

// Process POST command with JSON data
static void cmd_post(char *json_data) {
    printf("\n");
    
    // Parse the JSON input
    cJSON *request = cJSON_Parse(json_data);
    if (request == NULL) {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL) {
            printf("ERROR: JSON parse error before: %s\n", error_ptr);
        } else {
            printf("ERROR: Failed to parse JSON\n");
        }
        return;
    }
    
    // Call the unified POST handler
    cJSON *response = NULL;
    esp_err_t err = comms_handle_post(request, &response);
    cJSON_Delete(request);
    
    if (response == NULL) {
        printf("ERROR: Failed to generate POST response\n");
        return;
    }
    
    // Pretty print the response
    char *json_str = cJSON_Print(response);
    
    // Check for special response flags before deleting
    cJSON *reboot_flag = cJSON_GetObjectItem(response, "reboot");
    cJSON *sleep_flag = cJSON_GetObjectItem(response, "sleep");
    bool should_reboot = cJSON_IsTrue(reboot_flag);
    bool should_sleep = cJSON_IsTrue(sleep_flag);
    
    cJSON_Delete(response);
    
    if (json_str == NULL) {
        printf("ERROR: Failed to serialize JSON\n");
        return;
    }
    
    printf("%s\n", json_str);
    free(json_str);
    
    // Handle special actions
    if (should_reboot) {
        printf("\nRebooting in 2 seconds...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    
    if (should_sleep) {
        printf("\nEntering soft idle in 2 seconds...\n");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(2000));
        soft_idle_enter();
    }
}

// Process help command
static void cmd_help(void) {
    printf("\n=== UART JSON Interface ===\n");
    printf("\nCommands:\n");
    printf("  GET                  - Get complete system status (JSON)\n");
    printf("  POST: {json}         - Send command or update parameters (JSON)\n");
    printf("  RTCDEBUG             - Dump RTC timekeeping state (time, backup, sleep entry, clock source)\n");
    printf("  HELP                 - Show this help\n");
    printf("\nPOST JSON Format:\n");
    printf("{\n");
    printf("  \"time\": 1234567,          // Set RTC time (optional)\n");
    printf("  \"remaining_dist\": 100,    // Set remaining distance (optional)\n");
    printf("  \"cmd\": \"start\",           // Execute command (optional)\n");
    printf("  \"parameters\": {           // Update parameters (optional)\n");
    printf("    \"PARAM_NAME\": value,\n");
    printf("    \"PARAM_NAME2\": value\n");
    printf("  }\n");
    printf("}\n");
    printf("\nAvailable Commands:\n");
    printf("  start, stop, undo, fwd, rev, up, down, aux\n");
    printf("  reboot, sleep\n");
    printf("  rf_learn, rf_clear_temp, rf_disable, rf_enable, rf_status\n");
    printf("  cal_jack_start, cal_jack_finish, cal_drive_start, cal_drive_finish, cal_get\n");
    printf("\nExamples:\n");
    printf("  GET\n");
    printf("  POST: {\"cmd\":\"start\"}\n");
    printf("  POST: {\"time\":1704067200}\n");
    printf("  POST: {\"parameters\":{\"WIFI_SSID\":\"MyNetwork\",\"WIFI_CHANNEL\":6}}\n");
    printf("  POST: {\"cmd\":\"rf_learn\",\"channel\":0}\n");
    printf("\n");
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
    
    // Convert to uppercase for command matching
    char command[16] = {0};
    int i = 0;
    while (cmd[i] && cmd[i] != ' ' && cmd[i] != '\t' && cmd[i] != ':' && i < 15) {
        command[i] = (cmd[i] >= 'a' && cmd[i] <= 'z') ? (cmd[i] - 32) : cmd[i];
        i++;
    }
    command[i] = '\0';
    
    // Execute command
    if (strcmp(command, "GET") == 0) {
        cmd_get();
    }
    else if (strcmp(command, "POST") == 0) {
        // Find the start of JSON data (after ':')
        char *json_start = strchr(cmd, ':');
        if (json_start == NULL) {
            printf("ERROR: POST command requires JSON data after ':'\n");
            printf("Usage: POST: {json data}\n");
            return;
        }
        json_start++; // Skip the ':'
        
        // Trim whitespace
        while (*json_start == ' ' || *json_start == '\t') {
            json_start++;
        }
        
        if (*json_start == '\0') {
            printf("ERROR: No JSON data provided\n");
            return;
        }
        
        cmd_post(json_start);
    }
    else if (strcmp(command, "HELP") == 0) {
        cmd_help();
    }
    else if (strcmp(command, "RTCDEBUG") == 0) {
        rtc_print_debug();
    }
    else {
        printf("ERROR: Unknown command '%s'\n", command);
        printf("Type 'HELP' for available commands\n");
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
                    printf("\n> ");
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

esp_err_t uart_init(void) {
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
    printf("\n\n");
    printf("========================================\n");
    printf("  UART JSON Interface Ready\n");
    printf("========================================\n");
    printf("Type 'HELP' for available commands\n");
    printf("Type 'GET' to see system status\n");
    printf("\n> ");
    fflush(stdout);
    
    // Create UART task    
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 12, &uart_task_handle);
    
    ESP_LOGI(TAG, "UART interface started");
    return ESP_OK;
}

esp_err_t uart_stop(void) {
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