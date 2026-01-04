/*
 * system.h
 *
 * Public system services:
 *   • Battery charge-state machine
 *   • Inactivity / deep-sleep handling
 *   • RTC time & alarm
 *
 * All implementation lives in system.c
 */

#ifndef MAIN_RTC_H_
#define MAIN_RTC_H_

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_err.h"
#include <time.h>


#define POWER_INACTIVITY_TIMEOUT_MS 180000
#define DEEP_SLEEP_US 120000000ULL /* 120 seconds in deep sleep */

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t rtc_xtal_init();

bool rtc_is_set();


void rtc_check_shutdown_timer();
void rtc_reset_shutdown_timer(); // reset shutoff timer
void rtc_enter_deep_sleep();
esp_sleep_wakeup_cause_t rtc_wakeup_cause();

/*void adjust_rtc_hour(char *key, int8_t dir);
void adjust_rtc_min(char *key, int8_t dir);*/

int64_t rtc_get_s (void);
int64_t rtc_get_ms(void);
void rtc_set_s(int64_t);

void    rtc_schedule_next_alarm(void);
int64_t rtc_get_next_alarm_s();

bool rtc_alarm_tripped();

/* -------------------------------------------------------------------------- */
#endif /* MAIN_RTC_H_ */