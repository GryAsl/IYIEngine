"""Copy a built web_engine into a site folder (index.html at its root) and zip it for itch.io.

Usage: py package_web.py <build dir> <output dir> [zip name]
"""
import shutil
import sys
import zipfile
from pathlib import Path


def main(build: Path, out: Path, zip_name: str) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for p in out.iterdir():  # a stale fonts_cjk.* from an earlier game must not ship
        if p.is_file():
            p.unlink()
    shutil.copyfile(build / "web_engine.html", out / "index.html")
    names = [f"web_engine.{ext}" for ext in ("js", "wasm", "data")] + ["game.js", "game.data"]
    names += [n for n in ("fonts_cjk.js", "fonts_cjk.data") if (build / n).exists()]
    for name in names:
        shutil.copyfile(build / name, out / name)
    files = sorted(p for p in out.iterdir() if p.is_file() and p.suffix != ".zip")
    zip_path = out.parent / zip_name
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for p in files:
            z.write(p, p.name)  # flat: index.html at the zip root
    with zipfile.ZipFile(zip_path) as z:
        assert "index.html" in z.namelist(), "index.html must be at the zip root"
    for p in files:
        print(f"{p.stat().st_size:>12,}  {p.name}")
    print(f"{zip_path.stat().st_size:>12,}  {zip_path}")


if __name__ == "__main__":
    main(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else "web.zip")
