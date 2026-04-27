/*
 * bringup.c — manufacturing / bench bring-up procedure over UART.
 *
 * Line protocol specified in docs/SC-F001/BRINGUP.md §3.
 * All responses are written with printf() so they share the UART stream
 * with uart_comms; keep every line short and grep-friendly.
 *
 * Distinct from the per-module POST routines (adc_post, i2c_post, ...) that
 * run on every boot — those are the actual power-on self-tests.
 */

#include "bringup.h"
#include "board_config.h"
#include "control_fsm.h"
#include "i2c.h"
#include "power_mgmt.h"
#include "rf_433.h"
#include "sensors.h"
#include "solar.h"
#include "storage.h"
#include "version.h"
#include "webserver.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define TAG "BRINGUP"

static bool        s_active        = false;
static int64_t     s_start_us      = 0;
static volatile int s_http_reqs    = 0;

/* -------- state helpers -------- */

bool bringup_mode_is_active(void) { return s_active; }

void bringup_notify_http_request(void) { s_http_reqs++; }

void bringup_mode_enter(void)
{
    extern relay_port_t last_relay_state;
    s_active   = true;
    s_start_us = esp_timer_get_time();
    s_http_reqs = 0;
    /* All bridges off, sensor rail (P10) up — system is still on. */
    relay_port_t idle = {.bridges = {.SENSORS = 1}};
    last_relay_state = idle;
    i2c_relays_idle();
}

void bringup_mode_exit(void)
{
    extern relay_port_t last_relay_state;
    s_active = false;
    relay_port_t idle = {.bridges = {.SENSORS = 1}};
    last_relay_state = idle;
    i2c_relays_idle();
}

static float elapsed_s(void)
{
    return (esp_timer_get_time() - s_start_us) / 1e6f;
}

/* -------- output helpers -------- */

/* Build the line in a stack buffer and emit with a single write so concurrent
 * ESP_LOGx output (notably the wifi driver during BU.WIFI.START) cannot slice
 * into the middle of it. Leading '\n' protects against partial lines that
 * another task may have written without a terminator. */
__attribute__((format(printf, 2, 3)))
static void emit(const char *kind, const char *fmt, ...)
{
    char buf[256];
    /* Reserve one byte at the end for the trailing '\n' so a long line is
     * truncated within the body rather than dropping the newline. Without
     * this, a body that filled the buffer would produce a line glued to
     * whatever came next on the wire. */
    const int cap = (int)sizeof(buf) - 1;  // room for '\n'
    int n = snprintf(buf, cap, "\nBU.%s ", kind);
    if (n < 0) n = 0;
    if (n > cap) n = cap;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, cap - n, fmt, ap);
    va_end(ap);
    if (m > 0) n += (m < cap - n) ? m : cap - n;
    buf[n++] = '\n';
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
}

#define OK(fmt, ...)    emit("OK",    fmt, ##__VA_ARGS__)
#define ERR(fmt, ...)   emit("ERR",   fmt, ##__VA_ARGS__)
#define SKIP(fmt, ...)  emit("SKIP",  fmt, ##__VA_ARGS__)
#define EVT(fmt, ...)   emit("EVENT", fmt, ##__VA_ARGS__)

/* -------- tokenizer -------- */

/* strsep-style: mutate s in place, return next token (no quotes handled). */
static char *next_tok(char **s)
{
    if (!s || !*s) return NULL;
    while (**s == ' ' || **s == '\t') (*s)++;
    if (**s == '\0') return NULL;
    char *start = *s;
    while (**s && **s != ' ' && **s != '\t') (*s)++;
    if (**s) { *(*s)++ = '\0'; }
    return start;
}

static void str_upper(char *s)
{
    for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32;
}

/* -------- parameter lookup by name -------- */

static int param_find(const char *name)
{
    for (int i = 0; i < NUM_PARAMS; i++) {
        if (strcmp(name, get_param_name((param_idx_t)i)) == 0) return i;
    }
    return -1;
}

/* -------- command handlers -------- */

static void cmd_begin(char *args)
{
    (void)args;
    bringup_mode_enter();
    OK("begin fw=%s board=%s t=%.2f",
       FIRMWARE_VERSION,
#ifdef BOARD_V5
       "V5",
#else
       "V4",
#endif
       elapsed_s());
}

