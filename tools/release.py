#!/usr/bin/env python3
"""
IYIEngine One-Click Release Pipeline
------------------------------------
Executes the seven stages of the release build system (windows, linux, macos; builds on the host OS only):
  [1/7] Preflight
  [2/7] Clean staging
  [3/7] Generate secret and manifest
  [4/7] Build shipping exe
  [5/7] Pack
  [6/7] Assemble runtime
  [7/7] Validate

References:
  - IYIEngine_RELEASE_BUILD_SYSTEM_PLAN.md sections 8, 9, 10, 12, 13
  - IYIEngine/tools/iyipack/README.md
  - IYIEngine/include/IYI/ReleaseSecret.h
"""

import argparse
import binascii
import json
import marshal
import os
import posixpath
import py_compile
import re
import fnmatch
import hashlib
import secrets
import shutil
import struct
import plistlib
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

# Standalone localization tools
tools_dir = Path(__file__).resolve().parent
if str(tools_dir) not in sys.path:
    sys.path.insert(0, str(tools_dir))
import iyiloc


# =============================================================================
# Data Models & Configurations
# =============================================================================

@dataclass
class CompressionConfig:
    enabled: bool = True
    algorithm: str = "deflate"  # "deflate" or "none"
    mode: str = "auto"
    overrides: Dict[str, str] = field(default_factory=dict)


@dataclass
class EncryptionConfig:
    enabled: bool = True
    algorithm: str = "xchacha20-poly1305"
    key_mode: str = "generated"  # "generated" or "environment"


@dataclass
class PackageConfig:
    hide_resource_paths: bool = True
    include_debug_files: bool = False
    create_zip: bool = False


@dataclass
class SteamConfig:
    prepare: bool = False
    sdk_path: str = ""


@dataclass
class ReleaseConfig:
    output_name: str = ""
    output_dir: str = "release"
    exe_icon: str = ""  # Windows .ico, relative to the game folder; "" = no exe icon
    keep_assets: List[str] = field(default_factory=list)  # globs packed even when no text names them
    clean_build: bool = True
    compression: CompressionConfig = field(default_factory=CompressionConfig)
    encryption: EncryptionConfig = field(default_factory=EncryptionConfig)
    package: PackageConfig = field(default_factory=PackageConfig)
    steam: SteamConfig = field(default_factory=SteamConfig)
    # Keys laid over project.json for this build only (e.g. a demo's name, steam_app_id, edition)
    project_overrides: dict = field(default_factory=dict)
    android: dict = field(default_factory=dict)


class ReleaseError(Exception):
    """Exception raised when a release stage fails."""
    pass


@dataclass(frozen=True)
class Platform:
    id: str
    exe_suffix: str
    obj_suffix: str    # CMake object suffix (forces ResourceStore to rebind the fresh secret header)
    steam_lib: str     # runtime library, relative to <sdk>/redistributable_bin
    archive: str       # "zip" or "tar.gz"


PLATFORMS = {
    "windows": Platform("windows", ".exe", ".obj", "win64/steam_api64.dll", "zip"),
    "linux": Platform("linux", "", ".o", "linux64/libsteam_api.so", "tar.gz"),
    "macos": Platform("macos", "", ".o", "osx/libsteam_api.dylib", "tar.gz"),
    "android": Platform("android", "", ".o", "", "apk"),
}


def parse_android_config(data: dict, project_data: dict) -> dict:
    android = data.get("android", {})
    if not isinstance(android, dict):
        raise ReleaseError("release.json android must be an object")
    app_id = str(android.get("application_id", "")).strip()
    if not app_id:
        raise ReleaseError("release.json android.application_id is required for Android releases")
    try:
        version_code = int(android.get("version_code", 1))
    except (TypeError, ValueError):
        raise ReleaseError("release.json android.version_code must be an integer")
    if version_code < 1:
        raise ReleaseError("release.json android.version_code must be positive")
    for key in ("play_games_app_id", "license_public_key"):
        if not isinstance(android.get(key, ""), str):
            raise ReleaseError(f"release.json android.{key} must be a string")
    achievements = android.get("play_achievement_ids", {})
    if not isinstance(achievements, dict) or any(not isinstance(k, str) or not isinstance(v, str)
                                                  for k, v in achievements.items()):
        raise ReleaseError("release.json android.play_achievement_ids must map strings to strings")
    for name, achievement in achievements.items():
        if not name or not achievement or any(c in name + achievement for c in ";="):
            raise ReleaseError("release.json android.play_achievement_ids entries must be non-empty and "
                               "free of ';' and '=' (they are packed as NAME=playId;NAME=playId)")
    return {"application_id": app_id, "app_name": str(android.get("app_name", project_data.get("name", "IYIEngine"))),
            "version_code": version_code, "version_name": str(android.get("version_name", project_data.get("version", "0.1"))),
            "play_games_app_id": android.get("play_games_app_id", "").strip(),
            "license_public_key": android.get("license_public_key", "").strip(),
            "play_achievement_ids": achievements}


