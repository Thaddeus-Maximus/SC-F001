# SC-F001 Firmware

**Solar-powered automated crop harvesting robot** built on the ESP32. Drives a carriage horizontally via a drive motor, lifts/lowers a cutting head via a jack motor, with an auxiliary "fluffer" motor always running during operation. The firmware handles motor sequencing, safety interlocks, remote control, data logging, and a WiFi web interface.

**Primary operational cycle:** Idle → Move Start Delay → Jack Up → Drive → Jack Down → Idle

---

## Hardware Platform

**MCU:** ESP32 (Xtensa dual-core), ESP-IDF framework

**GPIO Map:**
| GPIO | Function |
|------|----------|
| 13 | Button interrupt (active low, pull-up) |
| 14 | Drive encoder |
| 16 | Jack position sensor |
| 19 | Aux sensor 2 (reserved) |
| 21/22 | I2C SDA/SCL (400kHz) → TCA9555 I/O expander |
| 25 | 433MHz RF receiver (RMT input) |
| 26 | Solar charger bulk enable (RTC GPIO) |
| 27 | Safety sensor (active low) |
| 32/33 | External 32.768 kHz RTC crystal (on PCB, not used — see RTC section) |
| 36 (VP) | ADC: drive current sense |
| 39 (VN) | ADC: battery voltage |
| 34 | ADC: jack current sense |
| 35 | ADC: aux current sense |

**TCA9555 (I2C at 0x21):**
- Port 0 (input): 2 physical buttons + 2 additional inputs
- Port 1 (output): 3× H-bridge relay pairs (DRIVE, JACK, AUX) + LEDs

**Motor / Bridge Specs:**
- `BRIDGE_DRIVE` — 100A max, ACS37220 sense chip (13.2 mV/A, inverted polarity)
- `BRIDGE_JACK` — 30A max, ACS37042 sense chip (44 mV/A)
- `BRIDGE_AUX` — 30A max, ACS37042 sense chip (44 mV/A)

---

## Software Architecture

```
app_main()
├── rtc_xtal_init()         Button GPIO setup
├── i2c_init()              TCA9555 init (relays off, LEDs off)
├── adc_init()              ADC1 calibration (12dB attenuation, line-fit)
├── storage_init()          Flash params
├── log_init()              Circular log buffer
├── solar_run_fsm()         (called in main loop too)
├── uart_init()             Serial JSON API task
├── sensors_init()          GPIO ISR setup for sensors/encoders
├── fsm_init()              Control FSM task (priority 10, 20ms tick)
├── rf_433_init()           433MHz RMT receiver task
├── bt_hid_init()           BLE HID host scanner task
└── webserver_init()        WiFi softAP + HTTP + mDNS + DNS

Main loop (50ms):
    i2c_poll_buttons()
    fsm_request() based on button events
    solar_run_fsm()
    drive_leds() status animation
    rtc_check_shutdown_timer() → soft idle on inactivity (180s)
```

**FreeRTOS Tasks:**
| Task | Created by | Priority | Tick | Purpose |
|------|-----------|----------|------|---------|
| `app_main` (main loop) | system | 1 (default) | 50ms | Button polling, LED animation, solar FSM, shutdown timer |
| `control_task` | `fsm_init()` | 10 | 20ms | FSM state machine, relay control, ADC current monitoring, e-fuse |
| UART task | `uart_init()` | default | event-driven | Serial JSON command processing |
| RF 433 task | `rf_433_init()` | default | event-driven | RMT receive + keycode matching |
| BT HID task | `bt_hid_init()` | default | event-driven | BLE HID host scanning + button mapping |
| httpd workers | `webserver_init()` | default | event-driven | HTTP request handling (multiple workers spawned by esp_http_server) |

---

## Key Files

