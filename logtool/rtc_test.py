#!/usr/bin/env python3
"""
rtc_test.py -- SC-F001 RTC timekeeping monitor

1. Reset board and wait for boot
2. Sync device RTC to host clock
3. Stream UART continuously -- no sleep commands issued
4. Every time the firmware logs a TIME line, record host time vs device time and print diff
5. Ctrl+C to stop

The device will sleep naturally after 180s of inactivity and wake every 120s,
producing a TIME line on each wakeup.  This script catches each one.

Firmware emits parseable lines of the form:
  I (uptime) RTC: TIME unix=<seconds> src=<SYNC|SLEEP|CRASH> uptime=<seconds>s

Generates two log files automatically:
  rtc_raw_TIMESTAMP.txt      -- every UART byte in/out, wall-clock timestamped
  rtc_analysis_TIMESTAMP.txt -- clean parsed comparison table, no ANSI codes

Usage:
  pip install pyserial
  python logtool/rtc_test.py COM3
  python logtool/rtc_test.py COM3 --verbose    # also print raw device output
"""

import serial
import time
import re
import sys
import argparse
from datetime import datetime, timezone

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

BAUD         = 115200
BOOT_TIMEOUT = 35     # seconds to wait for prompt after reset

_ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
# Matches: I (123) RTC: TIME unix=1773167687 src=SLEEP uptime=5s
_TIME_RE = re.compile(r"TIME unix=(\d+) src=(\S+) uptime=(\d+)s")

def _strip(s):
    return _ANSI.sub("", s)

def _ts():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]

def _utc(unix_s):
    return datetime.fromtimestamp(int(unix_s), tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%S")


# ---------------------------------------------------------------------------
# Log handles
# ---------------------------------------------------------------------------

_raw_log      = None
_analysis_log = None

def _rawlog(direction, text):
    if not _raw_log:
        return
    for line in text.splitlines(keepends=True):
        _raw_log.write(f"[{_ts()} {direction}] {_strip(line)}")
    if text and not text.endswith("\n"):
        _raw_log.write("\n")
    _raw_log.flush()

def aprint(*args, **kwargs):
    """Print to console AND analysis log (ANSI stripped in file)."""
    end  = kwargs.get("end", "\n")
    text = " ".join(str(a) for a in args) + end
    sys.stdout.write(text)
    sys.stdout.flush()
    if _analysis_log:
        _analysis_log.write(_strip(text))
        _analysis_log.flush()


# ---------------------------------------------------------------------------
# Serial helpers
# ---------------------------------------------------------------------------

def reset_board(ser):
    """Pulse EN via RTS; keep DTR=False so GPIO0 stays HIGH (normal boot)."""
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.05)


def wait_for_boot(ser, timeout=BOOT_TIMEOUT, verbose=False):
    """
    Read until the UART prompt (\\n> ) appears anywhere in the buffer.
    Nudges the device every 3s in case the prompt is buried under log spam.
    """
    buf            = ""
    deadline       = time.time() + timeout
    last_nudge     = time.time()
    prompt_seen_at = None

    while time.time() < deadline:
        chunk = ser.read(256).decode(errors="replace")
        if chunk:
            buf += chunk
            _rawlog("RX", chunk)
            if verbose:
                sys.stdout.write(chunk)
                sys.stdout.flush()
            if prompt_seen_at is None and ("\n> " in buf or "\r\n> " in buf):
                prompt_seen_at = time.time()

        if prompt_seen_at is not None and time.time() - prompt_seen_at >= 0.20:
            return buf

        if time.time() - last_nudge > 3.0:
            ser.write(b"\r\n")
            _rawlog("TX", "\r\n")
            last_nudge = time.time()

        time.sleep(0.05)

    raise TimeoutError(f"Boot prompt not seen within {timeout}s")


def send_cmd(ser, cmd, timeout=10.0, verbose=False):
    """Send a command and wait for the next prompt."""
    ser.read_all()
    ser.write((cmd + "\r\n").encode())
    _rawlog("TX", cmd + "\r\n")

    buf            = ""
    deadline       = time.time() + timeout
    prompt_seen_at = None

    while time.time() < deadline:
        chunk = ser.read(256).decode(errors="replace")
        if chunk:
            buf += chunk
            _rawlog("RX", chunk)
            if verbose:
                sys.stdout.write(chunk)
                sys.stdout.flush()
            if prompt_seen_at is None and ("\n> " in buf or "\r\n> " in buf):
                prompt_seen_at = time.time()

        if prompt_seen_at is not None and time.time() - prompt_seen_at >= 0.20:
            return buf

        time.sleep(0.05)

    return buf   # return whatever we got even on timeout