static void cmd_end(char *args)
{
    (void)args;
    OK("end reboot t=%.2f", elapsed_s());
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    bringup_mode_exit();
    esp_restart();
}

static void cmd_info(char *args)
{
    (void)args;
    esp_reset_reason_t r = esp_reset_reason();
    const char *rname = "?";
    switch (r) {
        case ESP_RST_POWERON:  rname = "POWERON"; break;
        case ESP_RST_EXT:      rname = "EXT";     break;
        case ESP_RST_SW:       rname = "SW";      break;
        case ESP_RST_PANIC:    rname = "PANIC";   break;
        case ESP_RST_INT_WDT:  rname = "INT_WDT"; break;
        case ESP_RST_TASK_WDT: rname = "TASK_WDT";break;
        case ESP_RST_WDT:      rname = "WDT";     break;
        case ESP_RST_DEEPSLEEP:rname = "DEEPSLEEP";break;
        case ESP_RST_BROWNOUT: rname = "BROWNOUT";break;
        default: break;
    }
    OK("info reset=%s heap=%u min_heap=%u fw=%s build=%s",
       rname,
       (unsigned)esp_get_free_heap_size(),
       (unsigned)esp_get_minimum_free_heap_size(),
       FIRMWARE_VERSION, BUILD_DATE);
}

static void cmd_flash(char *args)
{
    (void)args;
    const esp_partition_t *p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x42, "post_test");
    if (!p) { ERR("flash reason=\"no post_test partition\""); return; }

    /* Erase, write pattern, read back, compare. */
    const size_t N = 64;
    uint8_t pattern[N], readback[N];
    for (size_t i = 0; i < N; i++) pattern[i] = (uint8_t)(i ^ 0xA5);

    esp_err_t e = esp_partition_erase_range(p, 0, 4096);
    if (e != ESP_OK) { ERR("flash stage=erase err=%s", esp_err_to_name(e)); return; }
    e = esp_partition_write(p, 0, pattern, N);
    if (e != ESP_OK) { ERR("flash stage=write err=%s", esp_err_to_name(e)); return; }
    e = esp_partition_read(p, 0, readback, N);
    if (e != ESP_OK) { ERR("flash stage=read err=%s", esp_err_to_name(e)); return; }
    if (memcmp(pattern, readback, N) != 0) {
        ERR("flash stage=compare mismatch");
        return;
    }
    OK("flash post_part=roundtrip log_head=%u log_tail=%u partitions_size=%u",
       (unsigned)log_get_head(), (unsigned)log_get_tail(),
       (unsigned)(p->address));
}

static void cmd_i2c(char *args)
{
    (void)args;
    /* i2c_post re-probes TCA9555 by reading its input register. */
    esp_err_t e = i2c_post();
    if (e != ESP_OK) { ERR("i2c tca9555=nack err=%s", esp_err_to_name(e)); return; }
    OK("i2c tca9555=ack");
}

static void cmd_led(char *args)
{
    /* BU.LED <mask 0..7> [on|off] — just writes mask if given; ignores the
     * optional second token and uses it when present to set/clear. */
    char *s = args;
    char *tok = next_tok(&s);
    if (!tok) { ERR("led reason=\"missing mask\""); return; }
    unsigned mask = (unsigned)strtoul(tok, NULL, 0);
    if (mask > 7) mask = 7;
    i2c_set_led1((uint8_t)mask);
    OK("led mask=%u", mask);
}

/* BU.LED.WATCH
 *   LEDs solid (all on) while the physical button is released.
 *   LEDs waterfall at ~83 ms/step while the button is held.
 *   Runs indefinitely; any UART byte aborts.
 */
