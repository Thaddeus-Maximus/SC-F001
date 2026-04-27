"""
Binary log parser for SC-F001 flash logs.

On-disk entry format: [len u8][payload (len-1 bytes)][type u8] = len+1 total bytes
The firmware does len++ before writing, so stored len = payload_size + 1.

All values are little-endian.
"""

import struct
import json
import re
from pathlib import Path
from datetime import datetime

LOG_TYPE_BAT      = 100
LOG_TYPE_CRASH    = 101
LOG_TYPE_BOOT     = 102
LOG_TYPE_TIME_SET = 103

# Fallback FSM state map (matches control_fsm.h)
_FALLBACK_FSM_STATES = {
    0:  "IDLE",
    1:  "MOVE_START_DELAY",
    2:  "JACK_UP_START",
    3:  "JACK_UP",
    4:  "DRIVE_START_DELAY",
    5:  "DRIVE",
    6:  "DRIVE_END_DELAY",
    7:  "JACK_DOWN",
    8:  "UNDO_JACK_START",
    9:  "CALIBRATE_JACK_DELAY",
    10: "CALIBRATE_JACK_MOVE",
    11: "CALIBRATE_DRIVE_DELAY",
    12: "CALIBRATE_DRIVE_MOVE",
    13: "DRIVE_FLUFF_START",
}

ESP_RESET_REASONS = {
    0:  "UNKNOWN",
    1:  "POWERON",
    2:  "EXT",
    3:  "SW",
    4:  "PANIC",
    5:  "INT_WDT",
    6:  "TASK_WDT",
    7:  "WDT",
    8:  "DEEPSLEEP",
    9:  "BROWNOUT",
    10: "SDIO",
}


def load_fsm_states(fw_path=None) -> dict:
    """
    Parse FSM state names from control_fsm.h.
    Returns dict mapping int -> name string (e.g. {0: 'IDLE', ...}).
    Falls back to hardcoded dict if the file can't be found or parsed.
    """
    if fw_path is None:
        # Default: sibling directory ../main relative to this file
        fw_path = Path(__file__).parent.parent / "main"

    header = Path(fw_path) / "control_fsm.h"
    if not header.exists():
        return dict(_FALLBACK_FSM_STATES)

    try:
        text = header.read_text()
        # Find the fsm_state_t enum block
        m = re.search(r'typedef\s+enum\s*\{([^}]+)\}\s*fsm_state_t\s*;', text, re.DOTALL)
        if not m:
            return dict(_FALLBACK_FSM_STATES)

        states = {}
        value = 0
        for line in m.group(1).splitlines():
            line = line.strip().rstrip(',')
            if not line or line.startswith('//'):
                continue
            if '=' in line:
                name, val = line.split('=', 1)
                name = name.strip()
                val = val.strip().split('//')[0].strip()
                try:
                    value = int(val, 0)
                except ValueError:
                    pass
            else:
                name = line.split('//')[0].strip()
            if name:
                # Strip STATE_ prefix for display brevity
                display = name.removeprefix('STATE_') if hasattr(str, 'removeprefix') else (
                    name[6:] if name.startswith('STATE_') else name)
                states[value] = display
                value += 1
        return states if states else dict(_FALLBACK_FSM_STATES)
    except Exception:
        return dict(_FALLBACK_FSM_STATES)


def _ts_to_str(ts_ms: int) -> str:
    """Convert ms-since-epoch (local-as-UTC) to display string."""
    try:
        dt = datetime.utcfromtimestamp(ts_ms / 1000.0)
        return dt.strftime("%Y-%m-%d %H:%M:%S.") + f"{ts_ms % 1000:03d}"
    except (OSError, ValueError):
        return str(ts_ms)


def _unpack_fsm(payload: bytes, fsm_states: dict) -> dict:
    if len(payload) < 27:
        raise ValueError(f"FSM payload too short: {len(payload)} < 27")
    ts_ms, bat_V, drive_A, jack_A, aux_A, counter, sensors = \
        struct.unpack_from('<QffffhB', payload, 0)
    drive_heat = jack_heat = aux_heat = 0.0
    if len(payload) >= 31:
        drive_heat, = struct.unpack_from('<f', payload, 27)
    if len(payload) >= 39:
        jack_heat, aux_heat = struct.unpack_from('<ff', payload, 31)
    return {
        'ts_ms':       ts_ms,
        'time_str':    _ts_to_str(ts_ms),
        'bat_V':       round(bat_V, 3),
        'drive_A':     round(drive_A, 3),
        'jack_A':      round(jack_A, 3),
        'aux_A':       round(aux_A, 3),
        'counter':     counter,
        'sensors_stable': sensors & 0x0F,
        'sensors_raw':    (sensors >> 4) & 0x0F,
        'drive_heat':  round(drive_heat, 2),
        'jack_heat':   round(jack_heat, 2),
        'aux_heat':    round(aux_heat, 2),
    }