def generate_android_icons(icon_path: Path, output: Path) -> None:
    """Generate density-specific legacy and adaptive launcher artwork."""
    from PIL import Image, ImageOps

    if not icon_path.is_file():
        raise ReleaseError(f"Android icon not found: {icon_path}")
    try:
        with Image.open(icon_path) as source:
            if source.format == "ICO":
                source = source.ico.getimage(max(source.ico.sizes(), key=lambda size: size[0] * size[1]))
            image = source.convert("RGBA")
            for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                                  ("xxhdpi", 144), ("xxxhdpi", 192)):
                folder = output / f"mipmap-{density}"
                folder.mkdir(parents=True, exist_ok=True)
                ImageOps.contain(image, (size, size), Image.Resampling.LANCZOS).save(folder / "iyi_launcher.png")
                canvas_size = size * 9 // 4
                foreground = Image.new("RGBA", (canvas_size, canvas_size))
                art = ImageOps.contain(image, (size, size), Image.Resampling.LANCZOS)
                foreground.alpha_composite(art, ((canvas_size - art.width) // 2, (canvas_size - art.height) // 2))
                foreground.save(folder / "iyi_launcher_foreground.png")
    except (OSError, ValueError) as exc:
        raise ReleaseError(f"Cannot read Android icon {icon_path}: {exc}") from exc
    adaptive = output / "mipmap-anydpi-v26"
    adaptive.mkdir(parents=True, exist_ok=True)
    (adaptive / "iyi_launcher.xml").write_text(
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@android:color/black" />\n'
        '    <foreground android:drawable="@mipmap/iyi_launcher_foreground" />\n'
        '</adaptive-icon>\n', encoding="utf-8")


def check_android_load_alignment(text: str) -> None:
    aligns = [int(v, 16) for v in re.findall(r"^\s*LOAD\s+.*?\s(0x[0-9a-fA-F]+)\s*$", text, re.M)]
    if not aligns:
        raise ReleaseError("llvm-readelf output contains no LOAD segment alignment")
    if any(value < 0x4000 for value in aligns):
        raise ReleaseError("Android shared library has LOAD alignment below 16 KB: " + ", ".join(hex(v) for v in aligns))


def check_android_rres_stored(apk: Path) -> None:
    try:
        with zipfile.ZipFile(apk) as z:
            info = z.getinfo("assets/game.rres")
    except (KeyError, zipfile.BadZipFile) as e:
        raise ReleaseError(f"APK is missing assets/game.rres: {e}")
    if info.compress_type != zipfile.ZIP_STORED:
        raise ReleaseError("APK assets/game.rres must be stored without compression")
# Embedded CPython: 3.14 by default, 3.11 kept as the legacy option (CMake IYI_PYTHON_VERSION).
PYTHON_VERSIONS = ("3.14", "3.11")
DEFAULT_PYTHON = "3.14"
# raylib: the current version first, then legacy ones (CMake IYI_RAYLIB_VERSION; bump steps in SPEC §0.1).
RAYLIB_VERSIONS = ("6.2", "5.5")
DEFAULT_RAYLIB = "6.2"
WIN_CRT_DLLS = ["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"]
# Tkinter, tests and tooling are never needed by a shipped game (and tkinter drags in Tcl/Tk libraries).
STDLIB_IGNORE = shutil.ignore_patterns("site-packages", "test", "tests", "idlelib", "tkinter", "turtledemo",
                                       "ensurepip", "lib2to3", "__pycache__", "config-*", "_tkinter*")
# PATH dirs allowed to keep a python executable during the smoke test: Xvfb & friends live there, and the
# engine ignores system Python by construction (config.home + use_environment=0).
SYSTEM_PATH_DIRS = {"/bin", "/usr/bin", "/sbin", "/usr/sbin"}


def host_platform() -> str:
    return {"win32": "windows", "linux": "linux", "darwin": "macos"}.get(sys.platform, "")


def load_runtime_pin(platform: str, python: str = DEFAULT_PYTHON) -> dict:
    """Pinned runtime (version, URL, SHA-256, pylib, stdlib) for one Python version and platform
    from tools/runtime_manifest.json."""
    return json.loads((tools_dir / "runtime_manifest.json").read_text(encoding="utf-8"))[python][platform]


def runtime_archive_name(pin: dict) -> str:
    return urllib.parse.unquote(pin["url"].rsplit("/", 1)[-1])


@dataclass(frozen=True)
class Layout:
    exe: Path
    data: Path
    libs: Path             # where python / steam shared libraries sit
    app: Optional[Path]    # macOS .app bundle root


def package_layout(plat: Platform, out: Path, name: str) -> Layout:
    if plat.id == "macos":
        app = out / f"{name}.app"
        c = app / "Contents"
        return Layout(c / "MacOS" / name, c / "Resources" / f"{name}_Data", c / "Frameworks", app)
    return Layout(out / (name + plat.exe_suffix), out / f"{name}_Data", out, None)


def runtime_lib_names(plat: Platform, pin: dict) -> List[str]:
    return [pin["pylib"]] + (list(WIN_CRT_DLLS) if plat.id == "windows" else [])


def check_ldd_output(text: str, libs_dir: Path) -> None:
    """Linux: every dependency resolves, and libpython comes from the package, not the build machine."""
    missing = [l.strip() for l in text.splitlines() if "not found" in l]
    if missing:
        raise ReleaseError("ldd reports unresolved libraries:\n  " + "\n  ".join(missing))
    for line in text.splitlines():
        if "libpython" in line and libs_dir.as_posix() not in line:
            raise ReleaseError(f"libpython does not resolve inside the package: {line.strip()}")


_MAC_OK = ("@rpath/", "@executable_path/", "@loader_path/", "/usr/lib/", "/System/Library/")
_MAC_BAD = ("/Users/", "/opt/homebrew", "/usr/local/")


def check_otool_output(text: str, strict: bool = True) -> None:
    """macOS `otool -L`: no build-machine paths; strict also requires only @-relative or system dependencies."""
    for line in text.splitlines():
        if not line.startswith(("\t", " ")):
            continue  # "<file>:" header line
        dep = line.strip().split(" (")[0]
        if any(b in dep for b in _MAC_BAD) or (strict and not dep.startswith(_MAC_OK)):
            raise ReleaseError(f"Unportable dependency '{dep}' (build-machine or absolute path)")


def smoke_env(env: Dict[str, str]) -> Dict[str, str]:
    """Environment for the smoke test: no PYTHON*/loader overrides, and no PATH dir holding a python."""
    e = {k: v for k, v in env.items() if not k.upper().startswith(("PYTHON", "LD_LIBRARY", "DYLD_"))}
    names = ("python", "python3", "python.exe", "python3.exe")
    keep = [d for d in e.get("PATH", "").split(os.pathsep)
            if d and (d in SYSTEM_PATH_DIRS or not any((Path(d) / n).exists() for n in names))]
    e["PATH"] = os.pathsep.join(keep)
    return e


def libpython_mapped(lib: Path) -> bool:
    """Linux: some running process has this exact libpython mapped (proves the bundled one is loaded)."""
    target = str(lib.resolve())
    for m in Path("/proc").glob("[0-9]*/maps"):
        try:
            if target in m.read_text(errors="replace"):
                return True
        except OSError:
            pass
    return False


def run_tool(cmd: List[str], env: Optional[dict] = None) -> str:
    res = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if res.returncode != 0:
        raise ReleaseError(f"{cmd[0]} failed ({res.returncode}): {(res.stdout + res.stderr).strip()[-800:]}")
    return res.stdout


RUNTIME_READY_MARKER = "IYI_RUNTIME_READY"
TEST_STEAM_APP_ID = 480  # Spacewar


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_pinned_file(path: Path, expected_sha256: str) -> None:
    """Deletes the file and raises ReleaseError if its SHA-256 does not match."""
    actual = _sha256(path)
    if actual != expected_sha256.lower():
        path.unlink(missing_ok=True)
        raise ReleaseError(
            f"SHA-256 mismatch for '{path.name}': expected {expected_sha256}, got {actual}. "
            "The file was deleted; re-run to download it again."
        )


def ensure_pinned_download(dest: Path, entry: dict) -> None:
    """Verifies the cached file, or downloads to a temp name, verifies, then atomically renames."""
    if dest.exists() and dest.stat().st_size > 0:
        verify_pinned_file(dest, entry["sha256"])
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_name(dest.name + ".part")
    try:
        urllib.request.urlretrieve(entry["url"], tmp)
        verify_pinned_file(tmp, entry["sha256"])
        os.replace(tmp, dest)
    finally:
        tmp.unlink(missing_ok=True)


# assets/<dir>/<file> or assets/<dir>/ (a folder a path is built in, e.g. "assets/locales/" + code).
_ASSET_REF = re.compile(r"assets/[A-Za-z0-9_./-]+")
_ASSET_TEXT_SUFFIXES = (".json", ".txt", ".md", ".fs", ".vs")   # a shader names the .glsl files it #includes


def referenced_assets(asset_paths, read_text, seed_texts, keep_globs=()) -> Set[str]:
    """The asset paths a shipped game can reach; the rest are left out of the package.

    An asset is kept when its path or file name appears in the scanned text, when it sits in a
    folder a path is built in (the text names "assets/<dir>/" and appends to it at runtime), when
    it is a license, or when release.json "keep_assets" names it. The text is the game's and the
    engine's code plus every kept text asset, scanned until nothing new is kept, so a JSON config
    that names a sound keeps that sound. Erring is one-sided: a stray mention keeps a file, it
    never drops one; a path built with no folder in the text needs "keep_assets".
    """
    remaining = set(asset_paths)
    kept: Set[str] = set()
    texts = list(seed_texts)
    while texts:
        blob = "\n".join(texts).replace("\\", "/")
        texts = []
        refs = set(_ASSET_REF.findall(blob))
        folders = tuple(r for r in refs if r.endswith("/") and r.count("/") >= 2)
        for rel in sorted(remaining):
            name = rel.rsplit("/", 1)[-1]
            if (rel in refs or rel.startswith(folders) or name in blob or "LICENSE" in name.upper()
                    or any(fnmatch.fnmatch(rel, g) for g in keep_globs)):
                kept.add(rel)
                if rel.endswith(_ASSET_TEXT_SUFFIXES):
                    texts.append(read_text(rel))
        remaining -= kept
    return kept


# IYITrailer (docs/IYITRAILER.md) is development-only: its shots never ship and the shipping
# engine is built without it, so either one showing up in a package is a release blocker.
TRAILER_SCRIPT_RE = re.compile(r"^\s*(?:from|import)\s+marketing\b|\bhr_bridge\b|\bengine\.trailer\b", re.M)
TRAILER_EXE_MARKERS = (b"--trailer-shot", b"--trailer-config", b"trailer_shot")


def check_no_trailer_scripts(scripts) -> None:
    """scripts: (path, source text) of every packed script; none may use the trailer or its shots."""
    bad = sorted(rel for rel, text in scripts if TRAILER_SCRIPT_RE.search(text))
    if bad:
        raise ReleaseError(f"Packed scripts reference development-only IYITrailer code: {', '.join(bad)}")


def check_no_trailer_exe(data: bytes) -> None:
    found = [m.decode() for m in TRAILER_EXE_MARKERS if m in data]
    if found:
        raise ReleaseError(f"Validation failed: the shipping executable contains IYITrailer code ({', '.join(found)})")


def check_steam_app_id(project_data: dict, steam_prepare: bool) -> None:
    """Shipping (steam.prepare=true) must use a real AppID, never 0 or the Spacewar test ID."""
    app_id = int(project_data.get("steam_app_id", 0) or 0)
    if steam_prepare and app_id in (0, TEST_STEAM_APP_ID):
        raise ReleaseError(
            f"steam.prepare=true but project.json steam_app_id is {app_id}. "
            "480 is Spacewar, a test-only AppID, and 0 means none; a real Steam AppID is required "
            "for a shipping build. Dev builds (steam.prepare=false) may still use 480."
        )


THIRD_PARTY_NOTICES = "THIRD_PARTY_NOTICES.txt"
CRT_NOTICE = ("The Microsoft Visual C++ runtime files beside the executable (" + ", ".join(WIN_CRT_DLLS) + ") are "
              "redistributed unmodified under the Distributable Code terms of the Microsoft Visual Studio license.")


def license_text(path: Path, start: str = "", end: str = "") -> str:
    """The license in `path`: the whole file, or the header comment from the line holding `start` up to the
    line holding `end`, comment markers (`*`, `//`) removed."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if start:
        first = next((n for n, line in enumerate(lines) if start in line), None)
        if first is None:
            return ""
        last = next((n for n in range(first + 1, len(lines)) if end in lines[n]), len(lines))
        lines = [re.sub(r"^\s*(\*|//)\s?", "", line) for line in lines[first:last]]
    return "\n".join(lines).strip()


def notice_sources(deps: Path, engine: Path, python_license: Optional[Path]) -> List[Tuple[str, Path, str, str]]:
    """(name, file, start, end) of the license text of everything the shipped runtime contains, read from the
    sources the build used: `deps` is the build tree's _deps, `python_license` is CPython's (None: no Python)."""
    third = engine / "third_party"
    sources = [
        ("raylib", deps / "raylib-src" / "LICENSE", "", ""),
        ("GLFW (built into raylib)", deps / "raylib-src" / "src" / "external" / "glfw" / "LICENSE.md", "", ""),
        ("nlohmann/json", deps / "nlohmann_json-src" / "LICENSE.MIT", "", ""),
        ("pl_mpeg", third / "pl_mpeg" / "LICENSE", "", ""),
        ("rres", third / "rres" / "rres.h", "LICENSE: MIT", "*****"),
        ("Monocypher", third / "monocypher" / "monocypher.h", "This file is dual-licensed", "#ifndef"),
    ]
    if python_license:
        sources += [("CPython", python_license, "", ""), ("pybind11", deps / "pybind11-src" / "LICENSE", "", "")]
    return sources


def _notice_section(name: str, text: str) -> str:
    return f"{name}\n{'=' * len(name)}\n\n{text}\n"


def build_notices(sources, crt: bool) -> str:
    """THIRD_PARTY_NOTICES.txt from `notice_sources`; a license text that is not on disk is an error, never invented."""
    texts = [(name, license_text(path, start, end) if path.is_file() else "") for name, path, start, end in sources]
    missing = [f"{name} ({path})" for (name, text), (_, path, _, _) in zip(texts, sources) if not text]
    if missing:
        raise ReleaseError("No license text on disk for: " + "; ".join(missing))
    if crt:
        texts.append(("Microsoft Visual C++ runtime", CRT_NOTICE))
    return "\n\n".join(_notice_section(name, text) for name, text in texts)


def check_notices(text: str, sources, crt: bool) -> None:
    """Validation: every section `build_notices` writes is in the shipped file."""
    names = [name for name, *_ in sources] + (["Microsoft Visual C++ runtime"] if crt else [])
    absent = [name for name in names if _notice_section(name, "").rstrip("\n") + "\n\n" not in text]
    if absent:
        raise ReleaseError(f"Validation failed: {THIRD_PARTY_NOTICES} has no section for: {', '.join(absent)}")


def validate_runtime_log(log_content: str, log_path: object = "log", python: bool = True) -> None:
    """Checks a shipped runtime's log; raises ReleaseError on any failure. `python`: False for a native game."""
    if python and "m_python.Initialize returned true" not in log_content:
        raise ReleaseError(f"Smoke test failed: Python runtime failed to initialize. See '{log_path}'")
    if RUNTIME_READY_MARKER not in log_content:
        raise ReleaseError(
            f"Smoke test failed: {RUNTIME_READY_MARKER} never logged (GameStart/first SceneStart/first frame "
            f"did not succeed). See '{log_path}'"
        )
    if "hot reload: disabled" not in log_content:
        raise ReleaseError(
            f"Smoke test failed: Shipped runtime log does not confirm hot reload is disabled. See '{log_path}'"
        )
    error_lines = []
    for line in log_content.splitlines():
        if "[ERROR]" in line or "TRACEBACK (MOST RECENT CALL LAST)" in line.upper() or "FATAL" in line.upper():
            error_lines.append(line.strip())
    if error_lines:
        sample_errors = "\n  ".join(error_lines[:5])
        raise ReleaseError(
            f"Smoke test failed: Release runtime logged error(s) or traceback in '{log_path}':\n  {sample_errors}"
        )


# =============================================================================
# Helper Functions: Path Normalization & CRC32
# =============================================================================

def normalize_resource_path(path_str: str) -> str:
    """
    Normalizes a resource path according to the engine and iyipack specification:
      1. Backslashes replaced with forward slashes.
      2. ASCII converted to lowercase.
      3. Lexical cleanup (. and .. components resolved).
      4. Leading '/' or './' stripped.
      5. Empty or '.' results in ''.
    """
    if not path_str:
        return ""
    
    # 1. Backslashes to slashes
    norm = path_str.replace("\\", "/")
    # 2. ASCII lowercase
    norm = norm.lower()

    # 3. Lexical cleanup
    parts: List[str] = []
    for part in norm.split("/"):
        if part == "" or part == ".":
            continue
        if part == "..":
            if parts:
                parts.pop()
        else:
            parts.append(part)

    result = "/".join(parts)
    # 4. Strip leading '/'
    result = result.lstrip("/")
    if result == ".":
        result = ""
    return result


def compute_resource_id(norm_path: str) -> int:
    """Computes CRC32 ID from normalized relative path using IEEE 802.3 polynomial."""
    return binascii.crc32(norm_path.encode("utf-8")) & 0xFFFFFFFF


def sanitize_filename(name: str) -> str:
    """Sanitizes an executable/project name to safe characters."""
    sanitized = re.sub(r"[^\w\-]", "", name.strip().replace(" ", ""))
    return sanitized if sanitized else "IYIGame"


def parse_json_relaxed(filepath: Path) -> dict:
    """Parses JSON file with support for comments and utf-8 encoding."""
    content = filepath.read_text(encoding="utf-8")
    # Strip line comments //...
    lines = []
    for line in content.splitlines():
        stripped = line.strip()
        if stripped.startswith("//"):
            continue
        lines.append(line)
    clean_content = "\n".join(lines)
    return json.loads(clean_content)


def load_release_config(config_path: Path, project_data: dict) -> ReleaseConfig:
    """Loads release.json configuration and merges defaults."""
    cfg = ReleaseConfig()
    if config_path.exists():
        try:
            data = parse_json_relaxed(config_path)
        except Exception as e:
            raise ReleaseError(f"Failed to parse release.json: {e}")

        cfg.output_name = data.get("output_name", "").strip()
        cfg.output_dir = data.get("output_dir", "release").strip()
        cfg.exe_icon = str(data.get("exe_icon", "")).strip()
        cfg.keep_assets = [str(g) for g in data.get("keep_assets", [])]
        cfg.clean_build = bool(data.get("clean_build", True))

        # Compression
        comp = data.get("compression", {})
        if isinstance(comp, dict):
            cfg.compression.enabled = bool(comp.get("enabled", True))
            cfg.compression.algorithm = str(comp.get("algorithm", "deflate")).lower()
            cfg.compression.mode = str(comp.get("mode", "auto")).lower()
            cfg.compression.overrides = comp.get("overrides", {})
        elif isinstance(comp, str):
            cfg.compression.enabled = (comp.lower() != "none")
            cfg.compression.algorithm = comp.lower()

        # Encryption
        enc = data.get("encryption", {})
        if isinstance(enc, dict):
            cfg.encryption.enabled = bool(enc.get("enabled", True))
            cfg.encryption.algorithm = str(enc.get("algorithm", "xchacha20-poly1305")).lower()
            cfg.encryption.key_mode = str(enc.get("key_mode", "generated")).lower()
        elif isinstance(enc, bool):
            cfg.encryption.enabled = enc

        # Package
        pkg = data.get("package", {})
        if isinstance(pkg, dict):
            cfg.package.hide_resource_paths = bool(pkg.get("hide_resource_paths", True))
            cfg.package.include_debug_files = bool(pkg.get("include_debug_files", False))
            cfg.package.create_zip = bool(pkg.get("create_zip", False))

        # Steam
        stm = data.get("steam", {})
        if isinstance(stm, dict):
            cfg.steam.prepare = bool(stm.get("prepare", False))
            cfg.steam.sdk_path = str(stm.get("sdk_path", ""))

        overrides = data.get("project_overrides", {})
        if not isinstance(overrides, dict):
            raise ReleaseError("release.json project_overrides must be an object")
        cfg.project_overrides = dict(overrides)

    if config_path.exists() and "android" in data:
        cfg.android = parse_android_config(data, project_data)
    # Derive default output_name from project.json name if empty
    if not cfg.output_name:
        project_name = project_data.get("name", "IYIGame")
        cfg.output_name = sanitize_filename(project_name)

    return cfg


# =============================================================================
# Toolchain & Runtime Locators
# =============================================================================

class Toolchain:
    def __init__(self, engine_dir: Path):
        self.engine_dir = engine_dir
        self.vs_dir: Optional[Path] = None
        self.cmake_exe: Optional[Path] = None
        self.ninja_exe: Optional[Path] = None
        self.vcvars64_bat: Optional[Path] = None
        self.redist_dir: Optional[Path] = None
        self.msvc_dlls: List[Path] = []
        self._discover()

    def _discover(self):
        if sys.platform == "win32":
            self._discover_msvc()
        # cmake / ninja from PATH when not found in VS (the only source on Linux/macOS)
        if not self.cmake_exe and shutil.which("cmake"):
            self.cmake_exe = Path(shutil.which("cmake"))
        if not self.ninja_exe and shutil.which("ninja"):
            self.ninja_exe = Path(shutil.which("ninja"))

    def _discover_msvc(self):
        # 1. Locate Visual Studio via vswhere.exe
        vswhere = Path(os.path.expandvars(r"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"))
        if vswhere.exists():
            try:
                cmd = [str(vswhere), "-latest", "-property", "installationPath"]
                output = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL).strip()
                if output and Path(output).exists():
                    self.vs_dir = Path(output)
            except Exception:
                pass

        if not self.vs_dir:
            # Fallback checks for common installations
            for cand in [
                Path(r"C:\Program Files\Microsoft Visual Studio\18\Community"),
                Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community"),
                Path(r"C:\Program Files\Microsoft Visual Studio\2022\Professional"),
                Path(r"C:\Program Files\Microsoft Visual Studio\2022\Enterprise"),
            ]:
                if cand.exists():
                    self.vs_dir = cand
                    break

        if self.vs_dir:
            # CMake
            cmake_cand = self.vs_dir / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "CMake" / "bin" / "cmake.exe"
            if cmake_cand.exists():
                self.cmake_exe = cmake_cand
            # Ninja
            ninja_cand = self.vs_dir / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake" / "Ninja" / "ninja.exe"
            if ninja_cand.exists():
                self.ninja_exe = ninja_cand
            # vcvars64
            vcvars_cand = self.vs_dir / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
            if vcvars_cand.exists():
                self.vcvars64_bat = vcvars_cand

            # MSVC Redist DLLs, newest version first: VS keeps older redists beside the current one
            # (14.29 next to 14.51), and an exe built by a newer toolset crashes on an older msvcp140
            # (std::mutex changed in 14.40; it showed as an access violation at exit).
            redist_base = self.vs_dir / "VC" / "Redist" / "MSVC"
            if redist_base.exists():
                for root, _, files in sorted(os.walk(redist_base), reverse=True):
                    if "x64" in root and "Microsoft.VC" in root and "onecore" not in root.lower():
                        if "vcruntime140.dll" in files:
                            self.redist_dir = Path(root)
                            for dll_name in ["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"]:
                                dll_path = self.redist_dir / dll_name
                                if dll_path.exists():
                                    self.msvc_dlls.append(dll_path)
                            break

        # Fallback for MSVC DLLs from System32 if redist folder was not discovered
        if len(self.msvc_dlls) < 3:
            sys32 = Path(os.path.expandvars(r"%SystemRoot%\System32"))
            sys_dlls = []
            for dll_name in ["vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"]:
                dll_path = sys32 / dll_name
                if dll_path.exists():
                    sys_dlls.append(dll_path)
            if len(sys_dlls) == 3:
                self.msvc_dlls = sys_dlls

    def get_build_env(self) -> Dict[str, str]:
        """Loads full MSVC developer environment from vcvars64.bat and prepares PATH."""
        env = os.environ.copy()
        if self.vcvars64_bat and self.vcvars64_bat.exists():
            cmd = f'call "{self.vcvars64_bat}" >nul 2>&1 && set'
            try:
                output = subprocess.check_output(cmd, shell=True, text=True)
                for line in output.splitlines():
                    if "=" in line:
                        k, v = line.split("=", 1)
                        env[k] = v
            except Exception as e:
                print(f"  [WARN] Failed to load vcvars64.bat environment: {e}")

        # Ensure VS CMake and Ninja directories are present in PATH
        path_dirs = []
        if self.cmake_exe:
            path_dirs.append(str(self.cmake_exe.parent))
        if self.ninja_exe:
            path_dirs.append(str(self.ninja_exe.parent))
        current_path = env.get("PATH", "")
        for d in path_dirs:
            if d.lower() not in current_path.lower():
                current_path = f"{d}{os.pathsep}{current_path}"
        env["PATH"] = current_path
        return env

    def locate_iyipack(self, build_dir: Optional[Path] = None) -> Optional[Path]:
        """Locates prebuilt iyipack if present; `build_dir`: the build tree of this release (a native game's holds the engine in IYIEngine/)."""
        exe = "iyipack" + PLATFORMS[host_platform()].exe_suffix
        candidates = [
            *([build_dir / "IYIEngine" / "tools" / "iyipack" / exe] if build_dir else []),
            self.engine_dir / "build" / "shipping" / "tools" / "iyipack" / exe,
            self.engine_dir / "build" / "shipping" / exe,
            self.engine_dir / "build" / "ninja" / "tools" / "iyipack" / exe,
            self.engine_dir / "build" / "ninja" / exe,
            self.engine_dir / "tools" / "iyipack" / exe,
        ]
        for cand in candidates:
            if cand.exists():
                return cand
        which_iyipack = shutil.which("iyipack")
        if which_iyipack:
            return Path(which_iyipack)
        return None


# =============================================================================
# Release Pipeline Implementation
# =============================================================================

class ReleasePipeline:
    def __init__(self, game_dir: Path, engine_dir: Path, config_override: Optional[Path] = None,
                 skip_game_tests: bool = False, platform: Optional[str] = None, no_steam: bool = False,
                 skip_smoke: bool = False, python: str = DEFAULT_PYTHON, raylib: str = DEFAULT_RAYLIB):
        if python not in PYTHON_VERSIONS:
            raise ReleaseError(f"Python {python} is not supported; choose one of {', '.join(PYTHON_VERSIONS)}")
        if raylib not in RAYLIB_VERSIONS:
            raise ReleaseError(f"raylib {raylib} is not supported; choose one of {', '.join(RAYLIB_VERSIONS)}")
        self.python = python
        self.raylib = raylib
        self.skip_game_tests = skip_game_tests
        self.skip_smoke = skip_smoke
        self.plat = PLATFORMS[platform or host_platform()]
        self.no_steam = no_steam
        self.python_root: Optional[Path] = None  # extracted Linux/macOS runtime prefix (linked and bundled)
        self.native = False                      # a C++ game (SPEC §2c): set by preflight from project.json
        self.cmake_dir: Optional[Path] = None    # a native game's CMakeLists.txt folder
        self.notices: List[Tuple[str, Path, str, str]] = []
        self.game_dir = game_dir.resolve()
        self.engine_dir = engine_dir.resolve()
        self.project_json_path = self.game_dir / "project.json"
        self.release_json_path = config_override if config_override else (self.game_dir / "release.json")

        self.project_data: dict = {}
        self.config: ReleaseConfig = ReleaseConfig()
        self.toolchain = Toolchain(self.engine_dir)

        self.staging_dir = self.engine_dir / "build" / "release_staging"
        self.gen_include_dir = self.engine_dir / "build" / "generated_include"
        self.output_path: Optional[Path] = None
        self.layout: Optional[Layout] = None

        self._secret: Optional[str] = None
        self._is_generated_key: bool = False
        self.iyipack_path: Optional[Path] = None

    @property
    def steam_enabled(self) -> bool:
        return self.plat.id != "android" and self.config.steam.prepare and not self.no_steam

    @property
    def pin(self) -> dict:
        return load_runtime_pin(self.plat.id, self.python)

    @property
    def shipping_dir(self) -> Path:
        """One build tree per Python and raylib version, so switching never reuses a cache built for the other one."""
        name = "shipping" if self.python == DEFAULT_PYTHON else "shipping-py" + self.python.replace(".", "")
        if self.raylib != DEFAULT_RAYLIB:
            name += "-rl" + self.raylib.replace(".", "")
        if self.native:   # the engine is built inside the game's own tree (cmake/iyi_game.cmake)
            return self.cmake_dir / "build" / "shipping"
        return self.engine_dir / "build" / name

    @property
    def runtime_archive(self) -> Path:
        return self.engine_dir / "third_party" / runtime_archive_name(self.pin)

    def _steam_sdk_dir(self) -> Path:
        if self.config.steam.sdk_path:
            p = Path(self.config.steam.sdk_path)
            if not p.is_absolute():
                p = (self.game_dir / p) if (self.game_dir / p).exists() else (self.engine_dir / p)
            return p.resolve()
        return (self.engine_dir.parent / "steamworks_sdk_165" / "sdk").resolve()

    def _steam_lib_path(self) -> Path:
        return self._steam_sdk_dir() / "redistributable_bin" / self.plat.steam_lib

    # -------------------------------------------------------------------------
    # Stage 1: Preflight
    # -------------------------------------------------------------------------
    def libraries(self) -> List[str]:
        """project.json "libraries", without its "desktop_libraries" on Android/iOS (project.md §2)."""
        skip = set() if self.plat.id in ("windows", "linux", "macos") else set(self.project_data.get("desktop_libraries", []))
        return [rel for rel in self.project_data.get("libraries", []) if rel not in skip]

    def content_roots(self) -> List[Path]:
        """The game folder, then each project.json "libraries" folder (engine SPEC §2) in order.
        A library is packed as if its scripts/ and assets/ were the game's own."""
        roots = [self.game_dir]
        for rel in self.libraries():
            lib = (self.game_dir / rel).resolve()
            if not lib.is_dir():
                raise ReleaseError(f"Library folder not found: {lib}")
            roots.append(lib)
        return roots

    def content_files(self, sub: str, suffix: str = "") -> List[Tuple[str, Path]]:
        """(root-relative path, file) under <root>/<sub> of every content root. A path an earlier
        root already has is skipped: the game wins over a library, exactly as the dev backend
        resolves it, so the packed game sees the files it saw in development."""
        seen: Set[str] = set()
        out: List[Tuple[str, Path]] = []
        for root in self.content_roots():
            base = root / sub
            if not base.exists():
                continue
            for dirpath, _, files in os.walk(base):
                if "__pycache__" in Path(dirpath).parts:
                    continue
                for f in files:
                    if suffix and not f.endswith(suffix):
                        continue
                    full = Path(dirpath) / f
                    rel = full.relative_to(root).as_posix()
                    if normalize_resource_path(rel) not in seen:
                        seen.add(normalize_resource_path(rel))
                        out.append((rel, full))
        return out

    def stage_preflight(self) -> List[Tuple[str, bool, str]]:
        """
        Validates all items listed in plan section 10:
          1. release configuration parses
          2. project.json parses
          3. entry script exists
          4. Python game scripts compile syntactically
          5. required rres/rrespacker tool is available (or buildable)
          6. resource IDs have no collisions
          7. requested compression/encryption algorithms are supported
          8. required CPython/MSVC runtime files are available
          9. output directory is writable
        """
        print("[1/7] Preflight validation...")
        results: List[Tuple[str, bool, str]] = []

        # 1. project.json parses
        if not self.project_json_path.exists():
            raise ReleaseError(f"project.json not found at {self.project_json_path}")
        try:
            self.project_data = parse_json_relaxed(self.project_json_path)
            if "name" not in self.project_data:
                raise ValueError("Missing 'name' field")
            self.native = "entry_script" not in self.project_data   # SPEC §2: a native game has none
            results.append(("project.json parses", True, f"Project '{self.project_data['name']}'"))
        except Exception as e:
            raise ReleaseError(f"project.json parse failure: {e}")

        # 2. release configuration parses
        try:
            self.config = load_release_config(self.release_json_path, self.project_data)
            if self.plat.id == "android" and not self.config.android:
                self.config.android = parse_android_config({}, self.project_data)
            results.append(("release configuration parses", True, f"Target: '{self.config.output_name}'"))
        except Exception as e:
            raise ReleaseError(f"release.json parse failure: {e}")
        if self.config.project_overrides:
            # Before every check and the staged project.json, so the build is validated as it ships.
            self.project_data.update(self.config.project_overrides)
            results.append(("project.json overrides", True, ", ".join(sorted(self.config.project_overrides))))

        # Compute effective output directory
        rel_out = Path(self.config.output_dir)
        pkg_name = self.config.output_name if self.plat.id == "windows" else f"{self.config.output_name}-{self.plat.id}"
        self.output_path = (self.game_dir / "release" / "android" if self.plat.id == "android" else
                            (rel_out if rel_out.is_absolute() else self.game_dir / rel_out) / pkg_name)
        self.layout = package_layout(self.plat, self.output_path, self.config.output_name)

        # 3. entry script exists (a native game has a CMakeLists.txt instead)
        if self.native:
            self.cmake_dir = next((d for d in (self.game_dir, self.game_dir / "src") if (d / "CMakeLists.txt").is_file()), None)
            if self.cmake_dir is None:
                raise ReleaseError(f"project.json has no entry_script, so {self.game_dir.name} is a native game, "
                                   f"but no CMakeLists.txt is in {self.game_dir} or its src/")
            results.append(("native game: CMakeLists.txt exists", True, str(self.cmake_dir / "CMakeLists.txt")))
        else:
            entry_script_rel = self.project_data.get("entry_script", "")
            entry_script_full = self.game_dir / entry_script_rel
            if not entry_script_full.exists() or not entry_script_full.is_file():
                raise ReleaseError(f"Entry script does not exist: {entry_script_full}")
            results.append(("entry script exists", True, str(entry_script_rel)))
            code = subprocess.call([sys.executable, str(self.engine_dir / "tools" / "pyi_sync.py"),
                                    "--engine-root", str(self.engine_dir), "--check"])
            if code != 0:
                raise ReleaseError("Shared engine Python stubs are out of date; run tools/pyi_sync.py --write")
            results.append(("shared engine Python stubs", True, str(self.engine_dir / "typings")))

        # 4. Python game scripts (and library scripts) compile syntactically
        py_scripts = self.content_files("scripts", ".py")
        py_files: List[Path] = [full for _, full in py_scripts]

        for py_path in py_files:
            try:
                py_compile.compile(str(py_path), doraise=True)
            except py_compile.PyCompileError as e:
                raise ReleaseError(f"Python syntax error in {py_path.name}: {e.msg}")
            except Exception as e:
                raise ReleaseError(f"Failed compiling {py_path.name}: {e}")

        # Clean any __pycache__ created during the syntax verification
        for root in self.content_roots():
            self._clean_pycache(root)
        results.append(("Python game scripts compile syntactically", True, f"{len(py_files)} scripts verified"))
        check_no_trailer_scripts((rel, full.read_text(encoding="utf-8", errors="replace")) for rel, full in py_scripts)
        results.append(("no IYITrailer code in packed scripts", True, ""))

        # Locales validation check (active only when assets/locales exists)
        locales_dir = self.game_dir / "assets" / "locales"
        if locales_dir.is_dir():
            fallback_lang = self.project_data.get("default_language", "en")
            if not isinstance(fallback_lang, str) or not fallback_lang.strip():
                fallback_lang = "en"
            loc_res = iyiloc.validate_locales(locales_dir, fallback_lang)
            for w in loc_res.warnings:
                print(f"  [WARN] {w}")
            if loc_res.errors:
                for err in loc_res.errors[1:]:
                    print(f"  [ERROR] {err}", file=sys.stderr)
                raise ReleaseError(f"Locale validation failed: {loc_res.errors[0]}")
            detail = f"{loc_res.locale_count} locales verified"
            if loc_res.warnings:
                detail += f", {len(loc_res.warnings)} warnings"
            results.append(("locale files valid", True, detail))

        # 5. required rres/rrespacker tool is available or buildable
        iyipack_exe = self.toolchain.locate_iyipack(self.shipping_dir if self.native else None)
        iyipack_source = self.engine_dir / "tools" / "iyipack" / "CMakeLists.txt"
        if iyipack_exe:
            results.append(("packaging tool (iyipack) available", True, f"Found prebuilt binary at {iyipack_exe.name}"))
        elif iyipack_source.exists() and self.toolchain.cmake_exe:
            results.append(("packaging tool (iyipack) available", True, "Source present; will build via CMake"))
        else:
            raise ReleaseError("iyipack tool is not available and cannot be built (missing CMake/toolchain or source)")

        # 6. Resource IDs have no collisions
        seen_ids: Dict[int, str] = {}
        # Collect candidate game files
        candidate_resources: List[Tuple[str, Path]] = [
            ("project.json", self.project_json_path),
        ]
        candidate_resources.extend(self.content_files("assets"))

        for rel_p, py_path in py_scripts:
            # Game scripts will be packaged as matching .pyc paths
            if rel_p.endswith(".py"):
                rel_pyc = rel_p + "c"
                candidate_resources.append((rel_pyc, py_path))

        for rel_name, _ in candidate_resources:
            norm_name = normalize_resource_path(rel_name)
            res_id = compute_resource_id(norm_name)
            if res_id in seen_ids and seen_ids[res_id] != norm_name:
                raise ReleaseError(
                    f"CRC32 resource ID collision detected (0x{res_id:08X}): "
                    f"'{norm_name}' collides with '{seen_ids[res_id]}'"
                )
            seen_ids[res_id] = norm_name

        results.append(("resource IDs have no collisions", True, f"{len(seen_ids)} unique resources checked"))

        # 7. requested compression/encryption algorithms supported
        comp_alg = self.config.compression.algorithm
        if comp_alg not in ["deflate", "none"]:
            raise ReleaseError(f"Unsupported compression algorithm '{comp_alg}'. Supported: deflate, none")
        for ext, ext_alg in self.config.compression.overrides.items():
            if ext_alg.lower() not in ["deflate", "none"]:
                raise ReleaseError(f"Unsupported override compression '{ext_alg}' for {ext}")

        if self.config.encryption.enabled:
            enc_alg = self.config.encryption.algorithm
            if enc_alg != "xchacha20-poly1305":
                raise ReleaseError(f"Unsupported encryption algorithm '{enc_alg}'. Supported: xchacha20-poly1305")
            key_mode = self.config.encryption.key_mode
            if key_mode not in ["generated", "environment"]:
                raise ReleaseError(f"Unsupported key_mode '{key_mode}'. Supported: generated, environment")
            if key_mode == "environment":
                env_key = os.environ.get("IYI_RELEASE_KEY") or os.environ.get("IYI_ENCRYPTION_KEY")
                if not env_key:
                    raise ReleaseError("key_mode is 'environment' but neither IYI_RELEASE_KEY nor IYI_ENCRYPTION_KEY is set")

        results.append(("compression & encryption algorithms supported", True, f"{comp_alg} / {self.config.encryption.algorithm}"))

        # 8. required CPython runtime (pinned in runtime_manifest.json) is available and verified
        if not self.native and self.plat.id != "android":
            ensure_pinned_download(self.runtime_archive, self.pin)
        if self.plat.id == "windows" and len(self.toolchain.msvc_dlls) < 3:
            print("  [WARN] MSVC CRT DLLs not fully detected; runtime might require vc_redist on target PC")
        results.append(("runtime dependencies available", True,
                        "Android CPython staged during build" if self.plat.id == "android" else
                        "native game: no Python runtime" if self.native else
                        f"{self.plat.id} CPython {self.pin['version']} OK ({self.runtime_archive.name})"))

        # 8b. the license texts kept in the repo for the third-party notices (the build's own come after it)
        repo_texts = [(n, f) for n, f, _, _ in notice_sources(self.shipping_dir / "_deps", self.engine_dir, None)
                      if f.is_relative_to(self.engine_dir / "third_party") and not f.is_file()]
        if repo_texts:
            raise ReleaseError("No license text on disk for: " + "; ".join(f"{n} ({f})" for n, f in repo_texts))

        # 9. output directory is writable
        try:
            assert self.output_path is not None
            parent_to_test = self.output_path if self.output_path.exists() else self.output_path.parent
            parent_to_test.mkdir(parents=True, exist_ok=True)
            test_file = parent_to_test / f".__release_write_test_{os.getpid()}__"
            test_file.write_text("test", encoding="utf-8")
            test_file.unlink()
            results.append(("output directory is writable", True, str(self.output_path)))
        except Exception as e:
            raise ReleaseError(f"Output directory '{self.output_path}' is not writable: {e}")

        # 10. Steam AppID + runtime check (if steam.prepare is enabled)
        check_steam_app_id(self.project_data, self.steam_enabled)
        if self.steam_enabled:
            steam_lib = self._steam_lib_path()
            if not steam_lib.exists():
                raise ReleaseError(
                    f"Steam preparation enabled (steam.prepare=true), but {steam_lib.name} was not found at '{steam_lib}'. "
                    "Ensure the Steamworks SDK is at ../steamworks_sdk_165/sdk or configure steam.sdk_path in "
                    "release.json (or pass --no-steam)."
                )
            results.append(("Steamworks SDK runtime available", True, f"{steam_lib.name} at '{steam_lib}'"))

        for item, status, detail in results:
            stat_str = "[OK]" if status else "[FAIL]"
            print(f"  {stat_str} {item} ({detail})")

        return results

    def stage_game_tests(self):
        """Runs the game's own `tools/verify.py full` before any shipping compilation."""
        print("[1b/7] Running game verification (verify.py full)...")
        verify = self.game_dir / "tools" / "verify.py"
        if not verify.exists():
            raise ReleaseError(f"Game verification script missing: {verify} (use --skip-game-tests for dev only)")
        cmd = [sys.executable, str(verify), "full"]
        code = subprocess.call(cmd, cwd=str(self.game_dir))
        if code != 0:
            raise ReleaseError(f"Game verification failed (exit code {code}): {' '.join(cmd)}")

    # -------------------------------------------------------------------------
    # Stage 2: Clean Staging
    # -------------------------------------------------------------------------
    def stage_clean_staging(self):
        """Prepares a clean staging directory."""
        print("[2/7] Cleaning staging...")
        def _remove_readonly(func, path, exc_info):
            import stat
            try:
                os.chmod(path, stat.S_IWRITE)
                func(path)
            except Exception:
                raise exc_info[1]

        if self.staging_dir.exists():
            try:
                shutil.rmtree(self.staging_dir, onerror=_remove_readonly)
            except Exception as e:
                raise ReleaseError(f"Failed to clean staging directory '{self.staging_dir}': {e}")

        self.staging_dir.mkdir(parents=True, exist_ok=True)
        (self.staging_dir / "assets").mkdir(parents=True, exist_ok=True)
        (self.staging_dir / "scripts").mkdir(parents=True, exist_ok=True)

        if self.config.clean_build and self.output_path and self.output_path.exists():
            print(f"  Cleaning existing release directory: {self.output_path}")
            try:
                for child in list(self.output_path.iterdir()):
                    if child.is_dir():
                        shutil.rmtree(child, onerror=_remove_readonly)
                    else:
                        try:
                            child.unlink()
                        except Exception:
                            os.chmod(child, 0o777)
                            child.unlink()
            except Exception as e:
                try:
                    shutil.rmtree(self.output_path, onerror=_remove_readonly)
                except Exception as e2:
                    raise ReleaseError(f"Failed to clean release output directory '{self.output_path}': {e2}")

    # -------------------------------------------------------------------------
    # Stage 3: Generate Secret and Manifest
    # -------------------------------------------------------------------------
    def _require_target_interpreter(self):
        """The running interpreter must be the one being packaged for.

        Game and library scripts are marshalled in-process (compile + marshal), so the .pyc files
        carry the *running* interpreter's bytecode format. `--python` only chooses the embedded
        runtime and its shipping build tree; it cannot change what this interpreter writes. A
        mismatch therefore produces a package the embedded engine cannot load (it fails on the first
        script, as a crash rather than an error), so refuse it here, before anything is built.
        """
        running = "%d.%d" % sys.version_info[:2]
        if running != self.python:
            raise ReleaseError(
                f"release.py is running under Python {running} but packages for Python {self.python}: "
                f"the game scripts are compiled to the running interpreter's marshalled bytecode, "
                f"which the packaged engine cannot load. Run it with the {self.python} interpreter "
                f"('py -{self.python} tools/release.py ...')."
            )

    def stage_generate_secret_and_manifest(self):
        """
        Generates secret header conforming to ReleaseSecret.h contract,
        compiles Python scripts to marshalled .pyc files, stages assets,
        and creates iyipack manifest.json.
        """
        print("[3/7] Generating release secret and packaging manifest...")
        if self.plat.id == "android" and sys.version_info[:2] != (3, 14):
            raise ReleaseError(f"Android release requires host Python 3.14 to match CPython 3.14.7, got {sys.version_info.major}.{sys.version_info.minor}")
        # The scripts below are marshalled by this interpreter, so it has to be the packaged one.
        if not self.native:
            self._require_target_interpreter()

        # 1. Generate or acquire release secret
        if self.config.encryption.enabled:
            if self.config.encryption.key_mode == "generated":
                # Strong 256-bit cryptographic secret in hex representation (64 chars)
                self._secret = secrets.token_hex(32)
                self._is_generated_key = True
            elif self.config.encryption.key_mode == "environment":
                self._secret = os.environ.get("IYI_RELEASE_KEY") or os.environ.get("IYI_ENCRYPTION_KEY")
                if not self._secret:
                    raise ReleaseError("Environment key mode specified but IYI_RELEASE_KEY is empty")
                self._is_generated_key = False
            else:
                raise ReleaseError(f"Invalid key_mode: {self.config.encryption.key_mode}")
        else:
            self._secret = None
            self._is_generated_key = False

        # 2. Write build-time ReleaseSecret.h header into build/generated_include/IYI/
        self._write_release_secret_header()
        self._write_version_resource()

        # 3. Compile every game and library .py to marshalled bytecode staged as matching .pyc path
        py_count = 0
        for rel, src_path in self.content_files("scripts", ".py"):
            # e.g., scripts/main.py -> scripts/main.pyc, scripts/actors/sim.py -> scripts/actors/sim.pyc
            dest_pyc_rel = Path(rel).with_suffix(".pyc")
            dest_pyc = self.staging_dir / dest_pyc_rel
            dest_pyc.parent.mkdir(parents=True, exist_ok=True)

            # Read and compile to marshalled bytecode
            source_code = src_path.read_text(encoding="utf-8")
            code_obj = compile(source_code, dest_pyc_rel.as_posix(), "exec")
            dest_pyc.write_bytes(marshal.dumps(code_obj))
            py_count += 1

        # The packaged import finder only knows regular packages: a script folder without __init__.py
        # (a namespace package such as scripts/actors) gets an empty one, or `import actors.x` fails.
        scripts_root = self.staging_dir / "scripts"
        pkgs = {d for p in scripts_root.rglob("*.pyc") for d in p.parents if scripts_root in d.parents}
        for pkg in pkgs:
            init = pkg / "__init__.pyc"
            if not init.exists():
                init.write_bytes(marshal.dumps(compile("", init.relative_to(self.staging_dir).as_posix(), "exec")))
        print(f"  Compiled {py_count} Python scripts to marshalled .pyc in staging")

        # 4. Copy the game and library assets the game can reach into staging
        staged_assets_dir = self.staging_dir / "assets"
        assets = dict(self.content_files("assets"))
        seed = [src.read_text(encoding="utf-8", errors="replace") for _, src in self.content_files("scripts", ".py")]
        seed.append(self.project_json_path.read_text(encoding="utf-8", errors="replace"))
        seed += [p.read_text(encoding="utf-8", errors="replace")
                 for p in (self.engine_dir / "src").glob("*.cpp")]  # the engine's own paths (locales)
        if self.native:   # a native game names its assets in C++ (the game's sources, its libraries' src/)
            skip = {"build", "bin", "assets", "release", "graft", ".git"}
            sources = [full for _, full in self.content_files("src")]
            for root, dirs, files in os.walk(self.cmake_dir):
                dirs[:] = [d for d in dirs if d not in skip]
                sources += [Path(root) / f for f in files if f.endswith((".cpp", ".h", ".hpp", ".cc"))]
            seed += [f.read_text(encoding="utf-8", errors="replace") for f in sources]
        kept = referenced_assets(assets, lambda rel: assets[rel].read_text(encoding="utf-8", errors="replace"),
                                 seed, self.config.keep_assets)
        dropped = sorted(set(assets) - kept)
        if dropped:
            size = sum(assets[rel].stat().st_size for rel in dropped)
            print(f"  Left out {len(dropped)} unreferenced assets ({size / 1048576:.1f} MB):")
            for rel in dropped:
                print(f"    - {rel}")
        for rel in sorted(kept):
            src_file = assets[rel]
            dest_file = self.staging_dir / rel
            dest_file.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src_file, dest_file)

        # 4b. Generate locale catalog in staging if assets/locales exists
        staged_locales_dir = staged_assets_dir / "locales"
        if staged_locales_dir.is_dir():
            catalog_file = iyiloc.write_catalog(staged_locales_dir)
            if catalog_file:
                print(f"  Generated locale catalog in staging: {catalog_file.relative_to(self.staging_dir).as_posix()}")

        # 5. Copy project.json to staging with hot_reload forced to false
        staged_project_json = self.staging_dir / "project.json"
        project_dict = dict(self.project_data)
        project_dict["hot_reload"] = False
        project_dict["libraries"] = self.libraries()
        staged_project_json.write_text(json.dumps(project_dict, indent=4), encoding="utf-8")

        # 6. Generate iyipack manifest.json
        manifest_data = {
            "input_root": ".",
            "output_file": "game.rres",
            "include": ["**/*"],
            "exclude": [
                "**/*.py",
                "**/manifest.json",
                "**/__pycache__/**",
                "**/.git*",
                "**/.DS_Store",
                "**/Thumbs.db",
            ],
            "compression": {
                "default": self.config.compression.algorithm if self.config.compression.enabled else "none",
                "extensions": self.config.compression.overrides,
            },
            "encryption": {
                "enabled": self.config.encryption.enabled,
                "key_env": "IYI_RELEASE_KEY" if self.config.encryption.enabled else "",
                "key_file": "",
            },
        }

        manifest_path = self.staging_dir / "manifest.json"
        manifest_path.write_text(json.dumps(manifest_data, indent=2), encoding="utf-8")
        print("  Generated manifest.json in staging (key material kept out of manifest)")

    def _write_version_resource(self):
        """Windows only: GameVersion.rc next to the generated headers, so the shipping exe carries
        the game's name (Task Manager, file Properties) instead of showing as a bare file name,
        and release.json's exe_icon as its Explorer icon.
        CMakeLists.txt compiles it in when it exists."""
        rc_path = self.gen_include_dir / "GameVersion.rc"
        if self.plat.id != "windows":
            rc_path.unlink(missing_ok=True)
            return
        name = str(self.project_data.get("name") or self.config.output_name).replace('"', '""')
        version = str(self.project_data.get("version") or "0")
        parts = ([int(p) for p in re.findall(r"\d+", version)] + [0, 0, 0, 0])[:4]
        numeric = ",".join(str(min(p, 65535)) for p in parts)
        exe_name = self.config.output_name + self.plat.exe_suffix
        icon_line = ""
        if self.config.exe_icon:
            icon_path = (self.game_dir / self.config.exe_icon).resolve()
            if not icon_path.is_file():
                raise ReleaseError(f"release.json exe_icon not found: {icon_path}")
            icon_line = f'IDI_APP_ICON ICON "{icon_path.as_posix()}"\n'
        self.gen_include_dir.mkdir(parents=True, exist_ok=True)
        rc_path.write_text(f"""#pragma code_page(65001)
#include <winver.h>
{icon_line}VS_VERSION_INFO VERSIONINFO
FILEVERSION {numeric}
PRODUCTVERSION {numeric}
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904B0"
        BEGIN
            VALUE "FileDescription", "{name}"
            VALUE "ProductName", "{name}"
            VALUE "FileVersion", "{version}"
            VALUE "ProductVersion", "{version}"
            VALUE "InternalName", "{self.config.output_name}"
            VALUE "OriginalFilename", "{exe_name}"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
""", encoding="utf-8")

    def _write_release_secret_header(self):
        """
        Creates an authentic ReleaseSecret.h header in self.gen_include_dir/IYI/
        satisfying the ReleaseSecret.h contract:
          - Never store the decryption passphrase as a contiguous plaintext string
          - Partition into XOR-masked byte arrays
          - Reassemble at runtime when ReleaseSecret is instantiated
          - Zero-wipe internal buffer via volatile writes in destructor and Wipe()
          - Reside in namespace IYI with required public API
          - No forbidden headers included
        """
        committed_placeholder = (self.engine_dir / "include" / "IYI" / "ReleaseSecret.h").resolve()
        target_dir = self.gen_include_dir / "IYI"
        if target_dir.resolve() == (self.engine_dir / "include" / "IYI").resolve():
            raise ReleaseError("gen_include_dir must not point to the committed include directory!")
        target_dir.mkdir(parents=True, exist_ok=True)
        header_path = (target_dir / "ReleaseSecret.h").resolve()
        if header_path == committed_placeholder:
            raise ReleaseError("Refusing to write to committed placeholder: IYIEngine/include/IYI/ReleaseSecret.h")

        if not self._secret or not self.config.encryption.enabled:
            # Generate placeholder-compatible empty secret header
            secret_code = """// Generated empty secret header
        m_buffer.clear();
        m_buffer.push_back('\\0');
"""
        else:
            # Partition secret into 4 chunks XOR-masked with a random non-zero byte
            mask_byte = secrets.randbelow(254) + 1
            secret_bytes = [ord(c) for c in self._secret]
            masked_bytes = [b ^ mask_byte for b in secret_bytes]

            chunk_len = (len(masked_bytes) + 3) // 4
            chunks = [
                masked_bytes[0:chunk_len],
                masked_bytes[chunk_len:chunk_len * 2],
                masked_bytes[chunk_len * 2:chunk_len * 3],
                masked_bytes[chunk_len * 3:],
            ]

            chunk_decls = []
            reassemble_lines = []
            for idx, ch in enumerate(chunks):
                ch_name = f"s_frag_{idx}"
                hex_vals = ", ".join(f"0x{b:02x}" for b in ch)
                chunk_decls.append(f"        static const unsigned char {ch_name}[] = {{ {hex_vals} }};")
                reassemble_lines.append(
                    f"        for (size_t i = 0; i < sizeof({ch_name}); ++i) {{ "
                    f"m_buffer.push_back(static_cast<char>({ch_name}[i] ^ s_mask)); }}"
                )

            decls_str = "\n".join(chunk_decls)
            loops_str = "\n".join(reassemble_lines)

            secret_code = f"""        static const unsigned char s_mask = 0x{mask_byte:02x};
{decls_str}
        m_buffer.clear();
        m_buffer.reserve({len(masked_bytes) + 1});
{loops_str}
        m_buffer.push_back('\\0');
"""

        header_content = f"""// =============================================================================
// Generated Build-Time ReleaseSecret.h — Automated Release Pipeline
// =============================================================================
#pragma once

#include <cstddef>
#include <vector>

namespace IYI {{

class ReleaseSecret
{{
public:
    ReleaseSecret()
    {{
        Reassemble();
    }}

    ~ReleaseSecret()
    {{
        Wipe();
    }}

    ReleaseSecret(const ReleaseSecret&) = delete;
    ReleaseSecret& operator=(const ReleaseSecret&) = delete;

    ReleaseSecret(ReleaseSecret&& other) noexcept
        : m_buffer(std::move(other.m_buffer))
    {{
    }}

    ReleaseSecret& operator=(ReleaseSecret&& other) noexcept
    {{
        if (this != &other) {{
            Wipe();
            m_buffer = std::move(other.m_buffer);
        }}
        return *this;
    }}

    const char* Get() const
    {{
        return m_buffer.empty() ? "" : m_buffer.data();
    }}

    size_t Size() const
    {{
        return m_buffer.empty() ? 0 : (m_buffer.size() - 1);
    }}

    bool IsEmpty() const
    {{
        return Size() == 0;
    }}

    void Wipe()
    {{
        if (!m_buffer.empty()) {{
            volatile char* p = m_buffer.data();
            for (size_t i = 0; i < m_buffer.size(); ++i) {{
                p[i] = 0;
            }}
            m_buffer.clear();
        }}
    }}

private:
    void Reassemble()
    {{
{secret_code}
    }}

    std::vector<char> m_buffer;
}};

}} // namespace IYI
"""
        header_path.write_text(header_content, encoding="utf-8")
        print("  Generated build-time ReleaseSecret.h in build/generated_include/IYI/")

    # -------------------------------------------------------------------------
    # Stage 4: Build Shipping Exe
    # -------------------------------------------------------------------------
    def _extract_runtime(self) -> Path:
        """Linux/macOS: unpack the pinned python-build-standalone prefix; the engine links it and the package bundles it."""
        dest = self.engine_dir / "build" / "runtime" / self.plat.id
        shutil.rmtree(dest, ignore_errors=True)
        dest.mkdir(parents=True)
        with tarfile.open(self.runtime_archive) as t:
            t.extractall(dest, filter="data")
        return dest / "python"

    def shipping_exe(self) -> Path:
        """The built game executable: IYIEngine, or the one exe of a native game's bin/ in its shipping tree."""
        if not self.native:
            return self.shipping_dir / ("IYIEngine" + self.plat.exe_suffix)
        found = [p for p in (self.shipping_dir / "bin").glob("*" + self.plat.exe_suffix) if p.is_file() and p.suffix == self.plat.exe_suffix]
        if len(found) != 1:
            raise ReleaseError(f"Expected one game executable in {self.shipping_dir / 'bin'}, found {len(found)}")
        return found[0]

    def stage_build_shipping_exe(self):
        """Configures CMake with IYI_SHIPPING_BUILD=ON and builds IYIEngine and iyipack."""
        if self.plat.id == "android":
            print("[4/7] Building host iyipack only...")
            if not self.toolchain.cmake_exe or not self.toolchain.ninja_exe:
                raise ReleaseError("CMake or Ninja not found; cannot build host iyipack")
            self.iyipack_path = self._build_host_iyipack()
            return
        print("[4/7] Building Shipping executable and packaging tools...")

        if not self.toolchain.cmake_exe:
            raise ReleaseError("CMake executable not found; cannot perform C++ build")
        if not self.toolchain.ninja_exe:
            raise ReleaseError("Ninja executable not found; cannot perform C++ build")

        # Shipping uses its own build tree so the developer's build/ninja cache
        # is never switched to IYI_SHIPPING_BUILD=ON behind their back.
        build_dir = self.shipping_dir
        build_dir.mkdir(parents=True, exist_ok=True)

        env = self.toolchain.get_build_env()

        # Force ResourceStore.cpp recompilation so it always binds the freshly generated ReleaseSecret.h
        engine_tree = build_dir / "IYIEngine" if self.native else build_dir   # add_subdirectory(): the engine's own binary dir
        res_store_obj = engine_tree / "CMakeFiles" / "IYIEngineCore.dir" / "src" / f"ResourceStore.cpp{self.plat.obj_suffix}"
        if res_store_obj.exists():
            try:
                res_store_obj.unlink()
            except Exception:
                pass

        cmake_gen_cmd = [
            str(self.toolchain.cmake_exe),
            "-S", str(self.cmake_dir if self.native else self.engine_dir),
            "-B", str(build_dir),
            "-G", "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={self.toolchain.ninja_exe}",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DIYI_SHIPPING_BUILD=ON",
            "-DIYI_BUILD_TOOLS=ON",
            f"-DIYI_RAYLIB_VERSION={self.raylib}",
            f"-DIYI_GENERATED_INCLUDE_DIR={self.gen_include_dir.as_posix()}",
            # ON makes CMake fail when the platform's Steam library is missing instead of silently omitting Steam.
            f"-DIYI_STEAM={'ON' if self.steam_enabled else 'OFF'}",
            # Multiplayer (SPEC §25) is compiled in only for a game that lists libraries/net.
            f"-DIYI_NET={'ON' if any(Path(rel).name == 'net' for rel in self.libraries()) else 'OFF'}",
        ]
        if self.native:   # no interpreter, no bindings; the exe goes to the shipping tree, not to the developer's bin/
            cmake_gen_cmd += ["-DIYI_WITH_PYTHON=OFF", f"-DIYI_GAME_BIN_DIR={(build_dir / 'bin').as_posix()}"]
        else:
            cmake_gen_cmd.append(f"-DIYI_PYTHON_VERSION={self.python}")
        if self.steam_enabled:
            cmake_gen_cmd.append(f"-DIYI_STEAMWORKS_SDK={self._steam_sdk_dir().as_posix()}")
        if self.plat.id != "windows" and not self.native:
            self.python_root = self._extract_runtime()
            cmake_gen_cmd.append(f"-DIYI_PYTHON_ROOT={self.python_root.as_posix()}")
        if self.plat.id == "macos":
            cmake_gen_cmd += ["-DCMAKE_OSX_ARCHITECTURES=arm64", "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"]

        print("  Configuring CMake for Shipping build...")
        res = subprocess.run(cmake_gen_cmd, cwd=self.engine_dir, env=env)
        if res.returncode != 0:
            raise ReleaseError(f"CMake configuration failed with code {res.returncode}")

        print("  Compiling IYIEngine and iyipack...")
        build_cmd = [
            str(self.toolchain.cmake_exe),
            "--build", str(build_dir),
            "--config", "Release",
        ] + ([] if self.native else ["--target", "IYIEngine", "iyipack"])
        res = subprocess.run(build_cmd, cwd=self.engine_dir, env=env)
        if res.returncode != 0:
            raise ReleaseError(f"Engine compilation failed with code {res.returncode}")

        exe_path = self.shipping_exe()
        if not exe_path.exists():
            raise ReleaseError(f"Build completed but executable was not found at {exe_path}")

        print("  Shipping executable built successfully")

    def _build_host_iyipack(self) -> Path:
        build_dir = self.engine_dir / "build" / "release-iyipack-host"
        cmd = [str(self.toolchain.cmake_exe), "-S", str(self.engine_dir / "tools" / "iyipack"), "-B", str(build_dir),
               "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={self.toolchain.ninja_exe}", "-DCMAKE_BUILD_TYPE=Release"]
        for args in (cmd, [str(self.toolchain.cmake_exe), "--build", str(build_dir), "--config", "Release"]):
            res = subprocess.run(args, cwd=self.engine_dir, env=self.toolchain.get_build_env())
            if res.returncode:
                raise ReleaseError(f"Host iyipack build failed ({res.returncode}): {' '.join(args)}")
        exe = build_dir / "iyipack.exe"
        if not exe.is_file():
            raise ReleaseError(f"Host iyipack build completed but binary is missing: {exe}")
        return exe

    def _ensure_android_python(self) -> Path:
        lock = json.loads((self.engine_dir / "tools" / "mobile" / "dependency-lock.json").read_text(encoding="utf-8"))
        pin = lock["android"]["cpython"]
        root = self.engine_dir / "build" / "deps" / "python-android"
        archive = root / urllib.parse.unquote(pin["url"].rsplit("/", 1)[-1])
        ensure_pinned_download(archive, pin)
        prefix = root / "prefix"
        if not (prefix / "include").is_dir() or not (prefix / "lib" / "python3.14").is_dir():
            shutil.rmtree(prefix, ignore_errors=True)
            prefix.mkdir(parents=True)
            with tarfile.open(archive, "r:gz") as tar:
                members = {m.name.rstrip("/"): m for m in tar.getmembers() if m.isfile() or m.isdir() or m.issym()}
                for name, member in members.items():
                    if member.isdir() or member.issym():
                        continue
                    target = member
                    seen = set()
                    while target.issym():
                        if target.name in seen:
                            raise ReleaseError(f"CPython archive has a symlink cycle at {target.name}")
                        seen.add(target.name)
                        linked = posixpath.normpath(posixpath.join(posixpath.dirname(target.name), target.linkname))
                        if linked not in members:
                            raise ReleaseError(f"CPython archive symlink target is missing: {target.name} -> {linked}")
                        target = members[linked]
                    dest = prefix.joinpath(*Path(name).parts[1:]) if name.startswith("python/") else prefix.joinpath(*Path(name).parts)
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    src = tar.extractfile(target)
                    if src:
                        dest.write_bytes(src.read())
            if not (prefix / "include").is_dir() or not (prefix / "lib" / "python3.14").is_dir():
                raise ReleaseError(f"CPython Android archive did not produce expected prefix at {prefix}")
        return prefix

    def stage_android(self):
        print("[6/7] Building packed Android APK and AAB...")
        python_prefix = self._ensure_android_python()
        gradle = self.engine_dir / "platform" / "android" / "gradlew.bat"
        sdk = Path(os.environ.get("ANDROID_HOME", Path(os.environ.get("LOCALAPPDATA", "")) / "Android" / "Sdk"))
        ndk = sdk / "ndk" / "28.2.13676358"
        readelf = ndk / "toolchains" / "llvm" / "prebuilt" / "windows-x86_64" / "bin" / "llvm-readelf.exe"
        zipalign = sdk / "build-tools" / "36.0.0" / "zipalign.exe"
        if not gradle.is_file() or not readelf.is_file() or not zipalign.is_file():
            raise ReleaseError(f"Android tools missing: gradle={gradle.is_file()}, llvm-readelf={readelf.is_file()}, zipalign={zipalign.is_file()}")
        cfg = self.config.android
        staged_rres = self.staging_dir / "game.rres"
        icon_res = self.staging_dir / "android-icon-res"
        if self.config.exe_icon:
            generate_android_icons(self.game_dir / self.config.exe_icon, icon_res)
        props = {"applicationId": cfg["application_id"], "appName": cfg["app_name"],
                 "versionCode": cfg["version_code"], "versionName": cfg["version_name"],
                 "gameRres": staged_rres, "pythonPrefix": python_prefix, "generatedInclude": self.gen_include_dir,
                 # Google Play bridge (engine.play): empty values leave every feature off.
                 "playGamesAppId": cfg["play_games_app_id"], "licensePublicKey": cfg["license_public_key"],
                 "playAchievementIds": ";".join(f"{name}={achievement}" for name, achievement
                                                in sorted(cfg["play_achievement_ids"].items()))}
        if self.config.exe_icon:
            props["iconRes"] = icon_res
        env = os.environ.copy()
        # Gradle reads ORG_GRADLE_PROJECT_<name> as -P<name>; cmd.exe would split values with spaces.
        env.update({f"ORG_GRADLE_PROJECT_iyi.{k}": str(v) for k, v in props.items()})
        env.setdefault("JAVA_HOME", r"C:\Program Files\Microsoft\jdk-17.0.21.21-hotspot")
        env["ANDROID_HOME"] = str(sdk)
        command = "call " + subprocess.list2cmdline([str(gradle), ":app:assembleRelease", ":app:bundleRelease"])
        res = subprocess.run(["cmd.exe", "/d", "/s", "/c", command], cwd=gradle.parent, env=env)
        if res.returncode:
            raise ReleaseError(f"Gradle Android release failed with code {res.returncode}")
        apk_src = gradle.parent / "app" / "build" / "outputs" / "apk" / "release" / "app-release.apk"
        aab_src = gradle.parent / "app" / "build" / "outputs" / "bundle" / "release" / "app-release.aab"
        if not apk_src.is_file() or not aab_src.is_file():
            raise ReleaseError(f"Gradle completed but APK/AAB outputs are missing: {apk_src}, {aab_src}")
        res = run_tool([str(zipalign), "-c", "-P", "16", "-v", "4", str(apk_src)])
        print(res.strip())
        with zipfile.ZipFile(apk_src) as z:
            so_names = [n for n in z.namelist() if n.endswith(".so")]
            if "lib/arm64-v8a/libiyigame.so" not in so_names:
                raise ReleaseError("APK is missing lib/arm64-v8a/libiyigame.so")
            for name in so_names:
                temp = self.engine_dir / "build" / Path(name).name
                temp.parent.mkdir(parents=True, exist_ok=True)
                temp.write_bytes(z.read(name))
                try:
                    check_android_load_alignment(run_tool([str(readelf), "-lW", str(temp)]))
                    if name.endswith("libiyigame.so"):
                        symbols = run_tool([str(readelf), "--dyn-syms", str(temp)]) + run_tool([str(readelf), "-d", str(temp)])
                        if re.search(r"steam_api|glfw", symbols, re.I):
                            raise ReleaseError("libiyigame.so unexpectedly references Steam or GLFW")
                finally:
                    temp.unlink(missing_ok=True)
        check_android_rres_stored(apk_src)
        self.output_path.mkdir(parents=True, exist_ok=True)
        apk = self.output_path / (self.config.output_name + ".apk")
        aab = self.output_path / (self.config.output_name + ".aab")
        shutil.copy2(apk_src, apk)
        shutil.copy2(aab_src, aab)
        print(f"  Android packages: {apk}, {aab}")
        print(f"  Install on a connected device: adb install -r \"{apk}\"")

    # -------------------------------------------------------------------------
    # Stage 5: Pack
    # -------------------------------------------------------------------------
    def stage_pack(self):
        """Packs resources into game.rres using iyipack."""
        print("[5/7] Packing game resources with iyipack into game.rres...")

        iyipack_exe = self.iyipack_path or self.toolchain.locate_iyipack(self.shipping_dir if self.native else None)
        if not iyipack_exe:
            raise ReleaseError("iyipack executable not found after build step")

        manifest_file = self.staging_dir / "manifest.json"
        cmd = [str(iyipack_exe), "-m", str(manifest_file)]

        env = os.environ.copy()
        if self.config.encryption.enabled and self._secret:
            env["IYI_RELEASE_KEY"] = self._secret

        res = subprocess.run(cmd, cwd=self.staging_dir, env=env)
        if res.returncode != 0:
            raise ReleaseError(f"iyipack failed with code {res.returncode}")

        staged_rres = self.staging_dir / "game.rres"
        if not staged_rres.exists() or staged_rres.stat().st_size == 0:
            raise ReleaseError("Packaging completed but game.rres was not created or is empty")

        size_mb = staged_rres.stat().st_size / (1024 * 1024)
        print(f"  game.rres generated ({size_mb:.2f} MB)")

    # -------------------------------------------------------------------------
    # Stage 6: Assemble Runtime
    # -------------------------------------------------------------------------
    # Embed members no engine/game code imports (no sqlite, asyncio, multiprocessing, xml); _hashlib+libcrypto stay
    # for hashlib, _ssl+libssl for HTTPS/WSS (libraries/streaming).
    UNUSED_EMBED_FILES = frozenset((
        "sqlite3.dll", "_sqlite3.pyd", "_msi.pyd", "_asyncio.pyd",
        "_multiprocessing.pyd", "_overlapped.pyd", "pyexpat.pyd", "_elementtree.pyd", "winsound.pyd", "python.cat"))

    def _install_windows_runtime(self):
        """Embeddable CPython into <Name>_Data; python3XX.dll and the MSVC CRT beside the exe."""
        L = self.layout
        py_zip = self.runtime_archive
        ensure_pinned_download(py_zip, self.pin)
        saved_bytes = 0
        with zipfile.ZipFile(py_zip, "r") as z:
            for member in z.infolist():
                name = member.filename
                # python.exe/pythonw.exe are never shipped; the CRT comes from the toolchain, not the embed zip
                if name in ("python.exe", "pythonw.exe", "vcruntime140.dll", "vcruntime140_1.dll") or name in self.UNUSED_EMBED_FILES:
                    saved_bytes += member.file_size
                    continue
                if name == self.pin["pylib"]:
                    saved_bytes += member.file_size
                    with z.open(member) as src, open(L.libs / name, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    continue
                z.extract(member, L.data)
        print(f"  Installed Python {self.pin['version']} embed runtime into {L.data.name}")
        print(f"  Omitted redundant binaries from runtime folder (saved {saved_bytes:,} bytes / {saved_bytes / (1024 * 1024):.2f} MB)")

        self._install_msvc_crt()

    def _install_msvc_crt(self):
        if self.plat.id != "windows":
            return
        copied_crt = 0
        for dll_path in self.toolchain.msvc_dlls:
            if dll_path.exists():
                shutil.copy2(dll_path, self.layout.libs / dll_path.name)
                copied_crt += 1
        print(f"  Installed {copied_crt} MSVC CRT DLLs at release root")

    def _install_posix_runtime(self):
        """Linux/macOS: stdlib + extension modules into <Data>/runtime, libpython beside the exe / in Frameworks."""
        L, pin = self.layout, self.pin
        assert self.python_root is not None, "runtime was not extracted (build stage did not run)"
        shutil.copytree(self.python_root / pin["stdlib"], L.data / "runtime" / pin["stdlib"],
                        ignore=STDLIB_IGNORE, symlinks=True)
        shutil.copy2(self.python_root / "lib" / pin["pylib"], L.libs / pin["pylib"])
        print(f"  Installed Python {pin['version']} runtime ({pin['pylib']} + stdlib)")

    def _install_native_modules(self):
        """project.json "native" (engine SPEC §2b) into one <Name>_Data/native.

        A .pyd cannot be imported from inside game.rres, so these files stay loose in the data
        folder, where the shipping engine appends <Data>/native to sys.path. Every declared folder
        is flattened into that one directory: module file names are unique per interpreter, so two
        folders cannot collide, and the engine never learns which folder a module came from.
        """
        native_dirs = self.project_data.get("native", [])
        if not isinstance(native_dirs, list) or not native_dirs:
            return
        dest = self.layout.data / "native"
        # The interpreter tag CPython writes into an extension file name, e.g. "cp314" in
        # "native_example.cp314-win_amd64.pyd"; the higher-level target is self.python (3.14/3.11).
        tag = "cp" + self.python.replace(".", "")
        installed = 0
        for rel in native_dirs:
            src = (self.game_dir / str(rel)).resolve()
            if not src.is_dir():
                print(f"  [WARN] native folder not found: {rel}")
                continue
            for path in sorted(src.rglob("*")):
                if not path.is_file():
                    continue
                if path.suffix in (".pyd", ".so") and tag not in path.name:
                    print(f"  [WARN] native module '{path.name}' does not carry the {tag} interpreter "
                          f"tag; the package targets Python {self.python} and the import will fail at runtime")
                dest.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, dest / path.name)
                installed += 1
        if installed:
            print(f"  Installed {installed} native module file(s) into {dest.parent.name}/{dest.name}")

    def _codesign(self):
        """Ad-hoc by default; set IYI_CODESIGN_IDENTITY to a Developer ID identity for public distribution
        (notarization is not automated)."""
        L = self.layout
        ident = os.environ.get("IYI_CODESIGN_IDENTITY", "-")
        extra = [] if ident == "-" else ["--options", "runtime", "--timestamp"]
        for f in sorted(L.app.rglob("*")):
            if f.is_file() and not f.is_symlink() and f.suffix in (".so", ".dylib"):
                run_tool(["codesign", "--force", *extra, "-s", ident, str(f)])
        run_tool(["codesign", "--force", "--deep", *extra, "-s", ident, str(L.app)])

    def _finish_macos(self):
        """Info.plist, @rpath install names for the bundled dylibs, then (re)sign the whole bundle."""
        L = self.layout
        name = self.config.output_name
        publisher = re.sub(r"[^A-Za-z0-9.-]", "", str(self.project_data.get("publisher", "IYI"))) or "IYI"
        plist = {
            "CFBundleExecutable": name,
            "CFBundleIdentifier": f"com.{publisher}.{name}".lower(),
            "CFBundleName": name,
            "CFBundlePackageType": "APPL",
            "CFBundleShortVersionString": str(self.project_data.get("version", "1.0")),
            "LSMinimumSystemVersion": "11.0",
            "NSHighResolutionCapable": True,
        }
        with open(L.app / "Contents" / "Info.plist", "wb") as f:
            plistlib.dump(plist, f)

        dylibs = {d.name: d for d in L.libs.glob("*.dylib")}
        for d in dylibs.values():
            run_tool(["install_name_tool", "-id", f"@rpath/{d.name}", str(d)])
        for line in run_tool(["otool", "-L", str(L.exe)]).splitlines()[1:]:
            dep = line.strip().split(" (")[0]
            base = dep.rsplit("/", 1)[-1]
            if base in dylibs and not dep.startswith("@rpath/"):
                run_tool(["install_name_tool", "-change", dep, f"@rpath/{base}", str(L.exe)])
        self._codesign()
        print("  Built .app bundle (Info.plist, @rpath install names, signed)")

    def stage_assemble_runtime(self):
        """
        Assembles the platform package (see package_layout): executable, bundled CPython runtime,
        Steam library when enabled, and one <OutputName>_Data folder containing game.rres.
        """
        print(f"[6/7] Assembling runtime into {self.output_path}...")
        assert self.output_path is not None and self.layout is not None
        L, plat = self.layout, self.plat
        for d in (L.exe.parent, L.data, L.libs):
            d.mkdir(parents=True, exist_ok=True)

        # 1. Shipping executable renamed to <output_name>
        shutil.copy2(self.shipping_exe(), L.exe)
        print(f"  Installed game executable: {L.exe.name}")

        # 2. game.rres into the data folder
        shutil.copy2(self.staging_dir / "game.rres", L.data / "game.rres")
        print(f"  Installed archive: {L.data.name}/game.rres")

        # 3. Python runtime (a native game has none; on Windows it still needs the MSVC CRT beside the exe)
        if self.native:
            self._install_msvc_crt()
        elif plat.id == "windows":
            self._install_windows_runtime()
        else:
            self._install_posix_runtime()

        # 3b. Project-scoped native modules (SPEC §2b): loose in <Data>/native, never in game.rres.
        if not self.native:
            self._install_native_modules()

        # 3c. License texts of everything shipped, at the release root
        python_license = None if self.native else (L.data / "LICENSE.txt" if plat.id == "windows"
                                                   else self.python_root / self.pin["stdlib"] / "LICENSE.txt")
        self.notices = notice_sources(self.shipping_dir / "_deps", self.engine_dir, python_license)
        (self.output_path / THIRD_PARTY_NOTICES).write_text(build_notices(self.notices, plat.id == "windows"), encoding="utf-8")
        print(f"  Wrote {THIRD_PARTY_NOTICES} ({len(self.notices)} components)")

        # 4. Steamworks runtime library when Steam is enabled
        if self.steam_enabled:
            steam_lib = self._steam_lib_path()
            if not steam_lib.exists():
                raise ReleaseError(f"Steam runtime library missing at '{steam_lib}'")
            shutil.copy2(steam_lib, L.libs / steam_lib.name)
            print(f"  Installed Steam runtime: {steam_lib.name}")

        # 5. macOS: finish the .app bundle
        if plat.id == "macos":
            self._finish_macos()

        # 6. Windows hidden attribute on <OutputName>_Data
        # SteamPipe may not preserve this attribute, so the folder layout is the real fix and hiding is cosmetic.
        if sys.platform == "win32":
            try:
                res = subprocess.run(["attrib", "+h", str(L.data)], capture_output=True, text=True)
                if res.returncode != 0:
                    print(f"  [WARN] Failed to set hidden attribute on {L.data.name}: {res.stderr.strip()}")
                else:
                    print(f"  Set hidden attribute on {L.data.name}")
            except Exception as e:
                print(f"  [WARN] Unable to execute attrib on {L.data.name}: {e}")

        # 7. Optional archive (zip on Windows, tar.gz elsewhere so the executable bit survives)
        if self.config.package.create_zip:
            dest = self.output_path.parent / f"{self.output_path.name}.{plat.archive}"
            print(f"  Creating distribution archive: {dest.name}...")
            if plat.archive == "zip":
                with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as zf:
                    for root, _, files in os.walk(self.output_path):
                        for file in files:
                            p = Path(root) / file
                            zf.write(p, p.relative_to(self.output_path))
            else:
                with tarfile.open(dest, "w:gz") as tf:
                    tf.add(self.output_path, arcname=self.output_path.name)

    # -------------------------------------------------------------------------
    # Stage 7: Validate
    # -------------------------------------------------------------------------
    def stage_validate(self):
        """
        Runs post-build checks:
          - game.rres opens successfully inside <OutputName>_Data
          - encrypted resources can be decrypted with Shipping runtime config
          - expected critical resources exist in package
          - no raw game .py files present in release output
          - no loose game asset tree present in release output
          - no naked python folder at release root
          - release executable starts using packaged runtime rather than system Python
          - release mode has hot reload disabled
          - package does not contain temporary keys, pdb, obj, caches
        """
        print("[7/7] Validating release package...")
        assert self.output_path is not None

        L, plat, pin = self.layout, self.plat, self.pin
        data_dir = L.data
        data_folder_name = data_dir.name
        runtime_dir = data_dir / "runtime"

        def release_files():
            # the Linux/macOS stdlib legitimately ships loose .py files and module names; everything else is ours
            for root, dirs, files in os.walk(self.output_path):
                dirs[:] = [d for d in dirs if Path(root) / d != runtime_dir]
                yield from files

        # 1. game.rres opens successfully inside <OutputName>_Data
        rres_path = data_dir / "game.rres"
        if not rres_path.exists():
            raise ReleaseError(f"Validation failed: game.rres missing from {data_folder_name}")

        with open(rres_path, "rb") as f:
            header_bytes = f.read(16)
            if len(header_bytes) < 16:
                raise ReleaseError("Validation failed: game.rres header is truncated")
            magic, version, chunk_count, cd_offset, reserved = struct.unpack("<4sHHII", header_bytes)
            if magic != b"rres" or version != 100:
                raise ReleaseError(f"Validation failed: Invalid rres header signature: {magic}, version: {version}")
            if cd_offset != 0:
                raise ReleaseError("Validation failed: Central Directory must be omitted for privacy")

            # 2. Inspect resource chunks
            chunk_ids: Set[int] = set()
            for _ in range(chunk_count):
                chunk_info_bytes = f.read(32)
                if len(chunk_info_bytes) < 32:
                    break
                (c_type, c_id, c_comp, c_cipher, c_flags,
                 c_packed_size, c_base_size, c_next, c_res, c_crc) = struct.unpack("<4sIBBHIIIII", chunk_info_bytes)

                chunk_ids.add(c_id)
                if self.config.encryption.enabled and c_cipher != 73:
                    raise ReleaseError(f"Validation failed: Chunk {c_id:08X} is not encrypted with XChaCha20-Poly1305")
                f.seek(c_packed_size, os.SEEK_CUR)

        print(f"  [OK] game.rres verified ({chunk_count} valid chunks, CD omitted)")

        # 3. Expected critical resources exist in package
        project_json_id = compute_resource_id("project.json")
        if project_json_id not in chunk_ids:
            raise ReleaseError("Validation failed: Critical resource 'project.json' not found in game.rres")
        if not self.native:
            entry_script_rel = self.project_data.get("entry_script", "scripts/main.py")
            entry_pyc_norm = normalize_resource_path(entry_script_rel.replace(".py", ".pyc"))
            if compute_resource_id(entry_pyc_norm) not in chunk_ids:
                raise ReleaseError(f"Validation failed: Critical entry script '{entry_pyc_norm}' not found in game.rres")

        print("  [OK] Critical packaged resources verified (project.json, entry script)")

        # 4. No raw game .py files in release output
        for f in release_files():
            if f.endswith(".py"):
                raise ReleaseError(f"Validation failed: Loose raw .py file found in release output: {f}")
        print("  [OK] No loose game .py files in release output")

        # 5. No loose game asset tree, loose scripts, loose project.json, or naked python/ directory
        if (self.output_path / "assets").exists() or (data_dir / "assets").exists():
            raise ReleaseError("Validation failed: Loose assets/ folder found in release output")
        if (self.output_path / "scripts").exists() or (data_dir / "scripts").exists():
            raise ReleaseError("Validation failed: Loose scripts/ folder found in release output")
        if (self.output_path / "project.json").exists() or (data_dir / "project.json").exists():
            raise ReleaseError("Validation failed: Loose project.json found in release output")
        if (self.output_path / "python").exists():
            raise ReleaseError("Validation failed: Naked python/ folder found at release root")
        print("  [OK] No loose game asset or script tree in release output")

        # 6. Executable, runtime and libraries present in the platform layout
        if not L.exe.exists():
            raise ReleaseError(f"Validation failed: Executable '{L.exe.name}' missing at {L.exe}")
        if plat.id != "windows" and not os.access(L.exe, os.X_OK):
            raise ReleaseError(f"Validation failed: '{L.exe.name}' is not executable")
        check_no_trailer_exe(L.exe.read_bytes())
        print("  [OK] No IYITrailer code in the executable")
        if not data_dir.exists():
            raise ReleaseError(f"Validation failed: Data directory '{data_folder_name}' missing from output")
        if self.native:
            pass   # no Python runtime
        elif plat.id == "windows":
            if not (data_dir / pin["stdlib"]).exists():
                raise ReleaseError(f"Validation failed: Python embed zip missing from {data_folder_name}")
            for dup in (pin["pylib"], "vcruntime140.dll", "vcruntime140_1.dll"):
                if (data_dir / dup).exists():
                    raise ReleaseError(f"Validation failed: Duplicate DLL '{dup}' found in {data_folder_name}")
        elif not (runtime_dir / pin["stdlib"] / "os.py").exists():
            raise ReleaseError(f"Validation failed: Python stdlib missing from {runtime_dir / pin['stdlib']}")
        libs = (list(WIN_CRT_DLLS) if plat.id == "windows" else []) if self.native else runtime_lib_names(plat, pin)
        libs += [Path(plat.steam_lib).name] if self.steam_enabled else []
        for n in libs:
            if not (L.libs / n).exists():
                raise ReleaseError(f"Validation failed: '{n}' missing from {L.libs}")

        # No python executable anywhere in the package
        for root, _, files in os.walk(self.output_path):
            for f in files:
                if f in ("python.exe", "pythonw.exe", "python", "python3", "python" + self.python):
                    raise ReleaseError(f"Validation failed: '{f}' must not be copied into release output")

        # Nothing else sits at the release root besides the platform layout
        allowed_files = {p.name.lower() for p in [L.exe] + [L.libs / n for n in libs] if p.parent == self.output_path}
        allowed_files.add(THIRD_PARTY_NOTICES.lower())
        allowed_dirs = {L.data.relative_to(self.output_path).parts[0]}

        # Explicitly ensure steam_appid.txt is never present in shipping builds
        for d in {self.output_path, L.exe.parent}:
            if (d / "steam_appid.txt").exists():
                raise ReleaseError("Validation failed: steam_appid.txt must not be present in shipping release")

        for item in self.output_path.iterdir():
            if item.is_dir():
                if item.name not in allowed_dirs:
                    raise ReleaseError(f"Validation failed: Unexpected directory '{item.name}' at release root")
            elif item.name.lower() not in allowed_files and not item.name.endswith((".zip", ".tar.gz")):
                raise ReleaseError(f"Validation failed: Unexpected file '{item.name}' at release root")

        print("  [OK] Standalone runtime verified (executable, python runtime, dependencies, clean root layout)")

        # 6b. Third-party license texts are shipped
        notices = self.output_path / THIRD_PARTY_NOTICES
        if not notices.is_file():
            raise ReleaseError(f"Validation failed: {THIRD_PARTY_NOTICES} missing from the release root")
        check_notices(notices.read_text(encoding="utf-8"), self.notices, plat.id == "windows")
        print(f"  [OK] {THIRD_PARTY_NOTICES} has a license text for each of {len(self.notices)} components")

        # 7. Hot reload disabled
        staged_proj = self.staging_dir / "project.json"
        if staged_proj.exists():
            staged_data = json.loads(staged_proj.read_text(encoding="utf-8"))
            if staged_data.get("hot_reload") is not False:
                raise ReleaseError("Validation failed: Staged project.json does not have hot_reload disabled")
        print("  [OK] Hot reload disabled in release metadata")

        # 8. No leftover key material, source maps, or build caches
        forbidden_extensions = {".key", ".pdb", ".obj", ".o", ".a", ".ninja", ".tmp"}
        for f in release_files():
            if Path(f).suffix.lower() in forbidden_extensions:
                raise ReleaseError(f"Validation failed: Developer/debug file found in release output: {f}")
            if "secret" in f.lower() or "key" in f.lower():
                raise ReleaseError(f"Validation failed: Potential secret artifact found in release output: {f}")
        print("  [OK] No leftover key material, compiler caches, or debug files")

        # 8b. Native dependency closure (Linux: ldd, macOS: otool/codesign) and no build-machine paths in the exe
        self._check_native_deps()

        # 9. Runtime smoke test: launch release executable, verify initialization & logs under per-user data root
        if self.skip_smoke:
            print("  [WARN] --skip-smoke: runtime smoke test SKIPPED (CI runners without a GPU only)")
            return
        print("  Running release executable smoke test...")
        if sys.platform == "win32":
            local_app = os.environ.get("LOCALAPPDATA")
            if local_app:
                base_dir = Path(local_app)
            else:
                user_prof = os.environ.get("USERPROFILE")
                if user_prof:
                    base_dir = Path(user_prof) / "AppData" / "Local"
                else:
                    base_dir = Path(os.environ.get("TEMP", "C:/Temp"))
        elif sys.platform == "darwin":
            home = os.environ.get("HOME", "")
            base_dir = Path(home) / "Library" / "Application Support" if home else Path("/tmp")
        else:
            xdg = os.environ.get("XDG_DATA_HOME")
            if xdg:
                base_dir = Path(xdg)
            else:
                home = os.environ.get("HOME", "")
                base_dir = Path(home) / ".local" / "share" if home else Path("/tmp")

        publisher = self.project_data.get("publisher", "IYI")
        game_name = self.project_data.get("name", "IYI Game")
        user_log_file = base_dir / publisher / game_name / "logs" / "latest.log"

        if user_log_file.exists():
            try:
                user_log_file.unlink()
            except Exception:
                pass

        # A Steam-enabled shipping exe launched outside Steam asks Steam to relaunch it and
        # exits at once (SteamAPI_RestartAppIfNecessary). steam_appid.txt beside the exe is
        # the SDK's documented opt-out, so it exists only while the smoke test runs; the
        # validation above has already proved the real output does not ship it.
        smoke_appid_file = None
        steam_app_id = int(self.project_data.get("steam_app_id", 0) or 0)
        if self.steam_enabled and steam_app_id:
            smoke_appid_file = L.exe.parent / "steam_appid.txt"
            smoke_appid_file.write_text(str(steam_app_id), encoding="ascii")

        # --frames makes the exe quit on its own, so the smoke test also covers shutdown: a crash
        # there (e.g. a CRT older than the toolset) only shows up in the exit code.
        cmd = [str(L.exe), "--frames", "120"]
        if plat.id == "linux" and not os.environ.get("DISPLAY"):
            xvfb = shutil.which("xvfb-run")
            if not xvfb:
                raise ReleaseError("Smoke test needs a display: set DISPLAY or install xvfb (xvfb-run)")
            cmd = [xvfb, "-a", "-s", "-screen 0 1280x720x24"] + cmd

        bundled_python = True   # a native game has no libpython to find
        try:
            try:
                # No PYTHON*/LD_LIBRARY_PATH/python on PATH; off-Windows also start from a foreign cwd.
                proc = subprocess.Popen(
                    cmd,
                    cwd=str(self.output_path if plat.id == "windows" else tempfile.gettempdir()),
                    env=smoke_env(os.environ.copy()),
                )
            except Exception as e:
                raise ReleaseError(f"Smoke test failed: Unable to launch executable '{L.exe.name}': {e}")

            # Wait for the engine's IYI_RUNTIME_READY marker rather than a fixed delay:
            # startup time varies (Steam init alone can take over a second), and a fixed
            # window kills a healthy exe mid-start. The timeout only bounds a hang.
            # A GPU machine quits in seconds; CI's software OpenGL (Mesa on Windows) needs far longer.
            smoke_duration = 90.0
            start_time = time.time()
            exit_code = None

            while time.time() - start_time < smoke_duration:
                exit_code = proc.poll()
                if exit_code is not None:
                    break
                try:
                    if RUNTIME_READY_MARKER in user_log_file.read_text(encoding="utf-8", errors="replace"):
                        break
                except OSError:
                    pass
                time.sleep(0.2)

            if exit_code is None:
                if plat.id == "linux" and not self.native:
                    bundled_python = libpython_mapped(L.libs / pin["pylib"])
                try:
                    exit_code = proc.wait(timeout=smoke_duration)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2.0)
                    raise ReleaseError(f"Smoke test failed: Release executable did not quit after --frames within {smoke_duration:.0f}s")

            if exit_code != 0:
                log_detail = ""
                if user_log_file.exists():
                    try:
                        log_detail = "\nLog tail:\n" + user_log_file.read_text(encoding="utf-8", errors="replace")[-1000:]
                    except Exception:
                        pass
                raise ReleaseError(
                    f"Smoke test failed: Release executable crashed or exited prematurely (exit code {exit_code}).{log_detail}"
                )

        finally:
            if smoke_appid_file is not None:
                smoke_appid_file.unlink(missing_ok=True)

        if not bundled_python:
            raise ReleaseError(f"Smoke test failed: the packaged {pin['pylib']} was not the libpython loaded")

        if not user_log_file.exists():
            raise ReleaseError(
                f"Smoke test failed: Log file was not created under per-user data root at '{user_log_file}'. "
                "Executable failed to initialize properly."
            )

        log_content = user_log_file.read_text(encoding="utf-8", errors="replace")
        if not log_content.strip():
            raise ReleaseError(f"Smoke test failed: Log file at '{user_log_file}' is empty")

        validate_runtime_log(log_content, user_log_file, python=not self.native)

        print(f"  [OK] Smoke test passed: executable started, initialized, and ran without error (log: {user_log_file})")

    def _check_native_deps(self):
        """Platform-only checks (need ldd / otool / codesign, so they run on the native host only)."""
        L, plat = self.layout, self.plat
        if plat.id == "windows":
            return
        exe_bytes = L.exe.read_bytes()
        for bad in (self.engine_dir.as_posix(), str(self.engine_dir)):
            if bad.encode() in exe_bytes:
                raise ReleaseError(f"Validation failed: build-machine path '{bad}' is embedded in {L.exe.name}")
        if plat.id == "linux":
            check_ldd_output(run_tool(["ldd", str(L.exe)], env=smoke_env(os.environ.copy())), L.libs.resolve())
        else:
            for f in [L.exe, *sorted(L.libs.glob("*.dylib"))]:
                check_otool_output(run_tool(["otool", "-L", str(f)]))
            for f in sorted(L.data.rglob("*.so")):
                check_otool_output(run_tool(["otool", "-L", str(f)]), strict=False)
            run_tool(["codesign", "--verify", "--deep", "--strict", str(L.app)])
        print("  [OK] Native dependency closure verified (no unresolved or build-machine libraries)")

    # -------------------------------------------------------------------------
    # Cleanup & Secret Destruction
    # -------------------------------------------------------------------------
    def cleanup_secrets_and_temps(self):
        """
        Destroys temporary secret material even on failure.
        Ensures secret header is wiped with zeroes before deletion.
        """
        # Wipe and remove build/generated_include only (never touch committed include directory)
        committed_placeholder = (self.engine_dir / "include" / "IYI" / "ReleaseSecret.h").resolve()
        committed_include = (self.engine_dir / "include").resolve()

        if self.gen_include_dir.exists() and self.gen_include_dir.resolve() != committed_include:
            secret_header = (self.gen_include_dir / "IYI" / "ReleaseSecret.h").resolve()
            if secret_header.exists() and secret_header != committed_placeholder:
                try:
                    # Overwrite generated build-time file content with zeros before deletion
                    secret_header.write_bytes(b"\x00" * 4096)
                except Exception:
                    pass
            shutil.rmtree(self.gen_include_dir, ignore_errors=True)

        # Clear environment variable if it was generated by the pipeline
        if self._is_generated_key and "IYI_RELEASE_KEY" in os.environ:
            del os.environ["IYI_RELEASE_KEY"]

        # Clear memory reference
        self._secret = None

    @staticmethod
    def _clean_pycache(directory: Path):
        """Deletes any __pycache__ subdirectories."""
        for root, dirs, _ in os.walk(directory):
            for d in list(dirs):
                if d == "__pycache__":
                    shutil.rmtree(Path(root) / d, ignore_errors=True)
                    dirs.remove(d)

    # -------------------------------------------------------------------------
    # Main Execution Runner
    # -------------------------------------------------------------------------
    def run(self, preflight_only: bool = False):
        try:
            self.stage_preflight()
            if preflight_only:
                print("Preflight validation succeeded. No build requested.")
                return 0

            if self.skip_game_tests:
                print("[WARN] --skip-game-tests: game verification SKIPPED (dev only, never ship this)")
            else:
                self.stage_game_tests()

            self.stage_clean_staging()
            self.stage_generate_secret_and_manifest()
            self.stage_build_shipping_exe()
            self.stage_pack()
            if self.plat.id == "android":
                self.stage_android()
                print("=======================================================================")
                print(f"SUCCESS: {self.output_path}")
                print("=======================================================================")
                return 0
            self.stage_assemble_runtime()
            self.stage_validate()

            print("=======================================================================")
            print(f"SUCCESS: {self.output_path}")
            print("=======================================================================")
            return 0
        except ReleaseError as e:
            print(f"[ERROR] {e}", file=sys.stderr)
            return 1
        except Exception as e:
            print(f"[ERROR] Unexpected release error: {e}", file=sys.stderr)
            return 1
        finally:
            self.cleanup_secrets_and_temps()


