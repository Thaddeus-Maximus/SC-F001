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
#include "sensors.h"
#include "storage.h"
#include "version.h"
#include <string.h>

static const char *TAG = "COMMS";

/* Decode a single JSON value into the parameter table. Returns true on
 * success (param updated), false if the JSON node type doesn't match the
 * parameter type (caller bumps params_failed). All numeric integer types
 * funnel through valueint, floats through valuedouble — matches what
 * cJSON_AddNumberToObject produced on the way out. */
static bool set_param_from_json(param_idx_t idx, cJSON *value_json) {
    if (get_param_type(idx) == PARAM_TYPE_str) {
        if (!cJSON_IsString(value_json)) return false;
        set_param_string(idx, value_json->valuestring);
        return true;
    }
    if (!cJSON_IsNumber(value_json)) return false;
    param_value_t v = {0};
    switch (get_param_type(idx)) {
        case PARAM_TYPE_u16: v.u16 = (uint16_t)value_json->valueint;    break;
        case PARAM_TYPE_i16: v.i16 = (int16_t)value_json->valueint;     break;
        case PARAM_TYPE_u32: v.u32 = (uint32_t)value_json->valueint;    break;
        case PARAM_TYPE_i32: v.i32 = (int32_t)value_json->valueint;     break;
        case PARAM_TYPE_f32: v.f32 = (float)value_json->valuedouble;    break;
        case PARAM_TYPE_f64: v.f64 = value_json->valuedouble;           break;
        default: return false;
    }
    set_param_value_t(idx, v);
    return true;
}

/**
 * Build a JSON object containing complete system status
 */
cJSON* comms_handle_get(void) {
    //ESP_LOGI(TAG, "GET request");
    
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
    cJSON_AddNumberToObject(root, "board_rev", hw_get_board_rev());
    cJSON_AddNumberToObject(root, "fsm_error", fsm_get_error());

    // Structured error flags (match LED error code bits)
    cJSON *errors = cJSON_CreateObject();
    bool efuse_trip = any_efuse_tripped();
    float bat_v = get_battery_V();
    float low_v = get_param_value_t(PARAM_LOW_PROTECTION_V).f32;
    bool low_bat = (bat_v > 0 && bat_v < low_v);
    bool safety_trip = !get_is_safe();
    bool leash_hit = (fsm_get_remaining_distance() <= 0);

    cJSON_AddBoolToObject(errors, "efuse_aux",   efuse_get(BRIDGE_AUX) != 0);
    cJSON_AddBoolToObject(errors, "efuse_jack",  efuse_get(BRIDGE_JACK) != 0);
    cJSON_AddBoolToObject(errors, "efuse_drive", efuse_get(BRIDGE_DRIVE) != 0);
    cJSON_AddBoolToObject(errors, "low_battery", low_bat);
    cJSON_AddBoolToObject(errors, "rtc_not_set", !rtc_is_set());
    cJSON_AddBoolToObject(errors, "safety_trip", safety_trip);
    cJSON_AddBoolToObject(errors, "leash_hit",   leash_hit);
    // LED error code: bit0=efuse/battery, bit1=RTC, bit2=safety/leash
    uint8_t led_code = 0;
    if (efuse_trip || low_bat)      led_code |= 0b001;
    if (!rtc_is_set())              led_code |= 0b010;
    if (safety_trip || leash_hit)   led_code |= 0b100;
    if (fsm_get_error() != 0 && led_code == 0) led_code = 0b111;
    cJSON_AddNumberToObject(errors, "led_code", led_code);
    cJSON_AddItemToObject(root, "errors", errors);

    // Status message array
    cJSON *msg_array = cJSON_CreateArray();
    if (msg_array == NULL) {
        ESP_LOGE(TAG, "Failed to create msg array");
        cJSON_Delete(root);
        return NULL;
    }

    switch(fsm_get_state()) {
        case STATE_IDLE:
            cJSON_AddItemToArray(msg_array, cJSON_CreateString("IDLE"));
            break;
        case STATE_UNDO_JACK_START:
            cJSON_AddItemToArray(msg_array, cJSON_CreateString("CANCELLING MOVE"));
            break;
        default:
            cJSON_AddItemToArray(msg_array, cJSON_CreateString("MOVING..."));
            break;
    }

    if (leash_hit)
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("DISTANCE LIMIT HIT"));
    // Per-bridge efuse messages. Preserve the original AUX → JACK → DRIVE
    // order via an explicit walk; bridge_t enum order is the opposite.
    static const bridge_t efuse_msg_order[] = { BRIDGE_AUX, BRIDGE_JACK, BRIDGE_DRIVE };
    for (size_t i = 0; i < sizeof(efuse_msg_order)/sizeof(efuse_msg_order[0]); i++) {
        bridge_t b = efuse_msg_order[i];
        if (efuse_get(b)) {
            char msg[32];
            snprintf(msg, sizeof(msg), "%s EFUSE TRIP", bridge_names[b]);
            cJSON_AddItemToArray(msg_array, cJSON_CreateString(msg));
        }
    }
    if (low_bat)
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("LOW BATTERY"));
    if (!rtc_is_set())
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("CLOCK NOT SET"));
    if (safety_trip)
        cJSON_AddItemToArray(msg_array, cJSON_CreateString("SAFETY SENSOR BREAK"));

    cJSON_AddItemToObject(root, "msg", msg_array);
    
    // Add parameters object
    cJSON *parameters = cJSON_CreateObject();
    if (parameters == NULL) {
        ESP_LOGE(TAG, "Failed to create parameters object");
        cJSON_Delete(root);
        return NULL;
    }
    
    // Add all parameters. Numeric params funnel through param_to_double() —
    // cJSON stores all numbers as double internally, so the type-specific
    // accessor was just feeding the same final value.
    for (param_idx_t i = 0; i < NUM_PARAMS; i++) {
        const char *name = get_param_name(i);
        if (get_param_type(i) == PARAM_TYPE_str) {
            cJSON_AddStringToObject(parameters, name, get_param_string(i));
        } else {
            cJSON_AddNumberToObject(parameters, name, param_to_double(i));
        }
    }
    
    cJSON_AddItemToObject(root, "parameters", parameters);
    
    return root;
}