# ---------------------------------------------------------------------------
# Comparison table
# ---------------------------------------------------------------------------

_HDR = (
    "\n"
    "  #   Wall clock (host)      Device time (UTC)    Diff    Source  Uptime\n"
    "  --  -------------------  -------------------  ------  --------  ------"
)
_ROW = "  {n:>2}  {ht:<19}  {dt:<19}  {diff:>+6}s  {src:<8}  {up}s"

def _table_row(n, host_ts, device_unix, src, uptime_s):
    diff = device_unix - host_ts
    return _ROW.format(
        n    = n,
        ht   = _utc(host_ts),
        dt   = _utc(device_unix),
        diff = diff,
        src  = src,
        up   = uptime_s,
    )


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    global _raw_log, _analysis_log

    parser = argparse.ArgumentParser(description="SC-F001 RTC timekeeping monitor")
    parser.add_argument("port", help="Serial port, e.g. COM3 or /dev/ttyUSB0")
    parser.add_argument("--boot-timeout", type=float, default=BOOT_TIMEOUT)
    parser.add_argument("--verbose", action="store_true",
                        help="Print raw device output to console (always in raw log)")
    args = parser.parse_args()

    stamp         = datetime.now().strftime("%Y%m%d_%H%M%S")
    raw_path      = f"logtool/rtc_raw_{stamp}.txt"
    analysis_path = f"logtool/rtc_analysis_{stamp}.txt"

    _raw_log      = open(raw_path,      "w", encoding="utf-8", errors="replace")
    _analysis_log = open(analysis_path, "w", encoding="utf-8", errors="replace")

    aprint(f"Raw log      : {raw_path}")
    aprint(f"Analysis log : {analysis_path}")
    aprint(f"Port         : {args.port}  {BAUD} baud")
    aprint(f"Ctrl+C to stop.\n")

    # ---- Open port -------------------------------------------------------- #
    ser = serial.Serial(args.port, BAUD, timeout=0.2)
    ser.dtr = False
    ser.rts = False
    time.sleep(0.2)
    ser.read_all()

    # ---- Reset and wait for boot ------------------------------------------ #
    aprint(f"[{_ts()}] Resetting board...")
    reset_board(ser)

    try:
        wait_for_boot(ser, timeout=args.boot_timeout, verbose=args.verbose)
    except TimeoutError as e:
        aprint(f"ERROR: {e}")
        ser.close(); sys.exit(1)

    aprint(f"[{_ts()}] Boot complete.\n")

    # ---- Sync RTC --------------------------------------------------------- #
    host_sync = int(time.time())
    aprint(f"[{_ts()}] Syncing device RTC to host: {host_sync} ({_utc(host_sync)} UTC)...")
    resp = send_cmd(ser, f'POST: {{"time": {host_sync}}}', verbose=args.verbose)
    if "ok" in resp.lower():
        aprint(f"[{_ts()}] Sync OK.\n")
    else:
        aprint(f"[{_ts()}] WARNING: unexpected response: {_strip(resp.strip())}\n")

    # ---- Table header ----------------------------------------------------- #
    aprint(_HDR)

    # ---- Stream loop ------------------------------------------------------ #
    event_n  = 0
    line_buf = ""

    aprint()
    aprint("Streaming... (device will sleep after 180s inactivity, wake every 120s)")
    aprint()

    try:
        while True:
            chunk = ser.read(256).decode(errors="replace")
            if not chunk:
                continue

            _rawlog("RX", chunk)
            if args.verbose:
                sys.stdout.write(chunk)
                sys.stdout.flush()

            line_buf += chunk

            # Process complete lines
            while "\n" in line_buf:
                line, line_buf = line_buf.split("\n", 1)
                line = line.rstrip("\r")

                m = _TIME_RE.search(_strip(line))
                if not m:
                    continue

                # Got a TIME event
                host_ts     = int(time.time())
                device_unix = int(m.group(1))
                src         = m.group(2)
                uptime_s    = m.group(3)
                event_n    += 1

                row = _table_row(event_n, host_ts, device_unix, src, uptime_s)
                aprint(row)

    except KeyboardInterrupt:
        aprint(f"\n\n[{_ts()}] Stopped by user.")
        if event_n == 0:
            aprint("No TIME events captured.")
        else:
            aprint(f"{event_n} event(s) logged to {analysis_path}")

    ser.close()
    _raw_log.close()
    _analysis_log.close()


if __name__ == "__main__":
    main()
