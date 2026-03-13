#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "i2c.h"
#include "log_test.h"
#include "partition_test.h"
#include "storage.h"
#include "uart_comms.h"
#include "esp_err.h"
#include "esp_log.h"
#include "endian.h"
#include "control_fsm.h"
#include "power_mgmt.h"
#include "rtc.h"
#include "sensors.h"
#include "solar.h"
#include "rf_433.h"
#include "bt_hid.h"
#include "webserver.h"
#include "comms_events.h"
#include "version.h"
#include <string.h>

EventGroupHandle_t comms_event_group = NULL;

#define TAG "MAIN"

#define POST_MAX_RETRIES 3
#define OTA_ROLLBACK_THRESHOLD 5
#define FACTORY_RESET_HOLD_MS 10000

// Survives resets (panic, WDT, sw reset) but NOT power-on or external reset
RTC_DATA_ATTR static uint8_t ota_reset_counter = 0;

// Try an init function up to POST_MAX_RETRIES times. On final failure, reboot.
// Critical inits (ADC, I2C, storage, FSM, sensors) use this — a permanent failure
// feeds the OTA rollback reset counter via the panic→reboot path.
static void init_critical(const char *name, esp_err_t (*fn)(void)) {
    for (int attempt = 1; attempt <= POST_MAX_RETRIES; attempt++) {
        esp_err_t err = fn();
        if (err == ESP_OK) return;
        ESP_LOGE(TAG, "%s FAILED (attempt %d/%d): %s", name, attempt, POST_MAX_RETRIES, esp_err_to_name(err));
        if (attempt < POST_MAX_RETRIES) vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGE(TAG, "%s FAILED after %d attempts — rebooting", name, POST_MAX_RETRIES);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

int64_t last_bat_log_time = 0;
esp_err_t send_bat_log() {
	if(!rtc_is_set()) return ESP_OK;
	
	uint8_t entry[12] = {};
	
    // Pack 64-bit timestamp into bytes 1-8
    uint64_t be_timestamp = rtc_get_ms();
    memcpy(&entry[0], &be_timestamp, 8);
    
    // Pack 32-bit voltages/currents into bytes 9-24
    float be_voltage = get_battery_V();
    memcpy(&entry[8],  &be_voltage,  4);
    
    last_bat_log_time = esp_timer_get_time();
    
    log_write(entry, 12, LOG_TYPE_BAT);
    
    return ESP_OK;
}


typedef enum {
	LED_STATE_DRIVING,
	LED_STATE_ERROR,
	LED_STATE_AWAKE,
	LED_STATE_CANCELLING,
	LED_STATE_ERRORED,
	LED_STATE_START1,
	LED_STATE_START2,
	LED_STATE_START3,
	LED_STATE_START4,
	LED_STATE_BOOTING
} led_state_t;

void drive_leds(led_state_t state) {
	uint8_t patterns[5][12] = {
		{1,3,7,6,4,0},
		{0b101,0b001},
		{1,1,1,1,1,1, 1,1,1,3},
		{4,2},
		{0b001, 0b101},
	};
	switch(state) {
		case LED_STATE_DRIVING:
			i2c_set_led1(patterns[state][(esp_timer_get_time()/100000) % 6]);
			break;
		case LED_STATE_ERROR:
			//ESP_LOGE(TAG, "SOME SORT OF ERROR");
			i2c_set_led1(patterns[state][(esp_timer_get_time()/1000000) % 2]);
			break;
		case LED_STATE_AWAKE:
			i2c_set_led1(patterns[state][(esp_timer_get_time()/200000) % 10]);
			break;
		case LED_STATE_CANCELLING:
			i2c_set_led1(patterns[state][(esp_timer_get_time()/200000) % 2]);
			break;
		
		case LED_STATE_ERRORED:
			i2c_set_led1(patterns[state][(esp_timer_get_time()/200000) % 2]);
			break;
			
		case LED_STATE_BOOTING:
			i2c_set_led1(0b001);
			break;
			
		case LED_STATE_START1:
			i2c_set_led1(0b000);
			break;
		case LED_STATE_START2:
			i2c_set_led1(0b001);
			break;
		case LED_STATE_START3:
			i2c_set_led1(0b011);
			break;
		case LED_STATE_START4:
			i2c_set_led1(0b111);
			break;
	}
}

void app_main(void) {esp_task_wdt_add(NULL);

    ESP_LOGI(TAG, "Firmware: %s", FIRMWARE_STRING);
    ESP_LOGI(TAG, "Version: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "Branch: %s", FIRMWARE_BRANCH);
    ESP_LOGI(TAG, "Built: %s", BUILD_DATE);
    
    // TODO: Confirm whether external RTC crystal can be dropped (see TODO.md #13)
    if (rtc_xtal_init() != ESP_OK) ESP_LOGE(TAG, "RTC FAILED");
    rtc_restore_time();  // Recover time from RTC domain if we crashed

    // Critical inits — retry up to 3 times, then reboot (feeds OTA rollback counter)
    init_critical("I2C", i2c_init);
    i2c_post();  // verify TCA9555 responds
    i2c_set_relays((relay_port_t){.raw=0});
    drive_leds(LED_STATE_BOOTING);
    
    
    // Factory reset: cold boot + button held for 10s
    // LEDs flash while waiting, go solid when triggered
    esp_reset_reason_t boot_reset_reason = esp_reset_reason();
    if ((boot_reset_reason == ESP_RST_POWERON || boot_reset_reason == ESP_RST_EXT)
        && gpio_get_level(GPIO_NUM_13) == 0) {
        ESP_LOGW(TAG, "Button held on cold boot — hold %ds for factory reset", FACTORY_RESET_HOLD_MS / 1000);

        // Flash all LEDs while user holds button (100ms on/off cycle)
        int held_ms = 0;
        while (gpio_get_level(GPIO_NUM_13) == 0 && held_ms < FACTORY_RESET_HOLD_MS) {
            i2c_set_led1((held_ms / 100) % 2 ? 0b111 : 0b000);
            vTaskDelay(pdMS_TO_TICKS(100));
            held_ms += 100;
            esp_task_wdt_reset();
        }

        if (held_ms < FACTORY_RESET_HOLD_MS) {
            ESP_LOGI(TAG, "Button released early (%dms) — skipping factory reset", held_ms);
            i2c_set_led1(0b000);
        } else {
            // Solid LEDs = reset triggered
            i2c_set_led1(0b111);
            ESP_LOGW(TAG, "FACTORY RESET TRIGGERED");

            // Initialize storage so we can erase it
            if (storage_init() != ESP_OK) ESP_LOGE(TAG, "STORAGE FAILED");

            esp_err_t reset_err = factory_reset();
            if (reset_err == ESP_OK) {
                ESP_LOGI(TAG, "Factory reset completed successfully");
                // Success: green blink
                for (int i = 0; i < 5; i++) {
                    i2c_set_led1(0b010);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    i2c_set_led1(0b000);
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            } else {
                ESP_LOGE(TAG, "Factory reset failed!");
                // Error: red blink
                for (int i = 0; i < 5; i++) {
                    i2c_set_led1(0b100);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    i2c_set_led1(0b000);
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }

            ESP_LOGI(TAG, "Rebooting system...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }
    
    // Critical inits — retry up to 3 times, then reboot
    init_critical("ADC", adc_init);
    init_critical("STORAGE", storage_init);
    init_critical("LOG", log_init);

    // POST checks — verify hardware is responding correctly
    adc_post();      // ADC channels readable and not frozen
    storage_post();  // flash write-read-verify on test sector

    run_all_log_tests();

    esp_reset_reason_t reset_reason = esp_reset_reason();
    esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();

    // Log every boot: boot_info = wake_cause[7:4] | reset_reason[3:0]
    {
        uint8_t boot_entry[9] = {};
        uint64_t ts = rtc_get_ms();
        memcpy(&boot_entry[0], &ts, 8);
        boot_entry[8] = ((uint8_t)wake_cause << 4) | ((uint8_t)reset_reason & 0x0F);
        log_write(boot_entry, sizeof(boot_entry), LOG_TYPE_BOOT);
    }

    // OTA rollback: count consecutive abnormal resets (panic/WDT).
    // Power-on and external resets clear the counter; crashes increment it.
    // After OTA_ROLLBACK_THRESHOLD consecutive crashes, roll back to the
    // previous OTA partition (if available).
    if (reset_reason == ESP_RST_POWERON || reset_reason == ESP_RST_EXT) {
        ota_reset_counter = 0;
    } else if (reset_reason == ESP_RST_PANIC    ||
               reset_reason == ESP_RST_INT_WDT  ||
               reset_reason == ESP_RST_TASK_WDT ||
               reset_reason == ESP_RST_WDT) {
        ota_reset_counter++;
        ESP_LOGW(TAG, "Crash detected (reason=%d), reset counter=%d/%d",
                 reset_reason, ota_reset_counter, OTA_ROLLBACK_THRESHOLD);

        uint8_t crash_entry[9] = {};
        uint64_t ts = rtc_get_ms();
        memcpy(&crash_entry[0], &ts, 8);
        crash_entry[8] = (uint8_t)reset_reason;
        log_write(crash_entry, sizeof(crash_entry), LOG_TYPE_CRASH);

        if (ota_reset_counter >= OTA_ROLLBACK_THRESHOLD) {
            ESP_LOGE(TAG, "Rollback threshold reached — marking app invalid");
            esp_ota_mark_app_invalid_rollback_and_reboot();
            // Does not return — reboots into previous OTA slot
        }
    }

    if (solar_run_fsm() != ESP_OK) ESP_LOGE(TAG, "SOLAR FAILED");

    send_bat_log();

    /*** FULL BOOT ***/
    // Critical — must succeed or reboot
    init_critical("UART", uart_init);
    init_critical("SENSORS", sensors_init);
    init_critical("FSM", fsm_init);

    // Create event group before non-critical inits (they set bits on it)
    comms_event_group = xEventGroupCreate();

    // Non-critical — retry once on failure, then log and continue
    if (rf_433_init() != ESP_OK) {
        ESP_LOGW(TAG, "RF init failed, retrying...");
        vTaskDelay(pdMS_TO_TICKS(200));
        if (rf_433_init() != ESP_OK) ESP_LOGE(TAG, "RF FAILED (continuing without RF)");
    }
    if (bt_hid_init() != ESP_OK) {
        ESP_LOGW(TAG, "BT init failed, retrying...");
        vTaskDelay(pdMS_TO_TICKS(200));
        if (bt_hid_init() != ESP_OK) ESP_LOGE(TAG, "BT HID FAILED (continuing without BT)");
    }
    if (webserver_init() != ESP_OK) {
        ESP_LOGW(TAG, "Webserver init failed, retrying...");
        vTaskDelay(pdMS_TO_TICKS(500));
        if (webserver_init() != ESP_OK) ESP_LOGE(TAG, "WEBSERVER FAILED (continuing without WiFi)");
    }

    // POST + FSM started successfully — this firmware is good.
    // Clear the rollback counter and mark the OTA partition as valid.
    ota_reset_counter = 0;
    esp_ota_mark_app_valid_cancel_rollback();

    /*** MAIN LOOP ***/
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(50);

    while(true) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        
        /* In soft idle: slow poll (5s) via direct GPIO, no I2C. */
        // TODO: Critique & confirm what we do in idle
        if (soft_idle_is_active()) {
            //vTaskDelay(pdMS_TO_TICKS(1000));
            if (soft_idle_button_raw()) {
                rtc_reset_shutdown_timer();
                soft_idle_exit();
                i2c_poll_buttons();  /* sync TCA9555 state after idle */
                xLastWakeTime = xTaskGetTickCount();
            }
            if (rtc_alarm_tripped()) {
                soft_idle_exit();
                xLastWakeTime = xTaskGetTickCount();
                // Wait for WiFi + BT to come back up (or timeout after 5s)
                if (comms_event_group) {
                    xEventGroupWaitBits(comms_event_group, COMMS_ALL_BITS,
                                        pdFALSE, pdTRUE, pdMS_TO_TICKS(5000));
                }
                esp_task_wdt_reset();
                fsm_request(FSM_CMD_START);
                rtc_schedule_next_alarm();
            }
            solar_run_fsm();
            rtc_check_shutdown_timer();
            esp_task_wdt_reset();
            continue;
        }

        i2c_poll_buttons();

        if (i2c_get_button_state(0)) {
        	rtc_reset_shutdown_timer();
        	soft_idle_exit();
        }
        
        // TODO: Make sure all ISRs are clean (very tight, no blocking functions)
        	
        switch (fsm_get_state()) {
			case STATE_IDLE:
        		// LED cue for user
        		if (i2c_get_button_ms(0) > 1600){
        			drive_leds(LED_STATE_START4);
				} else if (i2c_get_button_ms(0) > 1100){
        			drive_leds(LED_STATE_START3);
				} else if (i2c_get_button_ms(0) > 600){
        			drive_leds(LED_STATE_START2);
        		} else if (i2c_get_button_ms(0) > 100){
        			drive_leds(LED_STATE_START1);
        		} else {
					if (
						rtc_is_set() &&
						efuse_get(BRIDGE_JACK)==EFUSE_OK &&
						efuse_get(BRIDGE_AUX)==EFUSE_OK &&
						efuse_get(BRIDGE_DRIVE)==EFUSE_OK &&
						fsm_get_error() == ESP_OK
					) {
        				drive_leds(LED_STATE_AWAKE);
        			} else {
        				drive_leds(LED_STATE_ERROR);
        			}
        			
        			/*int8_t state = 0b001;
        			if (get_is_safe()) state |= 0b010;
        			if (get_sensor(SENSOR_SAFETY)) state |= 0b100;
        			i2c_set_led1(state);*/
				}
				
				// when not actively moving we log at a low frequency (every 120s)
				if ((esp_timer_get_time() > last_bat_log_time + 120000000ULL))
				  send_bat_log();
				
				if(i2c_get_button_ms(0) > 2100)
        			fsm_request(FSM_CMD_START);
        		break;
			//case STATE_UNDO_JACK:
			case STATE_UNDO_JACK_START:
				// it's running the jack, but undoing
				//send_log();
        		drive_leds(LED_STATE_CANCELLING);
				if (i2c_get_button_tripped(0)) {
					ESP_LOGI(TAG, "AAAAH STOP!!!");
        			fsm_request(FSM_CMD_STOP);
				}
        		break;
        		
        	case STATE_CALIBRATE_JACK_DELAY:
        		//send_log();
        		if (i2c_get_button_tripped(0))
	        		fsm_request(FSM_CMD_CALIBRATE_JACK_START);
        		break;
        	case STATE_CALIBRATE_JACK_MOVE:
        		//send_log();
        		if (i2c_get_button_tripped(0))
	        		fsm_request(FSM_CMD_CALIBRATE_JACK_END);
        		break;
        		
        		
        	case STATE_CALIBRATE_DRIVE_DELAY:
        		//send_log();
        		if (i2c_get_button_tripped(0))
	        		fsm_request(FSM_CMD_CALIBRATE_DRIVE_START);
        		break;
        	case STATE_CALIBRATE_DRIVE_MOVE:
        		//send_log();
        		if (i2c_get_button_tripped(0))
	        		fsm_request(FSM_CMD_CALIBRATE_DRIVE_END);
        		break;
        		
			default:
				// it's running in every other case
				//send_log();
        		drive_leds(LED_STATE_DRIVING);
				if (i2c_get_button_tripped(0)) {
        			fsm_request(FSM_CMD_UNDO);
				}
        		break;
		}
        
        
        
        
        if (rtc_alarm_tripped()) {
        	fsm_request(FSM_CMD_START);
        	rtc_schedule_next_alarm();
        }

        solar_run_fsm();
        rtc_check_shutdown_timer();
        esp_task_wdt_reset();
	}
}