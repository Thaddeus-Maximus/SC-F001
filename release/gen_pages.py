#!/usr/bin/env python3
"""Generate the SC-F001 firmware release site (a single static index.html).

Reads the repository's GitHub Releases and emits one page listing every release
newest-first, with a prominent "download the latest image" button and a link to
each release's machine-readable manifest (latest.json). No framework, no build
step — just the GitHub REST API and the standard library.

The page is styled to match thestockcropper.com (Montserrat, the green/brown/
gold palette, the same header logo and footer) so it drops straight into
update.thestockcropper.com.

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
from datetime import datetime, timezone
from pathlib import Path
from string import Template

API = "https://api.github.com"
PROJECT = "SC-F001"

SITE = "https://thestockcropper.com"
LOGO = f"{SITE}/wp-content/uploads/2024/04/StockCropper-Primary-1-1024x254.png"
FOOTER_LOGO = f"{SITE}/wp-content/uploads/2022/06/sc-footer-logo.png"
FAVICON = f"{SITE}/wp-content/uploads/2023/06/cropped-Stockcropper_favicon-32x32.png"


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


def fetch_manifest(url: str) -> dict:
    """Best-effort read of a release's latest.json asset. Never fatal."""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "sc-f001-pages"})
        with urllib.request.urlopen(req, timeout=30) as r:
            return json.loads(r.read().decode("utf-8"))
    except (urllib.error.URLError, ValueError, OSError):
        return {}


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


