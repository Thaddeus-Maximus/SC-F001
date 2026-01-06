#include "comms.h"
#include "cJSON.h"
#include "control_fsm.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_mgmt.h"
#include "rf_433.h"
#include "rtc.h"
#include "storage.h"
#include "version.h"
#include <string.h>

static const char *TAG = "COMMS";

/**
 * Build a JSON object containing complete system status
 */
cJSON* comms_handle_get(void) {
    ESP_LOGI(TAG, "GET request");
    
    rtc_reset_shutdown_timer();
    
    // Create root JSON object
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return NULL;
    }
    
    // Add basic system info
    cJSON_AddStringToObject(root, "build_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "build_date", BUILD_DATE);
    cJSON_AddNumberToObject(root, "time", (double)rtc_get_s());
    cJSON_AddBoolToObject(root, "rtc_set", rtc_is_set());
    cJSON_AddNumberToObject(root, "state", fsm_get_state());
    cJSON_AddNumberToObject(root, "voltage", get_battery_V());
    cJSON_AddNumberToObject(root, "remaining_dist", fsm_get_remaining_distance());
    cJSON_AddNumberToObject(root, "next_alarm", (double)rtc_get_next_alarm_s());
    
    cJSON *msg_array = cJSON_CreateArray();
    if (msg_array == NULL) {
        ESP_LOGE(TAG, "Failed to create msg array");
        cJSON_Delete(root);
        return NULL;
    }
    
    // Add state message
    switch(fsm_get_state()) {
        case STATE_IDLE:
            cJSON_AddItemToArray(msg_array, cJSON_CreateString("IDLE"));
            break;
        case STATE_UNDO_JACK:
        case STATE_UNDO_JACK_START:
            cJSON_AddItemToArray(msg_array, cJSON_CreateString("CANCELLING MOVE"));
            break;
        default:
            cJSON_AddItemToArray(msg_array, cJSON_CreateString("MOVING..."));
            break;
    }
    
    // Add warning/error messages
    if (fsm_get_remaining_distance() <= 0) {
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("DISTANCE LIMIT HIT"));
    }
    if (efuse_is_tripped(BRIDGE_AUX)) {
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("AUX EFUSE TRIP"));
    }
    if (efuse_is_tripped(BRIDGE_JACK)) {
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("JACK EFUSE TRIP"));
    }
    if (efuse_is_tripped(BRIDGE_DRIVE)) {
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("DRIVE EFUSE TRIP"));
    }
    if (!rtc_is_set()) {
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("CLOCK NOT SET"));
    }
    
    cJSON_AddItemToObject(root, "msg", msg_array);
    
    // Add parameters object
    cJSON *parameters = cJSON_CreateObject();
    if (parameters == NULL) {
        ESP_LOGE(TAG, "Failed to create parameters object");
        cJSON_Delete(root);
        return NULL;
    }
    
    // Add all parameters
    for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
        const char *name = get_param_name(i);
        param_value_t value = get_param_value_t(i);
        
        switch (get_param_type(i)) {
            case PARAM_TYPE_f32:
                cJSON_AddNumberToObject(parameters, name, value.f32);
                break;
            case PARAM_TYPE_f64:
                cJSON_AddNumberToObject(parameters, name, value.f64);
                break;
            case PARAM_TYPE_i32:
                cJSON_AddNumberToObject(parameters, name, value.i32);
                break;
            case PARAM_TYPE_i16:
                cJSON_AddNumberToObject(parameters, name, value.i16);
                break;
            case PARAM_TYPE_u32:
                cJSON_AddNumberToObject(parameters, name, value.u32);
                break;
            case PARAM_TYPE_u16:
                cJSON_AddNumberToObject(parameters, name, value.u16);
                break;
            case PARAM_TYPE_str:
                cJSON_AddStringToObject(parameters, name, get_param_string(i));
                break;
            default:
                cJSON_AddNullToObject(parameters, name);
                break;
        }
    }
    
    cJSON_AddItemToObject(root, "parameters", parameters);
    
    return root;
}

/**
 * Process a POST request with JSON data
 */