static void cmd_led_watch(char *args)
{
    (void)args;
    static const uint8_t waterfall[] = {
        0b001, 0b011, 0b111, 0b110, 0b100, 0b000
    };
    const size_t N_WF = sizeof(waterfall) / sizeof(waterfall[0]);
    const int64_t step_us = 83000;  /* ~83 ms — 3× the 250 ms host-side rate */

    int64_t next_step_us = esp_timer_get_time();
    size_t  wf_i = 0;
    int     last_pressed = -1;  /* force initial emit */
    uint8_t last_mask = 0xFF;

    while (1) {
        /* Abort on any UART input. */
        size_t available = 0;
        if (uart_get_buffered_data_len(UART_NUM_0, &available) == ESP_OK
            && available > 0) {
            uint8_t drain[64];
            while (available > 0) {
                int n = uart_read_bytes(UART_NUM_0, drain, sizeof(drain), 0);
                if (n <= 0) break;
                available = (size_t)n < available ? available - (size_t)n : 0;
            }
            break;
        }

        i2c_poll_buttons();
        bool pressed = i2c_get_button_state(0);
        int64_t now_us = esp_timer_get_time();

        uint8_t mask;
        if (pressed) {
            if (now_us >= next_step_us) {
                wf_i = (wf_i + 1) % N_WF;
                next_step_us = now_us + step_us;
            }
            mask = waterfall[wf_i];
        } else {
            wf_i = 0;
            next_step_us = now_us;  /* so the first press starts from step 0 */
            mask = 0b111;
        }

        if (mask != last_mask) {
            i2c_set_led1(mask);
            last_mask = mask;
        }
        if ((int)pressed != last_pressed) {
            EVT("led t=%.2f pressed=%d", elapsed_s(), pressed ? 1 : 0);
            last_pressed = pressed;
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    i2c_set_led1(0);
    OK("led.watch done");
}

static void cmd_adc_once(void)
{
    int bat_mv = get_bat_raw_mv();
    /* Bypass the EMA — process_battery_voltage() runs in the FSM task,
     * which is paused while bring-up is active, so get_battery_V() returns
     * a stale value that never reflects V_SENS_K / V_SENS_OFFSET writes
     * issued during calibration. Compute fresh from raw mV + current params. */
    float bat_V = bat_mv * get_param_value_t(PARAM_V_SENS_K).f32
                + get_param_value_t(PARAM_V_SENS_OFFSET).f32;
#ifdef BOARD_V5
    /* VOC and FAULT pins are unusable on V5 (input-only ESP32 GPIOs
     * without external pulls — see README "V5 hardware caveats"); skip. */
    int isens_mv = get_isens_raw_mv();
    float isens_A = -(isens_mv - 1650.0f) / 13.2f;
    OK("adc bat_mv=%d bat_V=%.3f isens_mv=%d isens_A=%+.2f",
       bat_mv, bat_V, isens_mv, isens_A);
#else
    OK("adc bat_mv=%d bat_V=%.3f", bat_mv, bat_V);
#endif
}

static void cmd_adc(char *args)
{
    (void)args;
    cmd_adc_once();
}

static void cmd_adc_stream(char *args)
{
    char *s = args;
    char *t = next_tok(&s);
    int sec = t ? atoi(t) : 5;
    if (sec < 1) sec = 1;
    if (sec > 60) sec = 60;

    int64_t end_us = esp_timer_get_time() + (int64_t)sec * 1000000;
    while (esp_timer_get_time() < end_us) {
#ifdef BOARD_V5
        int bat_mv = get_bat_raw_mv();
        int isens_mv = get_isens_raw_mv();
        EVT("adc t=%.2f bat_mv=%d isens_mv=%d", elapsed_s(), bat_mv, isens_mv);
#else
        int bat_mv = get_bat_raw_mv();
        EVT("adc t=%.2f bat_mv=%d", elapsed_s(), bat_mv);
#endif
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    OK("adc.stream sec=%d", sec);
}

static void cmd_sensors_watch(char *args)
{
    /* BU.SENSORS.WATCH [sec]
     *   sec omitted or 0 → watch indefinitely; exit when any byte arrives
     *                      on UART0 (operator hit Enter on the host side).
     *   sec > 0          → watch for that many seconds, then return.
     */
    /* Force the sensor rail (P10) up before we observe — covers cases where
     * the FSM or sensor task drove it low between boot and BU.BEGIN. */
    i2c_relays_idle();
    char *s = args;
    char *t = next_tok(&s);
    int sec = t ? atoi(t) : 0;
    bool indefinite = (sec <= 0);
    if (!indefinite && sec > 600) sec = 600;

    static const char *names[N_SENSORS] = {"SAFETY", "DRIVE", "JACK", "AUX"};
    bool last_state[N_SENSORS];
    bool make_seen[N_SENSORS]  = {false};
    bool break_seen[N_SENSORS] = {false};
    /* Read the GPIO directly — the normal sensor pipeline runs in the FSM
     * task (sensors_check()), which is paused while bring-up is active, so
     * get_sensor() returns stale state. Active-low → inverted. */
    #define _SENS_RAW(i) (!gpio_get_level(sensor_pins[i]))
    extern uint8_t sensor_pins[N_SENSORS];
    for (int i = 0; i < N_SENSORS; i++) last_state[i] = _SENS_RAW(i);

    int64_t end_us = esp_timer_get_time() + (int64_t)sec * 1000000;
    int64_t next_snapshot_us = esp_timer_get_time();
    while (indefinite || esp_timer_get_time() < end_us) {
        /* Abort on any UART input. */
        size_t available = 0;
        if (uart_get_buffered_data_len(UART_NUM_0, &available) == ESP_OK
            && available > 0) {
            uint8_t drain[64];
            while (available > 0) {
                int n = uart_read_bytes(UART_NUM_0, drain, sizeof(drain), 0);
                if (n <= 0) break;
                available = (size_t)n < available ? available - (size_t)n : 0;
            }
            break;
        }

        for (int i = 0; i < N_SENSORS; i++) {
            bool now = _SENS_RAW(i);
            if (now != last_state[i]) {
                const char *edge = now ? "make" : "break";
                if (now) make_seen[i] = true; else break_seen[i] = true;
                EVT("sensor name=%s edge=%s t=%.2f",
                    names[i], edge, elapsed_s());
                last_state[i] = now;
            }
        }

        /* Periodic state snapshot of all four sensors — includes the
         * no-connect slot so a floating/misrouted pin is visible. */
        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_snapshot_us) {
            next_snapshot_us = now_us + 250000;  /* 250 ms */
            EVT("state t=%.2f SAFETY=%d DRIVE=%d JACK=%d AUX=%d "
                "isr_s=%u isr_d=%u isr_j=%u isr_a=%u",
                elapsed_s(),
                (int)_SENS_RAW(SENSOR_SAFETY),
                (int)_SENS_RAW(SENSOR_DRIVE),
                (int)_SENS_RAW(SENSOR_JACK),
                (int)_SENS_RAW(SENSOR_AUX2),
                (unsigned)get_sensor_isr_edges(SENSOR_SAFETY),
                (unsigned)get_sensor_isr_edges(SENSOR_DRIVE),
                (unsigned)get_sensor_isr_edges(SENSOR_JACK),
                (unsigned)get_sensor_isr_edges(SENSOR_AUX2));
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    #undef _SENS_RAW

    /* Summary: which sensors saw both edges. */
    char buf[128];
    size_t used = 0;
    for (int i = 0; i < N_SENSORS; i++) {
        const char *tag =
            (make_seen[i] && break_seen[i]) ? "both"
            : make_seen[i]  ? "make_only"
            : break_seen[i] ? "break_only"
            : "none";
        int n = snprintf(buf + used, sizeof(buf) - used,
                         "%s%s=%s", used ? " " : "", names[i], tag);
        if (n < 0 || (size_t)n >= sizeof(buf) - used) break;
        used += n;
    }
    OK("sensors.watch sec=%d %s", indefinite ? -1 : sec, buf);
}

static bool parse_bridge(const char *s, bridge_t *out)
{
    if (strcasecmp(s, "DRIVE") == 0) { *out = BRIDGE_DRIVE; return true; }
    if (strcasecmp(s, "JACK")  == 0) { *out = BRIDGE_JACK;  return true; }
    if (strcasecmp(s, "AUX")   == 0) { *out = BRIDGE_AUX;   return true; }
    return false;
}

static bool parse_dir(const char *s, uint8_t *out)
{
    if (strcasecmp(s, "FWD") == 0 || strcasecmp(s, "UP") == 0)
        { *out = BRIDGE_FWD; return true; }
    if (strcasecmp(s, "REV") == 0 || strcasecmp(s, "DOWN") == 0)
        { *out = BRIDGE_REV; return true; }
    if (strcasecmp(s, "ON") == 0)
        { *out = BRIDGE_ON; return true; }
    if (strcasecmp(s, "OFF") == 0)
        { *out = BRIDGE_OFF; return true; }
    return false;
}

static void cmd_relay(char *args)
{
    char *s = args;
    char *t_bridge = next_tok(&s);
    char *t_dir    = next_tok(&s);
    char *t_ms     = next_tok(&s);
    if (!t_bridge || !t_dir) { ERR("relay reason=\"usage: <bridge> <dir> [ms]\""); return; }

    int ms = t_ms ? atoi(t_ms) : 150;
    if (ms < 10)   ms = 10;
    if (ms > 2000) ms = 2000;

    /* Read SAFETY directly: sensors_check() runs in the FSM task, which is
     * paused while bring-up is active, so is_safe / get_is_safe() are stale.
     * Safety pin is active-LOW. */
    extern uint8_t sensor_pins[N_SENSORS];
    #define _BU_SAFETY_OPEN() (gpio_get_level(sensor_pins[SENSOR_SAFETY]) != 0)
    if (_BU_SAFETY_OPEN()) { SKIP("relay reason=\"safety open\""); return; }

    /* P10 / sensor power rail. Default is ON; pulse it OFF to prove the line
     * can be driven, then restore. */
    if (strcasecmp(t_bridge, "SENSORS") == 0) {
        i2c_relays_sleep();                    /* P10 low */
        vTaskDelay(pdMS_TO_TICKS(ms));
        i2c_relays_idle();                     /* P10 high (restore) */
        OK("relay bridge=SENSORS ms=%d", ms);
        return;
    }

    bridge_t b;
    uint8_t  dir;
    if (!parse_bridge(t_bridge, &b)) { ERR("relay reason=\"bad bridge\""); return; }
    if (!parse_dir(t_dir, &dir))     { ERR("relay reason=\"bad dir\""); return; }

    /* Sample current before, pulse, sample at midpoint, release, sample after.
     * FSM is paused during POST, so we drive process_bridge_current() ourselves
     * to refresh isens[].current before each read. We also mirror the relay
     * state into last_relay_state so V5's shared autozero gate (which looks at
     * last_relay_state to decide if bridges are powered) stays truthful. */
    extern volatile int64_t fsm_now;
    extern relay_port_t     last_relay_state;

    vTaskDelay(pdMS_TO_TICKS(50));  /* let things settle */
    fsm_now = esp_timer_get_time();
    process_bridge_current(b);
    float I_before = get_bridge_A(b);

    /* Which sensor to count edges on during the pulse — ISR-level counter,
     * doesn't depend on sensors_check() running. */
    sensor_t which_sensor = N_SENSORS;
    if (b == BRIDGE_DRIVE) which_sensor = SENSOR_DRIVE;
    else if (b == BRIDGE_JACK) which_sensor = SENSOR_JACK;
    uint32_t edges_before = (which_sensor < N_SENSORS)
        ? get_sensor_isr_edges(which_sensor) : 0;

    relay_port_t rs = {.raw = 0};
    switch (b) {
        case BRIDGE_DRIVE: rs.bridges.DRIVE = dir; break;
        case BRIDGE_JACK:  rs.bridges.JACK  = dir; break;
        case BRIDGE_AUX:   rs.bridges.AUX   = dir; break;
        default: ERR("relay reason=\"bad bridge idx\""); return;
    }
    rs.bridges.SENSORS = 1;
    last_relay_state = rs;
    i2c_set_relays(rs);

    /* JACK DOWN should stop as soon as the JACK sensor goes active (LOW) so
     * the bring-up pulse can't drive the actuator into its mechanical limit.
     * Other directions/bridges run for the full requested duration. SAFETY
     * is checked every iteration regardless of bridge — multi-second pulses
     * during bring-up must still kill the motor on a safety break. */
    bool jack_down = (b == BRIDGE_JACK && dir == BRIDGE_REV);
    bool stopped_by_sensor = false;
    bool stopped_by_safety = false;

    int64_t pulse_start_us = esp_timer_get_time();
    int64_t mid_us = pulse_start_us + (int64_t)(ms / 2) * 1000;
    int64_t end_us = pulse_start_us + (int64_t)ms * 1000;

    float I_mid = NAN;
    while (esp_timer_get_time() < end_us) {
        if (_BU_SAFETY_OPEN()) {
            stopped_by_safety = true;
            break;
        }
        if (jack_down && gpio_get_level(sensor_pins[SENSOR_JACK]) == 0) {
            stopped_by_sensor = true;
            break;
        }
        if (isnan(I_mid) && esp_timer_get_time() >= mid_us) {
            fsm_now = esp_timer_get_time();
            process_bridge_current(b);
            I_mid = get_bridge_A(b);
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (isnan(I_mid)) {
        /* Sensor tripped before we hit the midpoint; sample current now so
         * the response still has a meaningful I_mid. */
        fsm_now = esp_timer_get_time();
        process_bridge_current(b);
        I_mid = get_bridge_A(b);
    }

    relay_port_t idle = {.bridges = {.SENSORS = 1}};
    last_relay_state = idle;
    i2c_relays_idle();
    vTaskDelay(pdMS_TO_TICKS(50));
    fsm_now = esp_timer_get_time();
    process_bridge_current(b);
    float I_after = get_bridge_A(b);
    float heat = efuse_get_heat(b);
    int tripped = efuse_get(b) ? 1 : 0;
    uint32_t edges_after = (which_sensor < N_SENSORS)
        ? get_sensor_isr_edges(which_sensor) : 0;
    uint32_t edges = edges_after - edges_before;

    int actual_ms = (int)((esp_timer_get_time() - pulse_start_us) / 1000);
    const char *stop_reason =
        stopped_by_safety ? "safety" :
        stopped_by_sensor ? "sensor" : "time";
    OK("relay bridge=%s dir=%s ms=%d actual_ms=%d stop=%s "
       "I_before=%+.2f I_mid=%+.2f I_after=%+.2f heat=%.3f tripped=%d edges=%u",
       t_bridge, t_dir, ms, actual_ms, stop_reason,
       I_before, I_mid, I_after, heat, tripped, (unsigned)edges);
    #undef _BU_SAFETY_OPEN
}

static void cmd_rf_watch(char *args)
{
    /* BU.RF.WATCH [sec]
     *   sec omitted or 0 → watch indefinitely; exit when any byte arrives
     *                      on UART0 (operator hit Enter on the host side).
     */
    char *s = args;
    char *t = next_tok(&s);
    int sec = t ? atoi(t) : 0;
    bool indefinite = (sec <= 0);
    if (!indefinite && sec > 600) sec = 600;

    int64_t end_us = esp_timer_get_time() + (int64_t)sec * 1000000;
    int count = 0;
    /* Drain any stale code from before the watch started. */
    (void)rf_433_peek_latest();
    while (indefinite || esp_timer_get_time() < end_us) {
        /* Abort on any UART input. */
        size_t available = 0;
        if (uart_get_buffered_data_len(UART_NUM_0, &available) == ESP_OK
            && available > 0) {
            uint8_t drain[64];
            while (available > 0) {
                int n = uart_read_bytes(UART_NUM_0, drain, sizeof(drain), 0);
                if (n <= 0) break;
                available = (size_t)n < available ? available - (size_t)n : 0;
            }
            break;
        }
        uint32_t code = rf_433_peek_latest();
        if (code) {
            EVT("rf code=0x%lX t=%.2f", (unsigned long)code, elapsed_s());
            count++;
        }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    OK("rf.watch sec=%d seen=%d", indefinite ? -1 : sec, count);
}

static void cmd_wifi_start(char *args)
{
    (void)args;
    esp_err_t e = webserver_init();
    if (e != ESP_OK) { ERR("wifi.start err=%s", esp_err_to_name(e)); return; }
    OK("wifi.start mode=AP ssid=\"%s\" ip=192.168.4.1",
       get_param_string(PARAM_WIFI_SSID));
}

static void cmd_wifi_wait(char *args)
{
    (void)args;  /* no timeout — BRINGUP.md §4 Stage 6. Operator aborts via Ctrl+C. */
    wifi_sta_list_t sta = {0};
    int last_n = 0;
    bool aborted = false;
    while (1) {
        /* Abort on any UART input — the host sends a stray byte to break out
         * of the wait so that a follow-up BU.END is actually dispatched
         * (otherwise the dispatcher stays blocked here forever). */
        size_t available = 0;
        if (uart_get_buffered_data_len(UART_NUM_0, &available) == ESP_OK
            && available > 0) {
            uint8_t drain[64];
            while (available > 0) {
                int n = uart_read_bytes(UART_NUM_0, drain, sizeof(drain), 0);
                if (n <= 0) break;
                available = (size_t)n < available ? available - (size_t)n : 0;
            }
            aborted = true;
            break;
        }

        if (esp_wifi_ap_get_sta_list(&sta) == ESP_OK && sta.num > last_n) {
            EVT("wifi.assoc n=%d t=%.2f", sta.num, elapsed_s());
            last_n = sta.num;
        }
        /* Bail when at least one client is associated AND the web UI has
         * issued at least one request (notified by webserver). */
        if (last_n > 0 && s_http_reqs > 0) break;
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    OK("wifi.wait clients=%d http_reqs=%d aborted=%d",
       last_n, s_http_reqs, aborted ? 1 : 0);
}

static void cmd_fsm(char *args)
{
    char *s = args;
    char *sub = next_tok(&s);
    if (!sub) sub = "INFO";
    str_upper(sub);
    if (strcmp(sub, "INFO") == 0) {
        OK("fsm state=%d err=%d idle=%d",
           (int)fsm_get_state(), (int)fsm_get_error(), fsm_is_idle() ? 1 : 0);
    } else {
        ERR("fsm reason=\"unknown subcommand\"");
    }
}

static void cmd_solar_tick(char *args)
{
    (void)args;
    (void)solar_run_fsm();
    OK("solar tick=ok chg_bulk=%d",
       gpio_get_level(GPIO_NUM_26));
}

/* BU.PARAM GET <key>   |   BU.PARAM SET <key> <value> */
static void cmd_param(char *args)
{
    char *s = args;
    char *op = next_tok(&s);
    char *key = next_tok(&s);
    if (!op || !key) { ERR("param reason=\"usage: GET <k> | SET <k> <v>\""); return; }
    str_upper(op);
    int idx = param_find(key);
    if (idx < 0) { ERR("param reason=\"unknown key\" key=%s", key); return; }

    param_type_e type = get_param_type((param_idx_t)idx);

    if (strcmp(op, "GET") == 0) {
        param_value_t v = get_param_value_t((param_idx_t)idx);
        switch (type) {
            case PARAM_TYPE_u16: OK("param key=%s value=%u",   key, v.u16); break;
            case PARAM_TYPE_i16: OK("param key=%s value=%d",   key, v.i16); break;
            case PARAM_TYPE_u32: OK("param key=%s value=%u",   key, (unsigned)v.u32); break;
            case PARAM_TYPE_i32: OK("param key=%s value=%d",   key, (int)v.i32); break;
            case PARAM_TYPE_f32: OK("param key=%s value=%.9g", key, v.f32); break;
            case PARAM_TYPE_f64: OK("param key=%s value=%.17g",key, v.f64); break;
            case PARAM_TYPE_str: OK("param key=%s value=\"%s\"", key, get_param_string((param_idx_t)idx)); break;
        }
        return;
    }

    if (strcmp(op, "SET") == 0) {
        /* SET writes flash. Require BU.BEGIN to prevent accidental persistence
         * from stray BU.PARAM lines outside of an active bring-up session. */
        if (!s_active) {
            ERR("param reason=\"BU.BEGIN required first\""); return;
        }
        char *val = next_tok(&s);
        if (!val) { ERR("param reason=\"missing value\""); return; }
        esp_err_t e = ESP_OK;
        if (type == PARAM_TYPE_str) {
            e = set_param_string((param_idx_t)idx, val);
        } else {
            param_value_t v = {0};
            switch (type) {
                case PARAM_TYPE_u16: v.u16 = (uint16_t)strtoul(val, NULL, 0); break;
                case PARAM_TYPE_i16: v.i16 = (int16_t)strtol(val, NULL, 0);   break;
                case PARAM_TYPE_u32: v.u32 = (uint32_t)strtoul(val, NULL, 0); break;
                case PARAM_TYPE_i32: v.i32 = (int32_t)strtol(val, NULL, 0);   break;
                case PARAM_TYPE_f32: v.f32 = strtof(val, NULL); break;
                case PARAM_TYPE_f64: v.f64 = strtod(val, NULL); break;
                default: break;
            }
            e = set_param_value_t((param_idx_t)idx, v);
        }
        if (e != ESP_OK) { ERR("param reason=\"set failed\" err=%s", esp_err_to_name(e)); return; }
        e = commit_params();
        if (e != ESP_OK) { ERR("param reason=\"commit failed\" err=%s", esp_err_to_name(e)); return; }
        /* If the conversion params changed, refresh the battery EMA so
         * get_battery_V() returns a value consistent with the new K/OFFSET
         * immediately rather than decaying through the EMA. */
        if (idx == PARAM_V_SENS_K || idx == PARAM_V_SENS_OFFSET) {
            reset_battery_ema();
        }
        OK("param key=%s set=ok committed=yes", key);
        return;
    }

    ERR("param reason=\"unknown op\" op=%s", op);
}

/* BU.FACTORY_RESET — wipe all params back to defaults, erase log partition,
 * then reboot. Equivalent to the cold-boot button-hold path in main.c, but
 * reachable from a host without physical access. Destructive — operator
 * must explicitly invoke it. */
static void cmd_factory_reset(char *args)
{
    (void)args;
    /* Refuse without an explicit BU.BEGIN. Without this guard, any party
     * that can write to UART0 can wipe params/log just by sending the
     * command — and uart_comms.c forwards bare BU.* lines to the dispatcher
     * even when bring-up mode is off. */
    if (!s_active) {
        ERR("factory_reset reason=\"BU.BEGIN required first\"");
        return;
    }
    OK("factory_reset stage=start");
    esp_err_t e = factory_reset();
    if (e != ESP_OK) {
        ERR("factory_reset stage=apply err=%s", esp_err_to_name(e));
        return;
    }
    OK("factory_reset stage=done reboot=2s");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

/* -------- dispatcher -------- */

typedef void (*cmd_fn)(char *args);

struct cmd_entry {
    const char *name;     /* uppercased, no BU. prefix */
    cmd_fn fn;
};

static const struct cmd_entry CMDS[] = {
    { "BEGIN",          cmd_begin        },
    { "END",            cmd_end          },
    { "INFO",           cmd_info         },
    { "FLASH",          cmd_flash        },
    { "I2C",            cmd_i2c          },
    { "LED",            cmd_led          },
    { "LED.WATCH",      cmd_led_watch    },
    { "ADC",            cmd_adc          },
    { "ADC.STREAM",     cmd_adc_stream   },
    { "SENSORS.WATCH",  cmd_sensors_watch},
    { "RELAY",          cmd_relay        },
    { "RF.WATCH",       cmd_rf_watch     },
    { "WIFI.START",     cmd_wifi_start   },
    { "WIFI.WAIT",      cmd_wifi_wait    },
    { "FSM",            cmd_fsm          },
    { "SOLAR.TICK",     cmd_solar_tick   },
    { "PARAM",          cmd_param        },
    { "FACTORY_RESET",  cmd_factory_reset},
};

void bringup_handle_line(char *line)
{
    /* Trim leading whitespace. */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return;

    /* Expect "BU.<CMD> [args]" */
    if (strncasecmp(line, "BU.", 3) != 0) {
        ERR("dispatch reason=\"missing BU. prefix\"");
        return;
    }
    line += 3;

    /* Split CMD token from args. */
    char *sp = line;
    while (*sp && *sp != ' ' && *sp != '\t') sp++;
    char *args = sp;
    if (*sp) { *sp = '\0'; args = sp + 1; }

    str_upper(line);

    for (size_t i = 0; i < sizeof(CMDS)/sizeof(CMDS[0]); i++) {
        if (strcmp(line, CMDS[i].name) == 0) {
            CMDS[i].fn(args);
            return;
        }
    }
    ERR("dispatch reason=\"unknown command\" cmd=%s", line);
}
