# IYI Engine contract: ResourceStore, shipping, release layouts

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 18. ResourceStore and shipping contract

### 18.1 ResourceStore abstraction and backends

`ResourceStore` (`include/IYI/ResourceStore.h`, `src/ResourceStore.cpp`) is the narrow
read-only abstraction between engine systems (`AssetManager`, `ProjectConfig`, `PythonRuntime`)
and game data files. It exposes:

- `bool Exists(const std::string& relPath) const`
- `bool ReadBytes(const std::string& relPath, std::vector<unsigned char>& outBytes) const`
- `bool ReadText(const std::string& relPath, std::string& outText) const`
- `std::string GetPhysicalPath(const std::string& relPath) const`

All query paths are normalized via `ResourceFsPath` (filesystem form, case
preserved) before backend lookup; `PackagedResourceBackend` additionally hashes the
case-folded `ResourceKey` (see §18.3).

The store delegates to an `IResourceBackend` implementation:

1. **`DevResourceBackend` (Development)**:
   Mounted in non-shipping builds (`#ifndef IYI_SHIPPING_BUILD`). Resolves paths directly
   against the filesystem `ProjectDir`. `GetPhysicalPath()` returns the full filesystem path.
   Supports live file modifications and runtime hot reloading.
   Every path is resolved through `ResourceContainedPath` (`include/IYI/ResourcePath.h`): a path
   that would leave `ProjectDir` after lexical normalization (`../`, absolute, drive-qualified,
   either separator) is refused, so `Exists`/`ReadBytes`/`ReadText` return false and
   `GetPhysicalPath` returns `""`, exactly as the packaged backend would. Inner normalization that
   stays inside (`assets/../assets/a.png`) still resolves. `engine.project_path()` applies the
   same rule to its would-be path and returns `""` for an escaping one.
2. **`PackagedResourceBackend` (Shipping)**:
   Mounted unconditionally in shipping builds (`#ifdef IYI_SHIPPING_BUILD`). Reads exclusively
   from `game.rres` located inside `<OutputName>_Data/game.rres` (`<exe dir>/<OutputName>_Data/game.rres`).
   Scans chunk headers at startup to index available resource IDs and their chunk offsets; a read
   seeks straight to its chunk. `GetPhysicalPath()` returns empty string (`""`).

### 18.2 Strict no-fallback invariant

A shipping build **never falls back to loose files on disk**.

- If a resource is missing from `game.rres`, the lookup fails immediately and logs a fatal error (`FATAL: Missing resource in archive`).
- The runtime never probes `<exe dir>/assets/`, `<exe dir>/scripts/`, or `<cwd>` when running a shipping binary.
- In shipping mode, `ResourceStore::SetBackend` rejects any backend that is not a `PackagedResourceBackend`.

This prevents releasing a broken archive that superficially appears to work on the developer's machine only because loose source files exist beside the executable.

### 18.3 Resource ID derivation and path normalization

`game.rres` archives omit the Central Directory (`cdOffset = 0`) to prevent leaking asset names and directory structures. Resources are stored and located exclusively by 32-bit CRC32 IDs.

#### Path normalization (`include/IYI/ResourcePath.h`)

Two distinct forms, defined once and shared by the engine and `tools/iyipack`:

**Filesystem form** (`ResourceFsPath`), what
`DevResourceBackend` opens. **Case is preserved**, so mixed-case assets
(`IYIEngineLOGO.png`) load on case-sensitive filesystems (Linux/macOS):