| File | Purpose |
|------|---------|
| `main.c` | Entry point, 50ms main loop, factory reset, LED animation |
| `control_fsm.c/h` | State machine, relay control, current monitoring, calibration |
| `power_mgmt.c/h` | ADC reading, e-fuse thermal algorithm, battery voltage |
| `sensors.c/h` | GPIO ISR-based sensor debouncing, encoder counters |
| `i2c.c/h` | TCA9555 relay/LED/button control |
| `storage.c/h` | 48-param NVM table + circular binary log buffer |
| `comms.c/h` | Unified GET/POST JSON API (shared by HTTP and UART) |
| `webserver.c/h` | WiFi softAP, HTTP server, embedded gzip webpage |
| `uart_comms.c/h` | Serial JSON interface (115200 8N1) |
| `rf_433.c/h` | 433MHz OOK receiver, keycode learn/match |
| `bt_hid.c/h` | BLE HID host, media remote button mapping |
| `rtc.c/h` | Unix time, harvest alarms, soft idle, inactivity timer |
| `solar.c/h` | Simple FLOAT/BULK solar charge state machine |
| `sc_err.h` | Error code definitions |
| `log_test.c/h` | Flash log unit tests |
| `hard_ui.c` | Legacy LCD code (unused/obsolete) |

---

## Control FSM States

```
STATE_IDLE
STATE_MOVE_START_DELAY      (1s)
STATE_JACK_UP_START         (detect current spike → jack engaged)
STATE_JACK_UP               (continue until timer/e-fuse)
STATE_DRIVE_START_DELAY     (1s)
STATE_DRIVE                 (encoder-based distance control)
STATE_DRIVE_END_DELAY       (1s)
STATE_JACK_DOWN             (reverse until e-fuse/sensor)
→ back to STATE_IDLE

STATE_UNDO_JACK_START       (emergency: reverse jack, run until e-fuse/sensor)
→ back to STATE_IDLE

CAL_JACK_DELAY / CAL_JACK_MOVE          (jack calibration sequence)
CAL_DRIVE_DELAY / CAL_DRIVE_MOVE        (drive calibration sequence)
```

**Guards before START:**
- Remaining distance > 0 (leash protection)
- Battery V ≥ `LOW_PROTECTION_V` (default 10V)
- Safety sensor active (debounced stable)
- All e-fuses not tripped

**FSM Loop (20ms tick in `control_task()`):**
1. `process_bridge_current()` — ADC → EMA → auto-zero → e-fuse
2. `process_battery_voltage()` — ADC → EMA
3. `sensors_check()` — drain ISR queue, update counters/debounce
4. State machine transitions (timer + sensor + efuse checks)
5. `drive_relays()` — write relay output from current state
6. `send_fsm_log()` — 39-byte timestamped entry to flash

---

## E-Fuse Algorithm (`power_mgmt.c`)

Per bridge, each 20ms tick:
1. Raw ADC → EMA filter (α = `ADC_ALPHA_ISENS`)
2. Auto-zero: learn zero offset when motor is off + grace period expired
3. Grace period: 250ms after relay closes (ignores startup inrush)
4. **Instant trip:** I ≥ `EFUSE_KINST` × I_nom (default 2×)
5. **Thermal trip:** heat accumulates as I²·Δt; dissipates at τ_cool rate
6. **Auto-reset:** after `EFUSE_TCOOL` seconds of cooling (default 5s)

---

## Safety Sensor Debouncing (Asymmetric)

```
LOW (safe):   1000ms make time  → slow to declare safe (SAFETY_MAKE_US)
HIGH (break): 300ms break time  → fast to kill operation (SAFETY_BREAK_US)
```

Safety break → immediate `STATE_UNDO_JACK_START`.

---

## Communication Interfaces

### WiFi (softAP)
- SSID/password/channel configurable via params (`WIFI_SSID`, `WIFI_PASS`, `WIFI_CHANNEL`)
- mDNS hostname: `sc.local`
- Captive portal DNS: all queries → 192.168.4.1
- HTTP port 80

### HTTP API (port 80)
| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Embedded gzip HTML webpage |
| `/get` | GET | JSON system status |
| `/post` | POST | JSON commands + parameter updates |
| `/log` | GET | Binary log download (4B JSON len + JSON + 8B offsets + log data) |
| `/ota` | POST | Firmware update upload |

### UART (115200 8N1)
- `GET` → same as HTTP GET /get
- `POST: {json}` → same as HTTP POST /post
- `RTCDEBUG` → dump RTC timekeeping state (time, backup, sleep entry, clock source)
- `HELP` → command reference
- Shares `comms_handle_get()` / `comms_handle_post()` with HTTP

### 433MHz RF (GPIO25, RMT)
- 24-bit OOK codes (P_HIGH≈1040µs, P_LOW≈340µs, margin 70µs)
- 8 stored keycodes → FSM_OVERRIDE_* commands
- Learn mode: capture next RX → temp buffer → user commits via web

