# SC-F001 Firmware

**Solar-powered autonomous livestock shelter mover** built on the ESP32. Drives horizontally via a motor, lifts/lowers a the structure via a jack motor, with an auxiliary "fluffer" motor always running while driving. The firmware handles motor sequencing, safety interlocks, remote control, data logging, and a WiFi web interface.

---

## Hardware Platform

**MCU:** ESP32 (Xtensa dual-core), ESP-IDF framework

**GPIO Map:**
| GPIO    | Function                                                                |
|---------|-------------------------------------------------------------------------|
| 13      | Button interrupt (active low, pull-up)                                  |
| 14      | Jack position sensor                                                    |
| 16      | Not Used                                                                |
| 19      | Drive encoder                                                           |
| 21/22   | I2C SDA/SCL (400kHz) → TCA9555 I/O expander                             |
| 25      | 433MHz RF receiver (RMT input)                                          |
| 26      | Solar charger bulk enable (RTC GPIO)                                    |
| 27      | Safety sensor (active low)                                              |
| 32/33   | External 32.768 kHz RTC crystal (on PCB, not used — see RTC section)    |
| 34      | ADC: Current Sensor                                                     |
| 35      | ADC: Battery Voltage                                                    |
| 36 (VP) | ADC: Current Sensor VOC                                                 |
| 39 (VN) | ADC: Current Sensor FAULT                                               |

**TCA9555 (I2C at 0x21):**
- Port 0 (input): 2 physical buttons + 2 additional inputs + LEDs
- Port 1 (output): 3× H-bridge relay pairs (DRIVE, JACK, AUX)

- P00: SW1 (has external 4.7kOhm pullup)
- P01: SW2 (not populated on SC-B001-V5)
- P02-P04: N/C
- P05-P07: LEDs (through 100ohm resistors)
- P10: Sensor enable (1=ENABLE, 0=DISABLE)
- P11: KC3 (not connected)
- P12: KB3 (not connected)
- P13: KA3 (aux relay)
- P14: KB2 (jack B)
- P15: KA2 (jack A)
- P16: KB1 (drive B)
- P17: KA1 (drive A)

All power goes through a ACS37220LEZATR-100B3 sense chip (13.2 mV/A)

---

## Software Architecture

```
app_main()
├── i2c_init()              TCA9555 init (relays off, LEDs off)
├── rtc_xtal_init()         Button GPIO setup
├── boot_reset_reason       Check boot reason for factory reset
├── adc_init()              ADC1 calibration (12dB attenuation, line-fit)
├── storage_init()          Flash params
├── log_init()              Circular log buffer
├── adc_post()
├── storage_post()
├── solar_run_fsm()         (called in main loop too)
├── uart_init()             Serial JSON API task
├── sensors_init()          GPIO ISR setup for sensors/encoders
├── fsm_init()              Control FSM task (priority 10, 20ms tick)
├── rf_433_init()           433MHz RMT receiver task
├── bt_hid_init()           BLE HID host scanner task
└── webserver_init()        WiFi softAP + HTTP + WebSocket + mDNS + DNS

Main loop (50ms):
    soft-idle check
    button hold-to-reboot
    triple-tap detection
    alarm detection
    periodic send_bat_log
    i2c_poll_buttons()
    fsm_request() based on button events
    solar_run_fsm()
    drive_leds() status animation
    rtc_check_shutdown_timer() → soft idle after INACTIVITY_TIMEOUT_S (default 300s)
    esp_task_wdt_reset()
```

