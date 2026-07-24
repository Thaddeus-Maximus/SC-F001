#!/usr/bin/env python3
"""
SC-F001 Log Tool

Usage:
  logtool.py <source> [options]

  <source>   *.bin file path  →  read local file
             anything else    →  treated as hostname/URL:
               sc.local           →  http://sc.local/log
               192.168.4.1        →  http://192.168.4.1/log
               http://host/log    →  used as-is

Output files are always written:
  <basename>.bin   raw bytes received (HTTP response or file contents)
  <basename>.txt   stdout capture (table / summary)

Default basename: 04MAR2026_1052 (date+time of invocation).
Specify with --out <basename>.

Options:
  --gui                Show matplotlib plots instead of CLI table
  --stream             Poll for new entries (HTTP only)
  --type fsm|bat|crash Filter entry type
  --tail <offset>      Start from a specific flash offset (HTTP POST mode)
  --interval <s>       Polling interval in seconds (default: 2.0)
  --fw <path>          Path to firmware main/ directory (for state names)
  --summary            Print summary statistics only
  --out <basename>     Output file basename (no extension)
"""

import sys
import io
import argparse
from datetime import datetime
from pathlib import Path

# Ensure logtool directory is on the path
sys.path.insert(0, str(Path(__file__).parent))

import parser as prs
import source as src
import cli_view
import gui_view


def _default_basename() -> str:
    return datetime.now().strftime('%d%b%Y_%H%M').upper()


def _normalize_source(raw: str) -> tuple:
    """
    Returns (is_http: bool, resolved: str).
    *.bin → (False, path)
    http(s)://...  → (True, url as-is)
    anything else  → (True, http://<raw>/log)
    """
    if raw.endswith('.bin'):
        return False, raw
    if raw.startswith('http://') or raw.startswith('https://'):
        # Append /log if URL has no path (or just /)
        from urllib.parse import urlparse
        p = urlparse(raw)
        if p.path in ('', '/'):
            raw = raw.rstrip('/') + '/log'
        return True, raw
    return True, f'http://{raw}/log'


class _Tee:
    """Write to two streams simultaneously (stdout + file)."""
    def __init__(self, primary, secondary):
        self.primary   = primary
        self.secondary = secondary

    def write(self, data):
        self.primary.write(data)
        self.secondary.write(data)
        return len(data)

    def flush(self):
        self.primary.flush()
        self.secondary.flush()

    def __getattr__(self, name):
        return getattr(self.primary, name)


