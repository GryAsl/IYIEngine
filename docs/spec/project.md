# IYI Engine contract: Repository layout, project.json, native modules, native games

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 1. Engine repository layout

```text
IYIEngine/
├── CMakeLists.txt, build.bat / build.sh / dev.bat / package.bat, README.md
├── cmake/                     (build helpers)
├── docs/                      (SPEC.md = the contract's index, spec/ = its parts, agents/ = agent rules in full, SPEC-IYIUI.md, ROADMAP.md, CHEATSHEET.md, ...)
├── include/IYI/               (public headers; IYIUI/ holds the UI subsystem's)
├── src/                       (C++ runtime: one .cpp per header, plus the *Bindings.cpp files that
│                               expose it to Python; PythonRuntime.cpp holds all pybind11 embed logic)
├── libraries/                 (Python + asset libraries a game lists in project.json, §2:
│                               sprite_stack, profiler_overlay, geo_world, nav, rts_select)
├── tools/                     (agent_probe.py, release.py, trailer.py, iyiloc.py, pyi_sync.py, iyipack/, web/)
├── tests/, plugins/, third_party/
```

The static library `IYIEngineSim` holds the simulation C++ with no window, draw, input, audio or GPU:
Life, Nav, NavSim, PyExact and Sight (§2b).

**Header discipline (plan §32/§33):** no public header may include `Python.h`,
`pybind11/*`, or `nlohmann/json.hpp`. `raylib.h` in public headers is fine and
expected. Trivial accessors may stay inline in headers; real logic goes in `.cpp`.

## 2. `project.json` (game side)

```json
{
    "name": "IYI Game",
    "window_width": 1280,
    "window_height": 720,
    "target_fps": 120,
    "entry_script": "scripts/main.py",
    "window_resizable": false,
    "hot_reload": true,
    "default_language": "en",
    "audio": {
        "master": 1.0,
        "music": 0.8,
        "sfx": 1.0
    },
    "libraries": ["../IYIEngine/libraries/sprite_stack"],
    "native": ["native/bin"]
}
```

`ProjectConfig` holds these fields plus the resolved absolute `ProjectDir`.
Every field is optional and has a default (`"IYI Game"`, 1280, 720, 60,
`"scripts/main.py"`, `false`, `true`, `"en"`); `libraries` and `native` default to `[]`.
A native game (§2c) needs no `entry_script`: the field is
ignored when the game exe passes its `Game` object. Missing or malformed
`project.json` → clear error message on stderr and exit code 1. No settings
framework.

`default_language` specifies the initial active and fallback language
code for the localization subsystem (see §19). Defaults to `"en"`.

`audio` specifies initial default volume multipliers for named audio buses
(`ProjectConfig::AudioBusDefaults`). Values are validated to be finite and
clamped to `[0.0, 1.0]`. If the per-user `audio.json` configuration file is absent
on startup (see §20.6), these bus defaults are applied. Built-in buses `"master"`,
`"music"`, and `"sfx"` default to `1.0` if not specified.

`libraries` lists folders, relative to the project, that are laid under the project in
development: each has the project layout (`scripts/`, `assets/`) and fills in any
project-relative path the project itself lacks (the project always wins, earlier libraries win
over later ones). The dev resource backend looks them up for every resource read
(`ResourceStore`, so `load_texture`, `load_shader`, actor scripts and `project_path` all see
them), their `scripts/` folders follow the project's on `sys.path`, and hot reload watches them.
`tools/release.py` and `package.bat` pack them into the game as if they were its own files, so a
packaged build needs nothing extra. A missing library folder fails startup with its name. Namespace a library's
files (`scripts/<name>/`, `assets/<name>/`) so they cannot collide with a game's. Libraries are
how code is shared between games: the engine only puts the project on `sys.path`.
`desktop_libraries` repeats the `libraries` entries that only desktop builds carry: `tools/release.py`
for Android/iOS and the web build (`tools/web/targets.cmake`) neither pack them nor keep them in the
packed project.json, so the game must cope with the import failing there.
A library may also have its own build switch, so a game that does not list it carries none of its
code: `libraries/net` (`IYI_NET`, §25.2) is the first.

