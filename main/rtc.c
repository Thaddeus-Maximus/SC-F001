/*
 * system.c
 *
 * Implementation of system.h services.
 * Battery charge-state machine, deep-sleep, RTC, inactivity handling.
 *
 * Battery voltage is read from the shared volatile updated by power_mgmt_task.
 */

#include <stdbool.h>
#include <time.h>
#include <sys/time.h>

#include "power_mgmt.h"
#include "rtc.h"
#include "control_fsm.h"
#include "esp_sleep.h"
#include "i2c.h" // for lcd_off()
#include "driver/gpio.h"
#include "rtc_wdt.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c.h"
#include "rtc_wdt.h"
#include "filemgmt.h"

//#include "esp32/rtc_clk.h"  // For RTC_SLOW_FREQ_32K_XTAL enum and rtc_clk_slow_freq_set()
#include "driver/rtc_io.h"  // For RTC I/O handling (optional but recommended for pin configuration)
#include "storage.h"

#define PIN_BTN_INTERRUPT GPIO_NUM_13

#define POWER_INACTIVITY_TIMEOUT_MS 20000
#define BATTERY_CHECK_INTERVAL_SEC  30
uint64_t last_activity_tick = 0;

#define DEEP_SLEEP_US 30000000ULL /* 30 seconds in deep sleep */

// RTC_DATA_ATTR keeps this var in RTC memory; persists across sleeps (but not across boots)
RTC_DATA_ATTR int64_t next_alarm_time_s = -1;
RTC_DATA_ATTR bool rtc_set = false;
bool rtc_is_set() {
	return rtc_set;
}

esp_err_t start_rtc(void) {
    /* ---- Wake sources ---- */
    esp_sleep_enable_ext0_wakeup(PIN_BTN_INTERRUPT, 0);
    gpio_set_direction(PIN_BTN_INTERRUPT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BTN_INTERRUPT, GPIO_PULLUP_ONLY);
    esp_sleep_enable_ext0_wakeup(PIN_BTN_INTERRUPT, 0);

    /* ---- Enable External 32 kHz Oscillator ---- */
    // Configure RTC I/O pins for crystal (hold in reset initially if needed)
    rtc_gpio_init(GPIO_NUM_32);
    rtc_gpio_init(GPIO_NUM_33);
    rtc_gpio_set_direction(GPIO_NUM_32, RTC_GPIO_MODE_DISABLED);
    rtc_gpio_set_direction(GPIO_NUM_33, RTC_GPIO_MODE_DISABLED);

    // Select 32 kHz XTAL as slow clock source (wait for stabilization)
    //rtc_clk_slow_freq_set(RTC_SLOW_FREQ_32K_XTAL);
    // Optional: Brief delay for crystal stabilization (typically <1 ms)
    vTaskDelay(pdMS_TO_TICKS(1));

    //ESP_LOGI("RTC", "Configured with external 32 kHz oscillator (freq: %d Hz)", rtc_clk_slow_freq_get_hz());

    // Existing log can now be data-driven
    
    return ESP_OK;
}

void reset_shutdown_timer(void)
{
    last_activity_tick = xTaskGetTickCount();
    rtc_wdt_feed();
}

void enter_deep_sleep(void)
{
	//close_current_log();
	//fsm_request(FSM_CMD_STOP);
    i2c_set_relays(0);
    //esp_sleep_enable_timer_wakeup(DEEP_SLEEP_US);
    esp_deep_sleep_start();
}

