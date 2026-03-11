# SC-F001 Firmware — CLAUDE.md

## Overview

The SC-F001 is a **solar-powered automated crop harvesting robot** built on the ESP32. It drives a carriage horizontally via a drive motor and lifts/lowers a cutting head via a jack motor, with an auxiliary "fluffer" motor always running during operation. The firmware handles motor sequencing, safety interlocks, remote control, data logging, and a WiFi web interface.

**Primary operational cycle:** Idle → Move Start Delay → Jack Up → Drive → Jack Down → Idle

---

## Hardware Platform

**MCU:** ESP32 (Xtensa dual-core), IDF framework

**GPIO Map:**
| GPIO | Function |
|------|----------|
| 13 | Button interrupt (active low, pull-up) — also EXT0 wakeup |
| 14 | Jack position sensor / encoder |
| 16 | Drive encoder |
| 19 | Aux sensor 2 (reserved) |
| 21/22 | I2C SDA/SCL (400kHz) → TCA9555 I/O expander |
| 25 | 433MHz RF receiver (RMT input) |
| 26 | Solar charger bulk enable (RTC GPIO, holds across deep sleep) |
| 27 | Safety sensor (active low) |
| 32/33 | External 32.768 kHz RTC crystal (standard watch crystal, 2¹⁵ Hz) |
| 36 (VP) | ADC: drive current sense |
| 39 (VN) | ADC: battery voltage |
| 34 | ADC: jack current sense |
| 35 | ADC: aux current sense |

**TCA9555 (I2C at 0x20):**
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
├── rtc_xtal_init()         RTC crystal + EXT0 wakeup + sleep wakeup check
├── i2c_init()              TCA9555 init (relays off, LEDs off)
├── adc_init()              ADC1 calibration (12dB attenuation, line-fit)
├── storage_init()          Flash params + circular log buffer
├── solar_run_fsm()         (called in main loop too)
├── uart_init()             Serial JSON API task
├── rf_433_init()           433MHz RMT receiver task
├── bt_hid_init()           BLE HID host scanner task
├── fsm_init()              Control FSM task (priority 10, 20ms tick)
└── webserver_init()        WiFi softAP + HTTP + mDNS + DNS

Main loop (50ms):
    i2c_poll_buttons()
    fsm_request() based on button events
    solar_run_fsm()
    driveLEDs() status animation
    rtc_check_shutdown_timer() → deep sleep on inactivity (180s)
