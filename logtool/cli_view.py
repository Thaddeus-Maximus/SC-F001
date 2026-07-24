"""
CLI table output for SC-F001 logtool.
"""

from parser import (LOG_TYPE_BAT, LOG_TYPE_CRASH, LOG_TYPE_BOOT,
                    LOG_TYPE_TIME_SET, is_fsm_type)

try:
    from tabulate import tabulate
    _TABULATE_OK = True
except ImportError:
    _TABULATE_OK = False


_SENSOR_BITS = ['SAFETY', 'JACK', 'DRIVE', 'AUX2']


def _sensor_str(nibble: int) -> str:
    active = [_SENSOR_BITS[i] for i in range(4) if (nibble >> i) & 1]
    return '+'.join(active) if active else '-'


def _row(e: dict) -> list:
    t = e.get('entry_type', -1)
    name = e.get('state_name', '?')

    if is_fsm_type(t):
        return [
            e.get('time_str', ''),
            name,
            f"{e.get('bat_V', 0):.3f}",
            f"{e.get('current_A', 0):.2f}",
            str(e.get('counter', 0)),
            _sensor_str(e.get('sensors_stable', 0)),
            _sensor_str(e.get('sensors_raw', 0)),
            f"{e.get('heat', 0):.1f}",
            f"0x{e.get('i2c_out', 0):04X}",
        ]
    elif t == LOG_TYPE_BAT:
        return [
            e.get('time_str', ''),
            'BAT',
            f"{e.get('bat_V', 0):.3f}",
            '—', '—', '—', '—', '—', '—',
        ]
    elif t == LOG_TYPE_CRASH:
        return [
            e.get('time_str', ''),
            f"*** CRASH: {e.get('reason_str', '?')}",
            '—', '—', '—', '—', '—', '—', '—',
        ]
    elif t == LOG_TYPE_BOOT:
        return [
            e.get('time_str', ''),
            f"BOOT  rst={e.get('reason_str', '?')}  wake={e.get('wake_str', '?')}",
            '—', '—', '—', '—', '—', '—', '—',
        ]
    elif t == LOG_TYPE_TIME_SET:
        return [
            e.get('time_str', ''),
            'TIME_SET',
            '—', '—', '—', '—', '—', '—', '—',
        ]
    else:
        return [
            e.get('time_str', ''),
            name,
            '—', '—', '—', '—', '—', '—', '—',
        ]


_HEADERS = ['Time', 'State', 'Bat(V)', 'Cur(A)',
            'Counter', 'Stable', 'Raw', 'Heat', 'I2COut']


def print_table(entries: list, type_filter: str = None):
    """Print a tabulate table of log entries to stdout."""
    if type_filter:
        tf = type_filter.lower()
        if tf == 'fsm':
            entries = [e for e in entries if is_fsm_type(e.get('entry_type', -1))]
        elif tf == 'bat':
            entries = [e for e in entries if e.get('entry_type') == LOG_TYPE_BAT]
        elif tf == 'crash':
            entries = [e for e in entries if e.get('entry_type') == LOG_TYPE_CRASH]

    rows = [_row(e) for e in entries]

    if not rows:
        print("(no entries)")
        return

    if _TABULATE_OK:
        print(tabulate(rows, headers=_HEADERS, tablefmt='simple'))
    else:
        # Manual fallback
        widths = [max(len(str(r[i])) for r in [_HEADERS] + rows) for i in range(len(_HEADERS))]
        fmt = '  '.join(f'{{:<{w}}}' for w in widths)
        print(fmt.format(*_HEADERS))
        print('  '.join('-' * w for w in widths))
        for row in rows:
            print(fmt.format(*row))


def print_summary(entries: list):
    """Print a brief summary: time range, entry counts, voltage range."""
    if not entries:
        print("(empty log)")
        return

    fsm_entries      = [e for e in entries if is_fsm_type(e.get('entry_type', -1))]
    bat_entries      = [e for e in entries if e.get('entry_type') == LOG_TYPE_BAT]
    crash_entries    = [e for e in entries if e.get('entry_type') == LOG_TYPE_CRASH]
    boot_entries     = [e for e in entries if e.get('entry_type') == LOG_TYPE_BOOT]
    time_set_entries = [e for e in entries if e.get('entry_type') == LOG_TYPE_TIME_SET]

    all_ts = [e.get('ts_ms', 0) for e in entries if e.get('ts_ms')]
    ts_min = min(all_ts) if all_ts else 0
    ts_max = max(all_ts) if all_ts else 0

    all_bat = [e['bat_V'] for e in entries if 'bat_V' in e]

    print(f"Entries : {len(entries)} total  "
          f"({len(fsm_entries)} FSM, {len(bat_entries)} BAT, "
          f"{len(crash_entries)} CRASH, {len(boot_entries)} BOOT, "
          f"{len(time_set_entries)} TIME_SET)")
    if all_ts:
        from parser import _ts_to_str
        print(f"Time    : {_ts_to_str(ts_min)}  →  {_ts_to_str(ts_max)}")
        dur_s = (ts_max - ts_min) / 1000
        print(f"Duration: {dur_s:.1f} s  ({dur_s/60:.1f} min)")
    if all_bat:
        print(f"Battery : {min(all_bat):.3f} V – {max(all_bat):.3f} V")
    if boot_entries:
        print(f"\nBOOT events:")
        for e in boot_entries:
            print(f"  {e.get('time_str', '?')}  rst={e.get('reason_str', '?')}  wake={e.get('wake_str', '?')}")
    if crash_entries:
        print(f"\nCRASH events:")
        for e in crash_entries:
            print(f"  {e.get('time_str', '?')}  reason={e.get('reason_str', '?')}")
    if time_set_entries:
        print(f"\nTIME_SET events:")
        for e in time_set_entries:
            print(f"  {e.get('time_str', '?')}")


def append_rows(new_entries: list):
    """Print new rows without header (for streaming append mode)."""
    for e in new_entries:
        row = _row(e)
        if _TABULATE_OK:
            print(tabulate([row], tablefmt='plain'))
        else:
            print('  '.join(str(c) for c in row))
