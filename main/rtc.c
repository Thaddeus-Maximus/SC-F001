/*
 * system.c
 *
 * Implementation of system.h services.
 * Battery charge-state machine, deep-sleep, RTC, inactivity handling.
 *
 * Battery voltage is read from the shared volatile updated by power_mgmt_task.
 */

#include <stdbool.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "power_mgmt.h"
#include "rtc.h"
#include "control_fsm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "i2c.h"
#include "driver/gpio.h"
#include "rtc_wdt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtc_wdt.h"
#include "soc/rtc.h"
#include "solar.h"
#include "storage.h"
#include "webserver.h"
#include "bt_hid.h"

#define PIN_BTN_INTERRUPT GPIO_NUM_13

// Return microseconds from the RTC hardware timer.
// Used ONLY in rtc_restore_time() for crash-recovery (survives panics/WDT via RTC domain).
// RC oscillator drift (~150 kHz, ±5%) is negligible over a <30s crash restart (~1.5s worst case).
static uint64_t rtc_hw_time_us(void)
{
    uint32_t cal   = rtc_clk_cal(RTC_CAL_RTC_MUX, 20);
    uint64_t ticks = rtc_time_get();
    return (ticks * (uint64_t)cal) >> 19;
}

uint64_t last_activity_tick = 0;

// RTC_DATA_ATTR keeps these in RTC memory; persists across software resets (panics, WDT).
// AUDIT: no init path zeroes these — rtc_restore_time() recovers via RTC HW counter,
// rtc_set_s() is only called explicitly by the user. Verified 2026-03-12.
RTC_DATA_ATTR int64_t  next_alarm_time_s = -1;
RTC_DATA_ATTR bool     rtc_set           = false;
RTC_DATA_ATTR int64_t  sync_unix_us      = 0;   // Unix time in µs at last rtc_set_s() call
RTC_DATA_ATTR uint64_t sync_rtc_us       = 0;   // rtc_hw_time_us() at last rtc_set_s() call (for crash recovery)

// esp_timer value at last rtc_set_s() call.  NOT RTC_DATA_ATTR — resets on every boot;
// rtc_restore_time() reinitialises it from the RTC hardware counter on crash recovery.
static uint64_t sync_esp_us = 0;

static bool in_soft_idle = false;

bool rtc_is_set() {
	return rtc_set;
}

esp_err_t rtc_xtal_init(void) {
    // 32kHz crystal no longer used — time tracking via esp_timer (40MHz APB crystal).
    // Just configure the button GPIO; no sleep wakeup sources needed.
    gpio_set_direction(PIN_BTN_INTERRUPT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_BTN_INTERRUPT, GPIO_PULLUP_ONLY);
    return ESP_OK;
}

void rtc_reset_shutdown_timer(void)
{
    last_activity_tick = xTaskGetTickCount();
    rtc_wdt_feed();
}

void soft_idle_enter(void)
{
    if (in_soft_idle) return;
    in_soft_idle = true;
    ESP_LOGI("RTC", "Entering soft idle (WiFi/BT off, LEDs off)");
    webserver_stop();
    bt_hid_stop();
    i2c_set_led1(0);
}

bool soft_idle_is_active(void)  { return in_soft_idle; }
bool soft_idle_button_raw(void) { return gpio_get_level(PIN_BTN_INTERRUPT) == 0; }

void soft_idle_exit(void)
{
    if (!in_soft_idle) return;
    in_soft_idle = false;
    ESP_LOGI("RTC", "Exiting soft idle");
    webserver_restart_wifi();
    bt_hid_resume();
    rtc_reset_shutdown_timer();
}

int64_t rtc_get_s(void)
{
    if (!rtc_set) return 0;
    return (int64_t)(sync_unix_us / 1000000LL)
           + (int64_t)((esp_timer_get_time() - sync_esp_us) / 1000000LL);
}


void rtc_set_s(int64_t tv_sec)
{
    sync_unix_us = tv_sec * 1000000LL;
    sync_rtc_us  = rtc_hw_time_us();  // kept for crash recovery in rtc_restore_time()
    sync_esp_us  = (uint64_t)esp_timer_get_time();
    rtc_set      = true;
    // Keep stdlib (gmtime_r etc.) in sync
    settimeofday(&(struct timeval){.tv_sec = tv_sec, .tv_usec = 0}, NULL);
    solar_reset_fsm();
    rtc_schedule_next_alarm();

    uint64_t ts_ms = (uint64_t)tv_sec * 1000ULL;
    log_write((uint8_t*)&ts_ms, sizeof(ts_ms), LOG_TYPE_TIME_SET);

    // Parseable marker used by logtool/rtc_test.py to compare device vs host time
    ESP_LOGI("RTC", "TIME unix=%lld src=SYNC uptime=%llds",
             (long long)tv_sec,
             (long long)(esp_timer_get_time() / 1000000ULL));
}

void rtc_save_time(void)
{
    // No-op: time is always derivable from sync_unix_us + rtc_hw_time_us() delta,
    // both of which survive deep sleep and crashes via RTC_DATA_ATTR / RTC hardware.
}

