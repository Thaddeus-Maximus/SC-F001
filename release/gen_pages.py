#!/usr/bin/env python3
"""Generate the SC-F001 firmware release site (a single static index.html).

Reads the repository's GitHub Releases and emits one page listing every release
newest-first, with a prominent "download the latest image" button and a link to
each release's machine-readable manifest (latest.json). No framework, no build
step — just the GitHub REST API and the standard library.

Run in CI with GH_TOKEN set (higher rate limit); works locally without a token
against public repos too.

    python release/gen_pages.py --repo Thaddeus-Maximus/SC-F001 --out site
"""
from __future__ import annotations

import argparse
import html
import json
import os
import sys
import urllib.error
import urllib.request
from datetime import datetime
from pathlib import Path

API = "https://api.github.com"
PROJECT = "SC-F001"


def api_get(url: str, token: str | None):
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "sc-f001-pages",
    })
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode("utf-8")), r.headers


def all_releases(repo: str, token: str | None) -> list:
    releases: list = []
    url = f"{API}/repos/{repo}/releases?per_page=100"
    while url:
        page, headers = api_get(url, token)
        releases.extend(page)
        url = None
        for part in headers.get("Link", "").split(","):
            if 'rel="next"' in part:
                url = part[part.find("<") + 1:part.find(">")]
    return releases


def fmt_size(n: int) -> str:
    return f"{n / 1048576:.2f} MB" if n >= 1048576 else f"{n / 1024:.1f} KB"


def fmt_date(iso: str) -> str:
    try:
        return datetime.strptime(iso, "%Y-%m-%dT%H:%M:%SZ").strftime("%d %b %Y")
    except (ValueError, TypeError):
        return iso or ""


def pick_asset(assets: list, suffix: str, prefix: str = "") -> dict | None:
    for a in assets:
        if a["name"].startswith(prefix) and a["name"].endswith(suffix):
            return a
    return None


PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SC-F001 Firmware</title>
<style>
  :root {{ --bg:#1a1a1a; --surface:#242424; --surface-2:#333; --text:#f0f0f0;
           --muted:#9a9a9a; --accent:#ba965b; --border:#3a3a3a; }}
  * {{ box-sizing:border-box; }}
  body {{ margin:0; padding:24px; background:var(--bg); color:var(--text);
          font-family:system-ui,-apple-system,Segoe UI,Roboto,sans-serif; line-height:1.5; }}
  main {{ max-width:820px; margin:0 auto; }}
  h1 {{ margin:0 0 4px; font-size:1.6rem; }}
  h1 .accent {{ color:var(--accent); }}
  .sub {{ color:var(--muted); margin:0 0 24px; font-size:0.9rem; }}
  .latest {{ background:var(--surface); border:1px solid var(--border);
             border-left:4px solid var(--accent); border-radius:8px;
             padding:16px 20px; margin-bottom:28px; }}
  .latest .ver {{ font-size:1.2rem; font-weight:700; }}
  .btn {{ display:inline-block; background:var(--accent); color:#1a1a1a;
          text-decoration:none; font-weight:700; padding:10px 18px;
          border-radius:6px; margin-top:10px; }}
  .btn.secondary {{ background:transparent; color:var(--accent);
                    border:1px solid var(--accent); margin-left:8px; }}
  table {{ width:100%; border-collapse:collapse; }}
  th, td {{ text-align:left; padding:10px 12px; border-bottom:1px solid var(--border);
            vertical-align:top; }}
  th {{ color:var(--muted); font-weight:600; font-size:0.8rem; text-transform:uppercase;
        letter-spacing:0.03em; }}
  td a {{ color:var(--accent); }}
  code {{ background:var(--surface-2); padding:1px 6px; border-radius:4px;
          font-size:0.85em; word-break:break-all; }}
  details {{ margin-top:6px; }}
  summary {{ cursor:pointer; color:var(--muted); font-size:0.85rem; }}
  pre {{ background:var(--surface); border:1px solid var(--border); border-radius:6px;
         padding:12px; overflow-x:auto; font-size:0.82rem; white-space:pre-wrap; }}
  footer {{ margin-top:32px; color:var(--muted); font-size:0.8rem; }}
  @media (prefers-color-scheme: light) {{
    :root {{ --bg:#faf9f7; --surface:#fff; --surface-2:#eee; --text:#1a1a1a;
             --muted:#666; --border:#ddd; }}
    .btn {{ color:#fff; }}
  }}
</style>
</head>
<body>
<main>
  <h1>SC-F001 <span class="accent">Firmware</span></h1>
  <p class="sub">Release archive. Download an image, then upload it to the
     device via <strong>DANGER ZONE &rarr; Upload Firmware</strong>.</p>
  {latest}
  {table}
  <footer>{footer}</footer>
</main>
</body>
</html>
"""


def render(repo: str, releases: list) -> str:
    latest_bin = f"https://github.com/{repo}/releases/latest/download/{PROJECT}.bin"
    latest_manifest = f"https://github.com/{repo}/releases/latest/download/latest.json"

    published = [r for r in releases if not r.get("draft")]
    published.sort(key=lambda r: r.get("published_at") or "", reverse=True)

    if not published:
        latest_html = ('<div class="latest"><div class="ver">No releases yet</div>'
                       '<p class="sub">Push a <code>v*</code> tag to publish the first one.</p></div>')
        table_html = ""
    else:
        newest = published[0]
        latest_html = (
            f'<div class="latest">'
            f'<div class="ver">Latest &middot; {html.escape(newest["tag_name"])}</div>'
            f'<div class="sub">{fmt_date(newest.get("published_at"))}</div>'
            f'<a class="btn" href="{html.escape(latest_bin)}">Download latest firmware</a>'
            f'<a class="btn secondary" href="{html.escape(latest_manifest)}">latest.json</a>'
            f'</div>'
        )

        rows = []
        for r in published:
            binasset = pick_asset(r.get("assets", []), ".bin", f"{PROJECT}-")
            size = fmt_size(binasset["size"]) if binasset else "—"
            dl = (f'<a href="{html.escape(binasset["browser_download_url"])}">'
                  f'{html.escape(binasset["name"])}</a>') if binasset else "—"
            notes = (r.get("body") or "").strip()
            notes_html = ""
            if notes:
                notes_html = (f'<details><summary>notes</summary>'
                              f'<pre>{html.escape(notes)}</pre></details>')
            rows.append(
                f"<tr><td><strong>{html.escape(r['tag_name'])}</strong></td>"
                f"<td>{fmt_date(r.get('published_at'))}</td>"
                f"<td>{size}</td>"
                f"<td>{dl}{notes_html}</td></tr>"
            )
        table_html = (
            "<table><thead><tr><th>Version</th><th>Date</th><th>Size</th>"
            "<th>Image / notes</th></tr></thead><tbody>"
            + "".join(rows) + "</tbody></table>"
        )

    footer = (f'<a href="https://github.com/{html.escape(repo)}">{html.escape(repo)}</a>'
              f' &middot; generated {datetime.utcnow().strftime("%Y-%m-%d %H:%M UTC")}')
    return PAGE.format(latest=latest_html, table=table_html, footer=footer)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the SC-F001 release Pages site")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"),
                    help="owner/name (defaults to $GITHUB_REPOSITORY)")
    ap.add_argument("--out", default="site")
    args = ap.parse_args()
    if not args.repo:
        sys.exit("--repo is required (or set GITHUB_REPOSITORY)")

    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    try:
        releases = all_releases(args.repo, token)
    except urllib.error.HTTPError as e:
        sys.exit(f"GitHub API error {e.code}: {e.reason}")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "index.html").write_text(render(args.repo, releases), encoding="utf-8")
    # Disable Jekyll so the static file is served verbatim.
    (out / ".nojekyll").write_text("", encoding="utf-8")
    print(f"wrote {out / 'index.html'} ({len(releases)} releases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
