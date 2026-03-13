# SC-F001 Firmware — TODO

1. - [clauded] sdkconfig audit
   - [clauded] Enable `CONFIG_ESP_TASK_WDT_PANIC=y` — added to sdkconfig.defaults and sdkconfig
   - [clauded] Verify `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` — confirmed canary method active
   - [clauded] Verify `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` — confirmed active
   - [clauded] Confirm brownout detector level — ~2.43V is correct (ESP32 rail protection; battery low-V handled by FSM's `LOW_PROTECTION_V`)
   - [clauded] Research sdkconfig management best practices — documented in CLAUDE.md "sdkconfig Management" section
2. - [clauded] Fix managed_components: removed unused `littlefs` and `tca95x5` deps, pinned `mdns` to `~1.9.1`, bumped IDF min to `>=5.0`; documented in CLAUDE.md
3. - [ ] OTA rollback via consecutive-reset counter
   - [ ] Add `RTC_DATA_ATTR uint8_t reset_counter` — increment on boot, clear after successful health check
   - [ ] On counter ≥ 5, call `esp_ota_mark_app_invalid_rollback_and_reboot()`
   - [ ] After POST passes and FSM starts, call `esp_ota_mark_app_valid_cancel_rollback()`
   - [ ] Decide what "health check passes" means (POST passes? 30s uptime? first successful FSM cycle?)
4. - [clauded] Critical init failures (ADC, storage, log, I2C, FSM, UART) → `init_critical()` retries 3×, then `esp_restart()`
5. - [clauded] Non-critical init failures (RF, BT, webserver) → log error, continue booting
   - [ ] WiFi/BT already have restart paths (`webserver_restart_wifi()`, `bt_hid_resume()`) — wire these into a retry-on-failure path at boot, not just soft idle exit
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
8. - [ ] Factory reset: erase entire storage partition (not just params), require 10s button hold, LED indication (flash all → hold solid once triggered)
9. - [ ] Ensure RTC_DATA_ATTR variables survive panics/WDT resets
   - [ ] Verify `sync_unix_us`, `sync_rtc_us`, `rtc_set` (time) are not corrupted by any init path
   - [ ] Verify `remaining_distance`, `fsm_error` (FSM state) are not zeroed except by intentional reset
   - [ ] Verify `log_head_offset`, `log_tail_offset` stay consistent after crash (no partial writes)
10. - [ ] Measure flash log write duration (bracket with `esp_timer_get_time()`, compare to WDT timeout of 5s)
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
13. - [ ] Confirm whether external RTC crystal can be dropped (device never enters deep sleep now) — if yes, remove `rtc_xtal_init()` and related sdkconfig entries; if no, document why it must stay
14. - [ ] Remove `rtc_wakeup_cause()` call (informational only, no longer needed)
15. - [ ] Confirm `rtc_check_shutdown_timer()` uses signed subtraction — then remove the esp_timer overflow TODO comment (int64_t overflows after 292K years)
16. - [ ] Extract pure logic (e-fuse thermal model, param serialization, sensor debounce) into host-testable modules with Unity/CMock
17. - [ ] UART integration test framework: Python runner + ESP-side test commands
18. - [test] Logtool GUI output (matplotlib)
19. - [test] Verify naming convention adherence across codebase
20. - [test] Verify WiFi SSID rename triggers comms reboot
21. - [clauded] Documentation restructure
    - [clauded] Move project/hardware documentation from CLAUDE.md → README.md; keep CLAUDE.md for AI-specific instructions and conventions only
    - [clauded] Document all FreeRTOS tasks and priorities in README.md
    - [clauded] Add terse comments to FSM state transitions in `control_fsm.c` (focus on "why", not "what")