### Bluetooth HID Host
- Scans for BLE HID devices (service UUID 0x1812)
- Tries saved BDA first, then scans for best RSSI
- Button mapping:
  - VOL_UP → Jack Up (override pulse)
  - VOL_DOWN → Jack Down
  - PREV → Drive Reverse
  - NEXT → Drive Forward

---

## Storage Layout

**Flash partitions (8MB flash):**

| Partition | Offset | Size | Purpose |
|-----------|--------|------|---------|
| post_test | 0x310000 | 4K | Power-on self-test scratch sector |
| params | 0x311000 | 16K | CRC32-protected parameter storage (48 params) |
| log | 0x315000 | ~4.9MB | Circular binary log buffer (head/tail tracked) |

Also includes NVS partition (0x9000, 16K) for WiFi/BT config, board revision, and RTC time backup.

**Log entry format (39 bytes typical):**
```
[0:8]   Timestamp ms (u64 BE)
[8:12]  Battery voltage (f32)
[12:16] Drive current (f32)
[16:20] Jack current (f32)
[20:24] Aux current (f32)
[24:26] Drive encoder count (i16)
[26]    Sensor states (packed)
[27:31] Drive heat (f32)
[31:35] Jack heat (f32)
[35:39] Aux heat (f32)
```

**Key Parameters:**
- Motion: `DRIVE_DIST`, `JACK_DIST`, `DRIVE_KT`, `JACK_KT`, `DRIVE_KE`
- E-fuse: `EFUSE_INOM_1/2/3`, `EFUSE_HEAT_THRESH`, `EFUSE_KINST`, `EFUSE_TCOOL`
- Safety: `SAFETY_BREAK_US`, `SAFETY_MAKE_US`, `LOW_PROTECTION_V`
- RF: `KEYCODE_0` … `KEYCODE_7`
- WiFi: `WIFI_SSID`, `WIFI_PASS`, `WIFI_CHANNEL`
- Schedule: `NUM_MOVES`, `MOVE_START`, `MOVE_END` (seconds-since-midnight)

---

## RTC & Timekeeping

**Time source:** `esp_timer` (40 MHz APB crystal, ~20 ppm accuracy). The external 32.768 kHz crystal on GPIO32/33 is present on the PCB but **not used** — deep sleep is disabled (soft idle instead), so RTC slow clock accuracy is irrelevant. The RTC slow clock uses the default internal RC oscillator.

**`rtc_xtal_init()` in `rtc.c`:** Configures the button GPIO (GPIO13); no crystal bootstrap or sleep wakeup sources.

**Time persistence across resets:** `rtc_save_time()` writes the current unix timestamp to NVS (namespace `"hw"`, key `"rtc_time"`). On boot, `rtc_restore_time()` tries `RTC_DATA_ATTR` first, then falls back to NVS. This ensures time survives software resets even when the bootloader reloads RTC slow memory. The saved time will be stale by the reboot duration (~2s), which is acceptable.

**Diagnosing time issues:** Run `RTCDEBUG` over UART. Reports current time, sync time, elapsed since sync, next alarm, uptime, and soft idle state.

---

## Button & LED Behavior

Single physical button (button 0 via TCA9555 I2C expander) controls all interactions. All logic lives in the main loop (50ms tick) in `main.c`.

### Button Actions by State

**IDLE — Triple-tap to start:**
- 3 taps within a 2-second window triggers `FSM_CMD_START`
- Start fires immediately on the 3rd tap
- LED feedback: 1 tap → LED 1, 2 taps → LED 1+2, 3 taps → LED 1+2+3 (then start)
- LEDs persist until next tap or window expiry; counter resets on expiry

**IDLE / CALIBRATE — 3-second hold to reboot:**
- Saves RTC time to NVS, then calls `esp_restart()`
- LED progression: off (0–750ms) → LED 1 (750–1500ms) → LED 1+2 (1500–2250ms) → LED 1+2+3 (2250–3000ms) → flash all (6× at 150ms) → reboot

**Moving states** — any tap sends `FSM_CMD_UNDO`

**UNDO state (UNDO_JACK_START)** — any tap sends `FSM_CMD_STOP` (emergency stop)