PAGE = Template("""<!DOCTYPE html>
<html lang="en-US">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="dark">
<title>SC-F001 Firmware &middot; The Stock Cropper</title>
<meta name="description" content="Download the latest SC-F001 controller firmware for the ClusterCluck Drive.">
<link rel="icon" href="$favicon" sizes="32x32">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet"
      href="https://fonts.googleapis.com/css2?family=Montserrat:wght@400;600;700&display=swap">
<style>
  /* Dark throughout — neutral grays, not the brand's green. Typography and the
     gold/brown accents still come from thestockcropper.com so the page reads as
     part of the site. */
  :root {
    --bg:#1A1A1A;
    --surface:#242424;
    --surface-2:#2C2C2C;
    --border:#3A3A3A;
    --text:#F0F0F0;
    --body:#C2C2C2;
    --muted:#8E8E8E;
    --gold:#BA965B;
    --gold-lift:#CFAC72;
    --brown:#926538;
    --code:#1E1E1E;
  }
  * { box-sizing:border-box; }
  html { -webkit-text-size-adjust:100%; }
  body {
    margin:0; background:var(--bg); color:var(--body);
    font-family:"Montserrat",-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
    font-weight:400; font-size:16px; line-height:1.65;
  }
  h1,h2,h3 { font-weight:700; letter-spacing:-0.035em; color:var(--text); margin:0; }
  a { color:var(--gold); text-decoration:none; }
  a:hover { color:var(--gold-lift); }
  .wrap { width:92%; max-width:940px; margin:0 auto; }

  /* ---- header: logo only ---- */
  .site-head { border-bottom:1px solid var(--border); padding:20px 0; }
  .site-head img { width:206px; max-width:56vw; height:auto; display:block;
                   filter:brightness(0) invert(1); opacity:.9; }
  .site-head a:hover img { opacity:1; }

  /* ---- hero ---- */
  .hero { background:var(--surface); border-bottom:4px solid var(--gold);
          padding:46px 0 42px; }
  .hero .eyebrow { font-size:.72rem; font-weight:700; letter-spacing:.16em;
                   text-transform:uppercase; color:var(--gold); margin:0 0 10px; }
  .hero h1 { font-size:2.5rem; line-height:1.1; margin:0 0 12px; }
  .hero p { margin:0; max-width:56ch; color:var(--body); font-size:1rem; }

  main { padding:44px 0 8px; }
  section + section { margin-top:40px; }
  .sec-title { font-size:.74rem; font-weight:700; letter-spacing:.16em;
               text-transform:uppercase; color:var(--gold); margin:0 0 14px; }

  /* ---- latest release card ---- */
  .latest {
    background:var(--surface); border:1px solid var(--border);
    border-left:5px solid var(--gold); border-radius:4px; padding:26px 28px;
  }
  .latest .tagline { display:flex; align-items:baseline; gap:12px; flex-wrap:wrap; }
  .latest .ver { font-size:2rem; font-weight:700; letter-spacing:-0.035em;
                 color:var(--text); line-height:1.1; }
  .pill { display:inline-block; background:var(--brown); color:#fff;
          font-size:.62rem; font-weight:700; letter-spacing:.14em;
          text-transform:uppercase; padding:4px 10px; border-radius:2px; }
  .meta { color:var(--muted); font-size:.85rem; margin:6px 0 0; }
  .meta span + span::before { content:"\\00b7"; margin:0 8px; color:var(--border); }
  .actions { margin-top:20px; }

  .btn {
    display:inline-block; font-weight:700; font-size:.78rem; letter-spacing:.09em;
    text-transform:uppercase; padding:13px 26px; border-radius:2px;
    border:3px solid var(--brown); background:var(--brown); color:#fff;
    text-shadow:0 .075em .075em rgba(0,0,0,.5); transition:.15s ease;
  }
  .btn:hover { background:var(--gold); border-color:var(--gold); color:#1A1A1A;
               text-shadow:none; }

  .sha { margin-top:18px; padding-top:16px; border-top:1px solid var(--border); }
  .sha .label { font-size:.66rem; font-weight:700; letter-spacing:.14em;
                text-transform:uppercase; color:var(--muted); }
  .sha code { display:block; margin-top:6px; }
  code { background:var(--code); border:1px solid var(--border); border-radius:2px;
         padding:3px 7px; font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
         font-size:.8rem; color:var(--text); word-break:break-all; }

  /* ---- install steps ---- */
  .steps { display:grid; gap:16px; grid-template-columns:repeat(3,1fr); }
  .step { background:var(--surface); border:1px solid var(--border); border-radius:4px;
          padding:20px 22px; }
  .step .n { display:inline-flex; align-items:center; justify-content:center;
             width:26px; height:26px; border-radius:50%; background:var(--brown);
             color:#fff; font-size:.78rem; font-weight:700; margin-bottom:10px; }
  .step h3 { font-size:.95rem; margin:0 0 4px; }
  .step p { margin:0; font-size:.85rem; color:var(--body); }

  /* ---- history table ---- */
  .tablewrap { background:var(--surface); border:1px solid var(--border);
               border-radius:4px; overflow-x:auto; }
  table { width:100%; border-collapse:collapse; min-width:520px; }
  th, td { text-align:left; padding:14px 18px; border-bottom:1px solid var(--border);
           vertical-align:top; font-size:.88rem; }
  thead th { background:var(--surface-2); color:var(--gold); font-weight:700;
             font-size:.66rem; letter-spacing:.14em; text-transform:uppercase; }
  tbody tr:last-child td { border-bottom:0; }
  tbody tr:hover { background:var(--surface-2); }
  td.ver { font-weight:700; color:var(--text); white-space:nowrap; }
  td.dim { color:var(--muted); white-space:nowrap; }
  details { margin-top:8px; }
  summary { cursor:pointer; color:var(--muted); font-size:.76rem; font-weight:600;
            letter-spacing:.06em; text-transform:uppercase; list-style:none; }
  summary::-webkit-details-marker { display:none; }
  summary::before { content:"\\25b8\\00a0"; color:var(--gold); }
  details[open] summary::before { content:"\\25be\\00a0"; }
  summary:hover { color:var(--gold); }
  pre { background:var(--code); border:1px solid var(--border); border-radius:2px;
        padding:12px 14px; margin:8px 0 0; overflow-x:auto; font-size:.8rem;
        line-height:1.55; white-space:pre-wrap; color:var(--body);
        font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; }
  .empty { background:var(--surface); border:1px solid var(--border); border-radius:4px;
           padding:26px 28px; }

  /* ---- footer ---- */
  .site-foot { margin-top:56px; border-top:1px solid var(--border);
               padding:30px 0 34px; font-size:.78rem; color:var(--muted); }
  .site-foot img { width:150px; height:auto; display:block; margin-bottom:16px;
                   filter:brightness(0) invert(1); opacity:.55; }
  .site-foot a:hover img { opacity:.8; }
  .site-foot .lines { display:flex; justify-content:space-between; gap:8px 24px;
                      flex-wrap:wrap; }

  @media (max-width:720px) {
    .hero { padding:36px 0 32px; }
    .hero h1 { font-size:1.9rem; }
    .latest { padding:22px 20px; }
    .latest .ver { font-size:1.6rem; }
    .steps { grid-template-columns:1fr; }
    .btn { display:block; text-align:center; }
  }
</style>
</head>
<body>

<header class="site-head">
  <div class="wrap">
    <a href="$site"><img src="$logo" alt="The Stock Cropper"></a>
  </div>
</header>

<div class="hero">
  <div class="wrap">
    <p class="eyebrow">SC-F001 Controller</p>
    <h1>Firmware Downloads</h1>
    <p>Every released image for the SC-F001 controller board, newest first.
       Grab the latest build and push it to your ClusterCluck Drive over Wi-Fi.</p>
  </div>
</div>

<main class="wrap">

  <section>
    <h2 class="sec-title">Latest Release</h2>
    $latest
  </section>

  <section>
    <h2 class="sec-title">How to install</h2>
    <div class="steps">
      <div class="step">
        <span class="n">1</span>
        <h3>Download the image</h3>
        <p>Save the <code>.bin</code> file to the phone or laptop you use to
           control the drive.</p>
      </div>
      <div class="step">
        <span class="n">2</span>
        <h3>Connect to the drive</h3>
        <p>Join the controller's Wi-Fi access point and open its web page in a
           browser.</p>
      </div>
      <div class="step">
        <span class="n">3</span>
        <h3>Upload it</h3>
        <p>Open <strong>DANGER ZONE &rarr; Upload Firmware</strong>, pick the
           file, and let the drive reboot itself.</p>
      </div>
    </div>
  </section>

  <section>
    <h2 class="sec-title">Release History</h2>
    $table
  </section>

</main>

<footer class="site-foot">
  <div class="wrap">
    <a href="$site"><img src="$footer_logo" alt="The Stock Cropper"></a>
    <div class="lines">
      <span>Copyright $year The Stock Cropper. All Rights Reserved.</span>
      <span>$footer</span>
    </div>
  </div>
</footer>

</body>
</html>
""")


