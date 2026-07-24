# Releasing SC-F001 firmware

Firmware is built and released **locally** — GitHub Actions never builds or
flashes anything. One script does the whole release; a tiny Pages workflow only
regenerates the download index afterward.

```
idf.py build                 # build the image (your machine / ESP-IDF shell)
./release/release.sh         # package + tag + publish the GitHub Release
```

## What `release.sh` does (all local)

1. Reads the version from `main/version.txt` (the single source of truth) and
   forms the tag `vX.Y.Z`. Refuses if that tag already exists.
2. Checks `build/SC-F001-<version>.bin` exists (fails with a nudge to build).
3. `gen_manifest.py` stages `dist/`:
   - `SC-F001-<version>.bin` — archival, versioned image
   - `SC-F001.bin` — stable-name copy (permanent "latest" URL)
   - `latest.json` — manifest: version, url, size, sha256, date
4. Commits tracked changes, tags, and pushes `main` + tag to `origin`
   (gitea **and** github via the dual push URLs).
5. `gh release create` publishes the GitHub Release with those three assets.
6. `gh workflow run pages.yml --ref main` kicks the Pages rebuild.

## What the Action does (`.github/workflows/pages.yml`)

Nothing but the site. It's **dispatch-only** — `release.sh` triggers it after
publishing, and you can run it by hand from the Actions tab. It regenerates
`index.html` from the repo's Releases with `gen_pages.py` and deploys to GitHub
Pages. Pages deployment genuinely has to run in Actions (OIDC + the Pages
environment) — that's the only reason any CI exists here. No firmware, no build.

Dispatching from `main` (rather than deploying off the release tag) keeps the
run inside the `github-pages` environment's default deploy policy, so no tag has
to be whitelisted.

## URLs the release produces

| Purpose | URL |
| --- | --- |
| Human index (Pages) | `https://thaddeus-maximus.github.io/SC-F001/` |
| Latest image (stable) | `https://github.com/Thaddeus-Maximus/SC-F001/releases/latest/download/SC-F001.bin` |
| Latest manifest | `.../releases/latest/download/latest.json` |
| A specific version | `.../releases/download/v1.1.2/SC-F001-1.1.2.bin` |

The stable latest URLs never change — they always resolve to the newest
release. That's what a future self-updating device polls, and what you hand to
anyone who just needs "the current firmware."

## Flashing

Download the `.bin` from the Pages site (or the latest URL) and upload it via
the device web UI: **DANGER ZONE → Upload Firmware**. For a dev-loop push
straight to a device on the bench, `ota_deploy.py` still does build + upload.

## One-time setup

Release/mirror repo: `git@github.com:Thaddeus-Maximus/SC-F001.git`.

1. **gh CLI**, authed to GitHub (this is how the release is published):
   ```
   gh auth login
   ```
2. **Dual push** so `git push origin` reaches gitea + GitHub — see the SSH +
   remote steps you already ran (also in the scratch notes).
3. **Enable Pages:** GitHub repo **Settings → Pages → Source: GitHub Actions**.
4. The Pages workflow uses the built-in token; no secrets.

Override the repo the manifest/release point at with `SC_REPO=owner/name` if it
ever moves.

## Cutting a release, step by step

1. Land changes on `main`, bump `main/version.txt` (semver — see
   `main/version.cmake`).
2. Optionally write `release/notes/<version>.md`.
3. Build, then release:
   ```
   idf.py build
   ./release/release.sh                 # or: ./release/release.sh "notes..."
   ```
4. The Release appears on GitHub with the `.bin`; the Pages index refreshes
   automatically.

## Follow-ups worth doing (not yet wired)

- **OTA image guard (firmware).** `webserver.c`'s `/ota` handler validates the
  image but not its identity. Before circulating binaries from a URL, read
  `esp_app_desc` from the incoming image and reject a wrong `project_name`
  (and optionally block downgrades).
- **Automatic OTA.** The device fetches `latest.json`, compares `version`
  against its own `esp_app_desc`, and `esp_https_ota`'s the `url` when newer.
  The manifest is already the right shape; this becomes mostly firmware +
  bundling a CA cert for the HTTPS fetch.
- **Stop committing binaries to git.** `SC-F001-released.bin` and the loose
  `*_*.bin` snapshots predate this pipeline; Releases hold images now.