**Calibration states** — tap advances through calibration steps (unchanged)

**Factory reset** — power cycle with GPIO13 held for 10 seconds. Resets all params and erases log/post_test partitions. Preserves NVS (board_rev, BT pairing, RTC time). Only triggers on `ESP_RST_POWERON` or `ESP_RST_EXT`.

### LED Status Indicators

**Physical LED layout** — the three LEDs are wired to TCA9555 port-0 pins
P05, P06, P07. Read bottom → top when checking error codes:

| TCA pin | Bit | Physical position | Called |
|---------|-----|-------------------|--------|
| P05     | 0   | bottom            | LED1 |
| P06     | 1   | middle            | LED2 |
| P07     | 2   | top               | LED3 |

A pattern written as `001` (LSB first) means **only the bottom LED is lit**,
`100` means **only the top LED is lit**, and `111` means all three.

| State | Pattern | Timing |
|-------|---------|--------|
| Idle | LED1 blink | 0.5Hz (1s on / 1s off) |
| Error | Rapid all-blink → error code hold | 5Hz for 1s, then code for 2s (3s cycle) |
| Moving / delays | Waterfall 001→011→111→110→100→000 | ~1 cycle/s (167ms per step) |
| Calibrating | All LEDs flash | 1Hz (500ms on / 500ms off) |
| Undo | All LEDs solid on | Continuous |
| Booting | LED1 solid | Until init complete |

**Error code bits (during 2s hold phase):**

| LED Pattern (bottom→top) | Meaning |
|--------------------------|---------|
| 001 — only bottom (P05) lit   | Efuse tripped (any bridge) or low battery |
| 010 — only middle (P06) lit   | RTC/clock not set |
| 100 — only top (P07) lit      | Safety sensor break or leash limit hit |
| 111 — all three lit           | Unknown FSM error (fallback) |

Error codes are also shown on the web interface status field with individual flag names.

### Implementation Details

- Tap detection uses **release edge** (`i2c_get_button_released()`) with `btn_held < 1000ms` guard (long presses don't count as taps)
- 2-second tap window starts on first tap, fixed duration (not reset by subsequent taps)
- All button state sampled once per tick: `btn_pressed`, `btn_tripped`, `btn_released`, `btn_held`

---

## Power Management

- **Battery voltage:** GPIO39, divider → `V = raw × V_SENS_K + V_SENS_OFFSET` (defaults: K=0.00766̄, offset=0.4)
- **Solar charger:** GPIO26 (RTC hold) — FLOAT/BULK FSM, bulk for 20s when V < 5V for 5s
- **Inactivity shutdown:** 180s → **soft idle** (WiFi/BT off, LEDs off — not deep sleep). Button press exits soft idle.
- **RTC_DATA_ATTR:** Sync timestamps, alarm times, charge state — survive software resets (panics, WDT)

---

## Error Codes (`sc_err.h`)

```c
SC_ERR_EFUSE_TRIP_1  = 0x201  // Drive overcurrent/overheat
SC_ERR_EFUSE_TRIP_2  = 0x202  // Jack
SC_ERR_EFUSE_TRIP_3  = 0x203  // Aux
SC_ERR_SAFETY_TRIP   = 0x210  // Safety sensor break
SC_ERR_LEASH_HIT     = 0x211  // Distance limit reached
SC_ERR_RTC_NOT_SET   = 0x220  // Clock not synchronized
SC_ERR_LOW_BATTERY   = 0x230  // Voltage below threshold
```

---

## Build System

- **Framework:** ESP-IDF (>=5.0)
- **Component deps** (`main/idf_component.yml`): `espressif/mdns ~1.9.1`
- **IDF requires:** `driver`, `esp_http_server`, `esp_netif`, `lwip`, `json`, `esp_timer`, `esp_adc`, `app_update`, `esp_wifi`, `nvs_flash`, `mdns`, `bt`, `esp_hid`
- **Webpage:** `webpage.html` → `webpage_compile.py` → `webpage_gzip.h` (embedded gzip binary). **Must re-run `webpage_compile.py` after any HTML edit before building.**
- **Version:** `version.h.in` filled by CMake from git tags → `FIRMWARE_VERSION`, `BUILD_DATE`
- **Factory reset:** Hold GPIO13 button on cold boot → full parameter + log erase
