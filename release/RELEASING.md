# Releasing SC-F001 firmware

This repo publishes firmware through **GitHub Releases** (the binaries) and a
**GitHub Pages** site (the human-facing index). Both are produced automatically
by `.github/workflows/release.yml` when you push a version tag.

```
git tag v1.1.2 && git push origin v1.1.2
```

That's the whole release action. Everything below is context and one-time setup.

## What a tag push does

1. **Verifies** the tag matches `main/version.txt` (the single source of truth),
   so `v1.1.2` and `version.txt: 1.1.2` can never drift.
2. **Builds** the firmware with ESP-IDF v5.3.1 (target esp32).
3. **Stages** the OTA payload with `release/gen_manifest.py`:
   - `SC-F001-<version>.bin` — the archival, versioned app image
   - `SC-F001.bin` — a stable-name copy (see the permanent URLs below)
   - `latest.json` — manifest with `version`, `url`, `size`, `sha256`, `date`
4. **Publishes** a GitHub Release with those three assets. Release notes come
   from `release/notes/<version>.md` if present, else auto-generated from
   commits.
5. **Regenerates** the Pages site with `release/gen_pages.py` (lists every
   release, newest first) and deploys it.

## URLs the release produces

| Purpose | URL |
| --- | --- |
| Human index (Pages) | `https://thaddeus-maximus.github.io/SC-F001/` |
| Latest image (stable) | `https://github.com/Thaddeus-Maximus/SC-F001/releases/latest/download/SC-F001.bin` |
| Latest manifest | `https://github.com/Thaddeus-Maximus/SC-F001/releases/latest/download/latest.json` |
| A specific version | `.../releases/download/v1.1.2/SC-F001-1.1.2.bin` |

The **stable latest** URLs never change — they always resolve to the newest
release. That's what a future self-updating device polls, and what you hand to
anyone who just needs "the current firmware."

## Flashing (today)

Download the `.bin` from the Pages site (or the latest URL) and upload it via
the device web UI: **DANGER ZONE → Upload Firmware**. For a dev-loop push
straight to a device on the bench, `ota_deploy.py` still does build + upload
over HTTP.

## One-time repo setup

The release/mirror lives on GitHub at `git@github.com:Thaddeus-Maximus/SC-F001.git`.

1. **Mirror or move to GitHub.** Development can stay on the gitea origin; add
   GitHub as a push mirror so CI runs there. From a normal shell (this is a git
   change — do it yourself, not via the assistant):
   ```
   git remote add github git@github.com:Thaddeus-Maximus/SC-F001.git
   git push github main --tags
   ```
   Or configure gitea's built-in **Settings → Repository → Mirror** to push to
   the GitHub URL on a schedule.
2. **Enable Pages:** GitHub repo **Settings → Pages → Build and deployment →
   Source: GitHub Actions**. (First `deploy-pages` run needs this on.)
3. Nothing else — the workflow uses the built-in `GITHUB_TOKEN`; no secrets.

## Cutting a release, step by step

1. Land your changes on `main`.
2. Bump `main/version.txt` (semver — see the guidance in `main/version.cmake`).
3. Optionally write `release/notes/<version>.md`.
4. Commit, then tag and push:
   ```
   git commit -am "release 1.1.2"
   git tag v1.1.2
   git push origin main --tags        # and to the github mirror if separate
   ```
5. Watch the **release** workflow. When it's green, the new `.bin` is on the
   Pages site and at the latest URL.

## Follow-ups worth doing (not yet wired)

- **OTA image guard (firmware).** `webserver.c`'s `/ota` handler validates the
  image but not its identity. Before circulating binaries from a URL, read
  `esp_app_desc` from the incoming image and reject a wrong `project_name`
  (and optionally block downgrades). Cheap insurance against a mis-pasted link.
- **Automatic OTA.** The device fetches `latest.json`, compares `version`
  against its own `esp_app_desc`, and `esp_https_ota`'s the `url` when newer.
  The manifest is already the right shape; this becomes mostly firmware +
  bundling a CA cert for the HTTPS fetch.
- **Stop committing binaries to git.** `SC-F001-released.bin` and the loose
  `*_*.bin` snapshots in the repo predate this pipeline; Releases hold images
  now, so those can be removed to keep history from bloating.
