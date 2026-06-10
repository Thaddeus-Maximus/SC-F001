# README.md Issues

Audit of `README.md` against the actual firmware code. The active board is **V5** (`#define BOARD_V5` in `board_config.h`).

---

## GPIO Map (lines 13–28) — Wrong for active board (V5)

The table documents the **V4 pinout**. On V5 the sensor and ADC GPIOs are completely different:

| README says | V4 pin | Actual V5 | Source |
|---|---|---|---|
| GPIO14 = Drive encoder | ISENS2 | **GPIO14 = JACK position sensor** | `sensors.c:26` |
| GPIO16 = Jack position sensor | ISENS3 | **GPIO16 = not used** (V5 uses GPIO14 for JACK) | `sensors.c:26` |
| GPIO19 = Aux sensor 2 (reserved) | BATTERY | **GPIO19 = DRIVE encoder** | `sensors.c:26` |
| GPIO23 = *missing* | — | **GPIO23 = AUX2** (unused, J3 unreliable) | `sensors.c:26` |
| GPIO34 = ADC: jack current sense | ISENS2 | **GPIO34 = V_ISENS_MAIN** (shared ACS) | `power_mgmt.c:49` |
| GPIO35 = ADC: aux current sense | ISENS3 | **GPIO35 = battery voltage** (ADC1_CH7) | `power_mgmt.c:51` |
| GPIO36/VP = ADC: drive current sense | ISENS1 | **GPIO36 = V_VOC** (OC threshold diag) | `power_mgmt.c:50` |
| GPIO39/VN = ADC: battery voltage | BATTERY | **GPIO39 = FAULT** (digital input, ACS37220) | `power_mgmt.c:52` |

The `sensors.h` enum comments (lines 19–22) are also stale — they list V4 pins but the V5 pins are assigned at runtime in `sensors.c:26`.

---

## TCA9555 description (line 32) — Wrong

> Port 1 (output): 3× H-bridge relay pairs (DRIVE, JACK, AUX) + LEDs

LEDs are on **Port 0** (P05–P07), not Port 1. See `i2c_set_led1()` at `i2c.c:98` writing to `TCA_REG_OUTPUT0`.

---

## ACS chip statement (line 47) — Unqualified

> All power goes through a ACS37220 sense chip (13.2 mV/A)

True for V5 (single shared ACS37220). On V4, JACK and AUX use **ACS37042 (44 mV/A)** — `power_mgmt.c:457-458`.

---

## Software Architecture diagram (lines 53–66) — Missing steps

The init sequence omits:
- Factory-reset detection loop (`main.c:174-227`)
- `rtc_restore_time()` after `storage_init()` (`main.c:232`)
- `adc_post()` after `log_init()` (`main.c:236`)
- `storage_post()` (`main.c:237`)
- OTA rollback counter check (`main.c:241-278`)
- `send_bat_log()` at boot (`main.c:282`)

---

## Main loop description (lines 68–73) — Major omissions

Missing from the described main loop:
- **Soft-idle polling path** (`main.c:340-363`) — handles wake-on-button, wake-on-alarm, solar FSM, and shutdown timer during sleep. This is ~25% of the main loop body.
- Button hold-to-reboot logic (`main.c:388-397`)
- Triple-tap detection (`main.c:411-431`)
- Alarm-triggered `FSM_CMD_START` (`main.c:496-499`)
- Periodic `send_bat_log()` every 120s in IDLE (`main.c:450-452`)
- `esp_task_wdt_reset()` at end of each tick (`main.c:503`)

---

## FreeRTOS Tasks table (lines 76–84) — Wrong priorities, missing task

| Task | README says | Actual priority | Source |
|---|---|---|---|
| UART task | "default" | **12** | `uart_comms.c:294` |
| RF 433 task | "default" | **5** | `rf_433.c:223` |
| BT HID task | "default" | **4** | `bt_hid.c:583` |

Missing task: **log_writer** (priority 5, created by `log_init()` at `storage.c:1069-1076`).

---

## Key Files — `hard_ui.c` doesn't exist

`hard_ui.c` listed at line 107 — no such file in `main/`. Either stale or was removed.

---

## FSM state diagram (lines 113–129) — Missing state

`STATE_DRIVE_FLUFF_START` is omitted. Actual sequence:

