# IYITrailer

Development-only recorder for marketing footage: it runs the real game, lets a Python *shot* drive
it, and writes an H.264 + AAC `.mp4` with the game's own sound (and a GIF made from it). No human,
no screen recorder. The contract is `spec/trailer.md` §22; this page is the how-to.

It never ships: the engine files build only without `IYI_SHIPPING_BUILD`, shots live outside
`scripts/`, and `tools/release.py` fails a release that references either (SPEC §18.6).

## Setup

FFmpeg with the `libx264` and `aac` encoders:

```bat
winget install Gyan.FFmpeg
```

`tools/trailer.py` looks for it in `--ffmpeg`, then `IYI_FFMPEG`, then `PATH`, then the winget
install folder (a fresh winget install is not on `PATH` until a new shell). Build the engine with
`build.bat`.

## Render

From `IYIEngine/`:

```bat
python tools/trailer.py list     --project ..\HumanResources
python tools/trailer.py validate --project ..\HumanResources --shot market_spike
python tools/trailer.py render   --project ..\HumanResources --shot market_spike --format both
```

| Option | Default | |
|---|---|---|
| `--format mp4\|gif\|both` | `mp4` | A GIF-only render is silent and deletes the intermediate `.mp4`. |
| `--resolution WxH`, `--fps N` | `1920x1080`, `25` | `1280x720 --fps 30` for small clips. Even sizes only. |
| `--gif-fps`, `--gif-width` | `12`, `720` | GIF frame rate and width (FFmpeg palettegen). |
| `--timeout S` | `120` | Seconds the shot may run before it fails. |
| `--out DIR` | `<project>/captures/<shot>/<timestamp>/` | Spaces and non-ASCII are fine. Takes of one shot started in the same second get `<timestamp>-2`, `-3`, ... A successful take there deletes all but the shot's 3 newest take folders (`KEEP_TAKES`); `--out` folders are never pruned. |
| `--no-audio` | off | Diagnostics only: an `.mp4` without sound. |
| `--mpg` | off | Also write `<shot>.mpg` (MPEG-1 `-q:v 4` + MP2) for `engine.video`, see Colour. |
| `--visible`, `--hot-reload` | off | Show the window (default `--hidden`: real input ignored); keep hot reload on (default `--no-hot-reload`). |
| `--encoder auto\|nvenc\|amf\|qsv\|cpu` | `IYI_ENCODER`, else `auto` | Final H.264 encoder, see GPU or CPU below. `result.json` has the one used under `encoder`. |
| `--json` | off | Print the report instead of its path. |

Quality: the engine captures losslessly (RGB) in real time, then encodes the final file once the
shot stops: H.264 4:2:0, BT.709-tagged, saturation x1.25 (players show the palette paler than
the game does), quality 18 (CPU: CRF 18 preset slow, about 2 Mbit/s for a 9 s 1080p25 HR shot), AAC 256 kbit/s. That final encode
takes a few seconds after the shot.

Colour: one pipeline. The game draws in sRGB, the capture is the BT.709 master (the `.mp4`), and
`--mpg` converts that master to BT.601 (`scale=in_color_matrix=bt709:out_color_matrix=bt601`) because
pl_mpeg, behind `engine.video`, decodes BT.601; without the conversion greens clip and reds go pale.

