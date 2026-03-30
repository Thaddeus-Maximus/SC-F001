# SC-F001 Firmware — TODO

1. - [clauded] sdkconfig audit
   - [clauded] Enable `CONFIG_ESP_TASK_WDT_PANIC=y` — added to sdkconfig.defaults and sdkconfig
   - [clauded] Verify `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` — confirmed canary method active
   - [clauded] Verify `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` — confirmed active
   - [clauded] Confirm brownout detector level — ~2.43V is correct (ESP32 rail protection; battery low-V handled by FSM's `LOW_PROTECTION_V`)
   - [clauded] Research sdkconfig management best practices — documented in CLAUDE.md "sdkconfig Management" section
2. - [clauded] Fix managed_components: removed unused `littlefs` and `tca95x5` deps, pinned `mdns` to `~1.9.1`, bumped IDF min to `>=5.0`; documented in CLAUDE.md
3. - [clauded] OTA rollback via consecutive-reset counter
   - [clauded] Add `RTC_DATA_ATTR uint8_t ota_reset_counter` — incremented on panic/WDT resets, cleared on power-on/ext reset
   - [clauded] On counter ≥ 5, call `esp_ota_mark_app_invalid_rollback_and_reboot()`
   - [clauded] After POST passes and FSM starts, call `esp_ota_mark_app_valid_cancel_rollback()` and clear counter
   - [clauded] Health check = POST passes + all critical inits + FSM task started + non-critical inits attempted
4. - [clauded] Critical init failures (ADC, storage, log, I2C, FSM, UART) → `init_critical()` retries 3×, then `esp_restart()`
5. - [clauded] Non-critical init failures (RF, BT, webserver) → log error, continue booting
   - [clauded] WiFi/BT/RF retry once on init failure at boot (200ms delay for RF/BT, 500ms for WiFi), then log and continue
6. - [clauded] Power-on self-test (POST) — `init_critical()` wrapper + dedicated POST checks after init
   - [clauded] ADC: `adc_post()` reads all 4 channels twice with 5ms delay, warns if frozen
   - [clauded] I2C: `i2c_post()` verifies TCA9555 responds (read port 0)
   - [clauded] Flash: `storage_post()` write-read-verify on last sector of storage partition
7. - [clauded] Parameter validation
   - [clauded] Add per-param bounds to `PARAM_LIST` macro (min, max) — extended PARAM_DEF 6-arg macro
   - [clauded] NaN/Inf → reset to default; out-of-range → clamp to min/max — `validate_param()` in storage.c
   - [clauded] Enforce validation in `storage_init()` (after flash load) and `commit_params()` (before flash write)
   - [clauded] Audit `set_param_value_t` calls outside comms.c — deleted dead code: `rf_433_set_keycode()`, `FSM_CMD_CALIBRATE_*_FINISH` handlers + FSM cases + `fsm_set_cal_val()` (web JS does cal math client-side, commits via standard param POST)
   - [clauded] Audit abandoned parameters — `JACK_IS_DOWN` marked deprecated (may duplicate `JACK_I_DOWN`); `BOOT_TIME` is informational-only
8. - [clauded] Factory reset: erases params + log + post_test partitions, requires 10s button hold on cold boot, LEDs flash during hold → solid when triggered
9. - [clauded] Ensure RTC_DATA_ATTR variables survive panics/WDT resets
   - [clauded] Verified `sync_unix_us`, `sync_rtc_us`, `rtc_set` — no init path zeroes them; `rtc_restore_time()` recovers via RTC HW counter
   - [clauded] Verified `remaining_distance`, `fsm_error` — `fsm_init()` does not touch them; only cleared by explicit user action
   - [clauded] Verified `log_head_offset`, `log_tail_offset` — `log_init()` always recovers from flash scan; RTC_DATA_ATTR is historical/harmless
10. - [clauded] Measure flash log write duration — `test_log_write_timing()` in log_test.c, runs 200 iterations of 39-byte writes, reports min/max/avg/sector-crossing times, compares to 5s WDT
11. - [clauded] WiFi STA mode with event-group signaling
    - [clauded] STA-first with softAP fallback was already implemented in `start_wifi()`
    - [clauded] Added `EventGroupHandle_t comms_event_group` in `comms_events.h` with `WIFI_READY_BIT` / `BT_READY_BIT`
    - [clauded] Replaced blind 500ms `vTaskDelay` on alarm wake with `xEventGroupWaitBits(COMMS_ALL_BITS, 5s timeout)`
    - [clauded] `soft_idle_exit()` → `webserver_restart_wifi()` / `bt_hid_resume()` set bits; `webserver_stop()` / `bt_hid_stop()` clear bits
    - [clauded] Bits set even on permanent init failure so alarm-wake never blocks forever
12. - [clauded] Verify `sensors_init()` placement and ISR safety
    - [clauded] Moved `sensors_init()` to main.c as `init_critical("SENSORS", sensors_init)` — runs before FSM
    - [clauded] Removed dead commented-out `sensors_init()` / `sensors_stop()` from sensors.c
    - [clauded] Audited ISR: `sensor_isr_handler` is IRAM_ATTR, uses only `esp_timer_get_time()` (IRAM-safe), `gpio_get_level()`, `xQueueSendFromISR()` — no logging/malloc/flash
    - [clauded] `sensors_init()` failure is now critical (→ reboot via `init_critical`)
13. - [clauded] External 32kHz crystal not needed (deep sleep disabled, soft idle instead) — removed crystal config from sdkconfig.defaults; `rtc_xtal_init()` already a no-op; crystal remains on PCB but unused
14. - [clauded] Removed `rtc_wakeup_cause()` — was unused (informational only, never called)
15. - [clauded] Confirmed `rtc_check_shutdown_timer()` uses unsigned `TickType_t` subtraction — wraps correctly; removed esp_timer overflow TODO comment from main.c
16. - [test] Logtool GUI output (matplotlib)
17. - [test] Verify naming convention adherence across codebase
18. - [test] Verify WiFi SSID rename triggers comms reboot
19. - [clauded] Documentation restructure
    - [clauded] Move project/hardware documentation from CLAUDE.md → README.md; keep CLAUDE.md for AI-specific instructions and conventions only
    - [clauded] Document all FreeRTOS tasks and priorities in README.md
    - [clauded] Add terse comments to FSM state transitions in `control_fsm.c` (focus on "why", not "what")


20. - [clauded] Fix compile warnings — unused vars (uart_comms.c, rf_433.c), const-correctness (log_write signatures), fallthrough annotation (control_fsm.c)
21. - [clauded] NVS is required: WiFi blob stores RF cal data (CONFIG_ESP_WIFI_NVS_ENABLED), Bluedroid stores bonding/GATT cache unconditionally, bt_hid.c stores last-connected BDA. Cannot remove nvs_flash_init().
22. - [clauded] NVS vs custom params: NVS serves WiFi/BT internals + BDA storage; custom flash partition serves app params with CRC32 protection. Different purposes, no consolidation needed.
23. - [clauded] BUG FIX: `FSM_CMD_START` fallthrough was overwriting `this_move_dist = MIN(...)` with unconditional `DRIVE_DIST` — replaced fallthrough with goto to shared start logic so leash limit is preserved

24. - [clauded] General bug scan (FSM, power, sensors, storage, comms, RTC, peripherals)
    - Ran 4 parallel deep-dive reviews across entire codebase. Findings below.
    - False positives eliminated: override fallthrough (breaks present), soft idle during motor ops (FSM resets timer), JACK_DOWN_TIME uninitialized first move (jack_finish_us always set before use)

## Suspected Bugs (from item 24 scan)

28. - [ ] **BUG [CRITICAL]:** `get_is_safe()` hardcoded `return true` — safety sensor completely bypassed
    - sensors.c:182 — `return true;` with `//return is_safe;` commented out below
    - All FSM safety checks (STATE_JACK_UP_START, JACK_UP, DRIVE_START_DELAY, DRIVE, DRIVE_END_DELAY, calibration states) are no-ops
    - Safety break will NOT trigger STATE_UNDO_JACK_START — machine runs through hazard conditions
    - Debounce logic in sensors_check() still runs but output is discarded

29. - [ ] **BUG [CRITICAL]:** E-fuse INOM params allow min=0.0 → division by zero
    - power_mgmt.c:380 — `float I_norm = fabsf(channel->current / I_nominal);`
    - storage.h EFUSE_INOM_1/2/3 bounds: min=0.0, max=200.0
    - If param=0 → I_norm=Inf → instant trip on any current (motor won't run)
    - If param=NaN (flash corruption) → I_norm=NaN → all comparisons false → e-fuse NEVER trips (motor can burn)
    - Fix: raise min bound to 0.1 or add explicit NaN/zero guard before division

30. - [ ] **BUG [HIGH]:** No timeout on STATE_UNDO_JACK_START
    - control_fsm.c:486-493 — waits for `!efuse_get(BRIDGE_JACK)` with no max wait
    - If jack efuse never cools (hardware fault, thermal runaway), FSM stuck indefinitely
    - User CAN send FSM_CMD_STOP to escape, but no automatic recovery
    - Fix: add timeout (e.g. 30-60s) before forcing transition to IDLE with error

31. - [ ] **BUG [HIGH]:** No e-fuse checks in calibration movement states
    - control_fsm.c:495-512 — STATE_CALIBRATE_JACK_MOVE and STATE_CALIBRATE_DRIVE_MOVE
    - Only check get_is_safe() and timer_done(), NOT efuse_get()
    - Relay outputs (lines 625-640) drive motors regardless of efuse status
    - Jack cal runs up to 3s, drive cal up to 6s without overcurrent protection
    - Fix: add efuse_get() check and abort calibration on trip

32. - [ ] **BUG [HIGH]:** BLE HID scan task missing watchdog registration
    - bt_hid.c — `bt_hid_scan_task()` never calls `esp_task_wdt_add(NULL)`
    - Task blocks on `xSemaphoreTake(s_scan_sem, portMAX_DELAY)` — if GAP callback never signals, hangs forever
    - Unlike rf_433 task (which registers WDT), BT task has no WDT coverage
    - Fix: add `esp_task_wdt_add(NULL)` and periodic `esp_task_wdt_reset()` (or use timeout on semaphore)

33. - [ ] **BUG [HIGH]:** ISR sensor queue full → events silently dropped
    - sensors.c:57 — queue size 16, `xQueueSendFromISR()` return value not checked
    - If sensors_check() consumer falls behind (4 sensors firing edges), events lost
    - Encoder counts become inaccurate → drive distance wrong
    - Fix: check return value, optionally increment a dropped-event counter for diagnostics

34. - [ ] **BUG [HIGH]:** Params not validated on set, only on commit — FSM reads unvalidated values
    - storage.c:268-273 — `set_param_value_t()` writes directly to `parameter_table[]` with no bounds check
    - `validate_param()` only called in `commit_params()` (before flash write)
    - Between POST and commit, FSM can read out-of-range values (e.g. DRIVE_DIST=999999)
    - Fix: call `validate_param()` inside `set_param_value_t()`, or at least in comms.c after setting

35. - [ ] **BUG [MEDIUM]:** Solar FSM timer uninitialized
    - solar.c:17 — `RTC_DATA_ATTR int64_t timer;` has no initializer
    - RTC memory may contain garbage on first cold boot before `solar_reset_fsm()` sets it to -1
    - `solar_run_fsm()` is called (main.c:253) before `solar_reset_fsm()` has run on first boot path
    - Fix: initialize to -1 in declaration: `RTC_DATA_ATTR int64_t timer = -1;`

36. - [ ] **BUG [MEDIUM]:** E-fuse param bounds too loose
    - EFUSE_HEAT_THRESH min=0.0 — allows instant trip on any current draw (storage.h)
    - EFUSE_INRUSH_US max=10000000 (10s) — allows 10s of unlimited current with no e-fuse protection
    - Fix: tighten bounds (e.g. HEAT_THRESH min=1.0, INRUSH_US max=2000000)

37. - [ ] **BUG [MEDIUM]:** No mutex on parameter_table[] — concurrent access from HTTP/UART/FSM tasks
    - storage.c — `parameter_table[]` read/written by HTTP POST handlers, UART handlers, and FSM task
    - 32-bit aligned reads/writes are atomic on ESP32, so u16/u32/i16/i32/f32 are safe
    - f64 (8 bytes) and str16 (16 bytes) could be torn reads — but no f64 or str params are read by FSM in hot path
    - Severity is low in practice but architecturally unsound

25. - [ ] Extract pure logic (e-fuse thermal model, param serialization, sensor debounce) into host-testable modules with Unity/CMock?
26. - [ ] UART integration test framework: Python runner + ESP-side test commands
27. - [ ] Bug: WiFi won't want to connect to STA except at first boot