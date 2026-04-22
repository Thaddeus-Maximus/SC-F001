"""Wrap esptool to flash the SC-F001 firmware from a build directory.

Reads `build/flasher_args.json` (produced by `idf.py build`) to get the
authoritative list of offsets + binaries, then invokes esptool once with all
of them — no hardcoded offsets here.

Requires esptool reachable either as a Python module (`pip install esptool`)
or as `esptool.py` on PATH (e.g., from an ESP-IDF activation).
"""

from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path


class FlashError(RuntimeError):
    pass


def _default_build_dir() -> Path:
    # bringup dir sits at SC-F001/bringup; build sits at SC-F001/build
    return Path(__file__).resolve().parent.parent / "build"


def _resolve_esptool_invocation() -> list[str]:
    """Return the command prefix to run esptool, preferring the installed
    module in the current interpreter, falling back to esptool.py on PATH.

    Raises FlashError with an actionable message if neither is available.
    """
    if importlib.util.find_spec("esptool") is not None:
        return [sys.executable, "-m", "esptool"]
    fallback = shutil.which("esptool.py") or shutil.which("esptool")
    if fallback:
        return [fallback]
    raise FlashError(
        "esptool is not installed in this Python and not on PATH.\n"
        "  Install with:  "
        f"{sys.executable} -m pip install -r "
        f"{Path(__file__).parent / 'requirements.txt'}\n"
        "  Or activate an ESP-IDF shell that provides esptool.py."
    )


def _load_manifest(build_dir: Path) -> dict:
    manifest = build_dir / "flasher_args.json"
    if not manifest.exists():
        raise FlashError(
            f"Build manifest not found: {manifest}\n"
            f"Run `idf.py build` from SC-F001/ first, or pass --build-dir."
        )
    return json.loads(manifest.read_text())


def _resolve_flash_args(build_dir: Path, manifest: dict) -> list[str]:
    """Expand manifest into a (offset, abs-path) list suitable for esptool."""
    args: list[str] = []
    # flasher_args.json's flash_files is {offset_hex: relpath}.
    for offset_hex, rel in manifest["flash_files"].items():
        p = (build_dir / rel).resolve()
        if not p.exists():
            raise FlashError(f"Missing firmware binary: {p}")
        args.append(offset_hex)
        args.append(str(p))
    return args


def flash(
    port: str,
    build_dir: Path | None = None,
    baud: int = 460800,
    erase_all: bool = False,
    log: callable = print,
) -> None:
    build_dir = (build_dir or _default_build_dir()).resolve()
    manifest = _load_manifest(build_dir)

    chip = manifest.get("extra_esptool_args", {}).get("chip", "esp32")
    before = manifest.get("extra_esptool_args", {}).get("before", "default_reset")
    after  = manifest.get("extra_esptool_args", {}).get("after", "hard_reset")

    esptool_cmd = _resolve_esptool_invocation()
    base_cmd = esptool_cmd + [
        "--chip", chip,
        "--port", port,
        "--baud", str(baud),
        "--before", before,
        "--after", after,
    ]

    if erase_all:
        log(f"  erase_flash @ {port}")
        subprocess.check_call(base_cmd + ["erase_flash"])

    write_args = manifest.get("write_flash_args", [])
    cmd = base_cmd + ["write_flash"] + write_args + _resolve_flash_args(build_dir, manifest)

    log(f"  flashing from {build_dir}")
    log(f"  files: {list(manifest['flash_files'].items())}")
    try:
        subprocess.check_call(cmd)
    except subprocess.CalledProcessError as e:
        raise FlashError(f"esptool failed (exit {e.returncode})") from e
