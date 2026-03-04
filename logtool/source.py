"""
Data sources for SC-F001 logtool.
Supports local .bin files and HTTP /log endpoint (full GET + incremental POST).
"""

import struct
import time
from pathlib import Path

try:
    import requests
    _REQUESTS_OK = True
except ImportError:
    _REQUESTS_OK = False


def read_file(path: str) -> bytes:
    return Path(path).read_bytes()


def _check_requests():
    if not _REQUESTS_OK:
        raise ImportError("'requests' package required for HTTP mode. Install: pip install requests")


def fetch_full(url: str, timeout: float = 10.0) -> bytes:
    """GET /log — returns full response blob."""
    _check_requests()
    resp = requests.get(url, timeout=timeout)
    resp.raise_for_status()
    return resp.content


def fetch_incremental(url: str, tail: int, timeout: float = 10.0) -> tuple:
    """
    POST /log with tail offset.
    Returns (raw_binary: bytes, new_head: int).
    The raw_binary starts at the given tail and ends at new_head.
    """
    _check_requests()
    resp = requests.post(url, data=str(tail), timeout=timeout)
    resp.raise_for_status()
    blob = resp.content

    # Response is the same format: [4B json_len BE][JSON][4B tail BE][4B head BE][binary]
    import struct as _s
    import json as _j
    if len(blob) < 8:
        return b'', tail

    json_len = _s.unpack_from('>I', blob, 0)[0]
    if json_len > 65536 or len(blob) < 4 + json_len + 8:
        return b'', tail

    new_tail, new_head = _s.unpack_from('>II', blob, 4 + json_len)
    binary = blob[4 + json_len + 8:]
    return binary, new_head


def stream(url: str, callback, interval_s: float = 2.0):
    """
    Stream new log entries from an HTTP /log endpoint.
    - Fetches the full log on first call (GET).
    - Polls for new entries every `interval_s` seconds (POST with last head as new tail).
    - Calls callback(entries: list, is_first: bool) for each batch.
    - Runs until KeyboardInterrupt.
    """
    from parser import autodetect_and_parse

    _check_requests()

    # Initial full fetch
    blob = fetch_full(url)
    meta, tail, head, entries = autodetect_and_parse(blob)
    callback(entries, meta, is_first=True)

    current_tail = head if head is not None else 0

    # Poll for incremental updates
    while True:
        time.sleep(interval_s)
        try:
            binary, new_head = fetch_incremental(url, current_tail)
            if binary:
                from parser import parse_entries
                new_entries = parse_entries(binary)
                if new_entries:
                    callback(new_entries, None, is_first=False)
            current_tail = new_head
        except Exception as exc:
            print(f"[stream] poll error: {exc}")
