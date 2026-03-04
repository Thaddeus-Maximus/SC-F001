# Logtool Plan

Python tool for viewing SC-F001 flash logs. Supports CLI table output, matplotlib GUI, and both file and HTTP sources.

---

## On-disk Entry Format

Each entry in flash:
```
[len u8] [payload (len-1 bytes)] [type u8]
```
Total bytes per entry = `len + 1`.

`log_write_blocking` does `len++` before writing, so `len` stored on disk = `payload_size + 1`, and `len - 1 = payload_size`. The write is correct and writes all payload bytes.

Padding bytes (0x00 = sector padding, 0xFF = erased flash) cause the reader to skip to the next sector boundary.

### Entry Types

| Type  | Name     | Payload (bytes) | Stored len | Total on disk |
|-------|----------|-----------------|------------|---------------|
| 0–12  | FSM log  | 39              | 40         | 41            |
| 100   | BAT log  | 12              | 13         | 14            |
| 101   | CRASH    | 9               | 10         | 11            |

All values **little-endian** (named `be_*` in source but no byte-swap — the names are misleading legacy).

### FSM payload layout (39 bytes)
```
[0:8]   ts_ms      u64  — ms since epoch (local-as-UTC, no timezone)
[8:12]  bat_V      f32  — battery voltage
[12:16] drive_A    f32  — drive bridge raw current
[16:20] jack_A     f32  — jack bridge raw current
[20:24] aux_A      f32  — aux bridge raw current
[24:26] counter    i16  — drive encoder counter (SENSOR_DRIVE)
[26]    sensors    u8   — bits[3:0]=stable state, bits[7:4]=raw state
                          bit position: SAFETY=0, JACK=1, DRIVE=2, AUX2=3
[27:31] drive_heat f32  — e-fuse thermal accumulator, drive bridge
[31:35] jack_heat  f32  — e-fuse thermal accumulator, jack bridge
[35:39] aux_heat   f32  — e-fuse thermal accumulator, aux bridge
```

### BAT payload layout (12 bytes)
```
[0:8]  ts_ms  u64
[8:12] bat_V  f32
```

### CRASH payload layout (9 bytes)
```
[0:8] ts_ms        u64
[8]   reset_reason u8  — esp_reset_reason_t value
```

### FSM state names (from control_fsm.h — single source of truth)
```
0  = STATE_IDLE
1  = STATE_MOVE_START_DELAY
2  = STATE_JACK_UP_START
3  = STATE_JACK_UP
4  = STATE_DRIVE_START_DELAY
5  = STATE_DRIVE
6  = STATE_DRIVE_END_DELAY
7  = STATE_JACK_DOWN
8  = STATE_UNDO_JACK_START
9  = STATE_CALIBRATE_JACK_DELAY
10 = STATE_CALIBRATE_JACK_MOVE
11 = STATE_CALIBRATE_DRIVE_DELAY
12 = STATE_CALIBRATE_DRIVE_MOVE
```

---

## HTTP /log Response Format

```
[4B json_len BE] [json_len bytes JSON] [4B tail_offset BE] [4B head_offset BE] [raw binary]
```
The raw binary is already linearized (tail→head wraparound handled server-side). Parse it sequentially as a stream of entries.

**Streaming**: GET once to get the full log + current head, then POST with the current head as the tail to get only new entries. Repeat on a timer.

---

## File Format Auto-detection

When opening a `.bin` file:
1. Read first 4 bytes as BE uint32 → `candidate_json_len`
2. If `candidate_json_len < 8192` AND byte at offset 4 is `{` → HTTP response format (parse header then binary)
3. Otherwise → raw flash binary (skip header, parse binary directly from offset 0)

---

## Timestamps

All timestamps are `ms since epoch` in **local-as-UTC** encoding: the device stores the user's local time directly as a Unix timestamp (no timezone offset). Display as-is using `datetime.utcfromtimestamp(ts_ms / 1000)` — no conversion needed.

---

## Implementation Plan

### File layout
```
logtool/
├── PLAN.md         ← this file
├── logtool.py      ← main entry point (CLI + dispatch)
├── parser.py       ← binary framing + struct unpack; reads state names from control_fsm.h
├── source.py       ← file reader and HTTP fetcher (GET full, POST incremental)
├── cli_view.py     ← tabular terminal output (tabulate)
└── gui_view.py     ← matplotlib time-series charts
```

### `parser.py`
- `load_fsm_states(fsm_h_path) -> dict[int, str]` — parses `control_fsm.h` to extract the `fsm_state_t` enum. Falls back to hardcoded dict if file not found.
- `parse_entries(data: bytes) -> list[dict]` — iterates raw binary, skips padding (0x00/0xFF), unpacks each entry by type
- `parse_response(blob: bytes) -> (dict, int, int, list[dict])` — parses full HTTP/file blob: json metadata, tail, head, entries

### `source.py`
- `read_file(path: str) -> bytes`
- `fetch_full(url: str) -> bytes` — HTTP GET /log
- `fetch_incremental(url: str, tail: int) -> (bytes, int)` — POST /log with tail as body, returns raw binary + new head
- `stream(url: str, callback, interval_s=2.0)` — poll loop: GET once for full log, then POST repeatedly; calls `callback(new_entries, new_head)` each iteration

### `cli_view.py`
- `print_table(entries: list[dict], fsm_states: dict)` — tabulate output
  - Columns: `time`, `type`, `state`, `bat_V`, `drive_A`, `jack_A`, `aux_A`, `counter`, `sensors_stable`, `sensors_raw`
  - BAT/CRASH rows show only relevant columns; FSM-only columns shown as `—`
  - CRASH rows prefixed with `***`
- `print_summary(entries)` — time range, count by type, min/max battery

### `gui_view.py`
- `show_plots(entries: list[dict])` — matplotlib figure with subplots:
  1. Battery voltage over time
  2. Currents (drive, jack, aux) over time
  3. FSM state over time (step/stairs plot)
  4. Thermal accumulators (drive_heat, jack_heat, aux_heat)
  - CRASH events: vertical red dashed lines across all subplots
- `live_plot(url: str, interval_s=2.0)` — `FuncAnimation` + streaming source, updates all axes

### `logtool.py` (CLI)
```
Usage:
  logtool.py <file.bin>                     # file → CLI table
  logtool.py <file.bin> --gui               # file → GUI
  logtool.py http://<ip>/log                # fetch once → CLI table
  logtool.py http://<ip>/log --gui          # fetch once → GUI
  logtool.py http://<ip>/log --stream       # live CLI (append rows)
  logtool.py http://<ip>/log --stream --gui # live GUI
  logtool.py <source> --type fsm|bat|crash  # filter by entry type
  logtool.py <source> --tail <offset>       # start from flash offset
  logtool.py <source> --fw ../main          # path to firmware source (for state names)
```

### Dependencies (`requirements.txt`)
```
requests
matplotlib
tabulate
```
Python 3.8+.

---

## Resolved Questions

1. **Entry size**: No bug. `len++` in `log_write_blocking` makes `len-1 = LOGSIZE = 39`, writing all payload bytes correctly.
2. **Streaming**: GET for initial full fetch, then POST with current head as tail for incremental polling.
3. **File format**: Auto-detect via 4-byte json_len + `{` check.
4. **Timestamps**: local-as-UTC, display with `datetime.utcfromtimestamp()`, no conversion.
5. **FSM state names**: Parse from `control_fsm.h` at startup (`--fw` flag or default `../main`). Fallback to hardcoded dict.
6. **tabulate**: Acceptable dependency.