# =============================================================================
# CLI Entry Point
# =============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(description="IYI Engine One-Click Release Pipeline")
    parser.add_argument("--game", type=str, default="", help="Path to game repository directory")
    parser.add_argument("--engine", type=str, default="", help="Path to engine repository directory")
    parser.add_argument("--config", type=str, default="", help="Path to release.json configuration override")
    parser.add_argument("--preflight-only", action="store_true", help="Run preflight checks only and exit")

    parser.add_argument("--platform", choices=sorted(PLATFORMS), default=host_platform(),
                        help="Target platform; must be the host OS (default: host)")
    parser.add_argument("--no-steam", action="store_true",
                        help="Build without Steam even if release.json has steam.prepare=true (CI without the SDK)")
    parser.add_argument("--skip-game-tests", action="store_true",
                        help="DEV ONLY: skip the game's verify.py full stage before building")
    parser.add_argument("--python", choices=PYTHON_VERSIONS, default=DEFAULT_PYTHON,
                        help="Embedded CPython version (default: %(default)s; 3.11 is the legacy option)")
    parser.add_argument("--raylib", choices=RAYLIB_VERSIONS, default=DEFAULT_RAYLIB,
                        help="raylib version (default: %(default)s; 5.5 is the legacy option)")
    parser.add_argument("--skip-smoke", action="store_true",
                        help="CI ONLY: skip launching the packaged game (runners without a GPU cannot open a window)")

    args = parser.parse_args()
    if args.platform == "android" and host_platform() != "windows":   # packaging uses .exe/.bat/cmd tools
        print("[ERROR] Android packaging currently runs on a Windows host only.", file=sys.stderr)
        return 1
    if args.platform not in (host_platform(), "android"):
        print(f"[ERROR] Cannot build '{args.platform}' on a {host_platform() or sys.platform} host; "
              "release builds must run natively on the target OS.", file=sys.stderr)
        return 1

    # Determine script and repository paths
    script_dir = Path(__file__).resolve().parent
    engine_dir = Path(args.engine).resolve() if args.engine else script_dir.parent

    if args.game:
        game_dir = Path(args.game).resolve()
    else:
        # Check standard sibling layout: ../HumanResources
        sibling_game = engine_dir.parent / "HumanResources"
        if sibling_game.exists():
            game_dir = sibling_game
        else:
            game_dir = Path.cwd()

    config_path = Path(args.config).resolve() if args.config else None

    pipeline = ReleasePipeline(game_dir=game_dir, engine_dir=engine_dir, config_override=config_path,
                               skip_game_tests=args.skip_game_tests, platform=args.platform, no_steam=args.no_steam,
                               skip_smoke=args.skip_smoke, python=args.python, raylib=args.raylib)
    return pipeline.run(preflight_only=args.preflight_only)


if __name__ == "__main__":
    sys.exit(main())