`native` lists folders, relative to the project, that hold modules the game itself built (a
`.pyd`). The engine never looks inside them: it only puts each folder on `sys.path`, after the
project's `scripts/` and every library's, so a Python module of the same name always wins. A
folder that is not there yet is logged (category `python`) and skipped, never a startup failure;
whether a module must exist, and what the game does without it, is the game's own Python policy.
A packaged build has one flat directory instead, filled by `tools/release.py`. Contract and
rationale: §2b.

`hot_reload` exists so a packaged release can ship with script watching off
without needing a launcher script. The CLI flag still wins: `--no-hot-reload`
forces it off even when the file says `true`.

## 2b. Project-scoped native modules

A game may keep its own C++ under `<project>/native/` and have it built into a module (a `.pyd`)
that the game's Python imports. The reason is a game whose simulation dominates the frame: it is
game code, so folding it into this repository would put game-specific code in the engine (rule 8).
For a Python game this is one narrow exception with a named place and a plain-data boundary (a
game written fully in C++ is a native game, §2c). It is deliberately minimal and does not grow into a subsystem.

- **Plain C++ core plus a thin binding.** The core (`<project>/native/*.cpp`, no `Python.h`) is
  ordinary C++, so the same code also builds into a plain C++ tool or test. Exactly one file
  (`bindings.cpp`) knows about Python, and it lives in the game project, not here. The engine is
  never rebuilt for a game's module.
- **Engine C++ through `IYIEngineSim`.** The core may include the engine's simulation headers and
  call them directly, with no Python in between: `IYIEngineSim` is a small static library (today
  `Nav.cpp` NavPlanner, `NavSim*.cpp` NavSim, `Life.cpp` Population and friends, `Sight.cpp` SightGrid) that `IYIEngineCore` links and a game module links
  too, so the engine and every game share one copy of the code. A file goes into `IYIEngineSim` only
  if it has no Python, window, GPU, audio or actor dependency. `raylib.h` stays usable: its types,
  `raymath.h` and the collision helpers are plain functions; the window, drawing, input, audio and
  GPU calls are not (they belong to the engine's own raylib inside the exe, which a module does not
  share).
- **Build: `cmake/iyi_native.cmake` + `tools/build_native.bat`.** A game's `native/CMakeLists.txt`
  includes the cmake file and calls `iyi_add_native_module(<name> <sources...>)`; it reads the
  engine's `build/ninja` cache (Python, its version, Release), adds the engine `include/`, raylib's
  headers and pybind11, links `IYIEngineSim.lib` and `raylib.lib`, and writes the `.pyd` to
  `native/bin`. `tools/build_native.bat <native dir>` finds Visual Studio, configures with Ninja and
  builds; a game's `native/build.bat` is one line calling it. Build the engine first: the module
  links the engine build's libraries, so rebuild the module after the engine's `IYIEngineSim`
  changes.
- **`project.json` `"native"`** (§2) names the folders holding the built modules. The engine puts
  them on `sys.path` in development and in a packaged build, and logs a missing folder. Whether a
  module must exist is the game's Python policy.
- **The boundary to Python is plain data**: numbers, strings, tuples, lists, `bytes`, float
  buffers. Engine C++ types stay inside the module (an `engine.Vector2` or `engine.SightGrid` object
  never crosses into it), and pybind11 objects of one module are never passed to another.
- **No hot reload.** A module is loaded at startup; replacing it means restarting the game
  (Windows locks a loaded file). Python hot reload is untouched.
