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

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

esp_err_t rtc_xtal_init();

bool rtc_is_set();


void rtc_check_shutdown_timer();
void rtc_reset_shutdown_timer(); // reset shutoff timer
void soft_idle_enter(void);
void soft_idle_exit(void);
bool soft_idle_is_active(void);
bool soft_idle_button_raw(void);   /* direct GPIO read, no I2C */

/*void adjust_rtc_hour(char *key, int8_t dir);
void adjust_rtc_min(char *key, int8_t dir);*/

int64_t rtc_get_s (void);
int64_t rtc_get_ms(void);
void rtc_set_s(int64_t);

void rtc_save_time(void);    // Writes current time to NVS (call before esp_restart)
void rtc_restore_time(void); // Recovers time from RTC memory or NVS; requires NVS init first

void    rtc_schedule_next_alarm(void);
int64_t rtc_get_next_alarm_s();

bool rtc_alarm_tripped();

void rtc_print_debug(void); // Dump full RTC state to stdout (UART)

/* -------------------------------------------------------------------------- */
#endif /* MAIN_RTC_H_ */