1. Replace all backslashes (`\`) with forward slashes (`/`).
2. Lexical cleanup: collapse redundant `.` and `..` components via `std::filesystem::path::lexically_normal()`.
3. Strip any leading `/` or `./` prefixes.
4. If the resulting path is `"."`, reduce it to `""`.

**Archive key** (`ResourceKey`), the filesystem
form with all ASCII characters lowercased (`std::tolower`). Only archive IDs use it; it is never
used to open a physical file. This guarantees identical resource IDs across operating systems
and query styles, and is byte-for-byte the form `iyipack` hashes.

#### Resource ID calculation

The resource ID is the IEEE 802.3 32-bit CRC32 hash (polynomial `0xEDB88320`) of the UTF-8 bytes of the normalized path:

```cpp
uint32_t id = rresComputeCRC32(
    reinterpret_cast<const unsigned char*>(normPath.data()),
    static_cast<int>(normPath.length())
);
```

During packaging, `iyipack` and `tools/release.py` execute preflight collision detection. If any two distinct game files produce the same CRC32 ID, the build fails immediately with the conflicting paths reported.

### 18.4 Packaged Python importer

In shipping builds (`IYI_SHIPPING_BUILD`), Python source and bytecode are not stored on disk. `PythonRuntime` bootstraps a custom meta path importer (`_PackagedFinder` and `_PackagedLoader`) at index 0 of `sys.meta_path`.

When importing a module (e.g. `import actors.player` or `import helpers.math`):

1. The module's full name is resolved relative to the `scripts/` root.
2. The importer queries `engine._read_resource(path)` (backed by `ResourceStore`) in this exact search order:
   - Package bytecode: `scripts/<path>/__init__.pyc`
   - Package source:   `scripts/<path>/__init__.py`
   - Module bytecode:  `scripts/<path>.pyc`
   - Module source:    `scripts/<path>.py`
3. If bytecode (`.pyc`) is found, `_PackagedLoader` unmarshals the code object directly from memory via `marshal.loads(data)` (supporting raw code objects or skipping standard 16-byte headers).
4. If source (`.py`) is found, it is compiled into memory via Python's `compile(data, filename, 'exec')`.
5. Direct script loading for actors and scenes (`PythonRuntime::LoadModuleFromSource`) follows the same rule: it tests for `<path>c` (`.pyc`) in `ResourceStore` first, loading bytecode if present, before falling back to `.py`.
6. Decrypted Python code is executed in-memory and never written to temporary files on disk.

### 18.5 `engine.user_data_path` and `engine.is_shipping`

Exposed in the embedded `engine` module:

- `engine.user_data_path(subdir="") -> str`:
  Returns an absolute, normalized path to the per-user writable directory:
  - Windows: `%LOCALAPPDATA%/<Publisher>/<Name>/[subdir]` (falling back to `%USERPROFILE%/AppData/Local/...` or temp).
  - macOS: `$HOME/Library/Application Support/<Publisher>/<Name>/[subdir]`.
  - Linux: `$XDG_DATA_HOME/<Publisher>/<Name>/[subdir]` or `$HOME/.local/share/<Publisher>/<Name>/[subdir]`.
  
  The directory is created automatically if it does not exist. All persistent game data (save files, custom configs, keybindings) and engine log files MUST be stored here. A shipping game never writes to its installation folder.
- `engine.run_path(rel_path="") -> str`:
  This run's own folder, where the engine keeps `logs/` and `agentio/`: the `--user-data` folder of an
  isolated run (§3), else the project folder; in a shipping build the user-data root. Not created. A
  game puts files that belong to one run there (development saves, a probe's replies, benchmark output)
  so that two runs of the same project never share them.
- `engine.is_shipping() -> bool`:
  Returns `True` when running a binary compiled with `IYI_SHIPPING_BUILD`, `False` otherwise. Allows gameplay scripts to query the runtime environment cleanly without inspection heuristics.

### 18.6 `IYI_SHIPPING_BUILD` compile option and build isolation

Configured in CMake via `-DIYI_SHIPPING_BUILD=ON`:

- Compiles out the file watcher and disables hot reload (`m_hotReloadEnabled = false`).
- Forces `ResourceStore` to use `PackagedResourceBackend` and reject non-packaged backends.
- Includes and binds `IYI::ReleaseSecret` to provide the runtime decryption key for XChaCha20-Poly1305. The secret is split into XOR-masked fragments in generated code and wiped from memory on destruction (`Wipe()`).
- Deferral of file logging: In shipping, file logging is not initialized until `project.json` is read from `game.rres` and `UserDataRoot()` is determined. Logs are written to `<user_data_root>/logs/latest.log` rather than the install directory.

#### Build isolation (`build/shipping`)

The automated release pipeline (`tools/release.py`) configures and builds CMake in `build/shipping/`. It never switches or alters the configuration of the developer's everyday `build/ninja/` directory, preserving the developer's active development cache and loose-file workflow.

Windows shipping exe: GUI subsystem (no console window; output goes to the log), and a version
resource built from `project.json` `name`/`version` plus `release.json` `"exe_icon"` (an `.ico`
relative to the game folder, optional) so Task Manager and Explorer show the game, not `IYIEngine`.

Only reachable assets are packed (`referenced_assets()` in `tools/release.py`): an asset ships when
its path or file name appears in the game's/libraries' scripts, `project.json`, the engine's
`src/*.cpp` or another shipped text asset, when a path is built inside its folder
(`"assets/<dir>/" + name`), or when it is a license. The release prints what it left out. A path
built with no folder in the text needs `release.json` `"keep_assets": ["<glob>", ...]`.
`release.json` `"project_overrides": {...}` lays keys over `project.json` for that build only
(preflight, checks and the staged `project.json` all see them): one game ships a second product,
such as a demo with its own `name`, `steam_app_id` and game-defined keys, from one source tree
with `release.py --config <file>`.

Shipping guards: CMake leaves IYITrailer (§22) out of a shipping build (`Trailer.h` stops one with
`#error`), preflight rejects packed scripts that use `marketing`, `hr_bridge` or `engine.trailer`,
and validation rejects a shipping exe that contains the trailer's flags; preflight rejects `steam_app_id` 0 or 480 (Spacewar, test-only) when `steam.prepare=true`; the game's `tools/verify.py full` runs before compilation and aborts the release on failure (`--skip-game-tests` is a dev-only escape hatch); `release.py` refuses to run under an interpreter other than the version it packages for (§18.7); the downloaded/cached CPython runtime is SHA-256 pinned per platform in `tools/runtime_manifest.json`; the smoke test waits for `IYI_RUNTIME_READY`. Unit tests: `python tools/test_release.py`.

### 18.7 Shipping release folder layout

The release pipeline (`tools/release.py`) produces a distribution layout under `release/<OutputName>/`.
`--python 3.14` (default) or `--python 3.11` (legacy) picks the embedded runtime pinned in `tools/runtime_manifest.json`
and its shipping build tree (`build/shipping`, `build/shipping-py311`). Names below are for 3.14; 3.11 uses `311`.
`--raylib` picks raylib: the current version by default (6.2), or a legacy one (`--raylib 5.5`), which adds
`-rl55` to the build tree (`build/shipping-rl55`; §0.1).
The interpreter that runs `release.py` must be the one `--python` names (the tool refuses a mismatch,
SPEC §18.6): the game's and libraries' `.pyc` files are marshalled in-process, so they carry the
*running* interpreter's bytecode format, and the embedded engine cannot load another version's.

```text
release/<OutputName>/
├── <OutputName>.exe
├── python314.dll
├── vcruntime140.dll
├── vcruntime140_1.dll
├── msvcp140.dll
└── <OutputName>_Data/          (Windows hidden attribute set)
    ├── game.rres
    ├── python314.zip
    ├── python314._pth
    ├── python3.dll
    └── ... (CPython runtime support DLLs & .pyd extensions)
```

- **Third-party notices**: `THIRD_PARTY_NOTICES.txt` at the root holds the license text of raylib, GLFW, nlohmann/json, pl_mpeg, rres and Monocypher (plus CPython and pybind11 for a Python game, and a note on the MSVC runtime on Windows), read from the sources the build used (`notice_sources` in `release.py`). A text that is not on disk fails the release (preflight for the files kept in `third_party/`), and validation checks every section is in the shipped file.
- **Root purity**: Only `<OutputName>.exe`, `THIRD_PARTY_NOTICES.txt` and implicitly linked runtime DLLs (`python314.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`, `msvcp140.dll`, plus `steam_api64.dll` when Steam preparation is enabled) remain at the root alongside the single `<OutputName>_Data` folder. No loose script, asset, `steam_appid.txt`, or naked `python/` directory sits at the root.
- **Dedicated data folder**: All packaged game content (`game.rres`) and embedded CPython standard library / `.pyd` runtime extensions reside inside `<OutputName>_Data`.
- **Unused runtime modules**: `_install_windows_runtime` skips CPython modules no game imports (`_ssl`
  and libssl, `_sqlite3` and sqlite3, `_asyncio`, `_multiprocessing`, `_overlapped`, `_msi`, `pyexpat`,
  `_elementtree`, `winsound`, `python.cat`). `_hashlib` and libcrypto stay: `hashlib` is used.
- **Deduplication**: Redundant binaries (`python.exe`, `pythonw.exe`) are omitted entirely, and DLLs already present at root (`python314.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`) are not duplicated inside the runtime folder.
- **Hidden attribute**: The Windows hidden attribute (`+h`) is set on `<OutputName>_Data` as a cosmetic measure; SteamPipe or other distribution tooling may not preserve this attribute, so the folder layout itself is the real structural fix.

### 18.8 Linux / macOS release layouts (experimental)

`tools/release.py --platform linux|macos` (host OS only) runs the same seven stages with a per-platform table
(`Platform` / `package_layout` in `release.py`). The runtime is python-build-standalone CPython 3.14 (3.11 legacy)
(`tools/runtime_manifest.json`). Python home is `<Name>_Data/runtime` (stdlib in `runtime/lib/python3.14`);
Shipping ignores `PYTHON*` environment variables and the user site (`use_environment = 0`).

```text
release/<Name>-linux/                       release/<Name>-macos/<Name>.app/Contents/
├── <Name>            (RPATH $ORIGIN)       ├── Info.plist
├── libpython3.14.so.1.0                    ├── MacOS/<Name>   (RPATH @executable_path/../Frameworks)
├── libsteam_api.so   (Steam only)          ├── Frameworks/libpython3.14.dylib [+ libsteam_api.dylib]
└── <Name>_Data/{game.rres,runtime/}        └── Resources/<Name>_Data/{game.rres,runtime/}
```

macOS is Apple Silicon (arm64) only and ad-hoc signed unless `IYI_CODESIGN_IDENTITY` is set. Steam binaries are
optional per platform (`IYI_STEAM=AUTO|ON|OFF`; ON fails configure when the library is missing). Linux/macOS stay
**experimental / unsupported** until the native CI matrix passes.