def render(repo: str, releases: list) -> str:
    latest_bin = f"https://github.com/{repo}/releases/latest/download/{PROJECT}.bin"

    published = [r for r in releases if not r.get("draft")]
    published.sort(key=lambda r: r.get("published_at") or "", reverse=True)

    if not published:
        latest_html = ('<div class="empty"><div class="ver">No releases yet</div>'
                       '<p class="meta">Push a <code>v*</code> tag to publish the '
                       'first one.</p></div>')
        table_html = '<div class="empty">Nothing published yet.</div>'
    else:
        newest = published[0]
        newest_bin = pick_asset(newest.get("assets", []), ".bin", f"{PROJECT}-")

        bits = [f'<span>{fmt_date(newest.get("published_at"))}</span>']
        if newest_bin:
            bits.append(f'<span>{fmt_size(newest_bin["size"])}</span>')
        bits.append(f'<span><a href="{html.escape(newest["html_url"])}">'
                    f'Release notes</a></span>')

        manifest_asset = pick_asset(newest.get("assets", []), "latest.json")
        sha = ""
        if manifest_asset:
            digest = fetch_manifest(manifest_asset["browser_download_url"]).get("sha256")
            if digest:
                sha = (f'<div class="sha"><div class="label">SHA-256</div>'
                       f'<code>{html.escape(digest)}</code></div>')

        latest_html = (
            f'<div class="latest">'
            f'<div class="tagline"><span class="ver">'
            f'{html.escape(newest["tag_name"])}</span>'
            f'<span class="pill">Latest</span></div>'
            f'<p class="meta">{"".join(bits)}</p>'
            f'<div class="actions">'
            f'<a class="btn" href="{html.escape(latest_bin)}">Download firmware</a>'
            f'</div>{sha}</div>'
        )

        rows = []
        for r in published:
            binasset = pick_asset(r.get("assets", []), ".bin", f"{PROJECT}-")
            size = fmt_size(binasset["size"]) if binasset else "&mdash;"
            dl = (f'<a href="{html.escape(binasset["browser_download_url"])}">'
                  f'{html.escape(binasset["name"])}</a>') if binasset else "&mdash;"
            notes = (r.get("body") or "").strip()
            notes_html = ""
            if notes:
                notes_html = (f'<details><summary>notes</summary>'
                              f'<pre>{html.escape(notes)}</pre></details>')
            rows.append(
                f'<tr><td class="ver">{html.escape(r["tag_name"])}</td>'
                f'<td class="dim">{fmt_date(r.get("published_at"))}</td>'
                f'<td class="dim">{size}</td>'
                f"<td>{dl}{notes_html}</td></tr>"
            )
        table_html = (
            '<div class="tablewrap"><table><thead><tr><th>Version</th><th>Date</th>'
            "<th>Size</th><th>Image / notes</th></tr></thead><tbody>"
            + "".join(rows) + "</tbody></table></div>"
        )

    now = datetime.now(timezone.utc)
    footer = (f'<a href="https://github.com/{html.escape(repo)}">{html.escape(repo)}</a>'
              f' &middot; generated {now.strftime("%Y-%m-%d %H:%M UTC")}')
    return PAGE.substitute(
        latest=latest_html, table=table_html, footer=footer,
        site=SITE, logo=LOGO, footer_logo=FOOTER_LOGO, favicon=FAVICON,
        year=now.strftime("%Y"),
    )


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate the SC-F001 release Pages site")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY"),
                    help="owner/name (defaults to $GITHUB_REPOSITORY)")
    ap.add_argument("--out", default="site")
    ap.add_argument("--cname", default=os.environ.get("SC_PAGES_CNAME"),
                    help="custom domain to write into site/CNAME, e.g. "
                         "update.thestockcropper.com. Only set this once DNS "
                         "points there — GitHub Pages will redirect the "
                         "github.io URL to it.")
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
    # Custom domain, only when asked for (see --cname).
    if args.cname:
        (out / "CNAME").write_text(f"{args.cname.strip()}\n", encoding="utf-8")
    print(f"wrote {out / 'index.html'} ({len(releases)} releases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
