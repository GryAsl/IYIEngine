# IYI Engine contract: IYITrailer (dev only)

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 22. IYITrailer (development builds only)

Records a game's scripted marketing shot without a human: the real game, driven by a Python shot,
into an H.264 + AAC `.mp4` (and a GIF made from it). Full guide: `docs/IYITRAILER.md`.

- **Never ships.** `src/Trailer.cpp`, `TrailerProcess.cpp` and `TrailerBindings.cpp` build only when
  `IYI_SHIPPING_BUILD` is off; every hook in `Application.cpp`, `main.cpp` and `PythonRuntime.cpp` is
  inside `#ifndef IYI_SHIPPING_BUILD`. A game keeps its shots in `<project>/marketing/` (never packed:
  only `scripts/` and `assets/` are) and recordings in `<project>/captures/`. `release.py` hard-fails
  otherwise (§18.6).
- **Launcher.** `python tools/trailer.py render|list|validate --project <game> --shot <name>`. `render`
  runs `IYIEngine --project <game> --no-hot-reload --trailer-shot <name> --trailer-config <json>`,
  checks the result with ffprobe, makes the GIF (FFmpeg palettegen) and writes `result.json`; exit
  code 0 only on success. FFmpeg (with libx264 and aac) is found from `--ffmpeg`, `IYI_FFMPEG`, `PATH`,
  then a winget install. Defaults: 1920x1080 at 25 fps (1280x720 at 30 is also supported), quality 18,
  output `<project>/captures/<shot>/<timestamp>/`. `--encoder` (default `IYI_ENCODER`, else `auto`) picks the
  final H.264 encoder: `auto` takes the first of `nvenc`, `amf`, `qsv` that passes a test encode, else `cpu`
  (libx264, CRF 18 preset slow); a forced one that fails is an error. It is passed to the engine as the
  config's `video_args` and reported as `result.json` `encoder`.
- **Isolated run.** `render` starts the engine with `--agent-run` (`engine.is_agent_run()`: a game skips its
  intro and tutorials; `IYI_SKIP_INTRO=1` is still set for games not yet moved), `--user-data <temp folder>`
  (deleted after the take), `--hidden` (`--visible` opts out) and `--no-hot-reload` (`--hot-reload` opts back
  in). `result.json` also reports `audio_levels` (LUFS, true peak), `changed_project_files` (mtime changed
  during the take) and `failed_expectations` (read from `<user data>/expectations.json`, which fail the take).
  `--mpg` adds an MPEG-1 (`-q:v 4`) + MP2 `.mpg` converted BT.709 to BT.601 for `engine.video`. A successful take in the
  default output folder deletes all but the shot's 3 newest take folders.
- **Shot.** `marketing/shots/<name>.py` is an actor script. Once `IYI_RUNTIME_READY` is reached the
  engine spawns it as the persistent actor `trailer_shot`. A missing script, any error logged after the
  spawn, or the config `timeout` (default 120 s) fails the shot and quits. Exit codes: 0 done,
  2 shot error, 3 the recording never finished.
- **Capture.** A shot run draws every frame into an offscreen canvas of the config's `resolution`
  (`TrailerCanvasSize`), not the window: the game's screen size, layout and pointer are the canvas's,
  the window shows it fitted, and the size does not depend on the monitor. The render side reads each finished frame (after the screenshot, before
  `EndDrawing`; the canvas in a shot run, else the window's framebuffer) and a writer thread pipes it to FFmpeg (lossless `libx264rgb`) at real-time pace: frames are placed by wall
  clock, skipped above the target rate (`surplus_frames`) and repeated below it (`repeated_frames`); `dropped_frames`
  counts frames lost to a full writer queue. A source size other
  than the target (a `start(width=, height=)` other than the canvas, or a window outside a shot run) is scaled and letterboxed by FFmpeg; a resize during recording fails it. Audio is
  the game's own final mix (`AttachAudioMixedProcessor`, float32 stereo), resampled to 48 kHz AAC
  256 kbit/s. `stop` then encodes the final file from both (H.264 4:2:0, BT.709, saturation x1.25, the
  config's `video_args`, default libx264 CRF 18 preset slow), so it blocks the render side for a few seconds. `capture.json` (written atomically next to the `.mp4`) reports `ok`, the output,
  video and audio facts, `capture_stats` and `errors`.

```python
engine.trailer.start(name: str = "", fps: int = 0, width: int = 0, height: int = 0,
                     audio: bool | None = None, output_dir: str = "") -> None   # 0/"" = configured default
engine.trailer.stop() -> None        # finalize; status() becomes "done" or "error"
engine.trailer.status() -> dict      # state, elapsed, mp4, report, error
```

`start` and `stop` only queue work for the render side (§8a) and never wait; a second `start` while
one records raises. The engine finalizes a recording that is still running when it quits.
