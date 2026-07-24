#!/usr/bin/env bash
# Cut a firmware release — entirely locally.
#
# The firmware is built on YOUR machine; GitHub Actions never builds it. This
# script packages the already-built image, pushes the tag, and publishes a
# GitHub Release with the .bin + manifest. A tiny Actions workflow then only
# regenerates the Pages index from the published release.
#
# Prereqs (one-time):
#   - `idf.py build` has produced build/SC-F001-<version>.bin  (run it first)
#   - gh CLI installed and authed to GitHub:  gh auth login
#   - origin pushes to gitea + github (see RELEASING.md)
#
# Usage (Git Bash, from anywhere in the repo):
#   ./release/release.sh              # notes auto-generated from commits
#   ./release/release.sh "notes..."   # custom commit message + release notes
set -euo pipefail

REPO="${SC_REPO:-Thaddeus-Maximus/SC-F001}"

# Repo root from this script's location (it lives in release/), so it works
# from any subdirectory.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

ver="$(head -n1 main/version.txt | tr -d '[:space:]')"
[ -n "$ver" ] || { echo "error: main/version.txt is empty" >&2; exit 1; }
tag="v$ver"
msg="${1:-$tag}"

command -v gh >/dev/null || {
  echo "error: gh CLI not found — needed to publish the release." >&2
  echo "       Install it, then: gh auth login" >&2
  exit 1
}

if [ ! -f "build/SC-F001-$ver.bin" ] && [ ! -f "build/SC-F001.bin" ]; then
  echo "error: no firmware image in build/ for $ver — run 'idf.py build' first." >&2
  exit 1
fi

if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
  echo "error: tag $tag already exists." >&2
  echo "       Bump main/version.txt, or delete the tag first:" >&2
  echo "         git push origin :$tag && git tag -d $tag" >&2
  exit 1
fi

echo ">> packaging $tag"
python release/gen_manifest.py \
  --version "$ver" --repo "$REPO" --tag "$tag" --build-dir build --out dist

echo ">> committing + tagging"
if git commit -am "$msg"; then :; else echo ">> nothing to commit; tagging existing HEAD"; fi
git tag -a "$tag" -m "$msg"
git push origin main --tags        # both remotes; puts the tag on GitHub for --verify-tag

echo ">> publishing GitHub release"
notes_args=(--generate-notes)
if [ -f "release/notes/$ver.md" ]; then
  notes_args=(--notes-file "release/notes/$ver.md")
fi
if [ "$msg" != "$tag" ]; then
  notes_args=(--notes "$msg")
fi
gh release create "$tag" --repo "$REPO" --verify-tag --title "SC-F001 $ver" \
  "${notes_args[@]}" \
  "dist/SC-F001-$ver.bin" "dist/SC-F001.bin" "dist/latest.json"

echo ">> done: $tag published — the Pages index refreshes via the release workflow."
