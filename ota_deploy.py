#!/usr/bin/env python3
"""SC-F001 build + OTA deploy.

Runs `idf.py build` and POSTs the resulting firmware binary to the device's
/ota endpoint. Progress is printed as the upload streams.

Builds produce a versioned image, build/SC-F001-<version>.bin, where the
version comes from main/version.txt. By default this deploys the latest one;
--version pulls a specific archived build instead.

Usage:
    python ota_deploy.py [--ip 192.168.4.1] [--no-build]
    python ota_deploy.py --version 1.0.0     # re-deploy an older image
    python ota_deploy.py --list              # show what's in the build dir

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
VERSION_FILE        = Path(__file__).resolve().parent / "main" / "version.txt"
BIN_PREFIX          = f"{DEFAULT_PROJECT}-"   # -> SC-F001-<version>.bin
OTA_PATH            = "/ota"
CONNECT_TIMEOUT_S   = 10.0
UPLOAD_TIMEOUT_S    = 120.0
CHUNK_BYTES         = 4096


def current_version() -> str | None:
    """The version the next build will produce — main/version.txt, the single
    source of truth that CMake also reads."""
    try:
        first = VERSION_FILE.read_text(encoding="utf-8").strip().splitlines()
    except OSError:
        return None
    return first[0].strip() if first else None


def _semver_key(version: str) -> tuple:
    """Sort key for a MAJOR.MINOR.PATCH string. Anything unparseable sorts
    below every well-formed version rather than blowing up the comparison."""
    try:
        return (1, tuple(int(p) for p in version.split(".")))
    except ValueError:
        return (0, ())


def available_versions(build_dir: Path) -> list[str]:
    """Versions present in the build dir, oldest first."""
    names = [p.name[len(BIN_PREFIX):-len(".bin")]
             for p in build_dir.glob(f"{BIN_PREFIX}*.bin")]
    return sorted(names, key=_semver_key)


def resolve_binary(build_dir: Path, requested: str | None, project: str) -> tuple[Path, str]:
    """Pick the image to upload. Returns (path, why) so the choice is printed
    rather than silently guessed at."""
    if requested:
        binary = build_dir / f"{BIN_PREFIX}{requested}.bin"
        if not binary.exists():
            have = available_versions(build_dir)
            sys.exit(f"no image for version {requested}: {binary}\n"
                     f"available: {', '.join(have) if have else '(none)'}")
        return binary, f"requested version {requested}"

    # Default: whatever version.txt says, which is what a build just produced.
    version = current_version()
    if version:
        binary = build_dir / f"{BIN_PREFIX}{version}.bin"
        if binary.exists():
            return binary, f"version {version} (main/version.txt)"

    # version.txt's image isn't there (e.g. --no-build after a clean): fall
    # back to the highest version actually present.
    have = available_versions(build_dir)
    if have:
        return build_dir / f"{BIN_PREFIX}{have[-1]}.bin", f"latest in build dir ({have[-1]})"

    # Pre-versioning build dir.
    legacy = build_dir / f"{project}.bin"
    if legacy.exists():
        return legacy, "unversioned image (build predates versioned output)"

    sys.exit(f"no firmware binary found in {build_dir} — run a build first")


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
    ap.add_argument("--version",     default=None,
                    help="deploy a specific archived version (default: the latest build)")
    ap.add_argument("--list",        action="store_true",      help="list versions in the build dir and exit")
    args = ap.parse_args()

    project_dir = Path(__file__).resolve().parent
    build_dir   = Path(args.build_dir).resolve()

    if args.list:
        have = available_versions(build_dir)
        current = current_version()
        print(f"main/version.txt: {current or '(unreadable)'}")
        print(f"images in {build_dir}:")
        for v in have:
            print(f"    {BIN_PREFIX}{v}.bin{'   <- current' if v == current else ''}")
        if not have:
            print("    (none)")
        return 0

    do_build = not args.no_build
    if args.version and args.version != current_version() and do_build:
        # Building would just produce version.txt's version, not the one asked
        # for, and would then upload the wrong image.
        print(f">> --version {args.version} differs from main/version.txt "
              f"({current_version()}); skipping build, using the archived image")
        do_build = False

    if do_build:
        run_build(project_dir)

    binary, why = resolve_binary(build_dir, args.version, args.project)
    print(f">> selected {binary.name}  [{why}]")

    post_ota(args.ip, binary)
    print(">> device is rebooting into the new image")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