def main():
    ap = argparse.ArgumentParser(
        description='SC-F001 flash log viewer',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    ap.add_argument('source', help='*.bin file, hostname, or full URL')
    ap.add_argument('--gui', action='store_true', help='Show matplotlib GUI')
    ap.add_argument('--stream', action='store_true', help='Live streaming mode (HTTP only)')
    ap.add_argument('--type', choices=['fsm', 'bat', 'crash'], dest='entry_type',
                    help='Filter by entry type')
    ap.add_argument('--tail', type=int, default=None,
                    help='Start from flash offset (HTTP only)')
    ap.add_argument('--interval', type=float, default=2.0,
                    help='Polling interval in seconds (default: 2.0)')
    ap.add_argument('--fw', default=None,
                    help='Path to firmware main/ directory (default: ../main)')
    ap.add_argument('--summary', action='store_true',
                    help='Print summary statistics only')
    ap.add_argument('--out', default=None, metavar='BASENAME',
                    help='Output file basename (default: 04MAR2026_1052 style)')
    args = ap.parse_args()

    # Resolve source
    is_http, resolved = _normalize_source(args.source)

    # Output file basename
    basename = args.out or _default_basename()
    bin_path = Path(basename + '.bin')
    txt_path = Path(basename + '.txt')

    # Tee stdout → .txt file
    txt_file = txt_path.open('w', encoding='utf-8')
    sys.stdout = _Tee(sys.__stdout__, txt_file)

    try:
        _run(args, is_http, resolved, bin_path, basename)
    finally:
        sys.stdout = sys.__stdout__
        txt_file.close()
        print(f"Saved: {bin_path}  {txt_path}")


def _run(args, is_http, resolved, bin_path, basename):
    # Load FSM state names from firmware source
    fw_path = args.fw or (Path(__file__).parent.parent / 'main')
    fsm_states = prs.load_fsm_states(fw_path)

    # ── Streaming mode ────────────────────────────────────────────────────────
    if args.stream:
        if not is_http:
            print("Error: --stream requires an HTTP source", file=sys.stderr)
            sys.exit(1)

        if args.gui:
            print(f"Starting live GUI from {resolved} ...")
            # GUI handles its own data fetching; raw bytes aren't easily capturable
            bin_path.write_bytes(b'')
            gui_view.live_plot(resolved, interval_s=args.interval)
            return

        print(f"Streaming from {resolved} (Ctrl-C to stop)\n")
        print('  '.join(cli_view._HEADERS))
        print('  '.join('-' * len(h) for h in cli_view._HEADERS))

        accumulated_bin = io.BytesIO()

        def on_batch(entries, meta, is_first):
            if args.entry_type:
                tf = args.entry_type.lower()
                if tf == 'fsm':
                    entries = [e for e in entries if prs.is_fsm_type(e.get('entry_type', -1))]
                elif tf == 'bat':
                    entries = [e for e in entries if e.get('entry_type') == prs.LOG_TYPE_BAT]
                elif tf == 'crash':
                    entries = [e for e in entries if e.get('entry_type') == prs.LOG_TYPE_CRASH]
            if entries:
                cli_view.append_rows(entries)

        def _patched_stream():
            """Like source.stream but also captures raw bytes."""
            blob = src.fetch_full(resolved)
            accumulated_bin.write(blob)
            meta, tail, head, entries = prs.autodetect_and_parse(blob, fsm_states)
            on_batch(entries, meta, is_first=True)

            current_tail = head or 0
            import time
            while True:
                time.sleep(args.interval)
                try:
                    binary, new_head = src.fetch_incremental(resolved, current_tail)
                    if binary:
                        accumulated_bin.write(binary)
                        new_entries = prs.parse_entries(binary, fsm_states)
                        if new_entries:
                            on_batch(new_entries, None, is_first=False)
                    current_tail = new_head
                except Exception as exc:
                    print(f"[stream] poll error: {exc}")

        try:
            _patched_stream()
        except KeyboardInterrupt:
            print("\nStopped.")
        finally:
            bin_path.write_bytes(accumulated_bin.getvalue())
        return

    # ── One-shot mode ─────────────────────────────────────────────────────────
    if is_http:
        print(f"Fetching {resolved} ...")
        if args.tail is not None:
            binary, new_head = src.fetch_incremental(resolved, args.tail)
            blob = binary
            entries = prs.parse_entries(binary, fsm_states)
            meta = None
            print(f"Incremental fetch: new_head={new_head}  entries={len(entries)}")
        else:
            blob = src.fetch_full(resolved)
            meta, tail, head, entries = prs.parse_response(blob, fsm_states)
            print(f"Log offsets: tail={tail}  head={head}  entries={len(entries)}")
    else:
        print(f"Reading {resolved} ...")
        blob = src.read_file(resolved)
        meta, tail, head, entries = prs.autodetect_and_parse(blob, fsm_states)
        if head is not None:
            print(f"Log offsets: tail={tail}  head={head}")
        print(f"Parsed {len(entries)} entries")

    # Save raw binary
    bin_path.write_bytes(blob or b'')

    if meta:
        ver = meta.get('version', '?')
        t   = meta.get('time', '?')
        print(f"Device: version={ver}  time={t}")

    if args.summary:
        cli_view.print_summary(entries)
    elif args.gui:
        title = f"SC-F001 Log — {args.source}"
        gui_view.show_plots(entries, title=title)
    else:
        cli_view.print_table(entries, type_filter=args.entry_type)
        print()
        cli_view.print_summary(entries)


if __name__ == '__main__':
    main()
