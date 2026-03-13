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
7. - [ ] Parameter validation
   - [ ] Add per-param bounds to `PARAM_LIST` macro (min, max, flags)
   - [ ] NaN/Inf → reset to default; out-of-range → clamp to min/max
   - [ ] Enforce validation inside `commit_params()` (covers both `storage_init()` load and `/set` POST)
   - [ ] Audit for anywhere params are set without an immediate `commit_params()` call
   - [ ] Audit abandoned parameters (e.g. jack current) — add comments marking them deprecated
8. - [clauded] Factory reset: erases params + log + post_test partitions, requires 10s button hold on cold boot, LEDs flash during hold → solid when triggered
9. - [clauded] Ensure RTC_DATA_ATTR variables survive panics/WDT resets
   - [clauded] Verified `sync_unix_us`, `sync_rtc_us`, `rtc_set` — no init path zeroes them; `rtc_restore_time()` recovers via RTC HW counter
   - [clauded] Verified `remaining_distance`, `fsm_error` — `fsm_init()` does not touch them; only cleared by explicit user action
   - [clauded] Verified `log_head_offset`, `log_tail_offset` — `log_init()` always recovers from flash scan; RTC_DATA_ATTR is historical/harmless
10. - [clauded] Measure flash log write duration — `test_log_write_timing()` in log_test.c, runs 200 iterations of 39-byte writes, reports min/max/avg/sector-crossing times, compares to 5s WDT
11. - [ ] WiFi STA mode with event-group signaling
    - [ ] Try connecting to saved STA network first, fall back to softAP on failure/timeout
    - [ ] Add `EventGroupHandle_t` with `WIFI_READY_BIT` (set when STA connected or softAP up) and `BT_READY_BIT` (set when BT scan task starts)
    - [ ] Replace blind 500ms `vTaskDelay` on alarm wake with `xEventGroupWaitBits()` + timeout
    - [ ] Use same event group in `soft_idle_exit()` path
12. - [ ] Verify `sensors_init()` placement and ISR safety
    - [ ] Confirm `sensors_init()` is safe to call from `app_main()` (research says yes — creates queue + installs ISR service, no task-context dependency)
    - [ ] Decide: move to main.c (simpler) or keep in `control_task()` (current) — either way, remove the dead commented-out call in main.c and add a clarifying comment
    - [ ] Audit all ISRs are IRAM-safe: no `ESP_LOGx`, `printf`, `malloc`, or flash access — only `xQueueSendFromISR()`
    - [ ] Handle `sensors_init()` failure as critical (→ reboot)
13. - [clauded] External 32kHz crystal not needed (deep sleep disabled, soft idle instead) — removed crystal config from sdkconfig.defaults; `rtc_xtal_init()` already a no-op; crystal remains on PCB but unused
14. - [clauded] Removed `rtc_wakeup_cause()` — was unused (informational only, never called)
15. - [clauded] Confirmed `rtc_check_shutdown_timer()` uses unsigned `TickType_t` subtraction — wraps correctly; removed esp_timer overflow TODO comment from main.c
16. - [ ] Extract pure logic (e-fuse thermal model, param serialization, sensor debounce) into host-testable modules with Unity/CMock
17. - [ ] UART integration test framework: Python runner + ESP-side test commands
18. - [test] Logtool GUI output (matplotlib)
19. - [test] Verify naming convention adherence across codebase
20. - [test] Verify WiFi SSID rename triggers comms reboot
21. - [clauded] Documentation restructure
    - [clauded] Move project/hardware documentation from CLAUDE.md → README.md; keep CLAUDE.md for AI-specific instructions and conventions only
    - [clauded] Document all FreeRTOS tasks and priorities in README.md
    - [clauded] Add terse comments to FSM state transitions in `control_fsm.c` (focus on "why", not "what")