```
START_DELAY → JACK_UP_START → JACK_UP → DRIVE_START_DELAY → DRIVE_FLUFF_START → DRIVE → DRIVE_END_DELAY → JACK_DOWN
```

Cal states listed as `CAL_JACK_DELAY` but actual enum names are `STATE_CALIBRATE_JACK_DELAY`, `STATE_CALIBRATE_JACK_MOVE`, `STATE_CALIBRATE_DRIVE_DELAY`, `STATE_CALIBRATE_DRIVE_MOVE`.

---

## FSM Loop — wrong log size (lines 137–143)

> send_fsm_log() — 39-byte timestamped entry

`LOGSIZE = 25` at `control_fsm.c:179`. The actual 25-byte layout is:

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



---

## `/log` HTTP endpoint (line 184) — Wrong method

> `/log` — GET

Registered as `HTTP_ANY` (`webserver.c:870`). Handles both GET and POST (`webserver.c:150-153`).

---

## UART `POST:` example (line 189) — Wrong syntax

> POST: {json}

The UART handler (`uart_comms.c`) receives raw JSON strings, not HTTP-style method prefixes.

---

## Flash partitions (lines 212–220) — Wrong offsets and sizes

| Partition | README | Actual (`partitions.csv`) |
|---|---|---|
| post_test | offset 0x310000, 4K | offset **0x3F0000**, 4K |
| params | offset 0x311000, **16K** | offset **0x3F1000**, **32K** |
| log | offset 0x315000, **~4.9MB** | offset **0x400000**, **4096K** |

Missing from table: NVS (0x9000, 16K), otadata (0xD000), phy_init (0xF000), **ota_0 (0x10000, 1984K)**, **ota_1 (0x200000, 1984K)**.

---

## Log entry format (lines 222–234) — Describes old 39-byte format

The 39-byte layout with separate per-bridge currents and heats doesn't match `send_fsm_log()` which uses 25 bytes (combined current, single max heat, adds `i2c_out`).

---

## Parameter count — Off by one

> 48-param NVM table (`storage.c/h` description, line 97)

`storage.h:72-121` defines **49** parameters (check `NUM_PARAMS` from the enum).

---

## Key Parameters list (lines 236–242) — Missing 30+ params

Missing: `ADC_ALPHA_BATTERY`, `ADC_ALPHA_ISENS`, `ADC_ALPHA_IAZ`, `ADC_DB_IAZ`, `JACK_I_UP`, `JACK_I_DOWN`, `V_SENS_K`, `V_SENS_OFFSET`, `EFUSE_INRUSH_US`, `EFUSE_TAUCOOL`, `FLUFF_PREDRIVE_MS`, `CHG_LOW_V`, `CHG_LOW_S`, `CHG_BULK_S`, `RF_PULSE_LENGTH`, `LOW_PROTECTION_S`, `BUILD_VERSION`, `BOOT_TIME`, `JACK_IS_DOWN`, `WIFI_CHANNEL`, `NET_SSID`, `NET_PASS`.

---

## Deep sleep claim (line 248) — Incorrect

> deep sleep is disabled (soft idle instead)

`hibernate_enter()` at `rtc.c:184` calls `esp_deep_sleep_start()`. Deep sleep is used for hibernate mode (triggered by explicit web/UART command). Soft idle is the inactivity-triggered default, but deep sleep is not disabled.

---

## Factory reset GPIO reference (lines 270, 280, 354) — Wrong pin

> Hold GPIO13 button / power cycle with GPIO13 held

GPIO13 is the **NCA9535 INT line**, not a button. The actual button state is read via I2C (`i2c_button_held_raw(0)` at `main.c:176`). The user holds the physical button on the I2C expander, not GPIO13.

---

## Battery voltage GPIO (line 326) — Wrong for V5

> Battery voltage: GPIO39

On V5, battery voltage is on **GPIO35** (ADC1_CH7). GPIO39 is the **FAULT digital input** (`power_mgmt.c:51-52`). This correctly describes V4 only.

---

## "fluffer motor always running" (line 3) — Overstated

> auxiliary "fluffer" motor always running during operation

The fluffer (AUX) only runs during `STATE_DRIVE_FLUFF_START` and `STATE_DRIVE`. It does not run during jack-up, jack-down, or delay states (`control_fsm.c:716-776`).
