# IYI Engine contract: Localization (`engine.loc`)

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 19. Localization subsystem

The localization subsystem provides flat-file JSON translation string management, placeholder formatting, fallback language resolution, development hot-reloading, and UI integration.

### 19.1 Locale files and directory convention

Locale files live in the game project under `assets/locales/<code>.json`:

```text
assets/locales/
├── en.json
├── es.json
├── fr.json
├── pt-br.json
└── zh-cn.json
```

> [!IMPORTANT]
> **Lowercase naming invariant**: Language codes and locale filenames are **always strictly lowercase** (e.g. `zh-cn.json`, `pt-br.json`), because packaged resource IDs are derived from the lowercased path (`ResourceKey`). Any uppercase characters in requested language codes or file stems are converted to lowercase automatically.

### 19.2 JSON schema and `_meta`

Locale files must be flat JSON objects mapping string keys to string values:

```json
{
    "_meta": {
        "name": "Spanish",
        "native_name": "Español"
    },
    "ui.play": "Jugar",
    "ui.quit": "Salir",
    "hud.score": "Puntuación: ${score}",
    "hud.wave": "Oleada ${wave} de ${total}"
}
```

- **Root object**: The root must be a JSON object (`{}`). Arrays or non-objects at the root cause parsing errors.
- **Translation values**: All translation values must be JSON strings (`"key": "value"`). Non-string values (numbers, booleans, arrays, nested objects) are invalid.
- **Optional `_meta`**: An optional metadata object containing:
  - `"name"` (`string`): English or display name of the language (defaults to the language code if omitted).
  - `"native_name"` (`string`): Endonym / native name of the language (defaults to `"name"` or code if omitted).
  - `_meta` is stripped from translation key lookups and does not pollute `engine.loc.has()` or translation tables.

### 19.3 String resolution order

Resolution always follows a deterministic three-stage chain:

$$\text{current language} \longrightarrow \text{fallback language} \longrightarrow \text{"[key]"}$$

1. **Current language**: The key is queried in the active language dictionary (`m_currentStrings`). If found and non-empty, placeholder formatting is applied and the string is returned.
2. **Fallback language**: If missing or empty in the active language, the key is queried in the fallback language dictionary (`m_fallbackStrings`). If found and non-empty, placeholder formatting is applied and the string is returned.
3. **Fallback bracketed key**: If missing or empty in both, the key is inserted into `m_missingKeys`, a warning is logged once per `(language, key)` on the `"loc"` log channel (`Missing localization key '<key>' for language '<language>'`), and `"[<key>]"` is returned (e.g. `"[ui.missing_key]"`).

**Guarantees**:
- Resolution **never crashes**.
- Resolution **never returns an empty string** for a missing key.
- Resolution never throws exceptions across the C++/Python boundary.

### 19.4 Placeholders and argument formatting

Placeholders strictly use the `${name}` syntax:

```text
"hud.greeting": "Welcome back, ${player}! Rank: ${rank}"
```

- **Formatting**: When passed keyword arguments (`engine.loc.text("hud.greeting", player="Alex", rank=5)` or UI widget `loc_args`), each `${name}` token is replaced by its stringified argument value.
- **Missing arguments**: If an expected placeholder has no matching argument provided:
  - The raw placeholder token `${name}` is **preserved untouched** in the output string (e.g. `"Welcome back, Alex! Rank: ${rank}"`).
  - In non-shipping builds, a warning is logged once per `(key, placeholder)` to the `"loc"` log channel: `Missing placeholder argument '${name}' for key 'hud.greeting'`.
  - The engine never crashes or raises an exception on missing arguments.

> [!WARNING]
> **Copy-by-value across Python boundary**: `loc_args` on UI widgets (`UIText`, `UIButton`) is copied by value across the pybind11 boundary. Always assign a whole dictionary (`el.loc_args = {"player": "Alex", "rank": "1"}`). Mutating an item in-place (e.g. `el.loc_args["rank"] = "2"`) modifies a temporary copy and is **silently lost**.

### 19.5 Debug pseudo-language `__keys__`

The engine provides a built-in debug language identifier: `"__keys__"`.

- Activating it via `engine.loc.set_language("__keys__")` bypasses all translation tables.
- `engine.loc.text(key, ...)` unconditionally returns `"[key]"` for every key without performing dictionary lookups.
- `engine.loc.has(key)` unconditionally returns `true`.
- Allows developers and QA to immediately verify whether all visual text elements are properly wired to localization keys, and inspect layout bounds against raw key identifiers without modifying locale JSON files.

### 19.6 Development hot reload

In development builds (`#ifndef IYI_SHIPPING_BUILD`), `LocalizationManager::PollForChanges(dt)` checks the filesystem modification timestamps (`mtime`) of the active and fallback locale JSON files every 0.25 seconds:

- When an edit is detected, the file is re-read and parsed in-place.
- **Last valid data rule**: If a modified file contains malformed JSON or an invalid schema, an error is logged to the `"loc"` log channel and **the existing valid string dictionary is preserved**. A syntax typo in a JSON file never clears live text, never reverts to raw keys, and never crashes the engine.
- When fixed and saved again, the new contents load cleanly and live widgets reflect the update immediately.
- In shipping builds (`IYI_SHIPPING_BUILD`), hot reload is compiled out.

