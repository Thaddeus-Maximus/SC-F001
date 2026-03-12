# SC-F001 Firmware — TODO Tracker

Extracted from `// TODO` comments across the codebase plus legacy items. Excludes third-party managed components.

---

## Legacy Items (from previous TODO.md)

- [test] Seamless crashing — crashes need to not cause RTC to lose time; remaining_distance unaffected; equivalent of try-catch on whole program; should also make a log
- [test] Logtool GUI output (matplotlib)
- [test] Refactor; make sure everything adheres to naming conventions
- [test] Renaming wifi (should reboot the wifi/web comms to take effect)
- [ ] WiFi Network Connection — try STA first, fall back to softAP
- [ ] Hard Reset

---

## 1. Reliability & Crash Recovery

| File | Line | TODO |
|------|------|------|
| `main.c` | 192 | Make sure that this is "crash proof" |
| `main.c` | 193 | OTA rollback (triggered how? preferably with hardware... or if there are 5 resets in a row. also need way to nuke the storage partition or safe boot) |
| `main.c` | 194 | (maybe) recovery partition that allows uploading firmware |
| `storage.c` | 409 | WIPE ENTIRE PARTITION (factory reset only resets params, doesn't erase log) |

**Commentary:**
- OTA rollback is critical for field-deployed devices. ESP-IDF has built-in rollback support via `esp_ota_mark_app_valid_cancel_rollback()` — the app marks itself valid after a health check, otherwise the bootloader reverts on next reboot. This is the standard approach; a "5 resets in a row" counter can be stored in RTC_DATA_ATTR or an NVS counter.
- A recovery partition (minimal firmware with just WiFi + OTA upload) is a strong safety net. ESP-IDF's factory/OTA partition scheme supports this natively — the factory partition acts as the recovery image.
- Factory reset should absolutely erase the log partition. A `esp_partition_erase_range()` over the full log region is straightforward and should be added.
- "Crash proof" is vague — consider defining what this means concretely: e.g., no stuck relays after watchdog reset (already handled by TCA9555 power-on defaults), no corrupted params (CRC32 already protects), log head/tail consistency after power loss.

**Path Forwards:**
1. Implement a 5 reset in a row counter in RTC_DATA_ATTR that will rollback to the previous
2. Make the factory reset erase the storage partition. It should be triggered by the button being held on power on for at least 10 seconds, and should give LED indication (flash all LEDs off and on, then hold on once the reset is triggered)
3. crashproofing should mainly keep any RTC_DATA_ATTR variables from getting reset if anything panics - namely no losing time, no losing remaining position (other RTC_DATA_ATTR vars less important)

---

## 2. Error Handling & Logging

| File | Line | TODO |
|------|------|------|
| `main.c` | 174 | Do things with errors (put in real log? then reset. "assert with LOGE"?) |
| `main.c` | 228 | Seriously, log all the errors on bluetooth |
| `main.c` | 178 | Figure out how long logging takes (for reference, and comp to wdt) |

**Commentary:**
- Currently, init failures are logged but execution continues — this is the right pattern for a field device (degrade gracefully), but errors should be persisted. Use the existing `LOG_TYPE_ERROR` entry type to write a structured error log entry on each init failure, including the module ID and error code.
- BT errors are particularly important because BLE stacks fail silently in many edge cases (connection drops, service discovery timeouts, pairing failures). At minimum, log connection/disconnection events and HID report parse failures.
- Logging duration: flash writes on ESP32 typically take 5-20ms per sector erase + write. With a 10s WDT and 20ms FSM tick, this is fine, but verify empirically with `esp_timer_get_time()` bracketing. Consider using the async log queue (already in architecture) to keep flash writes off the FSM task.

**Path Forwards:**
1. wifi, webserver, rf, and bluetooth failures are acceptable gracefully, though they should try to start again if it still makes sense. All other failures are not acceptable and should cause a system reset.
2. Need to actually test and time the logging duration.

---

## 3. Safety & Robustness Audits

| File | Line | TODO |
|------|------|------|
| `main.c` | 113 | Check wdt stuff |
| `main.c` | 114 | Stack Overflow Detection |
| `main.c` | 273 | Make sure all ISRs are clean (very tight, no blocking functions) |
| `control_fsm.c` | 99 | Make sure this is threadsafe (fsm_request / xQueueSend) |

**Commentary:**
- **WDT:** The 10s timeout is already configured. Verify every task calls `esp_task_wdt_reset()` within its loop. Consider enabling `CONFIG_ESP_TASK_WDT_PANIC=y` in sdkconfig so a WDT timeout triggers a core dump rather than a silent reset.
- **Stack overflow:** Enable `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` (canary method) in sdkconfig. This catches overflows at context switch time. Also use `uxTaskGetStackHighWaterMark()` during development to right-size stacks.
- **ISR audit:** ESP-IDF provides `ESP_INTR_FLAG_IRAM` for ISR placement. Ensure no ISR calls `ESP_LOGx`, `printf`, `malloc`, or any flash-access function. The sensor ISR should only do `xQueueSendFromISR()`.
- **Thread safety of `fsm_request`:** `xQueueSend` is inherently thread-safe in FreeRTOS — it's designed for cross-task and ISR-to-task communication. This TODO can likely be closed. Just confirm `fsm_cmd_queue` is created before any caller can invoke `fsm_request()`.

**Path Forwards:**
1. Make the necessary changes to sdkconfig
2. Look at ISRs (I actually don't think we have any but double check)

---

## 4. Power Management & Sleep

| File | Line | TODO |
|------|------|------|
| `main.c` | 208 | Is this reasonable now that we eliminated deep sleep? (solar FSM call) |
| `main.c` | 210 | Do a 12V check and enter deep sleep if there's a problem |
| `main.c` | 365 | Will esp_timer overflow? Handle overflow if needed |

**Commentary:**
- **Solar FSM after eliminating deep sleep:** If deep sleep is no longer used, `solar_run_fsm()` still makes sense — it controls GPIO26 (bulk/float charge switching). Review whether the FLOAT→BULK transitions still trigger correctly without the deep sleep wake cycle.
- **12V check:** A critical low-voltage protection. If battery voltage is dangerously low (below the charger's cutoff), the ESP32 should enter deep sleep to let the panel charge without load. This prevents brown-out damage to flash. Threshold should be configurable via a parameter (it already has `LOW_PROTECTION_V`).
- **`esp_timer_get_time()` overflow:** Returns `int64_t` microseconds. It overflows after ~292,000 years — this is a non-issue. This TODO can be closed after confirming `rtc_check_shutdown_timer()` uses signed subtraction.

**Path Forwards:**
1. Keep solar FSM just in the main loop
2. Don't do any low-voltage checking
3. Confirm that rtc_check_shutdown_timer() uses signed subtraction.

---

## 5. Startup & Initialization Hygiene

| File | Line | TODO |
|------|------|------|
| `main.c` | 115 | Remove XTAL crystal stuff |
| `main.c` | 120 | `rtc_wakeup_cause()` shouldn't be needed anymore |
| `main.c` | 125 | How many tasks do we have? |
| `main.c` | 232 | `sensors_init()` — Why is this off? |
| `control_fsm.c` | 185 | Why is `sensors_init()` here rather than in main? |

**Commentary:**
- **XTAL removal:** The CLAUDE.md documents the crystal bootstrap workaround as essential for RTC accuracy. Don't remove `rtc_xtal_init()` unless you've confirmed the hardware no longer uses the external crystal — removing it would silently degrade RTC accuracy to ±5%.
- **`rtc_wakeup_cause()`:** If it's purely informational logging, it's harmless. Either delete it or keep it — it's one log line at boot.
- **Task count:** Document the task list: main loop (implicit), `control_task`, UART task, RF 433 task, BT HID task, HTTP server workers. Use `uxTaskGetNumberOfTasks()` at boot to log the count.
- **`sensors_init()` location:** It's called inside `control_task()` (line 185) and commented out in `main.c` (line 232). This is intentional — sensors are initialized in the FSM task context because the ISR handlers need to send to queues created in that scope. The comment in main.c should be removed and the one in control_fsm.c clarified.

**Path Forwards:**
1. Confirm that we can stop using the RTC and just use the internal crystal (never go into low power state)
2. Remove wakeup cause
3. Document a list of tasks and put in README.md
4. Double check that sensors_init() shouldn't be called in main. Make sure that the error code from it is handled appropriately (it MUST init properly; if it doesn't, reboot)

---

## 6. Soft Idle & Wake Behavior

| File | Line | TODO |
|------|------|------|
| `main.c` | 243 | Critique & confirm what we do in idle |
| `main.c` | 256 | Do a hard wait until wifi and bluetooth come up, not just blindly wait; might be better to be non-blocking |

**Commentary:**
- **Idle behavior:** The soft idle mode polls at 5s via direct GPIO (no I2C). This saves power but means TCA9555 button state is stale. Confirm that the only wake sources (GPIO13 button, RTC alarm) don't depend on I2C. The current design looks correct.
- **WiFi/BT wake wait:** The 500ms `vTaskDelay` is fragile — WiFi association can take 1-5s depending on the AP. Use event groups: `wifi_event_group` with a `CONNECTED_BIT`, wait with `xEventGroupWaitBits()` and a timeout. This is the standard ESP-IDF pattern. For BT, the scan is already async so it doesn't need blocking.

**Path Forwards:**
1. Do a hard wait until wifi and bluetooth come up rather than blindly. Needs to be non-blocking. Need bluetooth to finish initting, and wifi to have connected to a network or brought up the softap.

---

## 7. Testing & Verification

| File | Line | TODO |
|------|------|------|
| `main.c` | 214 | Test strategy!!! (software verification, and unit bringup) |
| `main.c` | 215 | A→D bringup; sanity check (sum up all inputs, wait 5ms, sum again, make sure there is a change (not frozen)) |

**Commentary:**
- **Test strategy:** For embedded firmware, consider three layers:
  1. **Host-side unit tests** — extract pure logic (e-fuse thermal model, param serialization, log format) into testable modules. Run with Unity or CMock on the host.
  2. **On-target POST** — a power-on self-test routine that validates ADC readings are in-range, I2C responds, flash is accessible. Run once at boot, log results.
  3. **Integration tests** — scripted UART commands that exercise the FSM cycle end-to-end. The existing `log_test.c` is a start.
- **ADC sanity check:** Good idea. Read all ADC channels twice with a short delay; if any channel returns identical values both times (especially the current sense channels, which should have noise), flag it. This catches stuck ADC mux, disconnected sense resistors, or frozen DMA.

**Path Forwards:**
1. Make pure logic for sensors, FSMs, e-fusing. This is an entire project.
2. Make a POST: that would include ADC, I2C, flash accessibility
3. Make a UART integration test, both a python runner, and the requesite ESP code

---

## 8. Storage & Parameters

| File | Line | TODO |
|------|------|------|
| `storage.h` | 7 | Sanity check that the EEPROM is working (sacrifice sector 0?) |
| `storage.h` | 57 | Bounds checking / constraints (especially no division by zero, no NaNs, no infs) |
| `storage.h` | 58 | Abandoned parameters (esp. jack current) |

**Commentary:**
- **Flash health check:** A simple write-read-verify test on a dedicated test sector at boot is a good idea. Alternatively, rely on the CRC32 check already protecting the param sector — if CRC fails, you know flash is degraded.
- **Parameter validation is the highest-priority item here.** Any f32 parameter used as a divisor (e.g., `DRIVE_KT`, `DRIVE_KE`, `EFUSE_INOM_*`) must be validated on load: reject NaN, Inf, and zero. Clamp to sane ranges. A `param_validate()` function called after `storage_init()` and after any `/set` POST would prevent field bricking from a bad parameter write.
- **Abandoned parameters:** Audit the PARAM_LIST. If `JACK_CURRENT` or similar params are no longer used by any code path, remove them to avoid confusion. Verify with grep first — sometimes params are only referenced from the web UI.

**Path Forwards:**
1. Utilize the last sector of the flash as a read-write-verify test
2. Add bounds in the PARAMS_LIST. It should accept a function or macro with arguments. Per-parameter we might want different min/max. and some parameters we will want to check and ensure it is NaN/inf. If Nan/Inf/etc set to default, otherwise clamp to min/max
3. Check in code - is there anywhere we don't immediately commit after setting a parameter? add bounds checking to the commit. This should do it
4. Audit abandoned parameters. Don't outright remove them just put a comment for now

---

## 9. Build & Configuration

| File | Line | TODO |
|------|------|------|
| `main.c` | 217 | Make sure sdkconfig is sane. Make notes. |
| `main.c` | 218 | Fix managed_components |

**Commentary:**
- **sdkconfig audit — key settings to verify:**
  - `CONFIG_ESP_TASK_WDT_PANIC=y` (WDT triggers panic + core dump, not silent reset)
  - `CONFIG_FREERTOS_CHECK_STACKOVERFLOW=2` (canary-based detection)
  - `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` (print backtrace then reboot)
  - `CONFIG_PARTITION_TABLE_CUSTOM=y` with correct factory/OTA layout
  - `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` (already set per CLAUDE.md)
  - Confirm brownout detector level matches your minimum operating voltage
- **managed_components:** ESP-IDF's component manager can cause build reproducibility issues. Pin exact versions in `idf_component.yml`. Consider vendoring critical components (mdns, littlefs, tca95x5) into the repo if version drift causes problems.

**Path Forwards:**
1. Research best practices to manage sdkconfig; document in CLAUDE.md
2. Fix managed_components and remove anything that is not being utilized and then make idf.py happy; document in CLAUDE.md
3. Apply best practices to manage sdkconfig

---

## 10. Documentation

| File | Line | TODO |
|------|------|------|
| `control_fsm.c` | 9 | Comment, and even better, produce a README for this. |

**Commentary:**
- The CLAUDE.md already serves as comprehensive FSM documentation. Adding inline comments to `control_fsm.c` describing each state transition's guard conditions and timing rationale would be more valuable than a separate README. Focus comments on the "why" — the state names and relay outputs are self-documenting for the "what."

**Path Forwards:**
1. Copy FSM documentation to README.md
2. Add comments (terse but helpful) to the FSM

---

## 11. OTHER

**Path Forwards:**
1. Fix docs; CLAUDE.md should be claude-specific instructions and such; README.md should contain everything else.