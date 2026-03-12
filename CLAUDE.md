# SC-F001 Firmware — CLAUDE.md

See `README.md` for full project documentation (hardware, architecture, protocols, algorithms).

---

## Workflow

- **Minimize shell commands.** Every Bash call requires user approval. Prefer Read/Edit/Write/Glob/Grep tools. Only use Bash when a shell command is genuinely needed (e.g., `idf.py build`, git operations).
- **Webpage build step:** After editing `webpage.html`, run `webpage_compile.py` to regenerate `webpage_gzip.h` before building.
- **Don't touch git.**
---

## sdkconfig Management

**Two files, different roles:**
- `sdkconfig.defaults` — checked into git. Contains only intentional project overrides with comments explaining why. Applied by `idf.py reconfigure` on top of IDF defaults.
- `sdkconfig` — generated/modified by `idf.py menuconfig` or `reconfigure`. Contains every resolved setting. Also checked in for reproducibility, but treat `sdkconfig.defaults` as the source of truth for project-specific choices.

**Rules:**
- When changing a setting, add it to `sdkconfig.defaults` with a comment, then also apply it to `sdkconfig` so the next build picks it up without requiring `idf.py reconfigure`.
- Never hand-edit `sdkconfig` without also updating `sdkconfig.defaults` for the same setting — otherwise the change will be lost on the next `reconfigure`.
- Keep `sdkconfig.defaults` small and well-commented. Don't dump the full config into it.

**Current project-specific overrides (sdkconfig.defaults):**
| Setting | Value | Why |
|---------|-------|-----|
| `CONFIG_RTC_CLK_SRC_EXT_CRYS` | y | Use external 32kHz crystal for accurate RTC |
| `CONFIG_ESP32_RTC_EXT_CRYST_ADDIT_CURRENT_V2` | y | Drive high-ESR crystal during startup |
| `CONFIG_ESP_SYSTEM_RTC_EXT_XTAL_BOOTSTRAP_CYCLES` | 500 | Extra bootstrap for slow-starting crystal |
| `CONFIG_RTC_XTAL_CAL_RETRY` | 3 | More calibration attempts before RC fallback |
| `CONFIG_ESP_TASK_WDT_PANIC` | y | WDT timeout → panic → reboot (feeds OTA rollback counter) |

**Already correct at IDF defaults (verified, no override needed):**
| Setting | Value | Status |
|---------|-------|--------|
| `CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY` | y | Stack overflow detection via canary (method 2) |
| `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT` | y | Print backtrace then reboot on panic |
| `CONFIG_BROWNOUT_DET_LVL_SEL_0` | y | ~2.43V brownout on ESP32 3.3V rail (appropriate — battery low-V is handled by `LOW_PROTECTION_V` in FSM) |
| `CONFIG_PARTITION_TABLE_CUSTOM` | y | Custom partitions.csv with ota_0 + ota_1 |

---

## Conventions

- **Naming:** `snake_case` functions with module prefix (`fsm_init`, `i2c_poll_buttons`); `UPPER_SNAKE_CASE` constants/enums
- **Module pattern:** `.c` / `.h` pairs; headers expose only public API
- **Concurrency:** FSM commands via `xQueueSend`; log writes via async queue; GPIO ISR → minimal work → sensor queue
- **State machine pattern:** transitions in one `switch`, relay outputs in a second `switch` (separated)
- **Watchdog:** `esp_task_wdt_add/reset` in each task, 10s timeout
- **Logging:** `ESP_LOGI(TAG, ...)` per module; flash circular log for telemetry
- **No dynamic allocation** in ISR or high-priority paths

---

## Webpage (`main/webpage.html`)

Single-file SPA. Compiled to a gzip binary embedded in firmware. All JS is inline.

**Key globals:**
- `const ge = (id) => document.getElementById(id)` — shorthand used everywhere
- `let data = {}` — full `/get` JSON response, updated every poll cycle
- `let paramTableCreated = false` — tracks whether the DANGER ZONE param table has been built yet
- `let pollInterval` — handle for the 2-second `fetchStatus()` interval

**Endpoints used by JS (all relative):**
- `./get` — GET, returns full system status JSON; polled every 2 s by `fetchStatus()`
- `./post` — POST `application/json`, handles commands + parameter updates
- `./log` — GET/POST, binary log download
- `./ota` — POST, firmware upload

**POST body format** (`./post`):
```json
{ "cmd": "start", "parameters": { "KEY": value, ... }, "time": 1234567 }
```
All fields optional. `parameters` is a flat object of param key → value.

**Input / parameter binding convention:**
- Any `<input id="PARAM_<KEY>">` anywhere in the page is automatically updated by `updateParamTable()` on every poll (skipped if the input has class `changed` or is focused)
- `onchange="markChanged(this)"` — adds class `changed` (green), enables `commit_btn` / `cancel_btn`
- `commitParams()` (Save Changes button) collects all `.changed` inputs whose `id` starts with `PARAM_`, POSTs `{parameters: {...}}`, clears `changed` class
- `cancel_btn` calls `location.reload()`

**Sections (top to bottom):**
1. Status display (voltage, state, distance, etc.) — auto-updated from `data`
2. Schedule settings (`<details>`) — MOVE_START / MOVE_END / NUM_MOVES
3. Remote Control (`<details>`) — jog buttons + RF programming
4. **WiFi Settings** (`<details open>`) — NET_SSID, NET_PASS, WIFI_SSID, WIFI_PASS with dedicated `applyWifiSettings()` button
5. **DANGER ZONE** (`<details>`) — calibration, version, OTA upload, log download, auto-generated parameter table, REBOOT/SLEEP

**`updateParamTable()`:**
- On first call: builds a `<table id="table">` row per parameter, sorted alphabetically, skipping `WIFI_PARAM_KEYS = {NET_SSID, NET_PASS, WIFI_SSID, WIFI_PASS}` (those live in the dedicated WiFi section)
- On subsequent calls: updates existing input values (skips changed/focused inputs); if a new key appears, rebuilds

**Modal helpers** (all return Promises):
- `modalAlert(message, title?)` — OK only
- `modalConfirm(message, title?)` — OK / Cancel → resolves `true`/`false`
- `modalPrompt(message, title?, defaultValue?)` → resolves string or `null` on cancel

**Adding a new dedicated UI section:**
1. Add `<input id="PARAM_<KEY>" onchange="markChanged(this)"/>` in HTML
2. Add key to `WIFI_PARAM_KEYS` (or equivalent filter set) in `updateParamTable()` so it isn't duplicated in the raw table
3. Optionally add a dedicated apply function following `applyWifiSettings()` pattern