def _unpack_bat(payload: bytes) -> dict:
    if len(payload) < 12:
        raise ValueError(f"BAT payload too short: {len(payload)} < 12")
    ts_ms, bat_V = struct.unpack_from('<Qf', payload, 0)
    return {
        'ts_ms':    ts_ms,
        'time_str': _ts_to_str(ts_ms),
        'bat_V':    round(bat_V, 3),
    }


def _unpack_crash(payload: bytes) -> dict:
    if len(payload) < 9:
        raise ValueError(f"CRASH payload too short: {len(payload)} < 9")
    ts_ms, reason = struct.unpack_from('<QB', payload, 0)
    return {
        'ts_ms':        ts_ms,
        'time_str':     _ts_to_str(ts_ms),
        'reset_reason': reason,
        'reason_str':   ESP_RESET_REASONS.get(reason, f"UNKNOWN({reason})"),
    }


ESP_WAKEUP_CAUSES = {
    0: 'NORMAL',
    2: 'EXT0',
    4: 'TIMER',
    5: 'ULP',
    6: 'TOUCHPAD',
    7: 'ULP',
}


def _unpack_boot(payload: bytes) -> dict:
    if len(payload) < 9:
        raise ValueError(f"BOOT payload too short: {len(payload)} < 9")
    ts_ms, boot_info = struct.unpack_from('<QB', payload, 0)
    reset_reason = boot_info & 0x0F
    wake_cause   = (boot_info >> 4) & 0x0F
    return {
        'ts_ms':        ts_ms,
        'time_str':     _ts_to_str(ts_ms),
        'reset_reason': reset_reason,
        'reason_str':   ESP_RESET_REASONS.get(reset_reason, f"UNKNOWN({reset_reason})"),
        'wake_cause':   wake_cause,
        'wake_str':     ESP_WAKEUP_CAUSES.get(wake_cause, f"UNKNOWN({wake_cause})"),
    }


def _unpack_time_set(payload: bytes) -> dict:
    if len(payload) < 8:
        raise ValueError(f"TIME_SET payload too short: {len(payload)} < 8")
    ts_ms, = struct.unpack_from('<Q', payload, 0)
    return {
        'ts_ms':    ts_ms,
        'time_str': _ts_to_str(ts_ms),
    }


def _is_valid_entry_type(t: int) -> bool:
    return (0 <= t <= 13) or t in (LOG_TYPE_BAT, LOG_TYPE_CRASH, LOG_TYPE_BOOT, LOG_TYPE_TIME_SET)