void rtc_set_time(struct tm *tm) {
	rtc_set = true;
	start_new_log_file();
	struct timeval tv = { .tv_sec = mktime(tm), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    resetBatTimers();
}

void rtc_get_time(struct tm *tm)
{
    time_t raw;
    time(&raw);
    localtime_r(&raw, tm);
}

int64_t system_rtc_get_raw_time(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

uint64_t rtc_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

int64_t system_rtc_seconds_into_day(void)
{
    return system_rtc_get_raw_time() % 86400UL;
}

esp_sleep_wakeup_cause_t rtc_wakeup_cause(void)
{
    esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
    switch (c) {
        case ESP_SLEEP_WAKEUP_EXT0:  ESP_LOGI("RTC", "Wakeup: GPIO"); break;
        case ESP_SLEEP_WAKEUP_TIMER: ESP_LOGI("RTC", "Wakeup: timer"); break;
        default:                     ESP_LOGI("RTC", "Wakeup: normal boot"); break;
    }
    return c;
}

/* -------------------------------------------------------------------------- */
/*  Unified periodic update                                                   */
/* -------------------------------------------------------------------------- */
void check_shutdown_timer(void)
{
	
    TickType_t elapsed = xTaskGetTickCount() - last_activity_tick;
    if (elapsed * portTICK_PERIOD_MS >= POWER_INACTIVITY_TIMEOUT_MS)
        enter_deep_sleep();
}

/* -------------------------------------------------------------------------- */
/*  Time adjustment helpers                                                   */
/* -------------------------------------------------------------------------- */
void adjust_rtc_hour(char *key, int8_t dir)
{
    struct tm t;
    rtc_get_time(&t);
    if (dir>0) t.tm_hour ++;
    if (dir<0) t.tm_hour --;
    if (t.tm_hour > 23) t.tm_hour = 0;
    if (t.tm_hour < 0)  t.tm_hour = 23;
    rtc_set_time(&t);
    set_next_alarm();
}

void adjust_rtc_min(char *key, int8_t dir)
{
    struct tm t;
    rtc_get_time(&t);
    if (dir>0) t.tm_min ++;
    if (dir<0) t.tm_min --;
    if (t.tm_min > 59) t.tm_min = 0;
    if (t.tm_min < 0)  t.tm_min = 59;
    rtc_set_time(&t);
    set_next_alarm();
}


void set_next_alarm(void) {
    int8_t start_h = 0; //get_param_i8("sched_start");
    int8_t end_h   = 23; //get_param_i8("sched_end");
    int8_t num     = 0; //get_param_i8("sched_num");

    if (num <= 0) {
        next_alarm_time_s = -1;
        return;
    }

    // Current time info
    uint32_t s_into_day = system_rtc_seconds_into_day();
    time_t current_time = system_rtc_get_raw_time();
    time_t today_midnight = current_time - s_into_day;

    int start_sec = start_h * 3600;
    int end_sec   = end_h   * 3600;

    bool overnight = (start_h > end_h);
    int total_duration = overnight ? (86400 - start_sec) + end_sec : end_sec - start_sec;

    // Determine period start
    time_t period_start;
    if (overnight && s_into_day < end_sec) {
        // Current time is within overnight period → started yesterday
        period_start = (today_midnight - 86400) + start_sec;
    } else {
        // Normal or after end → starts today
        period_start = today_midnight + start_sec;
    }

    //time_t period_end = period_start + total_duration;

    if (num == 1) {
        // Single alarm: at period start, if passed, next day
        next_alarm_time_s = (current_time < period_start) ? period_start : period_start + 86400;
    	ESP_LOGI("ALARM", "SET FOR %lld (in %lld s)", next_alarm_time_s, next_alarm_time_s - current_time);
        return;
    }

    // Find next alarm
    int64_t spacing = total_duration / (num - 1);
    time_t next_alarm = -1;

    for (int8_t i = 0; i < num; i++) {
        time_t alarm_time = period_start + spacing * i;
        if (alarm_time > current_time) {
            next_alarm = alarm_time;
            break;
        }
    }

    // If all passed, first of next period
    if (next_alarm == -1) {
        next_alarm = period_start + 86400;
    }

    next_alarm_time_s = next_alarm;

    ESP_LOGI("ALARM", "SET FOR %lld (in %lld s)", next_alarm_time_s, next_alarm_time_s - current_time);
}

bool alarm_tripped() {
    if (!rtc_is_set())
        return false;
    if (next_alarm_time_s < 0) {
        set_next_alarm();
        return false;
    }
    return system_rtc_get_raw_time() > next_alarm_time_s;
}