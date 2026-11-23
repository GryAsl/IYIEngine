"""Download the pinned browser-build toolchain once: emsdk 3.1.61 and the prebuilt wasm CPython 3.14.

Usage: py setup_toolchain.py

Installs where the build looks for them: EMSDK (default third_party/web/emsdk) and IYI_WEB_PYTHON_SDK
(default third_party/web/python-wasm), both git-ignored. Whatever is already in place is skipped.
CPython is pygame-web/python-wasm-sdk's prebuilt archive (built with the same emsdk), checked by sha256;
only devices/emsdk/usr and prebuilt/emsdk are kept (~400 MB of the 880 MB archive). Needs the lz4
package (py -m pip install lz4): neither Python nor every Windows tar reads .tar.lz4.
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2] / "third_party" / "web"
EMSDK_REPO = "https://github.com/emscripten-core/emsdk.git"
EMSDK_COMMIT = "d7b4130688bfa348392b921ab04015aa15c4f154"
EMSDK_VERSION = "3.1.61"
PY_URL = ("https://github.com/pygame-web/python-wasm-sdk/releases/download/3.1.61.15bi/"
          "python3.14-wasm-sdk-Ubuntu-22.04.tar.lz4")
PY_SHA256 = "38bd1b169672864d0f948153e2f3083114ed83d2e8a044e2e47e4368f372bf7e"
PY_KEEP = ("devices/emsdk/usr/", "prebuilt/emsdk/")


def emsdk(dest: Path) -> None:
    if (dest / "upstream" / "emscripten").is_dir():
        print(f"emsdk: already at {dest}")
        return
    if not (dest / "emsdk.py").exists():
        subprocess.run(["git", "clone", EMSDK_REPO, str(dest)], check=True)
        subprocess.run(["git", "-C", str(dest), "checkout", "-q", EMSDK_COMMIT], check=True)
    # emsdk.bat runs the "python" on PATH, which on some machines is only the Microsoft Store alias
    # (exit 9009); install and activate need no shell environment, so run emsdk.py with this Python.
    tool = [sys.executable, str(dest / "emsdk.py")] if os.name == "nt" else [str(dest / "emsdk")]
    subprocess.run(tool + ["install", EMSDK_VERSION], check=True)
    subprocess.run(tool + ["activate", EMSDK_VERSION], check=True)


def python_wasm(dest: Path) -> None:
    if (dest / "devices" / "emsdk" / "usr" / "lib" / "libpython3.14.a").exists():
        print(f"python-wasm: already at {dest}")
        return
    try:
        import lz4.frame
    except ImportError:
        raise SystemExit("python-wasm: needs the lz4 package: py -m pip install lz4")
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=dest.parent) as tmp:
        archive = Path(tmp) / "python-wasm.tar.lz4"
        print(f"python-wasm: downloading {PY_URL}")
        digest = hashlib.sha256()
        with urllib.request.urlopen(PY_URL) as r, open(archive, "wb") as f:
            while chunk := r.read(1 << 20):
                digest.update(chunk)
                f.write(chunk)
        if digest.hexdigest() != PY_SHA256:
            raise SystemExit(f"python-wasm: sha256 mismatch ({digest.hexdigest()}), not extracted")
        out = Path(tmp) / "x"
        with lz4.frame.open(archive) as raw, tarfile.open(fileobj=raw, mode="r|") as tar:
            for m in tar:
                # The archive's top folder (opt/python-wasm-sdk/ or similar) is dropped; links are skipped
                # (Windows makes them only with extra rights, and the build needs none of them).
                cut = min((i for k in PY_KEEP if (i := m.name.find(k)) >= 0), default=-1)
                if cut >= 0 and (m.isfile() or m.isdir()):
                    m.name = m.name[cut:]
                    tar.extract(m, out, filter="data")
        if not (out / "devices").is_dir():
            raise SystemExit("python-wasm: no devices/emsdk/usr in the archive")
        shutil.move(str(out), str(dest))
    print(f"python-wasm: installed at {dest}")


def main() -> int:
    emsdk(Path(os.environ.get("EMSDK") or ROOT / "emsdk"))
    python_wasm(Path(os.environ.get("IYI_WEB_PYTHON_SDK") or ROOT / "python-wasm"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
