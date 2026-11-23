# IYI Engine contract: Approved decisions

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 0. Approved decisions

| Topic | Decision |
|---|---|
| Engine repo | `IYIEngine` (own git repo, next to the game repositories) |
| Game repos | Siblings of `IYIEngine/` under `IYIProjects/` (HumanResources, Synthopia, COIN, ...), each its own project; `IYIGame` is the inactive legacy one |
| Executable | `IYIEngine.exe` |
| Namespace / include dir | `namespace IYI`, headers under `include/IYI/` |
| C++ standard | **C++20** (`CMAKE_CXX_STANDARD 20`, single place, easy to bump) |
| Compiler | MSVC 19.51 (VS 2026 Community), x64 |
| Generators | Ninja (default, bundled with VS) + Visual Studio 2026 solution (optional target) |
| raylib | The current raylib release (now **6.2**) via `FetchContent`, cache var `IYI_RAYLIB_VERSION`; older ones stay buildable as legacy options (now **5.5**: `build.bat rl55` -> `build/ninja-rl55`). Rules and bump steps: §0.1 |
| pybind11 | **v3.1.0** via `FetchContent`, cache var `IYI_PYBIND11_VERSION` |
| nlohmann/json | **v3.12.0** via `FetchContent`, cache var `IYI_JSON_VERSION`, tests OFF |
| Python | System CPython **3.14** (embedded, default) or **3.11** (legacy), cache var `IYI_PYTHON_VERSION`; one build tree per version: `build/ninja` (3.14), `build/ninja-py311` (3.11, `build.bat py311`) |
| Game language | **Python** (hot reload) for every game, or **C++** for a native game: its own exe linking `IYIEngineCore`, no Python at runtime (§2c). Approved 2026-10-29 for COIN (reason: `docs/benchmark.md`, ~16 ms of COIN's 20 ms game thread is Python) |
| Python API style | Lifecycle hooks PascalCase (`Start`, `Update`, `FixedUpdate30`, `FixedUpdate60`, `Drawing`); everything else snake_case (`actor.world_pos`, `engine.spawn_actor`) |
| Scene switching | `engine.change_scene("scripts/scenes/level1.py")` → engine clears non-persistent actors, then calls that script's `SceneStart()` |
| Hot reload scope | Every `.py` under the project's `scripts/` (recursive), including the entry script and helper modules. `GameStart()` is **never** re-run on reload |
| Python errors | Traceback to console, game keeps running; the same (script, hook) pair prints **once** and is then suppressed until that file is saved again |
| Audio | Minimal, straight raylib: load/play/stop sound, load/play/stop/pause/resume music, master volume. Engine auto-calls `UpdateMusicStream` |
| Threads | **Main thread = render side** (window, input polling, every GL and GLFW call, audio for now); **game thread = game side** (embedded Python from start to finalize, every hook, IYIUI). Game steps are pipelined one frame ahead of the render, 1:1 (§8a). `--single-thread` runs both on the main thread. Python never runs anywhere else (no free-threaded build) |
| Camera | App-level only: `engine.set_camera(cam)` / `engine.set_camera_3d(cam)` / `engine.clear_camera()`. World actors draw inside `BeginMode2D`/`EndMode2D` (or the 3D camera's mode); actors with `screen_space = True` draw after it, in screen space (HUD) |
| Portability | Written portable from day one: platform-conditional Python discovery and compiler flags, `build.bat` (Windows) + `build.sh` (POSIX). Only Windows is verified; Linux/macOS release builds (§18.8) are experimental until CI is green |
| Helper scripts | `build.bat` in the engine repo. **No** `run.bat` |
| Multiplayer | Opt-in `libraries/net` (§25), approved 2026-11-21: host-authoritative with host migration, Steam + ENet + loopback transports, delta replication in C++, usable from Python (`engine.net`) and C++ (`IYI::Api`). Build switch `IYI_NET` (`ON`/`OFF`): `tools/release.py` turns it on only for a game that lists the library |
| ENet | **v1.3.18** (MIT) via `FetchContent`, cache var `IYI_ENET_VERSION`; compiled only with `IYI_NET=ON`. Approved 2026-11-21 for UDP play (LAN, direct address) next to Steam |

Version bumping must stay a small edit: one cache variable per dependency. raylib has a few more
places, listed in §0.1.

### 0.1 raylib versions

The engine follows raylib's releases. The **current** version (6.2 since 2026-10-20, before that 5.5) is
the one the engine is developed against, and it will be bumped again when a newer release (6.5, 7.0, ...)
is worth it, so no code or document treats the current version as final. Approved by Gry on 2026-10-20:

- **New code targets the current version** and may use its API freely. Its new features are adopted when
  a task benefits from them, not up front because they exist.
- **Older versions stay one switch away as legacy builds**: `build.bat rl55` / `build.sh rl55`
  (`build/ninja-rl55`; with `py311`: `build/ninja-py311-rl55`) and `release.py --raylib 5.5`
  (`build/shipping-rl55`). Legacy now: **5.5**. `package.bat` always packages the current version
  (`build/ninja`).
- **A legacy build only covers the code that existed when its version stopped being current** (5.5:
  2026-10-20). Later code does not have to build on it; fix a break only when that version is needed again.
- **Existing code that behaves differently across versions branches on raylib's own
  `RAYLIB_VERSION_MAJOR` / `RAYLIB_VERSION_MINOR`** (`raylib.h`) with a comment naming the change, and the
  Python API stays the same on every version. Branches today (all 6.0 changes): `LoadFontAtlas`
  (`LoadFontData` returns only the glyphs the font has, and their count), `ApplyEffectivePan`
  (`SetSoundPan` takes -1..1; the engine keeps 0..1), `Application::ToggleFullscreen` (borderless windowed
  is no longer pinned topmost, so only 5.5 needs the unpin).
- **Thin forwards pass a version's behaviour changes straight through** (hard rule 5); these are documented,
  not branched. 6.x: `draw_rectangle_rounded` picks the segment count itself only for `segments < 1`
  (5.5: `< 4`); `draw_rectangle_lines_ex` clamps the thickness to half the shorter side and draws nothing
  for a thickness of 0 or less; a character the font lacks is left out of the atlas, so it draws as raylib's
  `?` fallback (5.5 kept an empty, zero-width glyph; kept on purpose, Gry 2026-10-20: missing glyphs
  stay visible); fullscreen and HiDPI handling were redesigned.

Bumping to a newer raylib:

1. Read the new `CHANGELOG` (in `build/ninja/_deps/raylib-src/`) for `-WARNING-` (breaking) entries and
   grep the engine for each function they name.
2. Change the version in `CMakeLists.txt` (`IYI_RAYLIB_VERSION` default and its `STRINGS`), `build.bat` /
   `build.sh` (`RL_VERSION` and the legacy `rlXY` argument), `tools/release.py` (`RAYLIB_VERSIONS`,
   `DEFAULT_RAYLIB`; `tools/test_release.py`) and `tools/iyipack/CMakeLists.txt` (`GIT_TAG`). The scripts
   pass the version explicitly because a cache variable's default never reaches an existing build tree.
3. Branch what breaks as above, and update this section, the other documents' "current" mentions.
4. Gate the new and the legacy build: build, ctest, `--check-shaders`, `--frames 120` with
   `IYI_RUNTIME_READY` (and `--single-thread`), and compare frames by numbers (§3.1).
5. Ask Gry which older versions stay legacy options.
