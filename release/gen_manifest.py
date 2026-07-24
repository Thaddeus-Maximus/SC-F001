#!/usr/bin/env python3
"""Stage the OTA release payload for SC-F001.

Given a freshly built firmware image, this assembles the files that get
attached to a GitHub Release and writes latest.json — the machine-readable
manifest the Pages site links to and that a future self-updating device would
poll.

Staged into --out (default: dist/):
    SC-F001-<version>.bin   archival, versioned image (the OTA app image)
    SC-F001.bin             stable-name copy, so
                              releases/latest/download/SC-F001.bin
                            is a permanent "latest" URL
    latest.json             manifest: version, url, size, sha256, date, notes

The manifest download URLs are derived from --repo and --tag, so they point at
the release assets this same workflow is about to create.

Stdlib only — no pip install in CI.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

PROJECT = "SC-F001"


def find_image(build_dir: Path, version: str) -> Path:
    """The app image to publish. Prefer the versioned copy the build emits
    (build/SC-F001-<version>.bin); fall back to the stock build/SC-F001.bin."""
    versioned = build_dir / f"{PROJECT}-{version}.bin"
    if versioned.exists():
        return versioned
    plain = build_dir / f"{PROJECT}.bin"
    if plain.exists():
        return plain
    sys.exit(f"no firmware image in {build_dir} "
             f"(looked for {versioned.name} and {plain.name}) — run a build first")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage the SC-F001 OTA release payload")
    ap.add_argument("--version", required=True, help="e.g. 1.1.1")
    ap.add_argument("--repo", required=True, help="owner/name, e.g. Thaddeus-Maximus/SC-F001")
    ap.add_argument("--tag", required=True, help="release tag, e.g. v1.1.1")
    ap.add_argument("--build-dir", default="build")
    ap.add_argument("--out", default="dist")
    ap.add_argument("--min-hw-rev", type=int, default=0,
                    help="lowest board_rev this image supports (advisory)")
    ap.add_argument("--notes-file", default=None, help="optional release-notes markdown")
    args = ap.parse_args()

    build_dir = Path(args.build_dir)
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    image = find_image(build_dir, args.version)
    versioned_name = f"{PROJECT}-{args.version}.bin"
    versioned = out / versioned_name
    stable = out / f"{PROJECT}.bin"
    shutil.copyfile(image, versioned)
    shutil.copyfile(image, stable)

    digest = sha256(versioned)
    size = versioned.stat().st_size
    base = f"https://github.com/{args.repo}/releases"

    manifest = {
        "project": PROJECT,
        "version": args.version,
        "file": versioned_name,
        "size": size,
        "sha256": digest,
        "date": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "min_hw_rev": args.min_hw_rev,
        # Exact-version asset for this release (immutable):
        "url": f"{base}/download/{args.tag}/{versioned_name}",
        # Permanent "latest" aliases — always resolve to the newest release:
        "latest_bin": f"{base}/latest/download/{PROJECT}.bin",
        "latest_manifest": f"{base}/latest/download/latest.json",
        "notes_url": f"{base}/tag/{args.tag}",
    }
    if args.notes_file and Path(args.notes_file).exists():
        manifest["notes"] = Path(args.notes_file).read_text(encoding="utf-8").strip()

    (out / "latest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    print(f"staged {versioned.name} ({size} bytes)")
    print(f"  sha256 {digest}")
    print(f"  manifest -> {out / 'latest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
