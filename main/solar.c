#include "solar.h"
#include "driver/rtc_io.h"
#include "rtc.h"
#include "power_mgmt.h"
#include "esp_log.h"
#include "storage.h"

#define TAG "SOLAR"

#define PIN_CHG_BULK GPIO_NUM_26

typedef enum {
	CHG_STATE_FLOAT = 0,
	CHG_STATE_BULK = 1
} charge_state_t;
RTC_DATA_ATTR charge_state_t current_charge_state = CHG_STATE_FLOAT;
RTC_DATA_ATTR int64_t timer;

esp_err_t solar_reset_fsm() {
	timer = -1;
	current_charge_state = CHG_STATE_FLOAT;
	return ESP_OK;
}

RTC_DATA_ATTR bool solar_needs_init = true;
esp_err_t init_solar_gpio() {
	if (solar_needs_init) {
	    rtc_gpio_init(PIN_CHG_BULK);
	    rtc_gpio_set_direction(PIN_CHG_BULK, RTC_GPIO_MODE_OUTPUT_ONLY);
    	solar_needs_init = false;
    }
    return ESP_OK;
}

esp_err_t solar_run_fsm() {
	init_solar_gpio();
	
	int64_t now = rtc_get_ms();
	
	//ESP_LOGI("BAT", "FSM STATE %d", current_charge_state);
	
	float vbat = get_battery_V();
	
	/*
		The state machine is simple.
		- After a period of time when battery is low, switch to bulk
		- After a period of time in bulk, switch to float
	*/
	
	//if (rtc_is_set()) {
		switch(current_charge_state) {
			case CHG_STATE_BULK:
				if (now > timer+get_param_value_t(PARAM_CHG_BULK_S).u32) {
					current_charge_state = CHG_STATE_FLOAT;
				}
				
				break;
			case CHG_STATE_FLOAT:
				// if we have sufficient voltage, reset the timer
				if (vbat > get_param_value_t(PARAM_CHG_LOW_V).f32) {
					timer = now;
				}
			
				if (now > timer+get_param_value_t(PARAM_CHG_LOW_S).u32) {
					timer = now;
					current_charge_state = CHG_STATE_BULK;	
				}
				
				break;
		}
	/*} else {
		reset_solar_fsm();
		ESP_LOGI(TAG, "RESET SOLAR FSM");
	}*/
	
	rtc_gpio_hold_dis(PIN_CHG_BULK);
	switch(current_charge_state) {
		case CHG_STATE_BULK:
			rtc_gpio_set_level(PIN_CHG_BULK, 1);
			//ESP_LOGI(TAG, "BULK");
			break;
		case CHG_STATE_FLOAT:
			rtc_gpio_set_level(PIN_CHG_BULK, 0);
			//ESP_LOGI(TAG, "FLOAT");
			break;
	}
	rtc_gpio_hold_en(PIN_CHG_BULK);
	//rtc_gpio_hold_en(PIN_CHG_DISABLE);
	
	return ESP_OK;
}