def parse_entries(data: bytes, fsm_states: dict = None, type_first: bool = False) -> list:
    """
    Parse a stream of raw binary log entries.
    Returns list of dicts, each with 'entry_type' and type-specific fields.

    Entry format depends on type_first:
      False (current FW): [len u8][payload (len-1 bytes)][type u8]
      True  (old FW):     [len u8][type u8][payload (len-1 bytes)]
    In both cases total bytes consumed per entry = len + 1.
    """
    if fsm_states is None:
        fsm_states = _FALLBACK_FSM_STATES

    entries = []
    i = 0
    n = len(data)

    while i < n:
        b = data[i]

        # Erased flash or sector padding → skip to next sector
        if b == 0xFF or b == 0x00:
            sector_size = 4096
            next_sector = ((i // sector_size) + 1) * sector_size
            i = next_sector
            continue

        # In type_first (old FW) format, sectors have a small zero-pad header
        # that isn't full-sector padding. Only skip individual zero bytes.
        if type_first and b == 0x00:
            i += 1
            continue

        entry_len = b          # stored len = payload_size + 1
        payload_size = entry_len - 1
        end_offset = i + entry_len  # last byte of this entry's content

        if end_offset >= n:
            break  # truncated

        # Detect entry format: with type byte (total = len+1) or without (total = len).
        # Check if data[end_offset] is the start of the next entry (no type byte)
        # vs a type byte followed by the next entry at end_offset+1.
        has_type_byte = True
        if end_offset + 1 < n:
            next_at_len = data[end_offset]       # byte right after payload
            next_at_len1 = data[end_offset + 1]  # byte one further
            # If the byte at end_offset looks like a valid next-entry len byte
            # (matches current entry len or is another plausible len), and the
            # byte at end_offset+1 does NOT, then there's no type byte.
            next_ok = next_at_len not in (0x00, 0xFF) and next_at_len < 250
            next1_ok = next_at_len1 not in (0x00, 0xFF) and next_at_len1 < 250
            if next_ok and not _is_valid_entry_type(next_at_len):
                # end_offset byte isn't a valid type, treat as next entry (no type)
                has_type_byte = False
            elif next_ok and next_at_len == entry_len and not next1_ok:
                # Same len repeating at stride=len (not len+1) → no type byte
                has_type_byte = False

        if not has_type_byte:
            # No type byte: [len][payload], total = len bytes, FSM type implied
            payload = data[i + 1 : i + entry_len]
            entry_type = 0  # default to IDLE / FSM
            i = end_offset  # advance by len (not len+1)
        elif type_first:
            entry_type = data[i + 1]
            payload = data[i + 2 : i + 1 + entry_len]
            # Fallback: if type-first gives invalid type, try type-last
            if not _is_valid_entry_type(entry_type):
                alt_type = data[end_offset]
                if _is_valid_entry_type(alt_type):
                    entry_type = alt_type
                    payload = data[i + 1 : i + 1 + payload_size]
            i = end_offset + 1
        else:
            payload = data[i + 1 : i + 1 + payload_size]
            entry_type = data[end_offset]
            # Fallback: if type-last gives invalid type, try type-first
            if not _is_valid_entry_type(entry_type):
                alt_type = data[i + 1]
                if _is_valid_entry_type(alt_type):
                    entry_type = alt_type
                    payload = data[i + 2 : i + 1 + entry_len]
            i = end_offset + 1

        try:
            if 0 <= entry_type <= 13:
                e = _unpack_fsm(payload, fsm_states)
                e['entry_type'] = entry_type
                e['state_name'] = fsm_states.get(entry_type, f"STATE_{entry_type}")
            elif entry_type == LOG_TYPE_BAT:
                e = _unpack_bat(payload)
                e['entry_type'] = LOG_TYPE_BAT
                e['state_name'] = 'BAT'
            elif entry_type == LOG_TYPE_CRASH:
                e = _unpack_crash(payload)
                e['entry_type'] = LOG_TYPE_CRASH
                e['state_name'] = 'CRASH'
            elif entry_type == LOG_TYPE_BOOT:
                e = _unpack_boot(payload)
                e['entry_type'] = LOG_TYPE_BOOT
                e['state_name'] = 'BOOT'
            elif entry_type == LOG_TYPE_TIME_SET:
                e = _unpack_time_set(payload)
                e['entry_type'] = LOG_TYPE_TIME_SET
                e['state_name'] = 'TIME_SET'
            else:
                e = {
                    'entry_type': entry_type,
                    'state_name': f'UNK({entry_type:#04x})',
                    'raw': payload.hex(),
                }
        except Exception as exc:
            e = {
                'entry_type': entry_type,
                'state_name': 'PARSE_ERR',
                'error': str(exc),
                'raw': payload.hex(),
            }

        entries.append(e)
        # i was already advanced in the format-detection block above

    return entries


def parse_response(blob: bytes, fsm_states: dict = None) -> tuple:
    """
    Parse a full HTTP /log response blob.
    Returns (json_meta: dict, tail: int, head: int, entries: list).
    """
    if len(blob) < 8:
        raise ValueError("Response too short")

    # Detect HTML response (device served webpage instead of binary log)
    if blob[:5] in (b'<!DOC', b'<!doc', b'<html', b'<HTML'):
        raise ValueError("Got HTML instead of binary log — check URL resolves to /log endpoint")

    json_len = struct.unpack_from('>I', blob, 0)[0]
    if json_len > 65536 or len(blob) < 4 + json_len + 8:
        raise ValueError(f"Invalid json_len {json_len} (expected binary log format, got {blob[:20]})")

    json_bytes = blob[4 : 4 + json_len]
    meta = json.loads(json_bytes.decode('utf-8'))

    tail, head = struct.unpack_from('>II', blob, 4 + json_len)
    binary = blob[4 + json_len + 8:]

    entries = parse_entries(binary, fsm_states)
    return meta, tail, head, entries


def _detect_old_partition_dump(blob: bytes) -> int:
    """
    Detect old firmware partition dump format.
    Old format: 8-byte file header + 0x4000 bytes params + log entries
    with type byte at the start of each entry's content region.
    Returns the log data start offset, or 0 if not detected.
    """
    if len(blob) < 0x4100:
        return 0
    # Check if offset 0x4000 looks like a log sector: leading zero-pad
    # followed by a valid entry with a valid type byte at +1 (type-first format)
    base = 0x4000
    # Find first non-zero byte in the sector
    first_nz = 0
    while first_nz < 4096 and blob[base + first_nz] == 0x00:
        first_nz += 1
    if first_nz >= 4096:
        return 0
    entry_len = blob[base + first_nz]
    if entry_len < 2 or base + first_nz + 1 + entry_len > len(blob):
        return 0
    # In old format, the type byte is the first byte after the len byte
    entry_type = blob[base + first_nz + 1]
    if _is_valid_entry_type(entry_type):
        return base
    return 0


def _try_detect_type_first(data: bytes) -> bool:
    """
    Given raw log entry data, try to determine if entries use
    type-first format (old FW) vs type-last format (current FW).
    Samples multiple entries and checks which placement yields
    valid entry types, plausible timestamps, or reasonable voltages.
    """
    i = 0
    n = len(data)
    attempts = 0
    max_attempts = 200
    while i < n and attempts < max_attempts:
        b = data[i]
        if b == 0xFF:
            break
        if b == 0x00:
            i = ((i // 4096) + 1) * 4096
            continue
        entry_len = b
        end_offset = i + entry_len
        if end_offset >= n:
            break
        # type-last (current): type is at end_offset
        type_last = data[end_offset]
        # type-first (old): type is at i+1
        type_first_val = data[i + 1]
        last_valid = _is_valid_entry_type(type_last)
        first_valid = _is_valid_entry_type(type_first_val)
        if first_valid and not last_valid:
            return True
        if last_valid and not first_valid:
            return False
        # Both valid or neither — try parsing the payload to disambiguate
        if first_valid and last_valid:
            payload_first = data[i + 2 : i + 1 + entry_len]
            payload_last = data[i + 1 : i + 1 + entry_len - 1]
            for payload, is_first in [(payload_first, True), (payload_last, False)]:
                if len(payload) >= 12:
                    ts = struct.unpack_from('<Q', payload, 0)[0]
                    # Plausible if timestamp is 2020-2030 in ms
                    if 1577836800000 < ts < 1893456000000:
                        return is_first
                    # Also check if the float at offset 8 is a reasonable voltage (0-60V)
                    v = struct.unpack_from('<f', payload, 8)[0]
                    if 0.5 < v < 60.0:
                        return is_first
        # Advance to next entry and keep trying
        i = end_offset + 1
        attempts += 1
    return False


def autodetect_and_parse(blob: bytes, fsm_states: dict = None) -> tuple:
    """
    Auto-detect whether blob is HTTP response format, old partition dump,
    or raw flash binary.
    Returns (json_meta_or_None, tail_or_None, head_or_None, entries).
    """
    # HTTP format: first 4 bytes = BE uint32 json_len, byte 4 should be '{'
    if len(blob) >= 5:
        candidate_len = struct.unpack_from('>I', blob, 0)[0]
        if candidate_len < len(blob) and blob[4:5] == b'{':
            meta, tail, head, entries = parse_response(blob, fsm_states)
            return meta, tail, head, entries

    # Bare tail+head format: [4B tail BE][4B head BE][raw log data]
    # Detect by checking if head - tail == len(blob) - 8
    if len(blob) >= 16:
        tail_val, head_val = struct.unpack_from('>II', blob, 0)
        if head_val > tail_val and (head_val - tail_val) == len(blob) - 8:
            log_data = blob[8:]
            type_first = _try_detect_type_first(log_data)
            entries = parse_entries(log_data, fsm_states, type_first=type_first)
            return None, tail_val, head_val, entries

    # Old partition dump: 8-byte header + 0x4000 params + log entries (type-first)
    log_offset = _detect_old_partition_dump(blob)
    if log_offset > 0:
        log_data = blob[log_offset:]
        type_first = _try_detect_type_first(log_data)
        entries = parse_entries(log_data, fsm_states, type_first=type_first)
        return None, None, None, entries

    # Raw binary — auto-detect type placement
    type_first = _try_detect_type_first(blob)
    entries = parse_entries(blob, fsm_states, type_first=type_first)
    return None, None, None, entries