**FreeRTOS Tasks:**
| Task                      | Created by         | Priority      | Tick           | Purpose                                                              |
|---------------------------|--------------------|---------------|----------------|----------------------------------------------------------------------|
| `app_main` (main loop)    | system             | 1 (default)   | 50ms           | Button polling, LED animation, solar FSM, shutdown timer             |
| `control_task`            | `fsm_init()`       | 10            | 20ms           | FSM state machine, relay control, ADC current monitoring, e-fuse     |
| UART task                 | `uart_init()`      | default       | event-driven   | Serial JSON command processing                                       |
| RF 433 task               | `rf_433_init()`    | default       | event-driven   | RMT receive + keycode matching                                       |
| BT HID task               | `bt_hid_init()`    | default       | event-driven   | BLE HID host scanning + button mapping                               |
| httpd workers             | `webserver_init()` | default       | event-driven   | HTTP request handling (multiple workers spawned by esp_http_server)  |

---

## Key Files

| File                 | Purpose                                                              |
|----------------------|----------------------------------------------------------------------|
| `main.c`             | Entry point, 50ms main loop, factory reset, LED animation            |
| `control_fsm.c/h`    | State machine, relay control, current monitoring, calibration        |
| `power_mgmt.c/h`     | ADC reading, e-fuse thermal algorithm, battery voltage               |
| `sensors.c/h`        | GPIO ISR-based sensor debouncing, encoder counters                   |
| `i2c.c/h`            | TCA9555 relay/LED/button control                                     |
| `storage.c/h`        | NVM table + circular binary log buffer                      |
| `comms.c/h`          | Unified GET/POST JSON API (shared by HTTP and UART)                  |
| `webserver.c/h`      | WiFi softAP, HTTP server, embedded gzip webpage                      |
| `uart_comms.c/h`     | Serial JSON interface (115200 8N1)                                   |
| `rf_433.c/h`         | 433MHz OOK receiver, keycode learn/match                             |
| `bt_hid.c/h`         | BLE HID host, media remote button mapping                            |
| `rtc.c/h`            | Unix time, harvest alarms, soft idle, inactivity timer               |
| `solar.c/h`          | Simple FLOAT/BULK solar charge state machine                         |
| `sc_err.h`           | Error code definitions                                               |
| `log_test.c/h`       | Flash log unit tests                                                 |

---

## Control FSM States

```
STATE_IDLE
STATE_MOVE_START_DELAY      (1s)
STATE_MOVE_JACK_RETRACT     (retract jack to home first, so ride height is correct even
                             if the operator left it partway up; ends on home sensor /
                             jack e-fuse / timeout. SKIPPED when SENSOR_JACK already reads
                             home — then we go straight to JACK_UP_START.)
STATE_MOVE_JACK_SETTLE      (~1s all-off after a retract, so the jack-up is a clean
                             off→forward start, not a blocked REV→FWD reversal)
STATE_JACK_UP_START         (phase 1 / pre-jack: raise JACK_PRE_DIST in, or until the
                             jack up-current threshold JACK_I_UP / e-fuse → engaged)
STATE_JACK_UP               (phase 2: lift an additional JACK_DIST in, until timer/e-fuse)
STATE_DRIVE_START_DELAY     (1s)
STATE_DRIVE_FLUFF_START
STATE_DRIVE                 (encoder-based distance control)
STATE_DRIVE_END_DELAY       (1s)
STATE_JACK_DOWN             (reverse until e-fuse/sensor)
→ back to STATE_IDLE

STATE_UNDO_JACK_START       (emergency: reverse jack, run until e-fuse/sensor)
→ back to STATE_IDLE

STATE_CALIBRATE_JACK_DELAY  / STATE_CALIBRATE_JACK_MOVE   (jack calibration sequence)
STATE_CALIBRATE_DRIVE_DELAY / STATE_CALIBRATE_DRIVE_MOVE  (drive calibration sequence)
```

**Guards before START:**
- Remaining distance > 0 (travel-limit protection)
- Battery V ≥ `LOW_PROTECTION_V` (default 10V)
- Safety sensor active (debounced stable)
- All e-fuses not tripped

