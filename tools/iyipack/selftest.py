#!/usr/bin/env python3
"""
tools/iyipack/selftest.py — Bounded Self-Test for iyipack Invariants
-------------------------------------------------------------------
Verifies binary archive layout and resource identification invariants:
  1. Locates iyipack binary using the same candidate discovery as release.py.
  2. Creates a temporary project tree with mixed-case and nested files.
  3. Packages the tree into an encrypted game.rres with a key from an env var.
  4. Parses the binary rres output and asserts:
     - Header magic equals b"rres" (0x73657272)
     - Format version equals 100 (rres v1.0)
     - Central directory offset is 0 (omitted to avoid path leakage)
     - Chunk count matches expected file count
     - Every chunk ID matches the CRC32 of the normalized lowercase path
  5. Cleans up all temporary files on completion.
"""

import binascii
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, Optional, Set


def normalize_resource_path(path_str: str) -> str:
    """Normalizes path matching IYI::ResourceKey (engine and iyipack)."""
    if not path_str:
        return ""
    norm = path_str.replace("\\", "/").lower()
    parts = []
    for part in norm.split("/"):
        if part == "" or part == ".":
            continue
        if part == "..":
            if parts:
                parts.pop()
        else:
            parts.append(part)
    result = "/".join(parts).lstrip("/")
    return "" if result == "." else result


def compute_resource_id(norm_path: str) -> int:
    """Computes IEEE 802.3 CRC32 resource ID."""
    return binascii.crc32(norm_path.encode("utf-8")) & 0xFFFFFFFF


def locate_iyipack(engine_dir: Path) -> Optional[Path]:
    """Locates iyipack executable matching release.py discovery order."""
    candidates = [
        engine_dir / "build" / "shipping" / "tools" / "iyipack" / "iyipack.exe",
        engine_dir / "build" / "shipping" / "iyipack.exe",
        engine_dir / "build" / "ninja" / "tools" / "iyipack" / "iyipack.exe",
        engine_dir / "build" / "ninja" / "iyipack.exe",
        engine_dir / "tools" / "iyipack" / "iyipack.exe",
    ]
    for cand in candidates:
        if cand.exists():
            return cand
    which_iyipack = shutil.which("iyipack")
    if which_iyipack:
        return Path(which_iyipack)
    return None


