# Logtool Debug Notes — 16 MAR 2026

## Problem
`storage-16MAR2026-1728.bin` not parsing correctly.

## Root Cause (SOLVED)
Two issues in `parser.py`:

### 1. Unrecognized file format
The file format is `[4B tail BE][4B head BE][raw log data]` — no JSON header.
- `head - tail == file_size - 8` (the raw log data is exactly the flash region from tail to head)
- The old HTTP format was: `[4B json_len BE][json][4B tail BE][4B head BE][raw log data]`
- The parser's autodetect only recognized the old HTTP format (and required json_len < 8192)
- **Fix:** Added detection for bare tail+head format in `autodetect_and_parse()`

### 2. Type-first vs type-last detection failure
The log entries use **type-first** format: `[len][type][payload]`
But `_try_detect_type_first()` returned False because:
- First entry had type=0x00 at both positions (ambiguous)
- Timestamp was near-zero (RTC not yet set), so timestamp sanity check failed
- Function gave up after only 1 entry (`break` at end of loop)
- **Fix:** Loop over multiple entries (up to 200), added voltage sanity check (0.5-60V)

## Verification
- 54,831 entries parsed successfully
- FSM states: IDLE (51169), JACK_UP (1078), DRIVE (994), etc.
- Voltages: 3.3V–13.3V (reasonable for 3S LiPo system)
- Timestamps: Jan 13 – Feb 6, 2026 (after RTC set)
- Old HTTP-format .bin files still parse correctly
