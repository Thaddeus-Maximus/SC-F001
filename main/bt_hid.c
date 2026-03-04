/*
 * bt_hid.c
 *
 * BLE HID host for SC-F001.
 *
 * Architecture
 * ------------
 *   bt_hid_scan_task  (priority 4, 6 KB)
 *     - On boot: checks NVS for a saved BDA.  If found, attempts
 *       esp_hidh_dev_open() directly and waits BT_HID_BOND_TIMEOUT_MS.
 *       If that times out (device not nearby) falls through to scan.
 *     - While disconnected: runs a BLE scan for BT_HID_SCAN_DURATION_S
 *       seconds, picks the HID device with best RSSI, opens it.
 *     - While connected: sleeps 500 ms between checks.
 *     - Every BT_HID_REPEAT_MS the held-button state is sampled and
 *       pulse_override() is called if a mapped key is down.
 *
 *   hidh_callback  (runs in esp_hidh event task)
 *     - OPEN:  saves BDA to NVS, marks connected.
 *     - CLOSE: clears connected flag, clears held key.
 *     - INPUT: decodes usage code, sets s_held_key.
 *
 *   ble_gap_event_handler  (runs in BT stack task)
 *     - Collects HID-UUID scan results.
 *     - Signals scan_semaphore when scan params set / scan complete.
 *     - Handles pairing/auth confirmations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "esp_timer.h"

#include "bt_hid.h"
#include "control_fsm.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define TAG                     "BT_HID"
#define BT_HID_SCAN_DURATION_S  5
#define BT_HID_RECONNECT_MS     2000
#define BT_HID_BOND_TIMEOUT_MS  5000   /* wait for saved device before scanning */
#define BT_HID_CONNECT_WAIT_MS  5000   /* wait after open() before next loop */
#define BT_HID_NVS_NAMESPACE    "bt_hid"
#define BT_HID_NVS_BDA_KEY      "bda"

// ---------------------------------------------------------------------------
// HID usage codes -> machine actions
// ---------------------------------------------------------------------------

/*
 * Usage codes sent by generic BLE media remotes (HID Consumer Control page).
 * Only the four we care about are mapped; everything else is ignored.
 */
#define USAGE_VOL_UP    0x00E9u   /* -> jack up   */
#define USAGE_VOL_DOWN  0x00EAu   /* -> jack down */
#define USAGE_PREV      0x00B6u   /* -> reverse   */
#define USAGE_NEXT      0x00B5u   /* -> forward   */
#define USAGE_NONE      0x0000u

// ---------------------------------------------------------------------------
// Shared remote state
// ---------------------------------------------------------------------------

typedef enum {
    REMOTE_DISCONNECTED = 0,
    REMOTE_CONNECTED,
} remote_conn_state_t;

typedef struct {
    remote_conn_state_t conn_state;
    uint16_t            held_key;   /* current usage code, 0 = nothing held */
    esp_bd_addr_t       bda;        /* address of the connected device       */
} remote_state_t;

static remote_state_t    s_remote  = { .conn_state = REMOTE_DISCONNECTED };
static portMUX_TYPE      s_mux     = portMUX_INITIALIZER_UNLOCKED;

/* Semaphore signalled by GAP callback to unblock the scan task. */
static SemaphoreHandle_t s_scan_sem = NULL;

/* Set once we have a saved BDA from NVS (direct reconnect path). */
static bool          s_has_saved_bda  = false;
static esp_bd_addr_t s_saved_bda      = {0};

// ---------------------------------------------------------------------------
// Scan-result list (heap-allocated, freed after each scan round)
// ---------------------------------------------------------------------------

typedef struct scan_result_s {
    esp_bd_addr_t        bda;
    esp_ble_addr_type_t  addr_type;
    int                  rssi;
    char                 name[64];
    struct scan_result_s *next;
} scan_result_t;

static scan_result_t *s_scan_results     = NULL;
static size_t         s_num_scan_results = 0;

static scan_result_t *find_scan_result(const esp_bd_addr_t bda)
{
    scan_result_t *r = s_scan_results;
    while (r) {
        if (memcmp(bda, r->bda, sizeof(esp_bd_addr_t)) == 0) return r;
        r = r->next;
    }
    return NULL;
}