def run_selftest() -> int:
    # Resolve repository root from script path: tools/iyipack/selftest.py -> engine_dir
    script_path = Path(__file__).resolve()
    engine_dir = script_path.parent.parent.parent

    iyipack_exe = locate_iyipack(engine_dir)
    if not iyipack_exe:
        print("[FAIL] Could not locate iyipack executable in standard build directories or PATH", file=sys.stderr)
        return 1

    print(f"Using iyipack: {iyipack_exe}")

    temp_dir = Path(tempfile.mkdtemp(prefix="iyipack_selftest_"))
    try:
        # Define test files with mixed cases and nested paths
        test_files = {
            "project.json": '{"name": "SelftestGame", "entry_script": "scripts/Main.pyc"}',
            "scripts/Main.pyc": "FAKE_BYTECODE_MAIN",
            "Assets/Textures/Hero_Idle.png": "FAKE_PNG_BYTES_12345",
            "assets/Audio/SFX_jump.WAV": "FAKE_WAV_BYTES_67890",
        }

        # Populate temporary directory
        for rel_path, content in test_files.items():
            full_path = temp_dir / rel_path
            full_path.parent.mkdir(parents=True, exist_ok=True)
            full_path.write_text(content, encoding="utf-8")

        # Map expected normalized paths to CRC32 IDs
        expected_ids: Dict[int, str] = {}
        for rel_path in test_files.keys():
            norm = normalize_resource_path(rel_path)
            res_id = compute_resource_id(norm)
            expected_ids[res_id] = norm

        # Generate manifest.json in temp_dir
        manifest_path = temp_dir / "manifest.json"
        archive_path = temp_dir / "output.rres"
        manifest_data = {
            "input_root": ".",
            "output_file": str(archive_path.name),
            "include": ["**/*"],
            "exclude": ["**/manifest.json"],
            "compression": {
                "default": "deflate",
            },
            "encryption": {
                "enabled": True,
                "key_env": "IYI_RELEASE_KEY",
            },
        }
        manifest_path.write_text(json.dumps(manifest_data, indent=2), encoding="utf-8")

        # Pass encryption secret strictly via environment variable
        env = os.environ.copy()
        env["IYI_RELEASE_KEY"] = "iyipack_selftest_passphrase_secret_key"

        cmd = [str(iyipack_exe), "-m", str(manifest_path.name)]
        res = subprocess.run(cmd, cwd=temp_dir, env=env, capture_output=True, text=True)
        if res.returncode != 0:
            print(f"[FAIL] iyipack execution failed with code {res.returncode}:\n{res.stderr}", file=sys.stderr)
            return 1

        if not archive_path.exists():
            print(f"[FAIL] Expected archive not found at {archive_path}", file=sys.stderr)
            return 1

        # Read and validate rres archive binary invariants
        with open(archive_path, "rb") as f:
            header_bytes = f.read(16)
            if len(header_bytes) < 16:
                print(f"[FAIL] Truncated rres header: read {len(header_bytes)} bytes", file=sys.stderr)
                return 1

            magic, version, chunk_count, cd_offset, reserved = struct.unpack("<4sHHII", header_bytes)

            # Invariant 1: Header signature and version
            if magic != b"rres":
                print(f"[FAIL] Invalid rres magic signature: expected b'rres', got {magic!r}", file=sys.stderr)
                return 1
            if version != 100:
                print(f"[FAIL] Invalid rres format version: expected 100, got {version}", file=sys.stderr)
                return 1

            # Invariant 2: Central directory is omitted (cdOffset == 0)
            if cd_offset != 0:
                print(f"[FAIL] Central directory not omitted: cdOffset={cd_offset} (expected 0)", file=sys.stderr)
                return 1

            # Invariant 3: Chunk count matches expected file count
            if chunk_count != len(expected_ids):
                print(f"[FAIL] Chunk count mismatch: header reports {chunk_count}, expected {len(expected_ids)}", file=sys.stderr)
                return 1

            # Invariant 4: Every chunk id equals CRC32 of normalized forward-slash lowercase path
            found_ids: Set[int] = set()
            for i in range(chunk_count):
                chunk_info_bytes = f.read(32)
                if len(chunk_info_bytes) < 32:
                    print(f"[FAIL] Truncated chunk info header at index {i}", file=sys.stderr)
                    return 1

                (c_type, c_id, c_comp, c_cipher, c_flags,
                 c_packed_size, c_base_size, c_next, c_res, c_crc) = struct.unpack("<4sIBBHIIIII", chunk_info_bytes)

                if c_id not in expected_ids:
                    print(f"[FAIL] Unexpected chunk ID in archive: 0x{c_id:08X}", file=sys.stderr)
                    return 1

                if c_id in found_ids:
                    print(f"[FAIL] Duplicate chunk ID in archive: 0x{c_id:08X}", file=sys.stderr)
                    return 1

                found_ids.add(c_id)
                # Skip past chunk data payload to next chunk info
                f.seek(c_packed_size, os.SEEK_CUR)

            if found_ids != set(expected_ids.keys()):
                missing = set(expected_ids.keys()) - found_ids
                missing_names = [expected_ids[m] for m in missing]
                print(f"[FAIL] Missing expected resource IDs in archive: {missing_names}", file=sys.stderr)
                return 1

        print(f"[OK] Invariants verified: magic='rres', version=100, cdOffset=0, chunks={chunk_count}")
        for cid in found_ids:
            print(f"     Chunk 0x{cid:08X} -> '{expected_ids[cid]}'")
        return 0

    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(run_selftest())