### 19.7 Shipping catalog `_catalog.json`

Because shipping builds run against `game.rres` where directory enumeration is omitted to preserve binary performance and asset obfuscation:

- During release staging (`stage_generate_secret_and_manifest`), `tools/release.py` calls `iyiloc.write_catalog(staged_locales_dir)`, generating `assets/locales/_catalog.json`:
  ```json
  {
      "languages": [
          {
              "code": "en",
              "name": "English",
              "native_name": "English"
          },
          {
              "code": "es",
              "name": "Spanish",
              "native_name": "Español"
          }
      ]
  }
  ```
- In shipping mode, `LocalizationManager::DiscoverLanguages()` reads `assets/locales/_catalog.json` directly from `game.rres` to populate available language lists and metadata.
- In development mode, if `_catalog.json` does not exist, `DiscoverLanguages()` iterates `assets/locales/*.json` on disk and parses each file's `_meta` block.

### 19.8 `project.json` configuration

`project.json` supports an optional `default_language` string field:

```json
{
    "default_language": "en"
}
```

- Defaults to `"en"` if omitted.
- Sets the initial active language and fallback language during engine startup (`LocalizationManager::Initialize`).
- Validated by `tools/release.py` and `tools/iyiloc.py` during build preflight.

### 19.9 Python API (`engine.loc`)

The `engine.loc` submodule exposes the full localization subsystem:

```python
# Language selection
engine.loc.set_language(code: str) -> None
engine.loc.get_language() -> str
engine.loc.set_fallback_language(code: str) -> None
engine.loc.get_fallback_language() -> str

# Querying text
engine.loc.text(key: str, **kwargs) -> str
engine.loc.has(key: str) -> bool

# Introspection
engine.loc.languages() -> list[dict]        # [{"code": str, "name": str, "native_name": str}, ...]
engine.loc.language_info(code: str) -> dict | None
engine.loc.missing_keys() -> set[str]

# Reloading
engine.loc.reload() -> None
```

#### Example

```python
import engine

# Change active language
engine.loc.set_language("es")

# Retrieve formatted text
greeting = engine.loc.text("ui.welcome", name="Player 1")

# Enumerate available languages for an options menu
for lang in engine.loc.languages():
    print(f"{lang['code']}: {lang['native_name']} ({lang['name']})")
```

### 19.10 Font and glyph coverage scope

Text rendering in IYIEngine and raylib is UTF-8 native, but font rasterization is constrained by glyph atlas inclusion:

- **Bundled glyph coverage**: The engine loads font glyphs for:
  - Basic Latin (`0x0020`–`0x007E`, ASCII printable)
  - Latin-1 Supplement (`0x00A0`–`0x00FF`)
  - Latin Extended-A (`0x0100`–`0x017F`, accented European characters)
  - Cyrillic (`0x0400`–`0x04FF`, Russian, Ukrainian, etc.)
- **Unsupported scripts**: Scripts requiring complex text shaping, cursive attachment, or bidirectional / right-to-left (RTL) layout (such as Arabic, Hebrew, Persian, Devanagari, Thai) are **not supported**.
- **CJK**: `load_font(..., glyph_text="...")` adds the unique printable UTF-8 codepoints from the supplied text to the base ranges and U+2026. Cache identity includes the exact canonical glyph set. A suitable project font is required; the engine does not ship or guess a font. UI and `draw_text_ex` use private cumulative expanded atlases for text outside the initial repertoire (for example player names or hot-reloaded translations). New glyphs make a new, larger private version; the one it replaces is freed on the render side once every draw recorded with it has run. Public font handles remain immutable. Preseed known locale text to avoid runtime atlas creation. `Font.has_glyph(codepoint)` checks the rasterized font view, including an on-demand glyph if needed.
- **CJK layout**: Han/kana wrap without inserted spaces, with common opening/closing punctuation and small-kana restrictions. Korean uses spaces first and UTF-8 character boundaries for oversized words. Ellipsis removes complete UTF-8 codepoints. This is a practical horizontal UI breaker, not full Unicode UAX #14 or grapheme shaping. Hangul syllables, Jamo and Han/kana are accepted as letters by text-input filters.
- **Windows IME**: a small HWND subclass observes composition and forwards every message to GLFW/default processing. Windows owns the native composition/candidate windows, positioned at the focused visible text field's transformed caret. Committed text comes exclusively from raylib's `GetCharPressed` queue. Keyboard queries and UI keyboard actions are suppressed during composition and its commit/cancel frame; `is_ime_composing()` reports the live composition state. Focus changes, removal, window focus loss and shutdown cancel composition. The original window input context is attached only while a text field is drawn focused. Other platforms retain their existing input path. See `docs/IME.md` for verification limits and manual provider tests.
- **No ICU or HarfBuzz**: The engine does not bundle or link ICU, HarfBuzz, FriBidi, or complex text shaping libraries. Characters outside the loaded ranges or requiring shaping/RTL layout will not render correctly. A character the font itself lacks draws as raylib's `?` fallback on raylib 6.x (§0.1).
