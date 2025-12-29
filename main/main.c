#include "esp_task_wdt.h"
#include "i2c.h"
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
#include "webserver.h"

#define TAG "MAIN"


int64_t last_log_time = 0;
esp_err_t send_log() {
	
	char entry[LOG_ENTRY_SIZE] = {0};
    entry[0] = LOG_ENTRY_SIZE;
    
    // Pack 64-bit timestamp into bytes 1-8
    uint64_t be_timestamp = htobe64(rtc_time_ms());
    memcpy(&entry[1], &be_timestamp, 8);
    
    // Pack 32-bit voltages/currents into bytes 9-24
    float be_voltage = htobe32(get_battery_V());
    memcpy(&entry[9],  &be_voltage,  4);
    float be_current1 = htobe32(get_bridge_A(BRIDGE_DRIVE));
    memcpy(&entry[13], &be_current1, 4);
    float be_current2 = htobe32(get_bridge_A(BRIDGE_JACK));
    memcpy(&entry[17], &be_current2, 4);
    float be_current3 = htobe32(get_bridge_A(BRIDGE_AUX));
    memcpy(&entry[21], &be_current3, 4);
    
    int32_t be_counter = htobe32(get_sensor_counter(SENSOR_DRIVE));
    memcpy(&entry[25], &be_counter, 4);
    
    entry[29] = get_sensor(SENSOR_DRIVE);
    entry[30] = get_sensor(SENSOR_JACK);
    entry[31] = fsm_get_state();
    
    last_log_time = esp_timer_get_time();
    
    return write_log(entry);
}


typedef enum {
	LED_STATE_DRIVING,
	LED_STATE_ERROR,
	LED_STATE_AWAKE,
	LED_STATE_CANCELLING,
	LED_STATE_START1,
	LED_STATE_START2,
	LED_STATE_START3,
	LED_STATE_START4,
	LED_STATE_BOOTING
} led_state_t;

void driveLEDs(led_state_t state) {
	uint8_t patterns[4][12] = {
		{1,3,7,6,4,0},
		{7,0},
		{1,1,1,1,1,1, 1,1,1,3},
		{4,2}
	};
	switch(state) {
		case LED_STATE_DRIVING:
			i2c_set_led1(patterns[state][(esp_timer_get_time()/100000) % 6]);
			break;
		case LED_STATE_ERROR:
			i2c_set_led1(patterns[state][(esp_timer_get_time()/1000000) % 2]);
			break;
		case LED_STATE_AWAKE:
			i2c_set_led1(patterns[state][(esp_timer_get_time()/200000) % 10]);
			break;
		case LED_STATE_CANCELLING:
			i2c_set_led1(patterns[state][(esp_timer_get_time()/200000) % 2]);
			break;
		
		case LED_STATE_BOOTING:
			i2c_set_led1(1);
			break;
			
		case LED_STATE_START1:
			i2c_set_led1(0);
			break;
		case LED_STATE_START2:
			i2c_set_led1(1);
			break;
		case LED_STATE_START3:
			i2c_set_led1(3);
			break;
		case LED_STATE_START4:
			i2c_set_led1(7);
			break;
	}
}

