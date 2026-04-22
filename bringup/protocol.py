"""Line-oriented protocol over UART for the SC-F001 bring-up procedure.

The firmware side is specified in docs/SC-F001/BRINGUP.md §3.
Commands are prefixed `BU.`; responses are `BU.OK`, `BU.ERR`, `BU.SKIP`,
or streamed `BU.EVENT` lines.
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Callable, Iterator

import serial


LINE_TIMEOUT_S = 2.0


@dataclass
class Response:
    status: str                         # "OK" | "ERR" | "SKIP"
    cmd: str
    fields: dict[str, str] = field(default_factory=dict)
    raw: str = ""

    def get(self, key: str, default: str | None = None) -> str | None:
        return self.fields.get(key, default)

    def getf(self, key: str, default: float = float("nan")) -> float:
        v = self.fields.get(key)
        if v is None:
            return default
        try:
            return float(v)
        except ValueError:
            return default

    def geti(self, key: str, default: int = 0) -> int:
        v = self.fields.get(key)
        if v is None:
            return default
        try:
            return int(v, 0)
        except ValueError:
            return default


@dataclass
class Event:
    cmd: str
    fields: dict[str, str] = field(default_factory=dict)
    raw: str = ""


_KV_RE = re.compile(r'(\w[\w.]*)=("[^"]*"|\S+)')


def _parse_kv(rest: str) -> dict[str, str]:
    out: dict[str, str] = {}
    for m in _KV_RE.finditer(rest):
        k = m.group(1)
        v = m.group(2)
        if v.startswith('"') and v.endswith('"'):
            v = v[1:-1]
        out[k] = v
    return out


def parse_line(line: str) -> Response | Event | None:
    """Returns None for lines that aren't bring-up protocol (boot chatter etc.)."""
    line = line.rstrip("\r\n")
    if not line.startswith("BU."):
        return None
    tokens = line.split(None, 2)
    tag = tokens[0]   # BU.OK | BU.ERR | BU.EVENT | BU.SKIP
    if len(tokens) < 2:
        return None
    cmd = tokens[1]
    rest = tokens[2] if len(tokens) >= 3 else ""
    fields = _parse_kv(rest)
    if tag == "BU.EVENT":
        return Event(cmd=cmd, fields=fields, raw=line)
    status = tag.removeprefix("BU.")
    if status in ("OK", "ERR", "SKIP"):
        return Response(status=status, cmd=cmd, fields=fields, raw=line)
    return None


class Link:
    """Wraps a serial.Serial with line I/O + protocol parsing."""

    def __init__(self, port: str, baud: int = 115200, transcript: Callable[[str], None] | None = None):
        # Don't let pyserial auto-assert DTR/RTS on open. ESP32 dev boards
        # tie those into the BOOT/EN transistor pair — default-asserted lines
        # hold the chip in reset for as long as the port is open, which
        # silently blocks every command we send.
        self.ser = serial.Serial()
        self.ser.port     = port
        self.ser.baudrate = baud
        self.ser.timeout  = LINE_TIMEOUT_S
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        # After open, re-assert False (some platforms override on open).
        self.ser.dtr = False
        self.ser.rts = False
        self.transcript = transcript or (lambda _s: None)
        self._buf = b""

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    def _readline(self, deadline: float) -> str | None:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            if b"\n" in self._buf:
                line, _, self._buf = self._buf.partition(b"\n")
                s = line.decode("utf-8", errors="replace")
                self.transcript(f"<- {s}")
                return s
            self.ser.timeout = min(remaining, 0.5)
            chunk = self.ser.read(256)
            if chunk:
                self._buf += chunk

    def send(self, cmd: str) -> None:
        if not cmd.endswith("\n"):
            cmd = cmd + "\n"
        self.transcript(f"-> {cmd.rstrip()}")
        self.ser.write(cmd.encode("utf-8"))
        self.ser.flush()

    def request(self, cmd: str, overall_timeout_s: float = 5.0) -> Response:
        """Send a command and collect lines until a terminating OK/ERR/SKIP."""
        self.send(cmd)
        deadline = time.monotonic() + overall_timeout_s
        while True:
            line = self._readline(deadline)
            if line is None:
                raise TimeoutError(f"No terminating response to {cmd!r}")
            parsed = parse_line(line)
            if isinstance(parsed, Response):
                return parsed
            # Events during a non-streaming command are unexpected but not fatal
            # — swallow them and keep reading.

    def wait_ready(self, cmd: str = "BU.BEGIN",
                   per_attempt_s: float = 1.5,
                   overall_timeout_s: float = 30.0,
                   show_boot_chatter: bool = True) -> "Response":
        """Send `cmd` repeatedly until we get a Response back.

        Used once at the start of a session to ride out the boot/init time
        before uart_comms installs the UART driver — bytes sent earlier are
        dropped by the hardware FIFO and never reach the firmware.

        When `show_boot_chatter` is True (default), non-protocol lines
        (ESP_LOG output, boot banner) are printed to stdout so the operator
        can see what the device is actually doing while we wait.
        """
        deadline = time.monotonic() + overall_timeout_s
        last_err: Exception | None = None
        attempt = 0
        while time.monotonic() < deadline:
            attempt += 1
            remaining = deadline - time.monotonic()
            print(f"    [wait_ready] attempt {attempt}, {remaining:.0f}s left")
            self.send(cmd)
            per_deadline = time.monotonic() + per_attempt_s
            while True:
                line = self._readline(per_deadline)
                if line is None:
                    last_err = TimeoutError(f"no response to {cmd!r}")
                    break
                parsed = parse_line(line)
                if isinstance(parsed, Response):
                    return parsed
                if parsed is None and show_boot_chatter:
                    stripped = line.rstrip("\r\n")
                    if stripped:
                        print(f"    [uart] {stripped}")
        raise TimeoutError(
            f"Device never answered {cmd!r} within {overall_timeout_s:.0f}s "
            f"(last: {last_err})"
        )

    def request_stream(
        self, cmd: str, overall_timeout_s: float
    ) -> Iterator[Event | Response]:
        """Yield each Event, then the terminating Response."""
        self.send(cmd)
        deadline = time.monotonic() + overall_timeout_s
        while True:
            line = self._readline(deadline)
            if line is None:
                raise TimeoutError(f"Timed out during streaming {cmd!r}")
            parsed = parse_line(line)
            if parsed is None:
                continue
            yield parsed
            if isinstance(parsed, Response):
                return
