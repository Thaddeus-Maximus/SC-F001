/*
 * lcd.c
 *
 *  Created on: Dec 12, 2025
 *      Author: Thad
 */


/* NOTICE: THIS IS A DUMPING GROUND FOR OBSOLETE CODE SINCE WE NO LONGER HAVE AN LCD
   NONE OF THIS IS TESTED. 
   */



// Debounce & Repeat Settings
#define DEBOUNCE_MS        50
#define REPEAT_MS         200
#define REPEAT_START_MS   700

static uint8_t lcd_col = 0;
static uint8_t lcd_row = 0;

static bool debounced_state[4] = {false};
static bool last_known_state[4] = {false};
static uint64_t last_stable_time[4] = {0};
static uint64_t last_change_time[4] = {0};
static uint8_t claimed_repeats[4] = {0};


// === DELAY HELPERS ===
static inline void delay_us(uint32_t us) {
    esp_rom_delay_us(us);
}


static esp_err_t tca_write_word_16(uint8_t reg, uint16_t value) {
    uint8_t data[3] = { reg, (uint8_t)(value & 0xFF), (uint8_t)(value >> 8) };
    return i2c_master_write_to_device(I2C_PORT, TCA_ADDR, data, 3, pdMS_TO_TICKS(1000));
}


// === TCA9555 PORT CONTROL ===
static esp_err_t tca_set_config_port0(uint16_t config_port0) {
    return tca_write_word_16(TCA_REG_CONFIG0, config_port0);
}

static esp_err_t tca_port_write(uint8_t value) {
    return tca_write_word_8(TCA_REG_OUTPUT1, value);
}

static esp_err_t tca_port_read(uint16_t *value) {
    uint16_t low, high;
    ESP_ERROR_CHECK(tca_read_word(TCA_REG_INPUT0, &low));
    ESP_ERROR_CHECK(tca_read_word(TCA_REG_INPUT1, &high));
    *value = low | (high << 8);
    return ESP_OK;
}


// === LCD NIBBLE & COMMAND ===
static esp_err_t lcd_write_nibble(uint8_t nibble, bool rs) {
    uint8_t data_state = 0;
    if (rs) data_state |= (1 << LCD_RS);
    if (nibble & 0x01) data_state |= (1 << LCD_D4);
    if (nibble & 0x02) data_state |= (1 << LCD_D5);
    if (nibble & 0x04) data_state |= (1 << LCD_D6);
    if (nibble & 0x08) data_state |= (1 << LCD_D7);

    ESP_ERROR_CHECK(tca_port_write(data_state));
    ESP_ERROR_CHECK(tca_port_write(data_state | (1 << LCD_E)));
    ESP_ERROR_CHECK(tca_port_write(data_state));
    return ESP_OK;
}


static esp_err_t lcd_command(uint8_t cmd) {
    ESP_ERROR_CHECK(lcd_write_nibble(cmd >> 4, false));
    ESP_ERROR_CHECK(lcd_write_nibble(cmd & 0x0F, false));
    return ESP_OK;
}

static esp_err_t lcd_data(uint8_t data) {
    ESP_ERROR_CHECK(lcd_write_nibble(data >> 4, true));
    ESP_ERROR_CHECK(lcd_write_nibble(data & 0x0F, true));
    return ESP_OK;
}

void lcd_set_cursor(uint8_t row, uint8_t col) {
    uint8_t addr = (row == 0) ? 0x00 : 0x40;
    addr += col;
    lcd_row = row;
    lcd_col = col;
    lcd_command(0x80 | addr);
    delay_us(50);
}

