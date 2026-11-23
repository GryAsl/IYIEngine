#!/usr/bin/env python3
"""
IYIEngine Localization Tool (iyiloc)
------------------------------------
Standalone CLI and helper library for IYIEngine localization:
- Locale JSON file validation (check)
- New locale scaffolding from fallback (create)
- Catalog generation for release staging (_catalog.json)
- Placeholder extraction and mismatch detection
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional, Set, Tuple


class ValidationResult(NamedTuple):
    errors: list[str]
    warnings: list[str]
    locale_count: int

    @property
    def ok(self) -> bool:
        return len(self.errors) == 0


def extract_placeholders(text: str) -> set[str]:
    """Extracts the set of placeholder variable names ${name} from a string."""
    if not isinstance(text, str):
        return set()
    return set(re.findall(r"\$\{([^}]+)\}", text))


def find_placeholder_mismatches(
    fallback_dict: dict[str, str],
    locale_dict: dict[str, str],
) -> dict[str, tuple[set[str], set[str]]]:
    """
    Finds keys present in both dicts whose placeholder sets differ.
    Comparison is based on the set of placeholder names (order is irrelevant).
    Returns: {key: (fallback_placeholders, locale_placeholders)}
    """
    mismatches: dict[str, tuple[set[str], set[str]]] = {}
    for k in sorted(fallback_dict.keys()):
        if k == "_meta" or k not in locale_dict:
            continue
        fb_val = fallback_dict[k]
        loc_val = locale_dict[k]
        if isinstance(fb_val, str) and isinstance(loc_val, str):
            fb_ph = extract_placeholders(fb_val)
            loc_ph = extract_placeholders(loc_val)
            if fb_ph != loc_ph:
                mismatches[k] = (fb_ph, loc_ph)
    return mismatches


def get_fallback_language(game_dir: Path) -> str:
    """
    Determines fallback language from project.json default_language,
    falling back to 'en'.
    """
    candidate_paths = [
        game_dir / "project.json",
        game_dir.parent / "project.json",
        game_dir.parent.parent / "project.json",
    ]
    for proj_path in candidate_paths:
        if proj_path.is_file():
            try:
                content = proj_path.read_text(encoding="utf-8")
                # Support comments //...
                lines = [line for line in content.splitlines() if not line.strip().startswith("//")]
                pdata = json.loads("\n".join(lines))
                if isinstance(pdata, dict) and "default_language" in pdata:
                    val = pdata["default_language"]
                    if isinstance(val, str) and val.strip():
                        return val.strip().lower()
            except Exception:
                pass
    return "en"


def validate_locales(locales_dir: Path, fallback_lang: str = "en") -> ValidationResult:
    """
    Validates all locale JSON files in locales_dir.

    Hard errors (FAIL):
      - invalid JSON
      - root is not an object
      - non-string translation value
      - malformed _meta
      - missing fallback locale file
      - duplicate language codes

    Warnings (WARN only, never block):
      - keys missing relative to fallback
      - extra keys not in fallback
      - placeholder-set mismatches (order irrelevant)
    """
    errors: list[str] = []
    warnings: list[str] = []

    fallback_code = (fallback_lang or "en").lower().strip()
    if not fallback_code:
        fallback_code = "en"

    if not locales_dir.exists() or not locales_dir.is_dir():
        errors.append(f"Locales directory does not exist or is not a directory: {locales_dir}")
        return ValidationResult(errors, warnings, 0)

    # 1. Discover all locale files (.json files not starting with '_')
    code_to_files: dict[str, list[Path]] = {}
    for entry in sorted(locales_dir.iterdir()):
        if not entry.is_file():
            continue
        if entry.name.startswith("_"):
            continue
        if not entry.name.lower().endswith(".json"):
            continue

        code = entry.stem.lower()
        code_to_files.setdefault(code, []).append(entry)

    # Check for duplicate language codes
    for code, files in code_to_files.items():
        if len(files) > 1:
            file_names = ", ".join(f"'{f.name}'" for f in files)
            errors.append(f"Duplicate language code '{code}' in multiple files: {file_names}")

    # Check for missing fallback locale file
    if fallback_code not in code_to_files:
        errors.append(
            f"Missing fallback locale file: '{fallback_code}.json' not found in {locales_dir}"
        )

    # 2. Parse each file and perform schema validation
    parsed_locales: dict[str, dict] = {}

    for code, files in code_to_files.items():
        for file_path in files:
            try:
                raw_text = file_path.read_text(encoding="utf-8")
                data = json.loads(raw_text)
            except Exception as e:
                errors.append(f"Invalid JSON in '{file_path.name}': {e}")
                continue

            if not isinstance(data, dict):
                errors.append(f"Root of '{file_path.name}' is not a JSON object (got {type(data).__name__})")
                continue

            # Validate _meta
            if "_meta" in data:
                meta = data["_meta"]
                if not isinstance(meta, dict):
                    errors.append(f"Malformed _meta in '{file_path.name}': _meta must be a JSON object (got {type(meta).__name__})")
                else:
                    if "name" in meta and not isinstance(meta["name"], str):
                        errors.append(f"Malformed _meta in '{file_path.name}': 'name' must be a string (got {type(meta['name']).__name__})")
                    if "native_name" in meta and not isinstance(meta["native_name"], str):
                        errors.append(f"Malformed _meta in '{file_path.name}': 'native_name' must be a string (got {type(meta['native_name']).__name__})")

            # Validate translation values
            has_non_string_val = False
            for k, v in data.items():
                if k == "_meta":
                    continue
                if not isinstance(v, str):
                    errors.append(f"Non-string translation value in '{file_path.name}' for key '{k}': expected string, got {type(v).__name__}")
                    has_non_string_val = True

            # If no errors on this file, record for cross-file validation
            if code not in parsed_locales and not has_non_string_val:
                parsed_locales[code] = data

    # 3. If fallback locale parsed successfully, perform cross-locale validation
    locale_count = len(code_to_files)
    if fallback_code in parsed_locales:
        fb_data = parsed_locales[fallback_code]
        fb_keys = set(k for k in fb_data.keys() if k != "_meta")

        for code, loc_data in sorted(parsed_locales.items()):
            if code == fallback_code:
                continue

            loc_keys = set(k for k in loc_data.keys() if k != "_meta")

            # Check missing keys relative to fallback
            missing = fb_keys - loc_keys
            if missing:
                missing_list = ", ".join(sorted(missing))
                warnings.append(
                    f"Locale '{code}' missing {len(missing)} key(s) relative to '{fallback_code}': {missing_list}"
                )

            # Check extra keys not in fallback
            extra = loc_keys - fb_keys
            if extra:
                extra_list = ", ".join(sorted(extra))
                warnings.append(
                    f"Locale '{code}' has {len(extra)} extra key(s) not in '{fallback_code}': {extra_list}"
                )

            # Check placeholder mismatches on common keys
            mismatches = find_placeholder_mismatches(fb_data, loc_data)
            for k, (fb_ph, loc_ph) in sorted(mismatches.items()):
                warnings.append(
                    f"Locale '{code}' placeholder mismatch for key '{k}': "
                    f"expected {sorted(fb_ph)}, got {sorted(loc_ph)}"
                )

    return ValidationResult(errors=errors, warnings=warnings, locale_count=locale_count)


def generate_catalog(locales_dir: Path) -> dict:
    """
    Scans staged assets/locales/*.json (skipping files starting with '_')
    and builds catalog dictionary:
      {"languages":[{"code": ..., "name": ..., "native_name": ...}, ...]}
    name/native_name come from _meta, falling back to code.
    """
    languages: list[dict[str, str]] = []
    if locales_dir.is_dir():
        for p in sorted(locales_dir.iterdir()):
            if not p.is_file():
                continue
            if p.name.startswith("_"):
                continue
            if not p.name.lower().endswith(".json"):
                continue

            code = p.stem.lower()
            name = code
            native_name = code

            try:
                raw_text = p.read_text(encoding="utf-8")
                data = json.loads(raw_text)
                if isinstance(data, dict):
                    meta = data.get("_meta")
                    if isinstance(meta, dict):
                        m_name = meta.get("name")
                        if isinstance(m_name, str) and m_name.strip():
                            name = m_name.strip()
                        m_nat = meta.get("native_name")
                        if isinstance(m_nat, str) and m_nat.strip():
                            native_name = m_nat.strip()
                        elif name != code:
                            native_name = name
            except Exception:
                pass

            languages.append({
                "code": code,
                "name": name,
                "native_name": native_name,
            })

    languages.sort(key=lambda x: x["code"])
    return {"languages": languages}


def write_catalog(locales_dir: Path) -> Optional[Path]:
    """
    Generates and writes _catalog.json in locales_dir.
    Returns path to created file, or None if locales_dir is invalid.
    """
    if not locales_dir.is_dir():
        return None
    catalog = generate_catalog(locales_dir)
    catalog_path = locales_dir / "_catalog.json"
    catalog_path.write_text(json.dumps(catalog, indent=4, ensure_ascii=False) + "\n", encoding="utf-8")
    return catalog_path


def create_locale(lang: str, game_dir: Path) -> int:
    """
    Creates assets/locales/<lang>.json from fallback locale.
    Preserves ${...} placeholders and provides _meta scaffolding.
    Refuses to overwrite existing files.
    """
    lang_code = lang.lower().strip()
    if not lang_code:
        print("[ERROR] Language code cannot be empty", file=sys.stderr)
        return 1

    locales_dir = game_dir / "assets" / "locales"
    if not locales_dir.exists():
        if game_dir.name == "locales":
            locales_dir = game_dir
        elif (game_dir / "locales").exists():
            locales_dir = game_dir / "locales"
        else:
            locales_dir.mkdir(parents=True, exist_ok=True)

    target_file = locales_dir / f"{lang_code}.json"
    if target_file.exists():
        print(f"[ERROR] Refusing to overwrite existing file: {target_file}", file=sys.stderr)
        return 1

    fallback_lang = get_fallback_language(game_dir)
    fallback_file = locales_dir / f"{fallback_lang}.json"
    if not fallback_file.is_file():
        print(f"[ERROR] Fallback locale file not found: {fallback_file}", file=sys.stderr)
        return 1

    try:
        raw_text = fallback_file.read_text(encoding="utf-8")
        fallback_data = json.loads(raw_text)
    except Exception as e:
        print(f"[ERROR] Failed to read fallback locale file '{fallback_file.name}': {e}", file=sys.stderr)
        return 1

    if not isinstance(fallback_data, dict):
        print(f"[ERROR] Fallback locale root is not a JSON object: {fallback_file}", file=sys.stderr)
        return 1

    # Build new locale dictionary: copy fallback keys, preserving ${...} placeholders and _meta scaffolding
    new_data: dict = {
        "_meta": {
            "name": lang_code,
            "native_name": lang_code,
        }
    }
    for k, v in fallback_data.items():
        if k != "_meta":
            new_data[k] = v

    try:
        target_file.parent.mkdir(parents=True, exist_ok=True)
        target_file.write_text(json.dumps(new_data, indent=4, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"[OK] Created locale '{lang_code}' at {target_file}")
        return 0
    except Exception as e:
        print(f"[ERROR] Failed writing locale file '{target_file}': {e}", file=sys.stderr)
        return 1


def check_locales(game_dir: Path) -> int:
    """
    Runs validation on <game_dir>/assets/locales.
    Exits non-zero only on hard errors.
    """
    locales_dir = game_dir / "assets" / "locales"
    if not locales_dir.exists():
        if game_dir.name == "locales":
            locales_dir = game_dir
        elif (game_dir / "locales").exists():
            locales_dir = game_dir / "locales"
        else:
            print(f"[ERROR] Locales directory not found: {locales_dir}", file=sys.stderr)
            return 1

    fallback_lang = get_fallback_language(game_dir)
    result = validate_locales(locales_dir, fallback_lang)

    for w in result.warnings:
        print(f"[WARN] {w}")

    if result.errors:
        for e in result.errors:
            print(f"[ERROR] {e}", file=sys.stderr)
        return 1

    summary = f"[OK] {result.locale_count} locale(s) verified"
    if result.warnings:
        summary += f" ({len(result.warnings)} warnings)"
    print(summary)
    return 0


def demo() -> None:
    """Runnable assert-based self-check covering placeholder extraction and mismatch detection."""
    # Placeholder extraction
    assert extract_placeholders("Hello, ${name}!") == {"name"}
    assert extract_placeholders("Welcome ${user}! Score: ${score}.") == {"user", "score"}
    assert extract_placeholders("Order test: ${b} then ${a}") == {"a", "b"}
    assert extract_placeholders("Repeated ${var} and ${var}") == {"var"}
    assert extract_placeholders("No placeholders here.") == set()
    assert extract_placeholders("Malformed placeholder ${}") == set()
    assert extract_placeholders("Plain dollars $100 and $200") == set()

    # Mismatch detection
    fb = {
        "title": "Welcome, ${name}!",
        "score": "Score: ${points} (Rank ${rank})",
        "static": "Press Start",
        "same_diff_order": "${first} ${second}",
    }

    # Perfectly matching locale
    loc_good = {
        "title": "Bienvenue, ${name}!",
        "score": "Score: ${points} (Rang ${rank})",
        "static": "Appuyez sur Start",
        "same_diff_order": "${second} ${first}",  # Order difference is OK!
    }
    assert find_placeholder_mismatches(fb, loc_good) == {}

    # Mismatch: renamed placeholder
    loc_renamed = {
        "title": "Bienvenue, ${nom}!",
    }
    mm_renamed = find_placeholder_mismatches(fb, loc_renamed)
    assert "title" in mm_renamed
    assert mm_renamed["title"] == ({"name"}, {"nom"})

    # Mismatch: missing placeholder
    loc_missing = {
        "score": "Score: ${points}",
    }
    mm_missing = find_placeholder_mismatches(fb, loc_missing)
    assert "score" in mm_missing
    assert mm_missing["score"] == ({"points", "rank"}, {"points"})

    # Mismatch: extra placeholder
    loc_extra = {
        "static": "Appuyez sur ${button}",
    }
    mm_extra = find_placeholder_mismatches(fb, loc_extra)
    assert "static" in mm_extra
    assert mm_extra["static"] == (set(), {"button"})

    print("All self-checks passed successfully.")


def main() -> int:
    # If invoked with no args or demo flag, run self-check
    if len(sys.argv) == 1 or (len(sys.argv) == 2 and sys.argv[1] in ("demo", "--demo", "--self-check")):
        demo()
        return 0

    parser = argparse.ArgumentParser(description="IYIEngine Localization Tool")
    subparsers = parser.add_subparsers(dest="command")

    check_p = subparsers.add_parser("check", help="Check and validate locale files")
    check_p.add_argument("game_dir", type=str, help="Game directory containing project.json or assets/locales")

    create_p = subparsers.add_parser("create", help="Create a new locale file from fallback")
    create_p.add_argument("lang", type=str, help="Language code (e.g. 'fr')")
    create_p.add_argument("game_dir", type=str, help="Game directory containing project.json or assets/locales")

    args = parser.parse_args()

    if args.command == "check":
        return check_locales(Path(args.game_dir).resolve())
    elif args.command == "create":
        return create_locale(args.lang, Path(args.game_dir).resolve())
    else:
        parser.print_help()
        return 1


if __name__ == "__main__":
    sys.exit(main())