esp_err_t comms_handle_post(cJSON *root, cJSON **response_json) {
    ESP_LOGI(TAG, "POST request");
    
    if (root == NULL || response_json == NULL) {
        ESP_LOGE(TAG, "Invalid arguments to comms_handle_post");
        return ESP_FAIL;
    }
    
    rtc_reset_shutdown_timer();
    
    bool cmd_executed = false;
    bool sleep_requested = false;
    bool reboot_requested = false;
    const char *error_msg = NULL;
    int params_updated = 0;
    int params_failed = 0;
    
    // Process time if present
    cJSON *time = cJSON_GetObjectItem(root, "time");
    if (cJSON_IsNumber(time)) {
        int64_t new_time = (int64_t)cJSON_GetNumberValue(time);
        ESP_LOGI(TAG, "Setting time to %lld", new_time);
        rtc_set_s(new_time);
    }
    
    // Process remaining_dist if present
    cJSON *remaining_dist = cJSON_GetObjectItem(root, "remaining_dist");
    if (cJSON_IsNumber(remaining_dist)) {
        int64_t new_dist = (int64_t)cJSON_GetNumberValue(remaining_dist);
        ESP_LOGI(TAG, "Setting remaining_dist to %lld", new_dist);
        fsm_set_remaining_distance(new_dist);
    }
    
    // Process command if present
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd)) {
        const char *cmd_str = cmd->valuestring;
        ESP_LOGI(TAG, "Executing command: %s", cmd_str);
        
        if (strcmp(cmd_str, "start") == 0) {
            fsm_request(FSM_CMD_START);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "stop") == 0) {
            fsm_request(FSM_CMD_STOP);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "undo") == 0) {
            fsm_request(FSM_CMD_UNDO);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "fwd") == 0) {
            pulseOverride(RELAY_A1);
            pulseOverride(RELAY_A3);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rev") == 0) {
            pulseOverride(RELAY_B1);
            pulseOverride(RELAY_A3);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "up") == 0) {
            pulseOverride(RELAY_A2);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "down") == 0) {
            pulseOverride(RELAY_B2);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "aux") == 0) {
            pulseOverride(RELAY_A3);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "reboot") == 0) {
            reboot_requested = true;
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "sleep") == 0) {
            sleep_requested = true;
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rf_clear_temp") == 0) {
            rf_433_clear_temp_keycodes();
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rf_disable") == 0) {
            rf_433_disable_controls();
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rf_enable") == 0) {
            rf_433_enable_controls();
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rf_learn") == 0) {
            // Start learning for a specific channel
            cJSON *channel = cJSON_GetObjectItem(root, "channel");
            if (cJSON_IsNumber(channel)) {
                int ch = (int)cJSON_GetNumberValue(channel);
                if (ch >= 0 && ch < NUM_RF_BUTTONS) {
                    rf_433_learn_keycode(ch);
                    cmd_executed = true;
                } else if (ch == -1) {
                    rf_433_cancel_learn_keycode();
                    cmd_executed = true;
                } else {
                    error_msg = "Invalid channel number";
                }
            } else {
                error_msg = "rf_learn requires channel parameter";
            }
        }
        else if (strcmp(cmd_str, "rf_set_temp") == 0) {
            cJSON *index = cJSON_GetObjectItem(root, "index");
            cJSON *code = cJSON_GetObjectItem(root, "code");
            if (cJSON_IsNumber(index) && cJSON_IsNumber(code)) {
                int idx = (int)cJSON_GetNumberValue(index);
                int32_t rf_code = (int32_t)cJSON_GetNumberValue(code);
                rf_433_set_temp_keycode(idx, rf_code);
                cmd_executed = true;
            } else {
                error_msg = "rf_set_temp requires index and code parameters";
            }
        }
        else if (strcmp(cmd_str, "rf_status") == 0) {
            // Return current temp keycodes
            cJSON *response = cJSON_CreateObject();
            cJSON *codes_array = cJSON_CreateArray();
            
            for (int i = 0; i < 4; i++) {  // Only return first 4 for web UI
                int32_t code = rf_433_get_temp_keycode(i);
                cJSON_AddItemToArray(codes_array, cJSON_CreateNumber(code));
            }
            
            cJSON_AddItemToObject(response, "codes", codes_array);
            *response_json = response;
            return ESP_OK;
        }
        else if (strcmp(cmd_str, "cal_jack_start") == 0) {
            fsm_request(FSM_CMD_CALIBRATE_JACK_PREP);
            ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_PREP");
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "cal_jack_finish") == 0) {
            cJSON *amt = cJSON_GetObjectItem(root, "amt");
            if (cJSON_IsNumber(amt) && amt->valuedouble >= 0 && amt->valuedouble < 8) {
                ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_JACK_FINISH");
                fsm_set_cal_val(amt->valuedouble);
                fsm_request(FSM_CMD_CALIBRATE_JACK_FINISH);
                cmd_executed = true;
            } else {
                error_msg = "cal_jack_finish requires amt parameter (0-8)";
            }
        }
        else if (strcmp(cmd_str, "cal_drive_start") == 0) {
            fsm_request(FSM_CMD_CALIBRATE_DRIVE_PREP);
            ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_PREP");
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "cal_drive_finish") == 0) {
            cJSON *amt = cJSON_GetObjectItem(root, "amt");
            if (cJSON_IsNumber(amt) && amt->valuedouble >= 0 && amt->valuedouble < 8) {
                ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_FINISH");
                fsm_set_cal_val(amt->valuedouble);
                fsm_request(FSM_CMD_CALIBRATE_DRIVE_FINISH);
                cmd_executed = true;
            } else {
                error_msg = "cal_drive_finish requires amt parameter (0-8)";
            }
        }
        else if (strcmp(cmd_str, "cal_get") == 0) {
            ESP_LOGI(TAG, "CAL_GET");
            
            // Build JSON response with calibration values
            cJSON *response = cJSON_CreateObject();
            cJSON_AddItemToObject(response, "e", cJSON_CreateNumber(fsm_get_cal_e()));
            cJSON_AddItemToObject(response, "t", cJSON_CreateNumber(fsm_get_cal_t()));
            
            *response_json = response;
            return ESP_OK;
        }
        else {
            ESP_LOGW(TAG, "Unknown command: %s", cmd_str);
            error_msg = "Unknown command";
        }
    }
    
    // Process batch parameter updates if present
    cJSON *parameters = cJSON_GetObjectItem(root, "parameters");
    if (cJSON_IsObject(parameters)) {
        // Process individual parameter updates (direct key-value pairs)
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, parameters) {
            const char *key = item->string;
            ESP_LOGI(TAG, "Processing parameter: %s", key);
            
            // Find parameter index by name
            param_idx_t param_idx = NUM_PARAMS;
            for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
                if (strcmp(get_param_name(i), key) == 0) {
                    param_idx = i;
                    break;
                }
            }
            
            if (param_idx == NUM_PARAMS) {
                ESP_LOGW(TAG, "Unknown parameter: %s", key);
                params_failed++;
                continue;
            }
            
            cJSON *value_json = cJSON_GetObjectItem(parameters, key);
            
            // Set parameter value based on type
            switch (get_param_type(param_idx)) {
                case PARAM_TYPE_f32:
                    if (cJSON_IsNumber(value_json)) {
                        set_param_value_t(param_idx, (param_value_t){.f32 = value_json->valuedouble});
                        params_updated++;
                    } else {
                        ESP_LOGW(TAG, "Type mismatch for parameter: %s", key);
                        params_failed++;
                    }
                    break;
                case PARAM_TYPE_f64:
                    if (cJSON_IsNumber(value_json)) {
                        set_param_value_t(param_idx, (param_value_t){.f64 = value_json->valuedouble});
                        params_updated++;
                    } else {
                        ESP_LOGW(TAG, "Type mismatch for parameter: %s", key);
                        params_failed++;
                    }
                    break;
                case PARAM_TYPE_i32:
                    if (cJSON_IsNumber(value_json)) {
                        set_param_value_t(param_idx, (param_value_t){.i32 = value_json->valueint});
                        params_updated++;
                    } else {
                        ESP_LOGW(TAG, "Type mismatch for parameter: %s", key);
                        params_failed++;
                    }
                    break;
                case PARAM_TYPE_i16:
                    if (cJSON_IsNumber(value_json)) {
                        set_param_value_t(param_idx, (param_value_t){.i16 = value_json->valueint});
                        params_updated++;
                    } else {
                        ESP_LOGW(TAG, "Type mismatch for parameter: %s", key);
                        params_failed++;
                    }
                    break;
                case PARAM_TYPE_u32:
                    if (cJSON_IsNumber(value_json)) {
                        set_param_value_t(param_idx, (param_value_t){.u32 = value_json->valueint});
                        params_updated++;
                    } else {
                        ESP_LOGW(TAG, "Type mismatch for parameter: %s", key);
                        params_failed++;
                    }
                    break;
                case PARAM_TYPE_u16:
                    if (cJSON_IsNumber(value_json)) {
                        set_param_value_t(param_idx, (param_value_t){.u16 = value_json->valueint});
                        params_updated++;
                    } else {
                        ESP_LOGW(TAG, "Type mismatch for parameter: %s", key);
                        params_failed++;
                    }
                    break;
                case PARAM_TYPE_str:
                    if (cJSON_IsString(value_json)) {
                        set_param_string(param_idx, value_json->valuestring);
                        params_updated++;
                    } else {
                        ESP_LOGW(TAG, "Type mismatch for parameter: %s", key);
                        params_failed++;
                    }
                    break;
                default:
                    ESP_LOGW(TAG, "Unknown type for parameter: %s", key);
                    params_failed++;
                    break;
            }
        }
        
        if (params_updated > 0) {
            rtc_schedule_next_alarm();
            commit_params();
        }
    }
    
    // Build response
    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to create response JSON");
        return ESP_FAIL;
    }
    
    if (reboot_requested) {
        cJSON_AddStringToObject(response, "status", "ok");
        cJSON_AddStringToObject(response, "message", "Rebooting...");
        cJSON_AddBoolToObject(response, "reboot", true);
        *response_json = response;
        return ESP_OK;
    }
    
    if (sleep_requested) {
        cJSON_AddStringToObject(response, "status", "ok");
        cJSON_AddStringToObject(response, "message", "Sleeping...");
        cJSON_AddBoolToObject(response, "sleep", true);
        *response_json = response;
        return ESP_OK;
    }
    
    if (error_msg != NULL) {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", error_msg);
        *response_json = response;
        return ESP_FAIL;
    }
    
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddNumberToObject(response, "params_updated", params_updated);
    cJSON_AddNumberToObject(response, "params_failed", params_failed);
    cJSON_AddBoolToObject(response, "cmd_executed", cmd_executed);
    
    *response_json = response;
    return ESP_OK;
}