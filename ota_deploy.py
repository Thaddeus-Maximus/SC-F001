#!/usr/bin/env python3
"""SC-F001 build + OTA deploy.

Runs `idf.py build` and POSTs the resulting firmware binary to the device's
/ota endpoint. Progress is printed as the upload streams.

Usage:
    python ota_deploy.py [--ip 192.168.4.1] [--no-build]

The script relies on `idf.py` being on PATH — run it from an ESP-IDF shell.
"""

from __future__ import annotations

import argparse
import http.client
import shutil
import subprocess
import sys
import time
from pathlib import Path


DEFAULT_IP          = "192.168.4.1"
DEFAULT_PROJECT     = "SC-F001"
DEFAULT_BUILD_DIR   = Path(__file__).resolve().parent / "build"
OTA_PATH            = "/ota"
CONNECT_TIMEOUT_S   = 10.0
UPLOAD_TIMEOUT_S    = 120.0
CHUNK_BYTES         = 4096


def run_build(project_dir: Path) -> None:
    # On Windows `idf.py` is a Python script (no .exe), so CreateProcess can't
    # launch it directly. Resolve to an absolute path and run through the shell
    # so .py / .bat associations resolve correctly.
    resolved = shutil.which("idf.py") or shutil.which("idf.py.bat") or "idf.py"
    cmd = f'"{resolved}" build' if " " in resolved else f"{resolved} build"
    print(f">> {cmd}  (cwd={project_dir})")
    rc = subprocess.call(cmd, cwd=str(project_dir), shell=True)
    if rc != 0:
        sys.exit(f"idf.py build failed (exit {rc})")


def post_ota(ip: str, binary: Path) -> None:
    size = binary.stat().st_size
    print(f">> uploading {binary.name} ({size} bytes) to http://{ip}{OTA_PATH}")

    conn = http.client.HTTPConnection(ip, 80, timeout=CONNECT_TIMEOUT_S)
    conn.connect()
    # Switch to upload timeout now that we're connected.
    conn.sock.settimeout(UPLOAD_TIMEOUT_S)
    conn.putrequest("POST", OTA_PATH)
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(size))
    conn.endheaders()

    start = time.monotonic()
    sent = 0
    last_pct = -1
    with binary.open("rb") as f:
        while True:
            chunk = f.read(CHUNK_BYTES)
            if not chunk:
                break
            conn.send(chunk)
            sent += len(chunk)
            pct = (sent * 100) // size
            if pct != last_pct:
                print(f"\r    {pct:3d}%  ({sent}/{size})", end="", flush=True)
                last_pct = pct
    elapsed = time.monotonic() - start
    print(f"\n>> upload done in {elapsed:.1f}s, waiting for response...")

    resp = conn.getresponse()
    body = resp.read().decode("utf-8", errors="replace")
    conn.close()

    if resp.status // 100 != 2:
        sys.exit(f"OTA HTTP {resp.status} {resp.reason}: {body}")
    print(f">> device: {body.strip()}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Build + OTA deploy SC-F001 firmware")
    ap.add_argument("--ip",          default=DEFAULT_IP,       help=f"device IP (default {DEFAULT_IP})")
    ap.add_argument("--project",     default=DEFAULT_PROJECT,  help="project name (binary = <project>.bin)")
    ap.add_argument("--build-dir",   default=str(DEFAULT_BUILD_DIR),
                    help="build dir containing <project>.bin")
    ap.add_argument("--no-build",    action="store_true",      help="skip idf.py build, upload existing binary")
    args = ap.parse_args()

    project_dir = Path(__file__).resolve().parent
    build_dir   = Path(args.build_dir).resolve()
    binary      = build_dir / f"{args.project}.bin"

    if not args.no_build:
        run_build(project_dir)

    if not binary.exists():
        sys.exit(f"firmware binary not found: {binary}")

    post_ota(args.ip, binary)
    print(">> device is rebooting into the new image")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