```

**Task Priorities:**
- FSM control task: priority 10 (real-time)
- All others: default priority

---

## Key Files

| File | Purpose |
|------|---------|
| `main.c` | Entry point, 50ms main loop, factory reset, LED animation |
| `control_fsm.c/h` | State machine, relay control, current monitoring, calibration |
| `power_mgmt.c/h` | ADC reading, e-fuse thermal algorithm, battery voltage |
| `sensors.c/h` | GPIO ISR-based sensor debouncing, encoder counters |
| `i2c.c/h` | TCA9555 relay/LED/button control |
| `storage.c/h` | 47-param NVM table + circular binary log buffer |
| `comms.c/h` | Unified GET/POST JSON API (shared by HTTP and UART) |
| `webserver.c/h` | WiFi softAP, HTTP server, embedded gzip webpage |
| `uart_comms.c/h` | Serial JSON interface (115200 8N1) |
| `rf_433.c/h` | 433MHz OOK receiver, keycode learn/match |
| `bt_hid.c/h` | BLE HID host, media remote button mapping |
| `rtc.c/h` | Unix time, harvest alarms, deep sleep scheduling |
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

STATE_UNDO_JACK_START       (emergency: reverse jack immediately)
STATE_UNDO_JACK             (run until e-fuse/sensor)
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
5. `driveRelays()` — write relay output from current state
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
| `/set` | POST | JSON commands + parameter updates |
| `/log` | GET | Binary log download (4B JSON len + JSON + 8B offsets + log data) |

### UART (115200 8N1)
- `GET` → same as HTTP GET /get
- `POST: {json}` → same as HTTP POST /set
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

**Flash partition "storage":**
```
0x0000 – 0x0FFF   Parameters (4 sectors, CRC32-protected, 47 params)
0x1000 – end      Circular log buffer (head/tail tracked)
```

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

## RTC & 32.768 kHz Crystal

**Crystal:** Standard 32.768 kHz (32768 Hz = 2¹⁵ Hz) tuning-fork watch crystal on GPIO32/GPIO33. This frequency is universal for RTCs because it divides to exactly 1 Hz with a 15-bit binary counter.

**sdkconfig.defaults settings:**
- `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` — selects the external crystal as the RTC slow clock source instead of the internal ~150 kHz RC oscillator
- `CONFIG_ESP32_RTC_EXT_CRYST_ADDIT_CURRENT_V2=y` — enables extra drive current during the crystal startup window; required for high-ESR tuning-fork crystals (e.g. CM315D32768DZFT ~70 kΩ ESR)

**Known startup failure mode:** On power-on, the ESP32 bootloader attempts to calibrate the crystal. If it fails to detect oscillation within its calibration window, it logs `W: 32 kHz XTAL not found, switching to internal 150 kHz oscillator` and falls back to the RC oscillator. The RC oscillator has ±5% accuracy, producing up to ~180 s/hr of RTC drift — this completely breaks harvest scheduling.

**Firmware mitigation (`rtc_xtal_init()` in `rtc.c`):** If `rtc_clk_slow_src_get()` does not return `SOC_RTC_SLOW_CLK_SRC_XTAL32K` at startup, the code applies a manual bootstrap: `rtc_clk_32k_bootstrap(20000)` (~600 ms of extra drive current at 32 kHz cycles), waits 500 ms for oscillation to stabilise, then calls `rtc_clk_slow_src_set(SOC_RTC_SLOW_CLK_SRC_XTAL32K)` to switch explicitly. Success or failure is logged via `ESP_LOGI/LOGE`.

**Diagnosing crystal issues:** Run `RTCDEBUG` over UART and check `slow_clk_src`. It reports either `XTAL32K (OK)` or `NOT XTAL32K — check crystal!`. The `logtool/rtc_test.py` script automates this and runs multi-cycle drift tests.

**Time persistence across deep sleep:** `rtc_backup_s` and `rtc_sleep_entry_s` are `RTC_DATA_ATTR` (survive deep sleep). On wakeup, `rtc_restore_time()` adds exactly `DEEP_SLEEP_US / 1e6` seconds to `rtc_sleep_entry_s` to reconstruct the correct time without an NTP sync.

---

## Power Management

- **Battery voltage:** GPIO39, divider → `V = raw × 0.00767 + 0.4`
- **Solar charger:** GPIO26 (RTC hold) — FLOAT/BULK FSM, bulk for 20s when V < 5V for 5s
- **Inactivity shutdown:** 180s → deep sleep
- **Deep sleep wakeup:** RTC timer (120s), RTC alarm (next harvest), EXT0 GPIO13 (button)
- **RTC_DATA_ATTR:** FSM state, errors, alarm times, charge state — survive deep sleep

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

- **Framework:** ESP-IDF (>=4.1.0)
- **Component deps** (`idf_component.yml`): `espressif/mdns`, `joltwallet/littlefs`, `esp-idf-lib/tca95x5`
- **IDF requires:** `driver`, `esp_http_server`, `esp_netif`, `lwip`, `json`, `esp_timer`, `esp_adc`, `app_update`, `esp_wifi`, `nvs_flash`, `mdns`, `bt`, `esp_hid`
- **Webpage:** `webpage.html` → `webpage_compile.py` → `webpage_gzip.h` (embedded gzip binary)
- **Version:** `version.h.in` filled by CMake from git tags → `FIRMWARE_VERSION`, `BUILD_DATE`
- **Factory reset:** Hold GPIO13 button on cold boot → full parameter + log erase

---

## Conventions

- **Naming:** `snake_case` functions with module prefix (`fsm_init`, `i2c_poll_buttons`); `UPPER_SNAKE_CASE` constants/enums
- **Module pattern:** `.c` / `.h` pairs; headers expose only public API
- **Concurrency:** FSM commands via `xQueueSend`; log writes via async queue; GPIO ISR → minimal work → sensor queue
- **State machine pattern:** transitions in one `switch`, relay outputs in a second `switch` (separated)
- **Watchdog:** `esp_task_wdt_add/reset` in each task, 10s timeout
- **Logging:** `ESP_LOGI(TAG, ...)` per module; flash circular log for telemetry
- **No dynamic allocation** in ISR or high-priority paths