void rtc_restore_time(void)
{
    if (!rtc_set) return;
    // Recover time via RTC hardware counter (survives panics/WDT resets via RTC domain).
    // RC drift during a <30s crash restart is ~1.5s worst case — acceptable.
    int64_t t = (sync_unix_us + (int64_t)(rtc_hw_time_us() - sync_rtc_us)) / 1000000LL;
    // Anchor esp_timer tracking to recovered time — APB timer resets on every boot.
    sync_unix_us = t * 1000000LL;
    sync_esp_us  = (uint64_t)esp_timer_get_time();
    // Re-sync the stdlib clock (gettimeofday) for gmtime_r() etc.
    settimeofday(&(struct timeval){.tv_sec = t, .tv_usec = 0}, NULL);

    ESP_LOGI("RTC", "TIME unix=%lld src=CRASH uptime=%llds",
             (long long)t,
             (long long)(esp_timer_get_time() / 1000000ULL));
}

int64_t rtc_get_ms(void)
{
    if (!rtc_set) return 0;
    return sync_unix_us / 1000LL
           + (int64_t)((esp_timer_get_time() - sync_esp_us) / 1000LL);
}

int64_t rtc_get_s_in_day(void)
{
    return rtc_get_s() % 86400UL;
}

/* -------------------------------------------------------------------------- */
/*  Unified periodic update                                                   */
/* -------------------------------------------------------------------------- */
void rtc_check_shutdown_timer(void)
{
    // Unsigned subtraction handles TickType_t (uint32_t) wraparound correctly:
    // e.g. if tick wrapped from 0xFFFFFFFE to 5, elapsed = 5 - 0xFFFFFFFE = 7.
    // At 1ms/tick, uint32_t wraps after ~49.7 days — well beyond the 180s timeout.
    TickType_t elapsed = xTaskGetTickCount() - last_activity_tick;
    if (elapsed * portTICK_PERIOD_MS >= POWER_INACTIVITY_TIMEOUT_MS)
        soft_idle_enter();
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

static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "POWER_ON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SOFTWARE";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEP_SLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

static const char *wakeup_cause_str(esp_sleep_wakeup_cause_t c) {
    switch (c) {
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "UNDEFINED (normal boot/reset)";
        case ESP_SLEEP_WAKEUP_EXT0:      return "EXT0 (button)";
        case ESP_SLEEP_WAKEUP_TIMER:     return "TIMER";
        default:                         return "OTHER";
    }
}

void rtc_print_debug(void)
{
    int64_t now_s   = rtc_get_s();
    int64_t uptime  = (int64_t)(esp_timer_get_time() / 1000000ULL);

    // Human-readable timestamps
    char now_str[32]   = "N/A";
    char sync_str[32]  = "N/A";
    char alarm_str[32] = "N/A";

    if (rtc_set) {
        time_t t;
        struct tm tm;
        t = (time_t)now_s;                      gmtime_r(&t, &tm);
        strftime(now_str,  sizeof(now_str),  "%Y-%m-%d %H:%M:%S", &tm);
        t = (time_t)(sync_unix_us / 1000000LL); gmtime_r(&t, &tm);
        strftime(sync_str, sizeof(sync_str), "%Y-%m-%d %H:%M:%S", &tm);
        if (next_alarm_time_s > 0) {
            t = (time_t)next_alarm_time_s; gmtime_r(&t, &tm);
            strftime(alarm_str, sizeof(alarm_str), "%Y-%m-%d %H:%M:%S", &tm);
        }
    }

    esp_reset_reason_t       reset = esp_reset_reason();
    esp_sleep_wakeup_cause_t wake  = esp_sleep_get_wakeup_cause();

    printf("\n=== RTC DEBUG ===\n");
    printf("  reset_reason:      %s (%d)\n",  reset_reason_str(reset), (int)reset);
    printf("  wakeup_cause:      %s (%d)\n",  wakeup_cause_str(wake),  (int)wake);
    printf("  time_source:       esp_timer (40MHz APB crystal, ~20ppm)\n");
    printf("  32kHz_xtal:        NOT USED (deep sleep disabled)\n");
    printf("\n");
    uint64_t esp_us_now  = (uint64_t)esp_timer_get_time();
    uint64_t elapsed_s   = rtc_set ? (esp_us_now - sync_esp_us) / 1000000ULL : 0;

    printf("  rtc_set:           %s\n",       rtc_set ? "true" : "false");
    printf("  current_time:      %lld  (%s UTC)\n", (long long)now_s, now_str);
    printf("  sync_time:         %lld  (%s UTC)\n", (long long)(sync_unix_us / 1000000LL), sync_str);
    printf("  elapsed_since_sync:%llus\n",    (unsigned long long)elapsed_s);
    printf("  next_alarm_s:      %lld  (%s UTC)\n", (long long)next_alarm_time_s, alarm_str);
    printf("\n");
    printf("  uptime:            %llds\n",    (long long)uptime);
    printf("  esp_timer_us:      %llu\n",     (unsigned long long)esp_us_now);
    printf("  soft_idle:         %s\n",       in_soft_idle ? "YES" : "no");
    printf("=================\n\n");
}