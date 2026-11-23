# tools/web: browser (WebAssembly) build

The engine, CPython 3.14 (wasm) and a game, built with Emscripten into a static site plus an itch.io ZIP.
Engine code for the browser sits behind `#ifdef __EMSCRIPTEN__` / `if(EMSCRIPTEN)`; the desktop build never
sees it. Merged from the POC on 2026-10-28 (history in git).

## Use

- Once per machine: `py tools\web\setup_toolchain.py` puts emsdk 3.1.61 and the prebuilt wasm CPython
  (pygame-web/python-wasm-sdk `3.1.61.15bi`, sha256-checked) into `third_party\web\` (git-ignored, ~2 GB).
  The `EMSDK` and `IYI_WEB_PYTHON_SDK` env vars point the setup and the build elsewhere. Needs `git`
  and the `lz4` Python package (`py -m pip install lz4`).
- **Usual entry point:** `py tools\web\web_build.py <game folder> [--smoke] [--serve] [--out <dir>]` (HR:
  `web-build.bat`). Builds into `build\web\engine`, packages `<out>/site` +
  `<project>-web-<date>-<commit>[-dirty].zip` (default `<game>/release/web`); `--smoke` opens `?smoke=1`
  headless in Chrome/Edge and waits for the game's `IYI_WEB_SMOKE_OK` / `_FAIL` console line (a Python
  traceback also fails); `--serve` serves the site on localhost.
- Serve over HTTP, never `file://`.

## Files

- `web_env.bat`: emsdk + Visual Studio CMake/Ninja on PATH. `build_engine.bat [targets]`: the engine with
  Emscripten (`web.cmake` replaces desktop CPython with the wasm one) into `build\web\engine`.
- `targets.cmake`: `web_engine` (the real `src/main.cpp`) plus the game at `/game` in its own package,
  `game.data` + `game.js` (file_packager), remade whenever a game file changes, so a game-only change never
  relinks the .wasm. Only `project.json`, `scripts/`, `assets/` are packaged, for the game and each
  `project.json` "libraries" folder (at the path it resolves to from `/game`), minus `IYI_WEB_EXCLUDE`
  patterns (`web_build.py` passes what git ignores in the game). Saves persist via `persist.js` (IndexedDB).
- `assets/fonts/*CJK*.otf|ttf` go into `fonts_cjk.data` + `fonts_cjk.js`, which `shell.html` fetches once the
  runtime is up; the game must manage without them until they land (HR falls back to DejaVu).
- `package_web.py <build dir> <out dir> [zip]`: site folder with `index.html` + flat ZIP (`web_build.py` calls it).
- `smoke/`: `smoke/engine` is the default game; `build_smoke.bat` (raylib + WebGL 2 only, `build\web\smoke-raylib`)
  and `web_gate2` (CPython + pybind11, logs `IYI_WEB_PYBIND_OK`) isolate a toolchain problem.

## How a game runs in the browser

- One thread: `Application::Run` hands the frame to `emscripten_set_main_loop_arg` (single-thread mode),
  no FPS limiter (requestAnimationFrame paces it), no `WindowShouldClose`. `NavPlanner` plans a request at
  once instead of on its worker thread.
- `?smoke=1` sets `IYI_SMOKE=1` in the game's environment (`shell.html`); the game decides what a smoke run does.
- The page stretches the canvas over the tab; each frame `Application::PlatformFrame` sets the pixel buffer
  to the tab's CSS size x `devicePixelRatio` and scales the mouse to it, so a game cannot set its own
  resolution on web (hide that setting, HR does). The tab title is the project name.
- Audio: miniaudio plays on the main thread; raylib's `AUDIO_DEVICE_PERIOD_SIZE_IN_FRAMES=4096` (~85 ms)
  rides out one slow frame.
- Games detect the browser with `sys.platform == "emscripten"` (Quit has no meaning there).

## Known limits

- CPython is a downloaded prebuilt binary, not built from source (that needs WSL2/Linux).
- Gry checks the bigger audio period by ear; if it still crackles, move miniaudio to an AudioWorklet.
- Tested in Chrome on Windows over localhost only (no Firefox/Safari, no cold-cache load over a network).
- Hidden tabs pause requestAnimationFrame, so the game pauses.
- Only HR has a smoke run (`IYI_SMOKE`); other games print no `IYI_WEB_SMOKE_OK`, so `--smoke` times out for them.