void lcd_printf(const char *fmt, ...) {
    char buf[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    lcd_set_cursor(0, 0);
    for (int i = 0; i < 32 && buf[i]; i++) {
        if (i == 16) lcd_set_cursor(1, 0);
        lcd_data((uint8_t)buf[i]);
        delay_us(50);
    }
}

void lcd_print(const char *str) {
    lcd_set_cursor(0, 0);
    for (int i = 0; i < 32 && str[i]; i++) {
        if (i == 16) lcd_set_cursor(1, 0);
        lcd_data((uint8_t)str[i]);
        delay_us(50);
    }
}

void lcd_off(void) {
    if (i2c_initted) lcd_command(0x08);
}

esp_err_t lcd_init_4bit(void) {
    ESP_LOGI("I2C", "Starting LCD init...");
    ESP_ERROR_CHECK(tca_set_config_port0(0xFF));
    tca_port_write(0x00);
    delay_us(50000);

    ESP_ERROR_CHECK(lcd_write_nibble(0x3, false)); delay_us(4500);
    ESP_ERROR_CHECK(lcd_write_nibble(0x3, false)); delay_us(150);
    ESP_ERROR_CHECK(lcd_write_nibble(0x3, false)); delay_us(150);
    ESP_ERROR_CHECK(lcd_write_nibble(0x2, false)); delay_us(150);

    ESP_ERROR_CHECK(lcd_command(0x28)); delay_us(150);
    ESP_ERROR_CHECK(lcd_command(0x08)); delay_us(150);
    ESP_ERROR_CHECK(lcd_command(0x01)); delay_us(2000);
    ESP_ERROR_CHECK(lcd_command(0x06)); delay_us(150);
    ESP_ERROR_CHECK(lcd_command(0x0C)); delay_us(150);

    ESP_LOGI("I2C", "LCD init complete.");
    return ESP_OK;
}


// === BUTTON DEBOUNCE & REPEAT ===
void update_buttons(void) {
    for (uint8_t btn = 0; btn < 4; ++btn) {
        last_known_state[btn] = debounced_state[btn];
    }

    uint16_t port_val;
    ESP_ERROR_CHECK(tca_port_read(&port_val));
    uint8_t raw_buttons = (uint8_t)(port_val & 0x0F);
    uint8_t raw_states = ~raw_buttons & 0x0F;

    uint64_t now = esp_timer_get_time() / 1000;

    for (uint8_t btn = 0; btn < 4; ++btn) {
        bool raw_pressed = (raw_states & (1 << btn)) != 0;

        if (raw_pressed != debounced_state[btn]) {
            if (now - last_stable_time[btn] >= DEBOUNCE_MS) {
                debounced_state[btn] = raw_pressed;
                last_stable_time[btn] = now;
                last_change_time[btn] = now;
                claimed_repeats[btn] = 0;
            }
        } else {
            last_stable_time[btn] = now;
        }
    }
}

bool get_button_tripped(uint8_t button) {
    return (button < 4) && debounced_state[button] && !last_known_state[button];
}

bool get_button_released(uint8_t button) {
    return (button < 4) && !debounced_state[button] && last_known_state[button];
}

bool get_button_state(uint8_t button) {
    return (button < 4) && debounced_state[button];
}

bool get_button_repeat(uint8_t btn) {
    if (btn >= 4 || !debounced_state[btn]) return false;
    uint64_t now = esp_timer_get_time() / 1000;
    if (now + DEBOUNCE_MS < last_change_time[btn]) return false;
    if ((now - last_change_time[btn]) > (REPEAT_START_MS + REPEAT_MS * claimed_repeats[btn])) {
        claimed_repeats[btn]++;
        return true;
    }
    return false;
}

int8_t get_button_repeats(uint8_t btn) {
	if (!get_button_state(btn))
		return 0;
		
    if (btn >= 4 || !debounced_state[btn]) return false;
    uint64_t now = esp_timer_get_time() / 1000;
    if (now + DEBOUNCE_MS < last_change_time[btn]) return false;
    if ((now - last_change_time[btn]) > (REPEAT_START_MS + REPEAT_MS * claimed_repeats[btn])) {
        claimed_repeats[btn]++;
        if (claimed_repeats[btn] > 100)
        	claimed_repeats[btn] = 100;
        ESP_LOGI("BTN", "RPT %d", (uint8_t)claimed_repeats[btn]+2);
        return claimed_repeats[btn]+1;
    }
    if (debounced_state[btn] && !last_known_state[btn]) {
		
        ESP_LOGI("BTN", "FST %d", 1);
    	return 1;
    }
    
        //ESP_LOGI("BTN", "RPT %d", 0);
    return 0;
}

int64_t get_button_ms(uint8_t btn) {
	if (!get_button_state(btn))
		return 0;
	
    uint64_t now = esp_timer_get_time() / 1000;
    return now - last_change_time[btn];
}





// Parameter descriptor structure
typedef struct {
    const char key[24];           // NVS key name (null-terminated)
    uint8_t    type_size;         // Size in bytes: 1=uint8_t, 2=uint16_t, 4=uint32_t/float, 8=uint64_t/double
    uint8_t    type_flags;        // Bitfield: [0:1] signed, [2] float, [3:7] reserved
    const void *default_val;      // Pointer to default value (matches type)
} param_desc_t;

typedef struct param_group_s param_group_t;
typedef struct param_group_s {
    char* (*formatter)(const param_group_t*, uint8_t idx);
    const uint8_t num_keys;
    const uint8_t indices[8][2];
    const char    keys[8][20];
    void (*launch_functions[8])(char* key, int8_t dir);
} param_group_t;

// temp buffer for formatting stuff onto the LCD
static char formatting_buf[LCD_BUFLEN];

/* MENU DIALOG CONFIG */
char* schedule_format(const param_group_t *pg, uint8_t idx);
char* dist_format    (const param_group_t *pg, uint8_t idx);
char* reprog_format  (const param_group_t *pg, uint8_t idx);
char* override_format(const param_group_t *pg, uint8_t idx);
char* status_format  (const param_group_t *pg, uint8_t idx);
char* cal_format     (const param_group_t *pg, uint8_t idx);
char* efuse_format   (const param_group_t *pg, uint8_t idx);
char* ftp_format     (const param_group_t *pg, uint8_t idx);

// Launch functions (forward declarations)
void trigger_move(char* key, int8_t dir);
void rf_reprogram_remote(char* key, int8_t dir);

void adjust_hour     (char* key, int8_t dir);
void adjust_i8_0_99  (char* key, int8_t dir);
void adjust_generic  (int idx, int8_t amt);
void dummy_adjuster  (char* key, int8_t dir) {}; // do nothing
void launch_ftp      (char* key, int8_t dir);
void adjust_i32_smart_0_99999(char* key, int8_t dir);
void adjust_i32_smart_0_999  (char* key, int8_t dir);

// Parameter table (legible, declarative)
const param_desc_t param_table[] = {
    {
        .key = "sched_start",
        .type_size = TYPE_SIZE_1,
        .type_flags = TYPE_SIGNED,
        .default_val = &(int8_t){0}
    },
    {
        .key = "sched_end",
        .type_size = TYPE_SIZE_1,
        .type_flags = TYPE_SIGNED,
        .default_val = &(int8_t){0}
    },
    {
        .key = "sched_num",
        .type_size = TYPE_SIZE_1,
        .type_flags = TYPE_SIGNED,
        .default_val = &(int8_t){0}
    },
    
    
    {
		.key = "efuse_drive_A",
		.type_size = TYPE_SIZE_1,
		.type_flags = TYPE_SIGNED,
		.default_val = &(int8_t){99}
	},{
		.key = "efuse_jack_A",
		.type_size = TYPE_SIZE_1,
		.type_flags = TYPE_SIGNED,
		.default_val = &(int8_t){99}
	},{
		.key = "efuse_aux_A",
		.type_size = TYPE_SIZE_1,
		.type_flags = TYPE_SIGNED,
		.default_val = &(int8_t){99}
	},
    
    
    {
        .key = "drive_dist",
        .type_size = TYPE_SIZE_1,
        .type_flags = TYPE_SIGNED,
        .default_val = &(int16_t){10}
    },{
        .key = "drive_tpdf",
        .type_size = TYPE_SIZE_4,
        .type_flags = 0,
        .default_val = &(int32_t){70}
    },{
        .key = "drive_mspf",
        .type_size = TYPE_SIZE_4,
        .type_flags = 0,
        .default_val = &(int32_t){1000}
    },{
        .key = "jack_mspi",
        .type_size = TYPE_SIZE_4,
        .type_flags = 0,
        .default_val = &(int32_t){1000}
    },{
        .key = "jack_dist",
        .type_size = TYPE_SIZE_1,
        .type_flags = TYPE_SIGNED,
        .default_val = &(uint8_t){7}
    },
    
    
    
    
    
    {
        .key = "keycode0",
        .type_size = TYPE_SIZE_8,
        .type_flags = 0,
        .default_val = &(uint8_t){0}
    },{
        .key = "keycode1",
        .type_size = TYPE_SIZE_8,
        .type_flags = 0,
        .default_val = &(uint8_t){0}
    },{
        .key = "keycode2",
        .type_size = TYPE_SIZE_8,
        .type_flags = 0,
        .default_val = &(uint8_t){0}
    },{
        .key = "keycode3",
        .type_size = TYPE_SIZE_8,
        .type_flags = 0,
        .default_val = &(uint8_t){0}
    }
};

#define PARAM_COUNT (sizeof(param_table)/sizeof(param_table[0]))

// Runtime parameter values
static param_value_t param_values[PARAM_COUNT];

const param_group_t param_group_table[] = {
    {
        .formatter = status_format,
        .num_keys = 3,
        .keys = {"","",""},
        .launch_functions = {trigger_move, adjust_rtc_hour, adjust_rtc_min}
    },{
        .formatter = schedule_format,
        .num_keys = 3,
        .keys = {"sched_start", "sched_end", "sched_num"},
        .launch_functions = {adjust_hour, adjust_hour, adjust_i8_0_99}
    },
    {
        .formatter = dist_format,
        .num_keys = 2,
        .keys = {"drive_dist", "jack_dist"},
        .launch_functions = {adjust_i8_0_99, adjust_i8_0_99}
    },
    {
        .formatter = cal_format,
        .num_keys = 3,
        .keys = { "jack_mspi", "drive_mspf", "drive_tpdf"},
        .launch_functions = {adjust_i32_smart_0_99999, adjust_i32_smart_0_99999, adjust_i32_smart_0_999}
    },
    {
        .formatter = efuse_format,
        .num_keys = 3,
        .keys = { "efuse_aux_A", "efuse_jack_A", "efuse_drive_A"},
        .launch_functions = {adjust_i8_0_99, adjust_i8_0_99, adjust_i8_0_99}
    },
    {
        .formatter = override_format,
        .num_keys = 3,
        .keys = {"","",""},
        .launch_functions = {dummy_adjuster, dummy_adjuster, dummy_adjuster}
    },
    {
        .formatter = reprog_format,
        .num_keys = 1,
        .keys = {""},
        .launch_functions = {rf_reprogram_remote}
    },
    {
        .formatter = ftp_format,
        .num_keys = 1,
        .keys = {""},
        .launch_functions = {launch_ftp}
    }
};

#define PARAM_GROUP_RUNMTR 5
#define PARAM_GROUP_FTP 7
#define PARAM_GROUP_COUNT (sizeof(param_group_table)/sizeof(param_group_table[0]))

static const char schedule_fmts[3][3][LCD_BUFLEN] = {
    {
        "Start/End xTimes [-]    -   x%-2d ",
        "Start/End xTimes  -    [-]  x%-2d ",
        "Start/End xTimes  -     -  [x%-2d]"
    },{
        "Start/End xTimes[%2d%cM]   -  x%-2d ",
        "Start/End xTimes %2d%cM   [-] x%-2d ",
        "Start/End xTimes %2d%cM    - [x%-2d]"
    },{
        "Start/End xTimes[%2d%cM]-%2d%cM  x%-2d",
        "Start/End xTimes %2d%cM-[%2d%cM] x%-2d",
        "Start/End xTimes %2d%cM-%2d%cM [x%-2d]"
    }
};

static const char dist_fmts[3][LCD_BUFLEN] = {
    "Dist. Drive/Jack[%2d ft] / %2d in ",
    "Dist. Drive/Jack %2d ft / [%2d in]"
};

static const char override_fmts[3][LCD_BUFLEN] = {
    "   Run Motors   [AUX]JACK DRIVE ",
    "   Run Motors    AUX[JACK]DRIVE ",
    "   Run Motors    AUX JACK[DRIVE]"
};

static const char cal_fmts[3][LCD_BUFLEN] = {
    "Jack  ms/in:    [%4ld]%4ld %4ld ",
    "Drive ms/ft:     %4ld[%4ld]%4ld ",
    "Drive t/10ft:    %4ld %4ld[%4ld]"
};
static const char efuse_fmts[3][LCD_BUFLEN] = {
    "E-fuse Aux:      [%2dA] %2dA  %2dA ",
    "E-fuse Jack:      %2dA [%2dA] %2dA ",
    "E-fuse Drive:     %2dA  %2dA [%2dA]"
};

/* All function implementations remain unchanged and appear here in original form */
char* schedule_format(const param_group_t *pg, uint8_t idx)
{
    /* pg->keys[0..2] → "sched_start", "sched_end", "sched_num" */
    int8_t start = (int8_t)get_param_i8(pg->keys[0]);   // helper, see below
    int8_t end   = (int8_t)get_param_i8(pg->keys[1]);
    int8_t num   = (int8_t)get_param_i8(pg->keys[2]);
    
    
    char startAP = start<12 ? 'A':'P';
    char endAP   =   end<12 ? 'A':'P';
    start %= 12;
    end   %= 12;
    
    
    if (start == 0) start = 12;
    if (end   == 0) end   = 12;
    
    if (num == 0) {
        snprintf(formatting_buf, sizeof(formatting_buf),
                 schedule_fmts[0][idx], num);
        return formatting_buf;
    } else if (num == 1) {
        snprintf(formatting_buf, sizeof(formatting_buf),
                 schedule_fmts[1][idx], start, startAP, num);
        return formatting_buf;
    } else {
        snprintf(formatting_buf, sizeof(formatting_buf),
                 schedule_fmts[2][idx], start, startAP, end, endAP, num);
        return formatting_buf;
    }
}

char* dist_format(const param_group_t *pg, uint8_t idx) {
    int8_t drive = (int8_t)get_param_i8(pg->keys[0]);   // helper, see below
    int8_t jack  = (int8_t)get_param_i8(pg->keys[1]);
    
    snprintf(formatting_buf, sizeof(formatting_buf),
        dist_fmts[idx], drive, jack);
    return formatting_buf;   
}

char* reprog_format(const param_group_t *pg, uint8_t idx) {
    return "Reprogram Keyfob [Press ^ / v ] ";
}

char* override_format(const param_group_t *pg, uint8_t idx) {
    return override_fmts[idx];
}

char* ftp_format(const param_group_t *pg, uint8_t idx) {
	return " Start Wifi/FTP  [Press ^ / v ] ";
}

char charge_indicators[N_CHARGE_STATES] = {
	[CHG_STATE_OFF]   ='-',
	[CHG_STATE_FLOAT] ='F',
	[CHG_STATE_BULK]  ='B'
	
};

static const char status_fmts[4][LCD_BUFLEN] = {
    "%-6s%2dA %2lu.%02luV[MOVE]  %2d:%02d %cM",
    "%-6s%2dA %2lu.%02luV MOVE [%2d]:%02d %cM",
    "%-6s%2dA %2lu.%02luV MOVE  %2d:[%02d]%cM",
    "%-6s%2dA %2lu.%02luV[ SET TIME ^/v ]",
};

char* status_format(const param_group_t *pg, uint8_t idx) {
    uint32_t vbat = get_battery_mV();
        
    struct tm timeinfo;
    rtc_get_time(&timeinfo);

    // --- Build 7-char time: " 9:05PM" or "10:05PM" ---
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;  // 12-hour format
    
    int current_draw = abs(get_bridge_mA(BRIDGE_DRIVE)/1000) + abs(get_bridge_mA(BRIDGE_JACK)/1000) + abs(get_bridge_mA(BRIDGE_AUX)/1000);
    
    if (rtc_is_set())
        snprintf(formatting_buf, sizeof(formatting_buf),
                 status_fmts[idx],
                 "Idle",
                 current_draw,
                 (unsigned long)(vbat / 1000),
                 (unsigned long)((vbat % 1000) + 99) / 100,
                 hour12,
                 timeinfo.tm_min,
                 timeinfo.tm_hour < 12 ? 'A':'P'
                 );
    else
        snprintf(formatting_buf, sizeof(formatting_buf),
                 status_fmts[3],
                 "Idle",
                 current_draw,
                 (unsigned long)(vbat / 1000),
                 (unsigned long)((vbat % 1000) + 99) / 100
                 );
    
    return formatting_buf;
}

char* cal_format(const param_group_t *pg, uint8_t idx) {
    int32_t x1 = get_param_i32(pg->keys[0]);
    int32_t x2 = get_param_i32(pg->keys[1]);
    int32_t x3 = get_param_i32(pg->keys[2]);
    
    snprintf(formatting_buf, sizeof(formatting_buf),
        cal_fmts[idx], x1, x2, x3);
    return formatting_buf;   
}

char* efuse_format(const param_group_t *pg, uint8_t idx) {
    int32_t x1 = get_param_i32(pg->keys[0]);
    int32_t x2 = get_param_i32(pg->keys[1]);
    int32_t x3 = get_param_i32(pg->keys[2]);
	
    snprintf(formatting_buf, sizeof(formatting_buf),
        efuse_fmts[idx], x1, x2, x3);
	return formatting_buf;
}

// Generic adjustment fallback
void adjust_generic(int idx, int8_t amt) {
    const param_desc_t *p = &param_table[idx];
    if (p->type_flags & TYPE_FLOAT) {
        float step = 0.1f;
        param_values[idx].f32 += amt;
    } else {
        switch (p->type_size) {
            case 1: {
                int8_t v = (int8_t)param_values[idx].u8;
                v += amt;
                param_values[idx].u8 = (int8_t)v;
                break;
            }
            case 2: {
                int16_t v = (int16_t)param_values[idx].u16;
                v += amt;
                param_values[idx].u16 = (int16_t)v;
                break;
            }
        }
    }
    params_save(idx);
}

/**
 * adjust_time - Shared adjuster for any time parameter (HH:MM format)
 * @idx:  Index in param_table[]
 * @dir: +1 = increment, -1 = decrement
 *
 * Assumes value stored as minutes since 00:00 (0–1439)
 * Displays as "HH:MM"
 */
void adjust_hour(char* key, int8_t dir) {
    int8_t idx = params_find(key);
    if (idx<0) return;

    if (dir>0) param_values[idx].i8 += +1;
    if (dir<0) param_values[idx].i8 += -1;
    
    // wraparound
    if (param_values[idx].i8 > 23) param_values[idx].i8 = 0;
    if (param_values[idx].i8 < 0) param_values[idx].i8 = 23;
    
    params_save(idx);
    set_next_alarm();
}

void adjust_i8_0_99(char* key, int8_t dir) {
    int8_t idx = params_find(key);
    if (idx<0) return;

    if (dir>0) param_values[idx].i8 += +1;
    if (dir<0) param_values[idx].i8 += -1;
    
    // clamp
    if (param_values[idx].i8 > 99) param_values[idx].i8 = 99;
    if (param_values[idx].i8 < 0)  param_values[idx].i8 = 0;
    
    params_save(idx);   
    set_next_alarm();
}

void adjust_i16_0_9990_by_10(char* key, int8_t dir) {
    int8_t idx = params_find(key);
    if (idx<0) return;

    if (dir>0) param_values[idx].i16 += +1;
    if (dir<0) param_values[idx].i16 += -1;
    
    // clamp
    if (param_values[idx].i16 > 9990) param_values[idx].i16 = 9990;
    if (param_values[idx].i16 < 0)    param_values[idx].i16 = 0;
    
    params_save(idx);   
    set_next_alarm();   
}

//inline static int8_t abs(int8_t x) { return x<0?-x:x; }
void adjust_i32_smart_0_99999(char* key, int8_t dir) {
    int8_t idx = params_find(key);
    if (idx<0) return;
    
    int32_t inc = 1;
    if (abs(dir) > 5)  inc = 5;
    if (abs(dir) > 10) inc = 10;
    if (abs(dir) > 13) inc = 50;
    if (abs(dir) > 16) inc = 100;
    if (abs(dir) > 19) inc = 200;
    if (abs(dir) > 22) inc = 1000;

    if (dir>0) param_values[idx].i32 += +inc;
    if (dir<0) param_values[idx].i32 += -inc;
    param_values[idx].i32 = (param_values[idx].i32/inc)*inc;
    
    ESP_LOGI("ADJ", "P[%d] += %d => %ld", (int)idx, (int)inc, (long)param_values[idx].i32);
    
    // clamp
    if (param_values[idx].i32 > 99999) param_values[idx].i32 = 99999;
    if (param_values[idx].i32 < 0)     param_values[idx].i32 = 0;
    
    params_save(idx);   
    set_next_alarm();	
}
void adjust_i32_smart_0_999(char* key, int8_t dir) {
    int8_t idx = params_find(key);
    if (idx<0) return;
    
    int32_t inc = 1;
    if (abs(dir) > 5)  inc = 5;
    if (abs(dir) > 10) inc = 10;
    if (abs(dir) > 13) inc = 50;
    if (abs(dir) > 16) inc = 100;
    if (abs(dir) > 19) inc = 200;
    if (abs(dir) > 22) inc = 1000;

    if (dir>0) param_values[idx].i32 += +inc;
    if (dir<0) param_values[idx].i32 += -inc;
    param_values[idx].i32 = (param_values[idx].i32/inc)*inc;
    
    ESP_LOGI("ADJ", "p[%d] += %d => %ld", (int)idx, (int)inc, (long)param_values[idx].i32);
    
    // clamp
    if (param_values[idx].i32 > 999) param_values[idx].i32 = 999;
    if (param_values[idx].i32 < 0)   param_values[idx].i32 = 0;
    
    params_save(idx);   
    set_next_alarm();
}



static int8_t group_idx=0, entry_idx=0;
void run_parameter_ui() {
    
    if (get_button_repeats(BTN_L)) {
        reset_shutdown_timer();
        entry_idx--;
        if (entry_idx < 0) {
            group_idx--;
            if (group_idx < 0) {
                group_idx = PARAM_GROUP_COUNT-1;
            }
            entry_idx = param_group_table[group_idx].num_keys-1;
        }
    }
    if (get_button_repeats(BTN_R)) {
        reset_shutdown_timer();
        entry_idx++;
        if (entry_idx >= param_group_table[group_idx].num_keys) {
            group_idx++;
            if (group_idx >= PARAM_GROUP_COUNT) {
                group_idx = 0;
            }
            entry_idx = 0;
        }
    }
    // Forbid user from doing anything until they set the time
    if (!rtc_is_set()) {
        group_idx=0;
        entry_idx=1;
    }
    
    param_group_t pg = param_group_table[group_idx];
    
    lcd_print(pg.formatter(&pg, entry_idx));  // Formatted with botfmt + values
    
    int8_t n;
    if ((n=get_button_repeats(BTN_U))) {
        reset_shutdown_timer();
        pg.launch_functions[entry_idx](
            pg.keys[entry_idx], +n
        );
    }
    if ((n=get_button_repeats(BTN_D))) {
        reset_shutdown_timer();
        pg.launch_functions[entry_idx](
            pg.keys[entry_idx], -n
        );
    }
    
    /*int64_t ut = get_button_ms(BTN_U);
    if (ut) {
		reset_shutdown_timer();
        pg.launch_functions[entry_idx](
            pg.keys[entry_idx], +ut
        );
	}
    int64_t dt = get_button_ms(BTN_D);
    if (ut) {
		reset_shutdown_timer();
        pg.launch_functions[entry_idx](
            pg.keys[entry_idx], -ut
        );
	}*/
}

int8_t parameter_ux_in_override() {
    if(group_idx != PARAM_GROUP_RUNMTR)
        return -1;
    return entry_idx;
    
}

bool parameter_ux_in_ftp() {
	return group_idx == PARAM_GROUP_FTP;
}