void app_main(void) {
    esp_task_wdt_add(NULL);
    
	// Say hello; turn on the lights
	esp_sleep_wakeup_cause_t cause = rtc_wakeup_cause();
    if (i2c_init()      != ESP_OK) ESP_LOGE(TAG, "I2C FAILED");
    i2c_set_relays(0);
    driveLEDs(LED_STATE_BOOTING);
    
	// Every boot we load parameters and monitor solar, no matter what
	if (adc_init()      != ESP_OK) ESP_LOGE(TAG, "ADC FAILED");
	if (rtc_xtal_init() != ESP_OK) ESP_LOGE(TAG, "RTC FAILED");
    if (storage_init()  != ESP_OK) ESP_LOGE(TAG, "STORAGE FAILED");
    if (log_init()      != ESP_OK) ESP_LOGE(TAG, "LOG FAILED");
    if (run_solar_fsm() != ESP_OK) ESP_LOGE(TAG, "SOLAR FAILED");
    // TODO: Do a 12V check and enter deep sleep if there's a problem
    
    
	send_log();
	
	//write_dummy_log_1();
    
    // Check wake reasons
    // If button held, we stay #woke
    // If not it must've been the RTC - check alarms
    // If there's an alarm or the button was held, do a full boot
    // Full boot will handle things from there
    
    // only truly wake up if we saw it on EXT0, or from alarm
    /*if (!rtc_is_set()) {
		//ESP_LOGI("MAIN", "RTC is not set. Can't sleep til then.");
    } else */if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI("MAIN", "Woke from button press");
    } else {    	
    	if (!alarm_tripped()) {
	    	//enter_deep_sleep();
    	}
    }
    
    /*** FULL BOOT ***/
    if (uart_init()    != ESP_OK) ESP_LOGE(TAG, "UART FAILED");
    if (power_init()   != ESP_OK) ESP_LOGE(TAG, "POWER FAILED");
    if (rf_433_init()  != ESP_OK) ESP_LOGE(TAG, "RF FAILED");
    if (fsm_init()     != ESP_OK) ESP_LOGE(TAG, "FSM FAILED");
    if (sensors_init() != ESP_OK) ESP_LOGE(TAG, "SENSORS FAILED");
    if (webserver_init() != ESP_OK) ESP_LOGE(TAG, "WEBSERVER FAILED");

    /*** MAIN LOOP ***/
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(100);
    
    while(true) {
		ESP_LOGI(TAG, "TICK");
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
	}

    while(true) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        
        i2c_poll_buttons();
        
        if (i2c_get_button_state(0))
        	reset_shutdown_timer();
        	
        switch (fsm_get_state()) {
			case STATE_IDLE:
        		// LED cue for user
				if (i2c_get_button_ms(0) > 1600){
        			driveLEDs(LED_STATE_START4);
				} else if (i2c_get_button_ms(0) > 1100){
        			driveLEDs(LED_STATE_START3);
				} else if (i2c_get_button_ms(0) > 600){
        			driveLEDs(LED_STATE_START2);
        		} else if (i2c_get_button_ms(0) > 100){
        			driveLEDs(LED_STATE_START1);
        		} else{
        			driveLEDs(LED_STATE_AWAKE);
				}
				
				// when not actively moving we log at a low frequency
				if (esp_timer_get_time() > last_log_time + DEEP_SLEEP_US)
				  send_log();
				
				if(i2c_get_button_ms(0) > 2100)
        			fsm_request(FSM_CMD_START);
        		break;
			case STATE_UNDO_JACK:
			case STATE_UNDO_JACK_START:
				// it's running the jack, but undoing
				send_log();
        		driveLEDs(LED_STATE_CANCELLING);
				if (i2c_get_button_tripped(0)) {
					ESP_LOGI(TAG, "AAAAH STOP!!!");
        			fsm_request(FSM_CMD_STOP);
				}
        		break;
        		
			default:
				// it's running in every other case
				send_log();
        		driveLEDs(LED_STATE_DRIVING);
				if (i2c_get_button_tripped(0)) {
					ESP_LOGI(TAG, "CTRL + Z PLZ!");
        			fsm_request(FSM_CMD_UNDO);
				}
        		break;
		}
        
        
        
        
        if (alarm_tripped()) {
        	fsm_request(FSM_CMD_START);
        	set_next_alarm();
        }
        
        
	
		/*ESP_LOGI(TAG, "VOLTAGE: %2.3f | CURRENTS: %+2.8f %+2.8f %+2.8f",
		  get_battery_V(),
		  get_bridge_A(BRIDGE_DRIVE),
		  get_bridge_A(BRIDGE_JACK),
		  get_bridge_A(BRIDGE_AUX));*/
		  
		
        	
        run_solar_fsm();
        
        //check_shutdown_timer();
        esp_task_wdt_reset();
	}
}