**FSM Loop (20ms tick in `control_task()`):**
1. `process_bridge_current()` — ADC → EMA → auto-zero → e-fuse
2. `process_battery_voltage()` — ADC → EMA
3. `sensors_check()` — drain ISR queue, update counters/debounce
4. State machine transitions (timer + sensor + efuse checks)
5. `drive_relays()` — write relay output from current state
6. `send_fsm_log()` — timestamped entry to flash

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
- mDNS hostname: resolves as `sc.local` (firmware sets the mDNS label to `sc`; the mDNS component appends `.local`. Apple/iOS resolve `.local` names over mDNS only, so the label must **not** itself contain `.local` — otherwise it advertises as `sc.local.local` and Apple lookups fail).
- Captive portal: a wildcard DNS server answers every query with 192.168.4.1, and the OS connectivity probes (Apple `/hotspot-detect.html`, Android `/generate_204`, Windows `/connecttest.txt`, …) are served a small landing page by `catchall_handler` instead of each OS's "success" token — so joining the AP pops the captive sign-in sheet. The landing page sends the user to the full control UI at `http://sc.local` / `http://192.168.4.1` in a **real browser**, because the captive webview can't reliably run the `/ws` status push or OTA upload. (`CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024` — Android's probe sends larger headers than the 512-byte default, which otherwise returns "Header fields are too long".)
- HTTP only (no HTTPS), port 80
- The softAP and HTTP server stay up during soft idle so a client can always associate and revive the device (see Power Management).

### HTTP API (port 80)
| Endpoint   | Method | Description                                                          |
|------------|--------|----------------------------------------------------------------------|
| `/`        | GET    | Embedded gzip HTML webpage                                           |
| `/get`     | GET    | JSON system status (polling fallback when the WebSocket is down)     |
| `/post`    | POST   | JSON commands + parameter updates                                    |
| `/ws`      | GET    | WebSocket: real-time control channel (see below)                    |
| `/log`     | ANY    | Binary log download (4B JSON len + JSON + 8B offsets + log data)     |
| `/ota`     | POST   | Firmware update upload                                               |

### WebSocket (`/ws`) — real-time channel
Requires `CONFIG_HTTPD_WS_SUPPORT=y` (set in `sdkconfig.defaults`). The web UI opens a WebSocket on load and uses it for:
- **client → server:** low-latency remote-control commands (`fwd`/`rev`/`extend`/`retract`/`aux`, their `_force` variants `fwd_force`/`rev_force`/`extend_force`/`retract_force`, and `stop_override`) as small JSON text frames, routed through `comms_handle_post()` so they share the POST command vocabulary. The web UI exposes these through a blue **Remote Control modal** (below START/E-STOP; opening it also fires `stop`) with two labelled jog groups — **JACK** (extend ▲/▼, retract ▼/▲) and **TIRES** (forward ↑, reverse ↓) — plus **FLUFF**. An **Override limits** checkbox (off on every open, confirm-gated with Cancel as the default) arms the `_force` command variants for all JACK/TIRES jogs: `extend_force`/`retract_force` bypass the jack height cap / home sensor, `fwd_force`/`rev_force` are the normal drive overrides. The e-fuse always applies and FLUFF is never forced.
- **server → client:** a 1 Hz status push (same JSON as `/get`), replacing the old 2 s HTTP poll. Built only when ≥1 client is connected (no heap churn when idle).

**Safety / deadman:** a held jog re-sends every 150 ms, each re-arming an override deadman. The web/WS path uses `WEB_PULSE_LENGTH` (default **1 s**, `pulse_override_web()`) — deliberately longer than the RF/BT remotes' `RF_PULSE_LENGTH` (350 ms, `pulse_override()`) — because a browser jog rides a **TCP** WebSocket that stalls on head-of-line blocking during Wi-Fi loss (worst right when motors run). A too-short deadman starved on those stalls and stopped the motor mid-hold (the "remote-control timeout"). Stopping stays fast via reliable paths: on release the client sends `stop_override` (in-order over TCP), and any WS socket close (tab closed, Wi-Fi dropped, crash) fires `stop_override()` via the httpd `close_fn`. The deadman only bounds continued motion on a **silent** link blackout (no FIN) — hence ~1 s worst case, tunable live via `WEB_PULSE_LENGTH`.

**Robustness:** a vanished client leaves a stale TCP socket (no FIN). The broadcast pre-checks writability with a zero-timeout `select()` and sets a 2 s `SO_SNDTIMEO` on WS sockets, so a dead client is reclaimed (`httpd_sess_trigger_close`) instead of blocking the shared httpd task — which previously wedged the server and broke reconnects. The client falls back to `/get` polling + `/post` if the WS won't connect.

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

| Partition   | Offset    | Size      | Purpose                                                        |
|-------------|-----------|-----------|----------------------------------------------------------------|
| nvs         | 0x9000    | 16K       | WiFi/BT config, board revision, RTC time backup                |
| otadata     | 0xD000    | 8K        | OTA boot selection                                             |
| phy_init    | 0xF000    | 4K        | RF calibration data                                            |
| ota_0       | 0x10000   | 1984K     | Factory / primary app slot                                     |
| ota_1       | 0x200000  | 1984K     | OTA update slot                                                |
| post_test   | 0x3F0000  | 4K        | Power-on self-test scratch sector                              |
| params      | 0x3F1000  | 32K       | CRC32-protected parameter storage (49 params)                  |
| log         | 0x400000  | 4096K     | Circular binary log buffer (head/tail tracked)                 |

**Log entry format (25 bytes typical):**
```
[0:8]   ts_ms (u64)
[8:12]  bat_V (f32)
[12:16] current_A (f32)   — combined, not per-bridge
[16:18] counter (i16)
[18:19] sensors (u8)
[19:23] heat (f32)        — max across bridges
[23:25] i2c_out (u16)
```

---

## RTC & Timekeeping

**Time source:** `esp_timer` (40 MHz APB crystal, ~20 ppm accuracy). The external 32.768 kHz crystal on GPIO32/33 is present on the PCB but **not used** — deep sleep is not used normally (soft idle instead), so RTC slow clock accuracy is irrelevant. The RTC slow clock uses the default internal RC oscillator.

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

**Factory reset** — two ways, both run `factory_reset()`: (1) power cycle with GPIO13 held for 10 seconds (only triggers on `ESP_RST_POWERON` or `ESP_RST_EXT`), or (2) the **FACTORY RESET** button in the DANGER ZONE (web UI → `cmd: "factory_reset"` → reset + reboot). Resets all params and erases log/post_test partitions. Preserves NVS (board_rev, BT pairing, RTC time).

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

| State             | Pattern                                      | Timing                                    |
|-------------------|----------------------------------------------|-------------------------------------------|
| Idle              | LED1 blink                                   | 0.5Hz (1s on / 1s off)                    |
| Error             | Rapid all-blink → error code hold            | 5Hz for 1s, then code for 2s (3s cycle)   |
| Moving / delays   | Waterfall 001→011→111→110→100→000            | ~1 cycle/s (167ms per step)               |
| Calibrating       | All LEDs flash                               | 1Hz (500ms on / 500ms off)                |
| Undo              | All LEDs solid on                            | Continuous                                |
| Booting           | LED1 solid                                   | Until init complete                       |

**Error code bits (during 2s hold phase):**

| LED Pattern (bottom→top) | Meaning                                                |
|--------------------------|--------------------------------------------------------|
| 001 — only bottom (P05) lit | Efuse tripped (any bridge) or low battery           |
| 010 — only middle (P06) lit | RTC/clock not set                                   |
| 100 — only top (P07) lit    | Safety sensor break or travel limit hit             |
| 111 — all three lit         | Unknown FSM error (fallback)                        |

Error codes are also shown on the web interface status field with individual flag names.

### Implementation Details

- Tap detection uses **release edge** (`i2c_get_button_released()`) with `btn_held < 1000ms` guard (long presses don't count as taps)
- 2-second tap window starts on first tap, fixed duration (not reset by subsequent taps)
- All button state sampled once per tick: `btn_pressed`, `btn_tripped`, `btn_released`, `btn_held`

---

## Power Management

- **Battery voltage:** GPIO35, thru divider → `V = raw × V_SENS_K + V_SENS_OFFSET` (defaults: K=0.00766̄, offset=0.4)
- **Solar charger:** GPIO26 (RTC hold) — FLOAT/BULK FSM, bulk for 20s when V < 5V for 5s
- **Inactivity shutdown:** after `INACTIVITY_TIMEOUT_S` (default 300s) → **soft idle** (BT off, LEDs off, sensor rail off — not deep sleep). **WiFi softAP + HTTP server stay up.** Any incoming request (page load, `/get`, WS connect, command) or a button press calls `rtc_reset_shutdown_timer()`, which wakes from soft idle — so an already-associated client can revive the device just by reconnecting, without re-associating.
- **RTC_DATA_ATTR:** Sync timestamps, alarm times, charge state — survive software resets (panics, WDT)

---

## Error Codes (`sc_err.h`)

```c
SC_ERR_EFUSE_TRIP_1  = 0x201  // Drive overcurrent/overheat
SC_ERR_EFUSE_TRIP_2  = 0x202  // Jack
SC_ERR_EFUSE_TRIP_3  = 0x203  // Aux
SC_ERR_SAFETY_TRIP   = 0x210  // Safety sensor break
SC_ERR_TRAVEL_LIMIT  = 0x211  // Travel limit reached (remaining distance exhausted)
SC_ERR_RTC_NOT_SET   = 0x220  // Clock not synchronized
SC_ERR_LOW_BATTERY   = 0x230  // Voltage below threshold
```

---

## Build System

- **Framework:** ESP-IDF (>=5.0)
- **Component deps** (`main/idf_component.yml`): `espressif/mdns ~1.9.1`
- **IDF requires:** `driver`, `esp_http_server`, `esp_netif`, `lwip`, `json`, `esp_timer`, `esp_adc`, `app_update`, `esp_wifi`, `nvs_flash`, `mdns`, `bt`, `esp_hid`
- **Webpage:** `webpage.html` → `webpage_compile.py` → `webpage_gzip.h` (embedded gzip binary). **Must re-run `webpage_compile.py` after any HTML edit before building.**
- **Version:** `main/version.txt` (a single semver line) is the only source of truth. `main/version.cmake` reads it and fills `version.h.in` → `FIRMWARE_VERSION`, `BUILD_DATE`, `FIRMWARE_STRING`; the top-level `CMakeLists.txt` reads the same file into `PROJECT_VER` so the version lands in `esp_app_desc_t.version` and is readable from an unflashed `.bin`. Git is not consulted. Bump by hand: MAJOR = breaking change to params / `./post` / UART protocol / HW requirement, MINOR = features, PATCH = fixes. Max 15 chars — `FIRMWARE_VERSION` is stored in the `BUILD_VERSION` param (`PARAM_STR_SIZE` 16) and compared across boots to fire the migration hook in `main.c`; the build fails if it doesn't fit.
- **Build output:** `build/SC-F001.bin` (IDF's own name, used by flash/partition tooling) plus a versioned copy `build/SC-F001-<version>.bin`, e.g. `SC-F001-1.1.0.bin`. Old versions accumulate in `build/`, so an earlier image can be re-deployed without a rebuild.
- **Bumping the version:** edit `main/version.txt` — that's the whole procedure. No tag, no commit needed; the next `idf.py build` picks it up (the file is a CMake configure dependency, so `PROJECT_VER` and the app descriptor refresh too).
- **Deploying:** `python ota_deploy.py` builds and uploads the version in `version.txt`. `--version 1.0.0` uploads that archived image instead (and skips the build, since building would produce a different version). `--list` shows what's available.
