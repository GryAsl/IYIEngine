# IYI Engine contract: Build, packaging, validation scope, non-goals

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 14. Build

`CMakeLists.txt` requirements:

- `cmake_minimum_required(VERSION 3.24)`; project `IYIEngine`, languages `C CXX`.
- `set(CMAKE_CXX_STANDARD 20)`, `CMAKE_CXX_STANDARD_REQUIRED ON`,
  `CMAKE_CXX_EXTENSIONS OFF`.
- Cache vars with the pinned defaults: `IYI_RAYLIB_VERSION` (the current `6.2`, or the legacy `5.5`; §0.1),
  `IYI_PYBIND11_VERSION` (`v3.1.0`), `IYI_JSON_VERSION` (`v3.12.0`),
  plus `IYI_PYTHON_VERSION` (`3.14`, or the legacy `3.11`) and `IYI_PYTHON_ROOT`, which defaults to
  empty on every platform.
- `FetchContent` for raylib (with `BUILD_EXAMPLES OFF`, `BUILD_GAMES OFF`,
  `CUSTOMIZE_BUILD ON` not required, keep it minimal), pybind11, and
  nlohmann_json (`JSON_BuildTests OFF`). Use `FetchContent_MakeAvailable`.
  Third-party targets must be excluded from the engine's own warning flags.
- Find Python via `find_package(Python3 ${IYI_PYTHON_VERSION} EXACT COMPONENTS Interpreter Development.Embed)`
  (before pybind11, which is pointed at the same interpreter). `IYI_PYTHON_ROOT`
  defaults to empty everywhere and CMake's normal discovery is used. When it is
  non-empty it seeds `Python3_ROOT_DIR` and is a requirement: configuration fails
  if the resolved interpreter is not under it. A missing runtime of that version produces a
  clear fatal error. Windows development builds copy the linked `python3XX.dll` beside the
  exe and the tests, so a build tree never depends on which Python is on `PATH`. Runtime file
  names (`python3XX.zip`, `lib/python3.X`) come from `PY_MAJOR_VERSION` / `PY_MINOR_VERSION`. Never hard-code a machine-specific absolute path.
- A static library `IYIEngineSim` (`src/Nav.cpp`, `src/NavSim*.cpp`, `src/Life.cpp`, `src/Sight.cpp`; raylib only, no Python) that
  `IYIEngineCore` and a game's native module both link (§2b). `cmake/iyi_native.cmake` and
  `tools/build_native.bat` build such a module against `build/ninja`.
- One executable target `IYIEngine` from `src/*.cpp`, `target_include_directories`
  on `include/`, linking `raylib`, `pybind11::embed`, `nlohmann_json::nlohmann_json`,
  and `Python3::Python`.
- Warning flags must be platform-conditional and applied to the `IYIEngine`
  target only (never globally, never to third-party targets):
  MSVC → `/W4 /permissive- /utf-8 /MP`; GCC/Clang → `-Wall -Wextra -Wpedantic`.
  Do **not** enable `/WX` or `-Werror`.
- No other `if(WIN32)`-only assumption anywhere. Use `std::filesystem` for every
  path operation and forward slashes internally. The code must be plausibly
  buildable on Linux/macOS even though only Windows is verified now.
- A precompiled header is allowed (plan §32) but only for stable headers
  (`<string>`, `<vector>`, `<unordered_map>`, `<memory>`, `<filesystem>`,
  `raylib.h`), skip it if it complicates the build.
- Post-build: nothing is copied. The exe needs the system Python at runtime and
  finds it through the linked interpreter.

- `IYI_WITH_PYTHON` (default ON): OFF builds `IYIEngineCore` without `PythonRuntime.cpp`, every
  `*Bindings.cpp`, pybind11 and the Python link, for a native game (§2c). `IYIEngine.exe` is only
  built with it ON. `cmake/iyi_game.cmake` (`iyi_add_game`) and `tools/build_game.bat` build a
  native game's exe.

`build.bat` must:

- Locate VS via `vswhere.exe`, derive the bundled
  `Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` and
  `...\Ninja\ninja.exe` (do not assume they are on PATH).
- Call `vcvars64.bat`.
- Configure into `build\ninja` with the Ninja generator
  (`-DCMAKE_MAKE_PROGRAM=<bundled ninja>`), build config from `%1`
  (default `Release`, accept `Debug`).
- Accept a `vs` argument (`build.bat vs`) that instead generates a Visual Studio
  2026 x64 solution into `build\vs` and does not build it.