Isolation: a take runs `--agent-run` (a game skips its intro and tutorials), `--user-data <temp folder>`
(settings, `audio.json`, saves and run history of the take live there and are deleted after it; the
player's AppData is never touched) and `--hidden`. A game mutes buses for a take with
`engine.audio.set_bus_gain`, never `set_bus_volume` (which is saved).

The last stdout line is the path of `result.json`. Exit code 0 only when the file was recorded,
checked with ffprobe (H.264 at the asked size, AAC stereo) and every asked format exists.

Output folder:

| File | |
|---|---|
| `<shot>.mp4`, `<shot>.gif` | The footage. |
| `result.json` | `ok`, `output`, `video` (size, fps, frames, seconds), `audio`, `audio_levels` (`integrated_lufs`, `true_peak_dbfs`), `capture_stats`, `ui` (below), `changed_project_files` (files whose mtime changed during the take), `failed_expectations`, `errors`. |
| `capture.json` | The engine's own report, which `result.json` builds on. |
| `engine.log`, `video.log`, `mux.log`, `video.mp4`, `audio.f32` | Kept only when something failed. |

Size: a shot run draws every frame into an offscreen canvas of `--resolution`, whatever the window
or monitor allows, and records from it; the game (UI layout, `get_screen_width`, the pointer) sees
that size, and the window only shows a fitted preview. So `2560x1440` or `3840x2160` record on a
1080p monitor and in a `--hidden` window (which Windows clamps to the work area: 1920x1080 asked
came out 1920x1061 before the canvas). `result.json` `video.source_width`/`source_height` equal the
asked size. Measured on an RTX 4070 (HR `orientation_market`, 30 fps, one take at a time): the game
renders about 120 fps at 1080p and 1440p and about 34 fps at 4K, so 4K has little headroom and
parallel 4K takes repeat frames.

`capture_stats.surplus_frames` counts rendered frames above the target rate (normal: their slot
already has a frame, nothing is lost); `dropped_frames` counts frames whose content was lost because
the writer queue was full (should be 0); `repeated_frames` counts slots filled twice because the game
rendered slower (for example `--single-thread`, or 4K with other takes running). The video keeps
real-time pace either way. Before 2026-11-11 `dropped_frames` meant what `surplus_frames` means now.

## Write a shot

A shot is an actor script, `<project>/marketing/shots/<name>.py`. The engine spawns it as the
persistent actor `trailer_shot` once the game is up (`IYI_RUNTIME_READY`). Any error logged after
that, or the timeout, fails the shot. It records with `engine.trailer`:

```python
import engine

def Update(actor, dt):
    s = actor.state
    if "t" not in s:
        s["t"] = 0.0
        engine.trailer.start()            # record from here
    s["t"] += dt
    if s["t"] > 5.0 and engine.trailer.status()["state"] == "recording":
        engine.trailer.stop()             # finalize the .mp4
        engine.quit()
```

`engine.trailer.start(track_ui=["CapturePanel", ...])` follows those UI elements (by name) while it
records. The report's `ui` gives each one as time ranges in which it looks the same:
`{"CapturePanel": [{"from": 0.0, "to": 2.34, "rect": [x, y, w, h], "opacity": 1.0, "visible": true}, ...]}`
(seconds of the video; rect = on-screen bounds in whole UI canvas units: video pixels only when the UI
canvas reference size is the recording size. A 2560x1440 shot of a game whose UI references 1920x1080
reports 1920x1080 units: scale by recording width / reference width to reach video pixels, or use them as
they are in a 1920x1080 edit). An edit aims a focus box at a name instead of pixels
measured by eye. HR's `hr_bridge.play(..., track_ui=[...])` passes it on.

Takes are isolated runs (own `--user-data`, no Steam; SPEC §3), so several can render at once, the same
shot included.

A shot can state what must happen: the take writes `expectations.json` into its user-data folder
(`<user data>/expectations.json`, a JSON list of texts) and `trailer.py` fails the take with
each text in `errors` and `failed_expectations` (HR: `hr_bridge.expect(cond, "CEO hired")`).

Keep the game's own code paths: a shot should press the same functions the player's buttons do,
so the footage shows the real game. Put game-specific glue in one module next to the shots
(Human Resources: `marketing/hr_bridge.py`) rather than in each shot.

### Human Resources

`marketing/hr_bridge.py` sandboxes the run and drives it:
- **Sandbox.** Saves, run history and settings live in the take's own user-data folder. Settings are the
  game's defaults (English, every tutorial done, CRT/VHS and screen shake off). The take is an agent run
  (`--agent-run`), so the game itself shows no tutorials, never pauses on Alt+Tab and skips the outbreak
  flash; `--hidden` (the default) drops real input, so typing in another window cannot pause a take
  (with `--visible` it can). The window is 1920x1080
  windowed, so a take looks the same on every machine. The game's music is muted with
  `engine.audio.set_bus_gain("music", 0)` unless a shot asks for `begin(..., music=True)`; HR's training
  tape adds music and effects in its edit. `hr_bridge` helpers (`hire_ceo`, `buy`, `sell`, `outbreak`,
  ...) call `expect()` on their key effect, and `play()` expects the speed to stay above 0.
- **Run.** The run is debug-tainted, so it never reaches leaderboards or Steam.
- **Start.** It starts a new run from the Main Menu.
- **Played days.** Before recording, a plain player (`autoplay`: keep capture and harvest busy, sell
  stock, buy the cheapest affordable upgrade) plays `AGE_DAYS` (7) days through the real
  simulation step, so the footage shows a lived-in run: price history, the Activity Log, run
  statistics. `begin(money=...)` is starting cash given before those days and kept as a reserve.
- **`play()`.** Prepares the state, records, and fires timed cues.

The shots:

| Shot | Shows |
|---|---|
| `market_spike` | An outbreak moves prices on the Market screen, then the stock sells. |
| `ceo_purchase` | Hiring a CEO on the Company screen opens Investments, which buy Retention Access. |
| `contract_success` | A Retention contract is signed, delivered and paid with its bonus. |

```python
from marketing import hr_bridge

def prepare(state):                       # after the played days, before recording
    hr_bridge.prepare_run(state, organs=12)
    hr_bridge.show_screen("market")

def Start(actor):
    hr_bridge.begin(actor, seed=7, money=50_000)

def Update(actor, dt):
    hr_bridge.play(actor, dt, prepare, [(1.5, hr_bridge.outbreak), (6.0, None)])   # None = stop
```

Human Resources assembles these shots into its orientation tape with
`marketing/training_tape/tools/pipeline.py` (its README).

## GPU or CPU

The final encode uses a GPU encoder (NVENC, AMF, QSV) when one works on the machine, else libx264;
`--encoder` / `IYI_ENCODER` overrides the choice. Details, measurements and what is untested:
`docs/VIDEO-ENCODING.md`.

## Troubleshooting

- **`FFmpeg not found` / `lacks encoder(s)`.** Install the full build above, or pass `--ffmpeg`.
- **`encoder 'nvenc' does not work on this machine`.** The forced GPU encoder failed its test encode
  (no such GPU, or an old driver). Use `--encoder auto` or `cpu`.
- **`shot timed out`.** The shot never called `stop()`. Look in `engine.log` for what it waited
  on, or raise `--timeout`.
- **`an error was logged during the shot`.** A Python traceback or engine error is in `engine.log`.
  The partial recording is kept (`partial_capture_kept`).
- **`the window was resized during the recording`.** Do not resize or fullscreen-toggle the window
  while a shot runs.
