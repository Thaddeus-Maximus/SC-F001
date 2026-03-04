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

// External 32kHz crystal enabled via CONFIG_RTC_CLK_SRC_EXT_CRYS in sdkconfig.defaults
#include "driver/rtc_io.h"  // For RTC I/O handling (optional but recommended for pin configuration)
#include "soc/rtc.h"         // For rtc_clk_slow_src_get()
#include "solar.h"
#include "storage.h"

#define PIN_BTN_INTERRUPT GPIO_NUM_13

uint64_t last_activity_tick = 0;

// RTC_DATA_ATTR keeps these in RTC memory; persists across deep sleep AND software resets (panics, WDT)
RTC_DATA_ATTR int64_t next_alarm_time_s = -1;
RTC_DATA_ATTR bool rtc_set = false;
RTC_DATA_ATTR int64_t rtc_backup_s = 0;  // Crash-safe time snapshot
bool rtc_is_set() {
	return rtc_set;
}

esp_err_t rtc_xtal_init(void) {
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

    // 32kHz crystal selected as slow clock source via CONFIG_RTC_CLK_SRC_EXT_CRYS.
    // Verify at runtime that the clock source is actually the external crystal.
    soc_rtc_slow_clk_src_t slow_src = rtc_clk_slow_src_get();
    if (slow_src == SOC_RTC_SLOW_CLK_SRC_XTAL32K) {
        ESP_LOGI("RTC", "Slow clock: external 32kHz crystal");
    } else {
        ESP_LOGW("RTC", "Slow clock source is %d — expected XTAL32K (%d). Check sdkconfig.",
                 (int)slow_src, (int)SOC_RTC_SLOW_CLK_SRC_XTAL32K);
    }

    return ESP_OK;
}

void rtc_reset_shutdown_timer(void)
{
    last_activity_tick = xTaskGetTickCount();
    rtc_wdt_feed();
}

void rtc_enter_deep_sleep(void)
{
	//close_current_log();
	fsm_request(FSM_CMD_STOP);
    i2c_stop();
    esp_sleep_enable_timer_wakeup(DEEP_SLEEP_US);
    esp_deep_sleep_start();
}

int64_t rtc_get_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}


void rtc_set_s(int64_t tv_sec)
{
	rtc_set = true;
    settimeofday(&(struct timeval){.tv_sec = tv_sec, .tv_usec=0}, NULL);
    rtc_backup_s = tv_sec;
    solar_reset_fsm();
    rtc_schedule_next_alarm();
}

void rtc_save_time(void)
{
    if (rtc_set)
        rtc_backup_s = rtc_get_s();
}

void rtc_restore_time(void)
{
    if (rtc_set && rtc_backup_s > 0) {
        settimeofday(&(struct timeval){.tv_sec = rtc_backup_s, .tv_usec = 0}, NULL);
        ESP_LOGI("RTC", "Time restored from crash backup: %lld", (long long)rtc_backup_s);
    }
}

int64_t rtc_get_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

int64_t rtc_get_s_in_day(void)
{
    return rtc_get_s() % 86400UL;
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
void rtc_check_shutdown_timer(void)
{
	
    TickType_t elapsed = xTaskGetTickCount() - last_activity_tick;
    if (elapsed * portTICK_PERIOD_MS >= POWER_INACTIVITY_TIMEOUT_MS)
        rtc_enter_deep_sleep();
}

/* -------------------------------------------------------------------------- */
/*  Time adjustment helpers                                                   */
/* -------------------------------------------------------------------------- */
/*void adjust_rtc_hour(char *key, int8_t dir)
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
}*/


void rtc_schedule_next_alarm(void) {
    int64_t start_sec = get_param_value_t(PARAM_MOVE_START).u32;
    int64_t end_sec   = get_param_value_t(PARAM_MOVE_END).u32;
    int16_t  num      = get_param_value_t(PARAM_NUM_MOVES).i16;

    if (num <= 0) {
        next_alarm_time_s = -1;
        return;
    }

    // Current time info
    int64_t s_into_day = rtc_get_s_in_day();
    time_t current_time = rtc_get_s();
    time_t today_midnight = current_time - s_into_day;

    bool overnight = (start_sec > end_sec);
    int64_t total_duration = overnight ? (86400 - start_sec) + end_sec : end_sec - start_sec;

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

    for (int16_t i = 0; i < num; i++) {
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

int64_t rtc_get_next_alarm_s() {
	return next_alarm_time_s;
}

bool rtc_alarm_tripped() {
    if (!rtc_is_set())
        return false;
    if (next_alarm_time_s < 0) {
        rtc_schedule_next_alarm();
        return false;
    }
    return rtc_get_s() > next_alarm_time_s;
}