- **Packaged location.** `tools/release.py` copies the contents of every declared folder into one
  flat `<OutputName>_Data/native`, which the shipping engine appends to `sys.path` (found through
  the engine's existing data-folder helper, the same one that locates the CPython runtime). The
  files are never put into `game.rres`: a `.pyd` cannot be imported from an archive. The folders
  are flattened there because extension file names are unique per interpreter.

Deliberately not in this version: native code in libraries; native actors; symbols exported from
the exe (a module links `IYIEngineSim`, never the exe); per-interpreter module builds beyond declaring the module for the interpreter the engine
embeds (`build.bat py311` keeps the Python path for a game that needs it). The manual gate is
`tests/native_example`, built by its own `native/build.bat`.

The engine never loads or calls a game's module: Python does, with an ordinary `import`, from the
folders above. A game module therefore cannot run with the embedded interpreter out of the loop,
and nothing in the engine can drive it per frame; a project-scoped module is C++ that Python calls,
not C++ the engine runs. If a game wants its heavy path executed with no Python in the loop at all
(a native step the engine ticks, a sim thread calling straight into C++), the engine needs a small
C-ABI load/call hook. That is not built; it is the next step if the need appears, and it would
change nothing else here, the boundary stays plain data.

## 2c. Native games (C++)

A game may be written fully in C++ against the engine, with no Python in its runtime (approved
2026-10-29). It is its own exe: its `main()` calls
`IYI::RunGame(argc, argv, std::make_unique<MyGame>())` (`include/IYI/Game.h`), which runs the same
`Application` as `IYIEngine.exe` with the `Game` object in place of the entry script.

- **One implementation, two front ends.** Every engine feature has one C++ implementation; the
  Python bindings and a native game both call it. A binding is a thin call into a named C++
  function (`IYI::Api`, `include/IYI/Api.h`), never a second copy of the logic.
- **raylib first.** A native game uses raylib directly for what does not touch the window, GL,
  input or audio (types, `raymath.h`, collision, text and file helpers, `GetRandomValue`). Those
  that do go through `IYI::Api`, whose functions carry raylib's own names and arguments
  (`Api::DrawRectangle`, `Api::IsKeyPressed`): a recording wrapper on the game thread (§8a, rule
  29), nothing more.
- **Same threads.** The game's C++ runs on the game thread exactly as Python does.
- **Scenes** are functions registered by name (`RegisterScene("map", fn)`); `Api::ChangeScene("map")`
  applies at the end of the step like `engine.change_scene`, keeping persistent actors.
- **Actors** are `GameActor` subclasses overriding the lifecycle virtuals; state lives in their
  members. `Spawn<T>(name, pos, args...)`, `FindActor<T>(name)`; `ScreenSpace` and draw order (spawn
  order) as for Python actors.
- **Agent channel** (development builds): the game's keys go to `Game::AgentRequest(json)`, which
  returns the reply's `"game"` as JSON text; the engine's own keys and the format do not change.
- **Build**: the game's `CMakeLists.txt` includes `cmake/iyi_game.cmake` and calls
  `iyi_add_game(<name> <sources...>)`, which builds the engine source into the game's own tree
  (`add_subdirectory`) and writes `<project>/bin/<name>.exe`; `tools/build_game.bat <project>`
  builds it. `IYI_WITH_PYTHON` (default ON) OFF leaves the interpreter, every binding and pybind11
  out; a shipping native build is OFF (§14b). `package.bat native <game dir>` packages its exe
  with `project.json` and the assets (its libraries' assets folded in), no Python, as loose files. The
  shipping package (encrypted `game.rres`, §18.7) comes from `tools/release.py`: a `project.json` with
  no `entry_script` is a native game (needs a `CMakeLists.txt` in its folder or `src/`); the pipeline
  builds the game's own CMake tree with `IYI_SHIPPING_BUILD=ON IYI_WITH_PYTHON=OFF` into
  `<cmake dir>/build/shipping` (the developer's `bin/` stays untouched), packs the assets (a native
  game's C++ sources count as the text that names them), and ships the exe, the MSVC CRT and
  `<Name>_Data/game.rres`: no Python runtime, scripts, `native/` modules or `.pyc`. Verified on Windows only.
- **Libraries**: the runtime parts COIN uses are C++ in `libraries/<lib>/src/` (built into
  `IYIEngineCore`, included as `"<lib>/src/X.h"`); their Python modules wrap the same C++
  (`engine.sprite_stack`, `engine.geo_world`, `engine.rts_select`, `engine.profiler_overlay`).
- **No hot reload**: change, build, restart. Not here on purpose: loading game code as a DLL,
  plugins, reflection, a component system.