- Print the resulting exe path on success; return a non-zero exit code on failure.

`build.sh` (POSIX counterpart, for the portability goal, not tested here):

- `#!/usr/bin/env bash`, `set -euo pipefail`.
- Uses `cmake` from `PATH`, Ninja if available else Unix Makefiles, configures
  into `build/ninja` with `-DCMAKE_BUILD_TYPE=${1:-Release}`, then builds.
- Prints the resulting binary path. Keep it under ~30 lines.

`.gitignore`: `build/`, `.vs/`, `*.user`, `__pycache__/`, `*.pyc`, `out/`.

## 14b. Packaging a game for distribution

`package.bat [py311] [<game-project-path>] [<output-dir>]` produces a folder that runs on
a machine with **no Python installed**. Defaults: `..\HumanResources` and `dist\`, Python 3.14
(`py311` packages the legacy 3.11 build from `build\ninja-py311`).

Layout it produces:

```text
<output>/
├── IYIEngine.exe
├── python/            embeddable CPython (python314.dll, python314.zip, ...; 311 for legacy)
├── game/              the game's project.json, scripts/ and assets/, its libraries folded in
├── steam_api64.dll
└── *.dll              the MSVC runtime DLLs the exe and python3XX.dll need
```

Requirements:

- Build Release first (or fail with a clear message if no build exists).
- Fetch `python-3.14.8-embed-amd64.zip` (legacy: `python-3.11.9-embed-amd64.zip`) from python.org into `third_party/` and
  cache it, so repeat packaging needs no network. Extract it to `<output>/python/`.
- The packaged exe must resolve its Python home to `<exe dir>/python` **before**
  falling back to the compiled-in `IYI_PYTHON_HOME`, so it never picks up a
  system install. Extend the §11 resolution order accordingly: `PYTHONHOME`
  environment variable, then `<exe dir>/python`, then compiled-in.
- Copy the game's `project.json`, `scripts/` and `assets/` into `game/`, and set
  `"hot_reload": false` in the copied `project.json`. Never modify the source
  project. Each library in `libraries` is folded into `game/` the way §2 lays it (the game wins,
  earlier libraries win), and the copied `project.json` drops `libraries` and `desktop_libraries`;
  a missing library folder fails the package. Each `native` folder's built modules (`.pyd`, `.dll`)
  keep their project-relative path under `game/`.
- Copy the MSVC redistributable DLLs (`vcruntime140.dll`, `vcruntime140_1.dll`,
  `msvcp140.dll`) from the Visual Studio redist directory; warn clearly if they
  cannot be found rather than producing a silently broken folder.
- Exclude `__pycache__`, `.git` and build output from the copy.
- Copy the game's declared `project.json` `"native"` folders (SPEC §2b), flattened, into the
  package's data folder as `<OutputName>_Data/native` (the current release layout, §18.7), never
  into `game.rres`: a `.pyd` cannot be imported from an archive, so these files stay loose beside
  the runtime. A module file name that does not carry the interpreter tag of the packaged Python
  prints a warning; it never fails the release, because the tool may run under a different Python.

**Acceptance test**, a packaged folder that only works on the build machine is
worthless, so it must be verified with the system Python hidden: run the packaged
exe with `PATH` stripped of every Python directory and `PYTHONHOME` / `PYTHONPATH`
unset, from a different working directory. It must open the window and run the
game's Python. Testing it while the developer's Python is still reachable proves
nothing.

This is `ROADMAP.md` Tier 3's "bundled Python" item. No installer, no single-file
exe, no code signing, a plain folder is the whole scope.

## 15. Validation scope (plan §42)

- C++ must configure and build cleanly (warnings acceptable, errors not).
- Every game `.py` must pass `python -m py_compile`.
- No automated gameplay tests, no test framework, no manual QA run unless asked. The only automated check is the small plain-assert native suite `tests/engine_tests.cpp` (CTest, `IYI_BUILD_TESTS`).

Stop once it is structurally correct and builds.

## 16. Explicit non-goals

No custom collision, physics, pathfinding, spatial queries, job system or thread
pool (the two threads of §8a are the whole threading model), ECS, profiler, inspector,
debug UI, editor, prefabs, animation framework, script dependency graph,
NumPy, Numba, or free-threaded Python. No game code loaded as a DLL or plugin: a native game
is its own exe (§2c). Multiplayer is the opt-in `libraries/net` (§25), reopened 2026-11-21: host-based only.
