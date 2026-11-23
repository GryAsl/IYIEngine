"""One command for a game's browser build: build, package, and optionally smoke-test or serve it.

Usage: py web_build.py <game folder> [--smoke] [--serve] [--out <dir>]

- Builds web_engine with the game (build_engine.bat; only the game data repacks when only the game changed).
- Packages a site (index.html at its root) into <out>/site and an itch.io ZIP named
  <project>-web-<date>-<commit>[-dirty].zip into <out> (default <game>/release/web).
- --smoke: serves the site, opens it headless in Chrome or Edge with ?smoke=1 and waits for the game's
  IYI_WEB_SMOKE_OK / IYI_WEB_SMOKE_FAIL line; exit code 1 on a failure, a timeout or a Python traceback.
  shell.html turns the OK into a FAIL while page text (a stuck "Loading 100%") still covers the canvas.
- --serve: serves the site at http://127.0.0.1:<port>/ until Ctrl+C.
"""
import argparse
import datetime
import functools
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

import package_web

HERE = Path(__file__).resolve().parent
BUILD = HERE.parents[1] / "build" / "web" / "engine"
SMOKE_TIMEOUT = 240.0  # seconds: download, start, the run itself and the late CJK fonts
BROWSERS = (r"C:\Program Files\Google\Chrome\Application\chrome.exe",
            r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe")


def ignored_patterns(game: Path) -> list[str]:
    """file_packager patterns for what git ignores under the packaged folders (dev renders, caches)."""
    out = subprocess.run(["git", "-C", str(game), "ls-files", "--others", "--ignored", "--exclude-standard",
                          "--directory", "--", "scripts", "assets"], capture_output=True, text=True).stdout
    return [f"*/{p.rstrip('/')}" + ("/*" if p.endswith("/") else "") for p in out.split()]


def build(game: Path) -> None:
    env = dict(os.environ, IYI_WEB_GAME=str(game), IYI_WEB_EXCLUDE=";".join(ignored_patterns(game)))
    subprocess.run(["cmd", "/c", str(HERE / "build_engine.bat"), "web_engine"], env=env, check=True)


def zip_name(game: Path) -> str:
    name = json.loads((game / "project.json").read_text(encoding="utf-8")).get("name", game.name)
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")
    git = ["git", "-C", str(game)]
    commit = subprocess.run(git + ["rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(git + ["status", "--porcelain", "--", "project.json", "scripts", "assets"],
                           capture_output=True, text=True).stdout.strip()
    stamp = datetime.date.today().isoformat()
    return f"{slug}-web-{stamp}-{commit or 'nogit'}{'-dirty' if dirty else ''}.zip"


def serve(site: Path) -> http.server.ThreadingHTTPServer:
    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Quiet, directory=str(site)))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def smoke(site: Path) -> bool:
    browser = next((b for b in BROWSERS if Path(b).exists()), None) or shutil.which("chrome") or shutil.which("msedge")
    if browser is None:
        print("smoke: no Chrome or Edge found")
        return False
    server = serve(site)
    url = f"http://127.0.0.1:{server.server_address[1]}/?smoke=1"
    profile = tempfile.mkdtemp(prefix="iyi-web-smoke-")
    # The game logs to the console; Chrome writes console lines to stderr with --enable-logging.
    cmd = [browser, "--headless=new", "--enable-logging=stderr", "--v=0", f"--user-data-dir={profile}",
           "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--autoplay-policy=no-user-gesture-required",
           "--no-first-run", "--window-size=1280,720", url]
    proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True, errors="replace")
    lines: list[str] = []
    threading.Thread(target=lambda: lines.extend(proc.stderr), daemon=True).start()
    start, result, seen = time.monotonic(), None, 0
    print(f"smoke: {url}")
    try:
        while result is None and time.monotonic() - start < SMOKE_TIMEOUT and proc.poll() is None:
            time.sleep(0.5)
            for line in lines[seen:]:
                if "IYI_WEB_SMOKE" in line or "Traceback" in line:
                    print("  " + line.strip()[-160:])
                if "IYI_WEB_SMOKE_OK" in line:
                    result = True
                elif "IYI_WEB_SMOKE_FAIL" in line or "Traceback" in line:
                    result = False
            seen = len(lines)
    finally:
        proc.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
    if result is None:
        print(f"smoke: no result within {SMOKE_TIMEOUT:.0f} s")
    print(f"smoke: {'OK' if result else 'FAILED'} in {time.monotonic() - start:.1f} s")
    return bool(result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game", type=Path)
    parser.add_argument("--smoke", action="store_true")
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    game = args.game.resolve()
    out = (args.out or game / "release" / "web").resolve()
    build(game)
    package_web.main(BUILD, out / "site", zip_name(game))
    for stale in out.glob("*.zip"):  # package_web writes the zip next to the site folder; keep the newest only
        if stale.name != zip_name(game):
            stale.unlink()
    if args.smoke and not smoke(out / "site"):
        return 1
    if args.serve:
        server = serve(out / "site")
        print(f"serving http://127.0.0.1:{server.server_address[1]}/  (Ctrl+C stops)")
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            server.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
