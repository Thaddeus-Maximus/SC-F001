"""Terminal formatting helpers for the bring-up tool.

Thin wrapper around ANSI escapes so stages.py / bringup.py can emit
consistently-styled headings, prompts, status tags, and result
summaries without sprinkling raw escape codes everywhere.

Colors auto-disable when stdout is not a TTY or when the `NO_COLOR`
environment variable is set (see no-color.org).
"""

from __future__ import annotations

import os
import sys


def _color_supported() -> bool:
    if os.environ.get("NO_COLOR") is not None:
        return False
    if not sys.stdout.isatty():
        return False
    # Modern Windows 10+ terminals support VT100 once any ANSI sequence has
    # been emitted — a no-op system("") call flips the flag on cmd.exe.
    if os.name == "nt":
        try:
            os.system("")
        except Exception:
            pass
    return True


_USE = _color_supported()


def _c(code: str) -> str:
    return f"\x1b[{code}m" if _USE else ""


RESET   = _c("0")
BOLD    = _c("1")
DIM     = _c("2")

RED     = _c("31")
GREEN   = _c("32")
YELLOW  = _c("33")
BLUE    = _c("34")
MAGENTA = _c("35")
CYAN    = _c("36")


def stage(title: str) -> str:
    """Big block heading that opens a stage."""
    bar = "-" * 60
    return (
        f"\n{CYAN}{bar}{RESET}\n"
        f"{BOLD}{CYAN}  {title}{RESET}\n"
        f"{CYAN}{bar}{RESET}"
    )


def section(title: str) -> str:
    """Smaller sub-heading inside a stage."""
    return f"\n{BOLD}{MAGENTA}-- {title} --{RESET}"


def prompt(text: str) -> str:
    return f"{YELLOW}{text}{RESET}"


def tag(label: str, color: str) -> str:
    return f"[{color}{BOLD}{label}{RESET}]"


OK_TAG   = tag("OK",   GREEN)
ERR_TAG  = tag("ERR",  RED)
SKIP_TAG = tag("SKIP", YELLOW)
WARN_TAG = tag("WARN", YELLOW)
INFO_TAG = tag("INFO", BLUE)
EVT_TAG  = tag("EVT",  CYAN)


def status_tag(status: str) -> str:
    s = (status or "").upper()
    return {
        "OK":   OK_TAG,
        "ERR":  ERR_TAG,
        "SKIP": SKIP_TAG,
        "WARN": WARN_TAG,
    }.get(s, tag(s or "?", DIM))


def fail(text: str) -> str:
    return f"{RED}{BOLD}{text}{RESET}"


def pass_(text: str) -> str:
    return f"{GREEN}{BOLD}{text}{RESET}"


def warn(text: str) -> str:
    return f"{YELLOW}{text}{RESET}"


def dim(text: str) -> str:
    return f"{DIM}{text}{RESET}"


_ANSI_RE = None


def strip(text: str) -> str:
    """Return `text` with all ANSI escape sequences removed.

    Used by the transcript writer so log files don't contain `\x1b[...m`
    garbage when stdout is colored.
    """
    global _ANSI_RE
    if _ANSI_RE is None:
        import re
        _ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
    return _ANSI_RE.sub("", text)


def summary_line(passed: int, failed: int, warnings: int, skipped: int) -> str:
    color = GREEN if failed == 0 else RED
    return (f"  {color}{BOLD}pass={passed}{RESET}  "
            f"{RED if failed else DIM}{BOLD}fail={failed}{RESET}  "
            f"{YELLOW if warnings else DIM}warn={warnings}{RESET}  "
            f"{DIM}skip={skipped}{RESET}")