/**
 * Process a POST request with JSON data
 */
esp_err_t comms_handle_post(cJSON *root, cJSON **response_json) {
    //ESP_LOGI(TAG, "POST request");
    
    if (root == NULL || response_json == NULL) {
        ESP_LOGE(TAG, "Invalid arguments to comms_handle_post");
        return ESP_FAIL;
    }
    
    rtc_reset_shutdown_timer();
    
    bool cmd_executed = false;
    bool sleep_requested = false;
    bool hibernate_requested = false;
    bool reboot_requested = false;
    bool wifi_params_changed = false;
    bool wifi_restart_requested = false;
    bool refresh_battery_ema = false;
    const char *error_msg = NULL;
    int params_updated = 0;
    int params_failed = 0;
    
    // Process time if present
    cJSON *time = cJSON_GetObjectItem(root, "time");
    if (cJSON_IsNumber(time)) {
        int64_t new_time = (int64_t)cJSON_GetNumberValue(time);
        ESP_LOGI(TAG, "Setting time to %lld", (long long)new_time);
        rtc_set_s(new_time);
    }
    
    // Process remaining_dist if present
    cJSON *remaining_dist = cJSON_GetObjectItem(root, "remaining_dist");
    if (cJSON_IsNumber(remaining_dist)) {
        float new_dist = (float)cJSON_GetNumberValue(remaining_dist);
        ESP_LOGI(TAG, "Setting remaining_dist to %.3f", new_dist);
        fsm_set_remaining_distance(new_dist);
    }
    
    // Process command if present
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (cJSON_IsString(cmd)) {
        const char *cmd_str = cmd->valuestring;
       // ESP_LOGI(TAG, "Executing command: %s", cmd_str);
        
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
            pulse_override(FSM_OVERRIDE_DRIVE_FWD);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "rev") == 0) {
            pulse_override(FSM_OVERRIDE_DRIVE_REV);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "up") == 0) {
            pulse_override(FSM_OVERRIDE_JACK_UP);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "down") == 0) {
            pulse_override(FSM_OVERRIDE_JACK_DOWN);
            cmd_executed = true;
        }
        else if (strcmp(cmd_str, "aux") == 0) {
            pulse_override(FSM_OVERRIDE_AUX);
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
        else if (strcmp(cmd_str, "hibernate") == 0) {
            hibernate_requested = true;
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
        else if (strcmp(cmd_str, "cal_drive_start") == 0) {
            fsm_request(FSM_CMD_CALIBRATE_DRIVE_PREP);
            ESP_LOGI(TAG, "FSM_CMD_CALIBRATE_DRIVE_PREP");
            cmd_executed = true;
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
        else if (strcmp(cmd_str, "set_board_rev") == 0) {
            cJSON *rev = cJSON_GetObjectItem(root, "board_rev");
            if (cJSON_IsNumber(rev)) {
                uint16_t r = (uint16_t)cJSON_GetNumberValue(rev);
                if (hw_set_board_rev(r) == ESP_OK) {
                    ESP_LOGI(TAG, "Board rev set to %u", r);
                    cmd_executed = true;
                } else {
                    error_msg = "Failed to write board_rev to NVS";
                }
            } else {
                error_msg = "set_board_rev requires board_rev parameter";
            }
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

            if (param_idx == PARAM_WIFI_SSID ||
                param_idx == PARAM_WIFI_PASS ||
                param_idx == PARAM_WIFI_CHANNEL ||
                param_idx == PARAM_NET_SSID  ||
                param_idx == PARAM_NET_PASS) {
                wifi_params_changed = true;
            }

            /* If the battery conversion params change, refresh the EMA
             * so get_battery_V() reflects the new K/OFFSET immediately. */
            if (param_idx == PARAM_V_SENS_K || param_idx == PARAM_V_SENS_OFFSET) {
                refresh_battery_ema = true;
            }
            
            cJSON *value_json = cJSON_GetObjectItem(parameters, key);

            if (set_param_from_json(param_idx, value_json)) {
                params_updated++;
            } else {
                ESP_LOGW(TAG, "Type mismatch for parameter: %s", key);
                params_failed++;
            }
        }
        
        if (params_updated > 0) {
            rtc_schedule_next_alarm();
            commit_params();
            if (refresh_battery_ema) {
                reset_battery_ema();
            }
            if (wifi_params_changed) {
                ESP_LOGI(TAG, "WiFi params changed — restarting WiFi AP");
                wifi_restart_requested = true;
            }
        }
    }
    
    // Build response
    cJSON *response = cJSON_CreateObject();
    if (response == NULL) {
        ESP_LOGE(TAG, "Failed to create response JSON");
        return ESP_FAIL;
    }
    
    if (wifi_restart_requested) {
        cJSON_AddStringToObject(response, "status", "ok");
        cJSON_AddStringToObject(response, "message", "Restarting WiFi AP...");
        cJSON_AddBoolToObject(response, "wifi_restart", true);
        *response_json = response;
        return ESP_OK;
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

    if (hibernate_requested) {
        cJSON_AddStringToObject(response, "status", "ok");
        cJSON_AddStringToObject(response, "message", "Hibernating (button to wake)...");
        cJSON_AddBoolToObject(response, "hibernate", true);
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