static void add_scan_result(const esp_bd_addr_t bda,
                            esp_ble_addr_type_t  addr_type,
                            const char          *name,
                            int                  rssi)
{
    if (find_scan_result(bda)) return; /* deduplicate */
    scan_result_t *r = (scan_result_t *)calloc(1, sizeof(scan_result_t));
    if (!r) return;
    memcpy(r->bda, bda, sizeof(esp_bd_addr_t));
    r->addr_type = addr_type;
    r->rssi      = rssi;
    if (name) strncpy(r->name, name, sizeof(r->name) - 1);
    r->next          = s_scan_results;
    s_scan_results   = r;
    s_num_scan_results++;
}

static void free_scan_results(void)
{
    scan_result_t *r;
    while (s_scan_results) {
        r              = s_scan_results;
        s_scan_results = s_scan_results->next;
        free(r);
    }
    s_num_scan_results = 0;
}

// ---------------------------------------------------------------------------
// NVS helpers — store / load the last-connected BDA
// ---------------------------------------------------------------------------

static void nvs_save_bda(const esp_bd_addr_t bda)
{
    nvs_handle_t h;
    if (nvs_open(BT_HID_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, BT_HID_NVS_BDA_KEY, bda, sizeof(esp_bd_addr_t));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Saved BDA %02x:%02x:%02x:%02x:%02x:%02x to NVS",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

static bool nvs_load_bda(esp_bd_addr_t out_bda)
{
    nvs_handle_t h;
    if (nvs_open(BT_HID_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(esp_bd_addr_t);
    bool ok = (nvs_get_blob(h, BT_HID_NVS_BDA_KEY, out_bda, &len) == ESP_OK);
    nvs_close(h);
    return ok;
}

// ---------------------------------------------------------------------------
// Map a HID usage code to an FSM override command
// Returns false if the usage is not mapped.
// ---------------------------------------------------------------------------

static bool usage_to_override(uint16_t usage, fsm_override_t *out)
{
    switch (usage) {
    case USAGE_VOL_UP:   *out = FSM_OVERRIDE_JACK_UP;    return true;
    case USAGE_VOL_DOWN: *out = FSM_OVERRIDE_JACK_DOWN;  return true;
    case USAGE_PREV:     *out = FSM_OVERRIDE_DRIVE_REV;  return true;
    case USAGE_NEXT:     *out = FSM_OVERRIDE_DRIVE_FWD;  return true;
    default:                                              return false;
    }
}

// ---------------------------------------------------------------------------
// HID Host event callback
// Runs in the esp_hidh internal event task — keep it short, no blocking.
// ---------------------------------------------------------------------------

static void hidh_callback(void *handler_args,
                          esp_event_base_t base,
                          int32_t          id,
                          void            *event_data)
{
    esp_hidh_event_t       event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *p     = (esp_hidh_event_data_t *)event_data;

    switch (event) {

    case ESP_HIDH_OPEN_EVENT: {
        if (p->open.status == ESP_OK) {
            const uint8_t *bda = esp_hidh_dev_bda_get(p->open.dev);
            ESP_LOGI(TAG, "CONNECTED: %02x:%02x:%02x:%02x:%02x:%02x '%s'",
                     bda[0], bda[1], bda[2], bda[3], bda[4], bda[5],
                     esp_hidh_dev_name_get(p->open.dev));
            esp_hidh_dev_dump(p->open.dev, stdout);

            taskENTER_CRITICAL(&s_mux);
            s_remote.conn_state = REMOTE_CONNECTED;
            s_remote.held_key   = USAGE_NONE;
            memcpy(s_remote.bda, bda, sizeof(esp_bd_addr_t));
            taskEXIT_CRITICAL(&s_mux);

            nvs_save_bda(bda);
        } else {
            ESP_LOGE(TAG, "OPEN failed, status=%d", p->open.status);
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        taskENTER_CRITICAL(&s_mux);
        s_remote.conn_state = REMOTE_DISCONNECTED;
        s_remote.held_key   = USAGE_NONE;
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "DISCONNECTED — will rescan");
        break;
    }

    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "Battery: %d%%", p->battery.level);
        break;

    case ESP_HIDH_INPUT_EVENT: {
        /*
         * Decode usage code from the first two bytes of the input report.
         * BLE consumer-control remotes typically send a 16-bit usage on
         * report ID 3 (or similar), zero on key-up.
         */
        uint16_t usage = USAGE_NONE;
        if (p->input.length >= 2) {
            usage = (uint16_t)(p->input.data[0] | ((uint16_t)p->input.data[1] << 8));
        } else if (p->input.length == 1) {
            usage = p->input.data[0];
        }

        taskENTER_CRITICAL(&s_mux);
        s_remote.held_key = usage;
        taskEXIT_CRITICAL(&s_mux);

        /* Log mapped presses only (not key-up noise). */
        if (usage != USAGE_NONE) {
            fsm_override_t dummy;
            if (usage_to_override(usage, &dummy)) {
                ESP_LOGI(TAG, "KEY DOWN 0x%04X", usage);
            } else {
                ESP_LOGD(TAG, "KEY 0x%04X (unmapped)", usage);
            }
        } else {
            ESP_LOGD(TAG, "KEY UP");
        }
        break;
    }

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// BLE GAP event handler
// ---------------------------------------------------------------------------

static void ble_gap_event_handler(esp_gap_ble_cb_event_t   event,
                                  esp_ble_gap_cb_param_t  *param)
{
    switch (event) {

    /* Scan parameters accepted — safe to start scanning now. */
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        xSemaphoreGive(s_scan_sem);
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
        switch (param->scan_rst.search_evt) {

        case ESP_GAP_SEARCH_INQ_RES_EVT: {
            uint8_t *adv = param->scan_rst.ble_adv;

            /* Only keep devices that advertise the HID service UUID (0x1812). */
            uint8_t  uuid_len = 0;
            uint8_t *uuid_d   = esp_ble_resolve_adv_data(
                                    adv, ESP_BLE_AD_TYPE_16SRV_CMPL, &uuid_len);
            uint16_t uuid = 0;
            if (uuid_d && uuid_len >= 2) {
                uuid = (uint16_t)(uuid_d[0] | ((uint16_t)uuid_d[1] << 8));
            }
            if (uuid != ESP_GATT_UUID_HID_SVC) break;

            /* Parse device name (prefer complete, fall back to short). */
            uint8_t  name_len = 0;
            uint8_t *name_d   = esp_ble_resolve_adv_data(
                                    adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
            if (!name_d || !name_len) {
                name_d = esp_ble_resolve_adv_data(
                             adv, ESP_BLE_AD_TYPE_NAME_SHORT, &name_len);
            }
            char name[64] = {0};
            if (name_d && name_len > 0) {
                size_t copy = name_len < (sizeof(name) - 1) ? name_len : (sizeof(name) - 1);
                memcpy(name, name_d, copy);
            }

            ESP_LOGI(TAG, "SCAN %02x:%02x:%02x:%02x:%02x:%02x  RSSI:%d  '%s'  <<< HID",
                     param->scan_rst.bda[0], param->scan_rst.bda[1],
                     param->scan_rst.bda[2], param->scan_rst.bda[3],
                     param->scan_rst.bda[4], param->scan_rst.bda[5],
                     param->scan_rst.rssi, name);

            add_scan_result(param->scan_rst.bda,
                            param->scan_rst.ble_addr_type,
                            name,
                            param->scan_rst.rssi);
            break;
        }

        /* Scan complete — unblock the scan task. */
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            xSemaphoreGive(s_scan_sem);
            break;

        default:
            break;
        }
        break;
    }

    /* Pairing / security events — auto-accept so bonding completes. */
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (!param->ble_security.auth_cmpl.success) {
            ESP_LOGE(TAG, "AUTH ERROR 0x%x", param->ble_security.auth_cmpl.fail_reason);
        } else {
            ESP_LOGI(TAG, "AUTH SUCCESS");
        }
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Scan + reconnect task
// ---------------------------------------------------------------------------

static void bt_hid_scan_task(void *pvParameters)
{
    esp_ble_scan_params_t scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_ENABLE,
    };

    TickType_t repeat_tick = xTaskGetTickCount();

    /* ------------------------------------------------------------------
     * On the very first iteration, try to reconnect to the saved BDA.
     * ------------------------------------------------------------------ */
    if (s_has_saved_bda) {
        ESP_LOGI(TAG, "Trying saved device %02x:%02x:%02x:%02x:%02x:%02x ...",
                 s_saved_bda[0], s_saved_bda[1], s_saved_bda[2],
                 s_saved_bda[3], s_saved_bda[4], s_saved_bda[5]);

        /*
         * We don't know the address type of the saved device at this point.
         * BLE_ADDR_TYPE_RANDOM is the most common for consumer remotes; if it
         * fails the outer scan loop will find it correctly.
         */
        esp_hidh_dev_open(s_saved_bda, ESP_HID_TRANSPORT_BLE, BLE_ADDR_TYPE_RANDOM);
        vTaskDelay(pdMS_TO_TICKS(BT_HID_BOND_TIMEOUT_MS));

        /* If the open succeeded the state will be CONNECTED — skip to main loop. */
    }

    /* ------------------------------------------------------------------
     * Main loop: scan → connect → wait → repeat if disconnected.
     * ------------------------------------------------------------------ */
    while (1) {
        /* ---- While connected: check held key and call pulse_override() ---- */
        taskENTER_CRITICAL(&s_mux);
        remote_conn_state_t conn     = s_remote.conn_state;
        uint16_t            held_key = s_remote.held_key;
        taskEXIT_CRITICAL(&s_mux);

        if (conn == REMOTE_CONNECTED) {
            /* Fire pulse_override every BT_HID_REPEAT_MS while a key is held. */
            if (xTaskGetTickCount() - repeat_tick >= pdMS_TO_TICKS(BT_HID_REPEAT_MS)) {
                repeat_tick = xTaskGetTickCount();

                fsm_override_t override_cmd;
                if (held_key != USAGE_NONE && usage_to_override(held_key, &override_cmd)) {
                    pulse_override(override_cmd);
                }
            }

            vTaskDelay(pdMS_TO_TICKS(10)); /* Short sleep to yield; repeat timer governs rate. */
            continue;
        }

        /* ---- Not connected — run a scan ---- */
        ESP_LOGI(TAG, "Scanning for HID devices (%ds)...", BT_HID_SCAN_DURATION_S);

        free_scan_results();
        esp_ble_gap_set_scan_params(&scan_params);
        xSemaphoreTake(s_scan_sem, portMAX_DELAY); /* wait: SCAN_PARAM_SET_COMPLETE */
        esp_ble_gap_start_scanning(BT_HID_SCAN_DURATION_S);
        xSemaphoreTake(s_scan_sem, portMAX_DELAY); /* wait: SCAN_INQ_CMPL          */

        ESP_LOGI(TAG, "Found %u HID device(s)", (unsigned)s_num_scan_results);

        if (s_num_scan_results > 0) {
            /* Pick the device with the strongest signal. */
            scan_result_t *r    = s_scan_results;
            scan_result_t *best = r;
            while (r) {
                if (r->rssi > best->rssi) best = r;
                r = r->next;
            }

            ESP_LOGI(TAG, "Connecting to %02x:%02x:%02x:%02x:%02x:%02x '%s' (RSSI:%d)",
                     best->bda[0], best->bda[1], best->bda[2],
                     best->bda[3], best->bda[4], best->bda[5],
                     best->name, best->rssi);

            esp_hidh_dev_open(best->bda, ESP_HID_TRANSPORT_BLE, best->addr_type);
            free_scan_results();

            /* Give the connection time to establish before looping. */
            vTaskDelay(pdMS_TO_TICKS(BT_HID_CONNECT_WAIT_MS));
        } else {
            ESP_LOGI(TAG, "No HID devices found, retrying in %dms...", BT_HID_RECONNECT_MS);
            vTaskDelay(pdMS_TO_TICKS(BT_HID_RECONNECT_MS));
        }

        repeat_tick = xTaskGetTickCount(); /* reset repeat timer after reconnect */
    }
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------

esp_err_t bt_hid_init(void)
{
    esp_err_t ret;

    /* Release classic BT memory — we only use BLE. */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&bd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* esp_hidh posts events to the default event loop — create it if the
     * webserver hasn't done so yet.  ESP_ERR_INVALID_STATE means it already
     * exists, which is fine. */
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_ble_gap_register_callback(ble_gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler));

    esp_hidh_config_t hidh_cfg = {
        .callback         = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg     = NULL,
    };
    ret = esp_hidh_init(&hidh_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hidh_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Try to load a previously bonded device address from NVS. */
    s_has_saved_bda = nvs_load_bda(s_saved_bda);
    if (s_has_saved_bda) {
        ESP_LOGI(TAG, "Found saved BDA in NVS — will try direct reconnect first");
    }

    s_scan_sem = xSemaphoreCreateBinary();
    if (!s_scan_sem) {
        ESP_LOGE(TAG, "Failed to create scan semaphore");
        return ESP_ERR_NO_MEM;
    }

    /*
     * Priority 4: above webserver (typically 5–6 on core 0) is fine; below
     * the FSM (10) and RF task (5) so control always wins CPU.
     */
    xTaskCreate(bt_hid_scan_task, "bt_hid_scan", 6 * 1024, NULL, 4, NULL);

    ESP_LOGI(TAG, "BLE HID host initialised");
    return ESP_OK;
}
