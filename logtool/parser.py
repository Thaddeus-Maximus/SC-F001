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
    if len(payload) < 39:
        raise ValueError(f"FSM payload too short: {len(payload)} < 39")
    ts_ms, bat_V, drive_A, jack_A, aux_A, counter, sensors, \
        drive_heat, jack_heat, aux_heat = struct.unpack_from('<QffffhBfff', payload, 0)
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


def parse_entries(data: bytes, fsm_states: dict = None) -> list:
    """
    Parse a stream of raw binary log entries.
    Returns list of dicts, each with 'entry_type' and type-specific fields.
    """
    if fsm_states is None:
        fsm_states = _FALLBACK_FSM_STATES

    entries = []
    i = 0
    n = len(data)

    while i < n:
        b = data[i]

        # Erased flash or sector padding → done or skip sector
        if b == 0xFF:
            break
        if b == 0x00:
            # Sector padding: skip to next 4096-byte boundary
            sector_size = 4096
            next_sector = ((i // sector_size) + 1) * sector_size
            i = next_sector
            continue

        entry_len = b          # stored len = payload_size + 1
        payload_size = entry_len - 1
        type_offset = i + 1 + payload_size   # = i + entry_len

        if type_offset >= n:
            break  # truncated

        payload = data[i + 1 : i + 1 + payload_size]
        entry_type = data[type_offset]

        try:
            if 0 <= entry_type <= 12:
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
        i = type_offset + 1  # advance past type byte

    return entries


def parse_response(blob: bytes, fsm_states: dict = None) -> tuple:
    """
    Parse a full HTTP /log response blob.
    Returns (json_meta: dict, tail: int, head: int, entries: list).
    """
    if len(blob) < 8:
        raise ValueError("Response too short")

    json_len = struct.unpack_from('>I', blob, 0)[0]
    if json_len > 65536 or len(blob) < 4 + json_len + 8:
        raise ValueError(f"Invalid json_len {json_len}")

    json_bytes = blob[4 : 4 + json_len]
    meta = json.loads(json_bytes.decode('utf-8'))

    tail, head = struct.unpack_from('>II', blob, 4 + json_len)
    binary = blob[4 + json_len + 8:]

    entries = parse_entries(binary, fsm_states)
    return meta, tail, head, entries


def autodetect_and_parse(blob: bytes, fsm_states: dict = None) -> tuple:
    """
    Auto-detect whether blob is HTTP response format or raw flash binary.
    Returns (json_meta_or_None, tail_or_None, head_or_None, entries).
    """
    # HTTP format: first 4 bytes = BE uint32 json_len, byte 4 should be '{'
    if len(blob) >= 5:
        candidate_len = struct.unpack_from('>I', blob, 0)[0]
        if candidate_len < 8192 and blob[4:5] == b'{':
            meta, tail, head, entries = parse_response(blob, fsm_states)
            return meta, tail, head, entries

    # Raw binary
    entries = parse_entries(blob, fsm_states)
    return None, None, None, entries
