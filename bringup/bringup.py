#!/usr/bin/env python3
"""
SC-F001 Bring-up Tool

Flashes the firmware (optional) and then walks the operator through the
bring-up procedure documented in docs/SC-F001/BRINGUP.md.

Usage:
  bringup.py --port <COMx | /dev/ttyUSB0> [options]

Options:
  --port <p>        Serial port (required)
  --baud <n>        Baud rate for UART protocol (default: 115200)
  --out <basename>  Transcript basename (default: dated)

Flashing (optional):
  --flash           Flash the firmware before running tests
  --build-dir <p>   Build directory containing flasher_args.json
                    (default: ../build/ relative to this script)
  --flash-baud <n>  Baud for esptool (default: 460800)
  --erase           `esptool erase_flash` before writing (slow)
  --flash-only      Flash and exit (no bring-up tests)

Testing:
  --skip-relays     Skip the live relay pulse stage
  --no-calibrate    Skip battery-voltage calibration prompt
  --no-transcript   Do not write a .txt transcript file
"""

from __future__ import annotations

import argparse
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import fmt                             # noqa: E402
from protocol import Link          # noqa: E402
from stages import all_stages, Tally  # noqa: E402
import flash as flasher               # noqa: E402


def _default_basename() -> str:
    return "BRINGUP_" + datetime.now().strftime("%d%b%Y_%H%M").upper()


def _do_flash(args, log_fn) -> None:
    build_dir = Path(args.build_dir) if args.build_dir else None
    try:
        flasher.flash(
            port=args.port,
            build_dir=build_dir,
            baud=args.flash_baud,
            erase_all=args.erase,
            log=log_fn,
        )
    except flasher.FlashError as e:
        log_fn(f"FLASH FAILED: {e}")
        raise SystemExit(2)


def main() -> int:
    ap = argparse.ArgumentParser(description="SC-F001 bring-up tool (flash + test)")
    ap.add_argument("--port", required=True, help="serial port (COM5, /dev/ttyUSB0, ...)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default=None, help="transcript basename")

    ap.add_argument("--flash", action="store_true",
                    help="flash firmware before tests")
    ap.add_argument("--build-dir", default=None,
                    help="build dir with flasher_args.json (default: ../build)")
    ap.add_argument("--flash-baud", type=int, default=460800)
    ap.add_argument("--erase", action="store_true",
                    help="erase_flash before writing")
    ap.add_argument("--flash-only", action="store_true",
                    help="flash and exit; skip tests")

    ap.add_argument("--skip-relays", action="store_true")
    ap.add_argument("--no-calibrate", action="store_true")
    ap.add_argument("--no-transcript", action="store_true")
    args = ap.parse_args()

    basename = args.out or _default_basename()
    transcript_path = None if args.no_transcript else Path(basename + ".txt")
    transcript_file = None
    transcript_cb = None

    if transcript_path:
        transcript_file = transcript_path.open("w", encoding="utf-8")
        def _tx(line: str) -> None:
            ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]
            transcript_file.write(f"{ts}  {fmt.strip(line)}\n")
            transcript_file.flush()
        transcript_cb = _tx
        print(f"Transcript → {transcript_path}")

    def _log(msg: str) -> None:
        print(msg)
        if transcript_cb:
            transcript_cb(msg)

    # Phase 1: optional flash
    if args.flash or args.flash_only:
        _log(fmt.stage(f"Flashing {args.port}"))
        _do_flash(args, _log)
        _log(fmt.pass_("Flash complete"))
        if args.flash_only:
            if transcript_file:
                transcript_file.close()
            return 0
        # Give the chip a moment to finish hard_reset before we open the port
        time.sleep(1.5)

    # Phase 2: connect and walk bring-up stages
    _log(f"Connecting to {args.port} @ {args.baud} ...")
    link = Link(args.port, baud=args.baud, transcript=transcript_cb)
    link.ser.reset_input_buffer()

    tally = Tally()
    stages = all_stages(skip_relays=args.skip_relays,
                        no_calibrate=args.no_calibrate)

    def _snapshot(t: Tally) -> tuple[int, int, int, int]:
        return (t.passed, t.failed, t.warnings, t.skipped)

    def _restore(t: Tally, snap: tuple[int, int, int, int]) -> None:
        t.passed, t.failed, t.warnings, t.skipped = snap

    try:
        for stage in stages:
            while True:
                snap = _snapshot(tally)
                try:
                    stage(link, tally)
                except TimeoutError as e:
                    print(f"  TIMEOUT: {e}")
                    tally.note_fail()
                except Exception as e:
                    print(f"  EXCEPTION in stage: {e!r}")
                    tally.note_fail()
                if tally.failed > snap[1]:
                    ans = input(fmt.prompt("  Stage had FAILs — retry? [y/n]") + ": ").strip().lower()
                    if ans.startswith("y"):
                        _restore(tally, snap)
                        continue
                break
    except KeyboardInterrupt:
        print(fmt.warn("\nAborted by operator"))
        try:
            link.send("BU.END")
        except Exception:
            pass

    print(fmt.stage("Bring-up summary"))
    print(fmt.summary_line(tally.passed, tally.failed, tally.warnings, tally.skipped))
    if tally.failed == 0:
        print(f"  {fmt.pass_('ALL PASS')}")
    else:
        print(f"  {fmt.fail('FAILURES PRESENT — review above')}")

    link.close()
    if transcript_file:
        transcript_file.close()

    return 0 if tally.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
