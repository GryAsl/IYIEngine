<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="IYIEngineLOGO.png">
    <img src="docs/media/logo-dark.png" width="160" alt="IYI Engine logo">
  </picture>
</p>

# IYI Engine

A 2D-focused game engine written in C++20, with games written in Python or C++. raylib handles windowing,
rendering, input and audio. CPython is embedded through pybind11, and game scripts hot reload while
the game is running. The engine is about 32,000 lines of C++ (`src/`, `include/`), sized so one
person can read all of it.

I built it for my own commercial game projects. This repository is a snapshot of the engine; the
games and day-to-day development history live elsewhere.

Human Resources and Project COIN, two of my games built on the engine:

<p align="center">
  <img src="docs/media/human-resources.gif" width="49%" alt="Human Resources">
  <img src="docs/media/coin.webp" width="49%" alt="Project COIN">
</p>

## What it does

- **Two threads, one frame apart.** The main thread owns the window, OpenGL, input and audio. A game
  thread runs Python one frame ahead. Game-side code never calls OpenGL or audio directly: it records
  each call into a *frame packet*, and the render thread replays it
  (`src/FramePacket.cpp`, `docs/spec/runtime.md` §8a). `--single-thread` runs both on one thread.
- **Embedded Python with hot reload.** Saving a script reloads it in place. Actors keep their
  identity, position and state. Lifecycle hooks are looked up on every call, never cached, which is
  what keeps reload reliable (`src/PythonRuntime.cpp`).
- **Native C++ games.** A game can also be written entirely in C++ against the same API (`IYI::Api`)
  that the Python bindings call. See `tests/native_example/`.
- **IYIUI**, a retained-mode UI: hierarchy, layout with cached solves, styling, tweens and sequences,
  text fitting, IME input (`src/IYIUI*.cpp`, `docs/SPEC-IYIUI.md`).
- **Actors and scenes** with deferred destruction, fixed-step updates and per-actor script state.
- **Assets and packaging.** Development reads loose files. A release packs bytecode and assets into
  one compressed, encrypted `rres` archive, with hot reload compiled out (`tools/release.py`).
- **Multiplayer (optional `libraries/net`).** Host-authoritative replication with per-client deltas,
  inputs, interpolation and host migration, over ENet or a loopback transport, from Python or C++
  (`docs/spec/net.md`, `tests/net_example/`). Built only when the library is present (`IYI_NET`).
- **Rendering extras:** sprite stacks with lighting, particles, video playback, shaders.
- **Tooling:** native test suite, screenshot-based visual regression tests, a deterministic replay
  and playtest harness, a frame profiler, and a trailer recorder.

## Documentation Index

| Document | Answers |
|---|---|
| [Engine specification](docs/SPEC.md) | Project configuration, lifecycle, Python API, and the engine's runtime contracts. |
| [Runtime and threading](docs/spec/runtime.md) | Frame order, hot reload, and how the game and render threads exchange data. |
| [IYIUI specification](docs/SPEC-IYIUI.md) | UI hierarchy, layout, styling, localization, tweens, and sequences. |
| [Multiplayer](docs/spec/net.md) | Host-authoritative replication, inputs, interpolation, and host migration. |
| [C++ API cheatsheet](docs/CHEATSHEET.md) and [Python type stubs](typings/engine/) | Reference for native games and Python scripts. |
| [Testing](docs/TESTING.md), [visual regression](docs/VISUAL-TESTS.md), and [playtesting](docs/PLAYTEST.md) | Build checks, screenshot comparisons, and deterministic replays. |
| [IYITrailer](docs/IYITRAILER.md) and [video encoding](docs/VIDEO-ENCODING.md) | Scripted capture and encoding for game footage. |
| [Roadmap](docs/ROADMAP.md) | Design decisions, engine internals, and planned work. |

## Architecture Tour

```text
 main thread (render side)                      game thread (game side)
 ─────────────────────────                      ───────────────────────
 poll input ──► FrameInput copy ──────────────► Python hooks (update, draw, UI)
                                                 actors, scenes, IYIUI
 replay FramePacket ◄────────────────────────── record GL / audio calls
 (setup, audio, offscreen, world, screen)        into the next FramePacket
 present
```

The frame packet is the only bridge between the two sides: it records closures that capture values,
never live game state. The full contract is in `docs/SPEC.md`, which indexes `docs/spec/*.md`.


| Area | Start here | What it handles |
|---|---|---|
| Startup and frame loop | [main.cpp](src/main.cpp), [Application.cpp](src/Application.cpp), [FramePacket.h](include/IYI/FramePacket.h) | Setup and shutdown, fixed and variable updates, input snapshots, and render command replay. |
| Actors and scenes | [GameActor.h](include/IYI/GameActor.h), [ActorRegistry.cpp](src/ActorRegistry.cpp), [Scene.cpp](src/Scene.cpp) | Actor lifecycle, deferred destruction, and persistence across scene changes. |
| Projects and assets | [ProjectConfig.cpp](src/ProjectConfig.cpp), [ResourceStore.cpp](src/ResourceStore.cpp), [AssetManager.cpp](src/AssetManager.cpp) | Project settings, loose and packaged resources, and asset caches. |
| Python and native C++ | [PythonRuntime.cpp](src/PythonRuntime.cpp), [EngineBindings.cpp](src/EngineBindings.cpp), [Api.h](include/IYI/Api.h), [Game.h](include/IYI/Game.h) | Embedded CPython, script hooks and hot reload, and the shared C++ game API. |
| UI and localization | [IYIUI.cpp](src/IYIUI.cpp), [IYIUILayout.cpp](src/IYIUILayout.cpp), [LocalizationManager.cpp](src/LocalizationManager.cpp) | Retained widgets, layout, styling and animation, and localized text. |
| Rendering and media | [SpriteStack.cpp](src/SpriteStack.cpp), [StackLighting.cpp](src/StackLighting.cpp), [Video.cpp](src/Video.cpp), [AudioManager.cpp](src/AudioManager.cpp) | Batched sprite stacks and lighting, video playback, and audio. |
| Optional libraries | [Library index](libraries/README.md) | Modules for navigation, networking, selection, and profiling. |

## Building

Tested on Windows 10/11 with Visual Studio 2022 (C++ workload with CMake), Python 3.14 and Git.
raylib, pybind11 and nlohmann/json are fetched by CMake.

```bat
build.bat
build\ninja\IYIEngineTests.exe
```

Steamworks support is optional and off unless the SDK is present next to the repository.

## Running a game

```bat
build\ninja\IYIEngine.exe --project <path to a game folder>
```

A game folder holds `project.json`, `scripts/` and `assets/`. Two small ones ship here:

- `tools/web/smoke/engine/`: a Python scene with an actor and an IYIUI canvas
  (`build\ninja\IYIEngine.exe --project tools/web/smoke/engine`).
- `tests/native_example/`: a project whose game logic is a C++ module. Build the module first with
  `tests\native_example\native\build.bat` (needs CPython 3.14).
