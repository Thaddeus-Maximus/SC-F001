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


#define TRANSITION_DELAY_US 1000000
#define OVERRIDE_PULSE_RF 160000
#define OVERRIDE_PULSE_UX  80000


/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t start_rtc();

bool rtc_is_set();

void set_next_alarm(void);

void reset_shutdown_timer(); // reset shutoff timer
void enter_deep_sleep();
void check_shutdown_timer();
esp_sleep_wakeup_cause_t rtc_wakeup_cause();

void adjust_rtc_hour(char *key, int8_t dir);
void adjust_rtc_min(char *key, int8_t dir);

void rtc_get_time(struct tm * timeinfo);

int64_t system_rtc_get_raw_time(void);

bool alarm_tripped();

uint64_t rtc_time_ms();

/* -------------------------------------------------------------------------- */
#endif /* MAIN_RTC_H_ */