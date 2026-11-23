# IYI Engine contract: CLI, agent channel, crash reports

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 3. CLI

```text
IYIEngine.exe [--project <path-to-game-project>] [--no-hot-reload] [--log-level <level>]
              [--frames <n>] [--screenshot <file.png>] [--profile] [--check-shaders] [--single-thread]
              [--agent-run] [--user-data <dir>] [--hidden] [--fixed-dt <s>] [--seed <n>] [--unthrottled]
              [--window-size <WxH>]
IYIEngine.exe --compare-images <job.json>
```

- `--no-hot-reload` disables the file watcher regardless of `project.json`.
- `--frames <n>` quits after n frames (a scripted run: smoke checks, benchmarks).
- `--screenshot <file>` (with `--frames`) saves the last frame; the path is relative to the
  working directory.
- `--profile` logs the frame profiler's last one-second window at exit (category `profile`,
  one line per section: avg ms / max ms / calls per frame); a run shorter than a second logs
  what it has. `IYIEngine --project X --frames 600 --profile` is a benchmark.
- `--check-shaders` compiles every `<name>.fs` under the project's and its libraries' `assets/`
  (includes expanded) with each vertex shader it can really run with: `<name>.vs` beside it, else
  raylib's default when that writes every `in` of the `.fs`, else each `.vs` beside it whose `out`s
  cover them (a `.fs` with none fails), in a hidden window, logs each pair (`failed: X.fs with Y.vs`)
  that fails with raylib's compile error (category `raylib`, then `shaders`) and exits: 0 when all
  compile, 1 otherwise. A shader counts as failed when loading it logged a warning (raylib links a
  failed stage with its default, so the shader itself still looks valid). Run it after editing a
  shader, before launching the game.
- `--single-thread` runs the game step and the render on the main thread, one after the other
  (§8a): the same code without the game thread, for debugging and before/after measurements.
- Isolated agent runs (development builds only; `tools/agent_probe.py run` passes them, §3.1):
  - `--agent-run`: `engine.is_agent_run()` is True. Games skip intros, tutorials and other flows that
    are only for a person (HR: intro, tutorials, legacy data copy). The engine never starts Steam in
    such a run (no achievements, stats or "playing" on the owner's account, no shared Steam session).
  - `--user-data <dir>`: the user-data root (`engine.user_data_path`, §18.5: saves, settings,
    `audio.json`) and this run's own folder (`engine.run_path`, §18.5: `logs/`, `agentio/`) are `<dir>`.
    A game keeps every file a run writes under one of the two (HR's development saves and own probe are
    under `run_path`), so isolated runs never touch the player's files nor share one with each other:
    any number of them (and of trailer takes, `docs/IYITRAILER.md`) can run at once.
  - `--hidden`: no visible window (`FLAG_WINDOW_HIDDEN`); real keyboard, mouse and gamepad input is
    dropped in the frame input (the agent's own input still arrives, §3.1), the window reads as
    focused, and the 30 FPS background cap does not apply. Screenshots and `regions` match a visible run.
  - `--fixed-dt <s>`: every frame's `dt` is exactly `s` (the frame input's frame time). With
    `--agent-run` the agent channel runs in lockstep: after a reply the game runs one more frame,
    then waits (up to 60 s; a quit request or a closed window ends the wait at once, and after a close
    no frame waits again) for the next request, so the frames between two requests are always the same
    and a run repeats. Ten such 60 s waits in a row with no request quit the run.
  - An `--agent-run` never outlives its launcher: the engine keeps a handle to the process that
    started it and quits (a logged warning) within half a second of that process ending, a hard kill
    included (a tool timeout, a closed terminal). So a launcher must start the game as its own child
    and stay alive while it runs (never a detached `start`). `tools/agent_probe.py` also deletes its
    `iyi_agent_*` temp folders older than a day.
  - `--seed <n>`: `engine.run_seed()` returns n (None without it) for the game's own RNG.
  - `--unthrottled`: no FPS limit and no VSync. With `--fixed-dt` and `--seed` the same requests give
    the same state and the same `regions` on every run, as fast as the machine can. With `--fixed-dt`,
    `engine.get_time()` and the postprocess `uTime` are the sum of the fixed steps (FrameInput `Time`),
    not the wall clock, so time-driven shaders and animations repeat too.
  - `--window-size <WxH>`: the window opens at that size and the game's `set_window_size` and fullscreen
    toggles (Alt+Enter included) do nothing in this run: a visual test keeps its resolution.
- `--compare-images <job.json>` (development builds only, no window, no project): compares PNG pairs for
  visual regression tests and exits (`ImageCompare.h`, `docs/VISUAL-TESTS.md`). Started by
  `tools/visual_test.py`.
- `--trailer-shot <name>` / `--trailer-config <file.json>` (development builds only): record a
  game's scripted marketing shot (§22). Normally started by `tools/trailer.py`, not by hand.
- Unknown flag → usage message, exit 1.

raylib's own warnings and errors (`TraceLog`) go to the engine log under category `raylib`, so
they are counted (`health`) and reach every agent-channel reply. Its info lines (textures, fonts,
shaders, audio loaded) go to the engine log too, as Info, so they are in `logs/latest.log` (a game
may show them: HR's boot screen); its debug lines still only print.

### 3.1 Agent channel (development builds only)

Every game can be inspected by an agent without game code. The engine polls
`<project>/agentio/request.json` four times a second; a request is read, deleted and
answered in `<project>/agentio/<id>.json` (written to a temporary name and renamed, so a
reply that exists is complete):

```json
{"id": "town", "scene": "scripts/scenes/verify.py", "wait_frames": 4, "wait_seconds": 0,
 "screenshot": true, "profile": true, "quit": false, "...": "game keys"}
```

All keys are optional. `id` names the outputs (`out` by default; `/ \ : .` become `_`).
`scene` is `change_scene`. The whole request then goes to the entry script's
`AgentRequest(request)` hook (§13), whose return value comes back as the reply's `game`
(JSON; other values as `repr`; `{"error": ...}` if it raised; `null` without the hook). The
game then runs `wait_frames` (at least 2) and `wait_seconds`, so the change is visible; the
last of those frames is saved as `<id>.png` when `screenshot` is set, and the reply follows one
frame later with `fps`, `game`, `screenshot` and, with `profile`, the profiler's last window as
`[{name, avg_ms, max_ms, calls}]`, plus `counts` (`{counter: mean per frame}`) where a section
counts its work (`ProfileCount`): `sprite_stacks` has `stacks_in`, its `emit` has `stacks`,
`quads`, `textures` (draw calls) and `flushes` (batch full), and the sub-sections `place`, `flush`
(rlgl uploading and drawing) and `layers` (building the quads; `ProfileAdd`, only while the F3
overlay is open: `engine.set_profile_detail`), the terrain `cells`, particles `particles`. `quit` closes the game after replying. One
request at a time: a new file waits until the current one is answered. Shipping builds compile it
out.

Every reply has `log`: `new_problems` (warnings and errors logged since the request came in,
raylib's included, e.g. a shader that did not compile) and their `lines`.

Checking a picture by numbers instead of looking at it (each of these takes the frame's image as
`screenshot` does, saves it as `<id>.png` and measures the file; the reply gains `image`
`{file, size}`):

- `"regions": [cols, rows]` (or `true`: 8 x 4) → `regions` `{grid, cells, mean}`: the screen cut
  into cells, rows top first, each `[r, g, b, luma]` 0..255, and the whole frame's mean. Enough to
  tell a black or washed-out frame, a dark night, a missing shadow band, a HUD that moved.
- `"compare": "<earlier id>"` → `compare` `{against, changed_pct, mean_diff, bbox, diff_image}`:
  pixels whose largest channel moved more than 12, their bounding box `[x0, y0, x1, y1]` (null:
  none) and `<id>_diff.png` (changed pixels red over a dark copy, regions outlined yellow; the same
  `CompareImages` as `--compare-images`). A refactor that should change
  nothing proves it with `changed_pct` near 0; open the diff image only when it is not.
- `"pixels": [[x, y, z], {"world": [x, y, z]}, {"screen": [x, y]}]` → `pixels`, one entry each:
  `point`, `screen` (window pixels through the world camera, null behind a 3D camera), `px` (image
  pixel) and `rgb` (a 3 x 3 mean; null off the image). World points go through the 3D camera
  (`set_camera_3d`, z up: give the ground height yourself) or the 2D one.

`"track_ui": ["ElementName", ...]` records motion: for every waited frame, each named UI element
as that frame draws it (after every hook, laid out), `{rect: [x, y, w, h] on-screen bounds in canvas
units after scale, opacity, visible: it and every ancestor}`. The reply's `track_ui` is the list of
frames. For a zoom, an open animation or a screen switch: a part that lags shows as a frame where it
has not moved with the rest.

Acting on the game through its normal input path (applied to the frame input before every hook, one
step per frame; the outputs wait until every step has run):

- `"click": "ElementName"`: a left press, then a release on the next frame, at the centre of the first
  live canvas element of that name (through its transform, the canvas scale and the postprocess
  radial warp), so hover, pressed style, `clicked`, sounds and focus all happen as for a player.
- `"type": {"element": "Name", "text": "..."}`: clicks the element, then types the text as characters.
- `"key": "ESCAPE"` (or a raylib key code): pressed on one frame, released on the next. Names: letters,
  digits, `F1`..`F12`, `ESCAPE`, `ENTER`, `SPACE`, `TAB`, `BACKSPACE`, `DELETE`, arrows, `HOME`, `END`,
  `PAGE_UP`, `PAGE_DOWN`, `LEFT_`/`RIGHT_` `SHIFT`, `CONTROL`, `ALT`. The object form
  `{"key": "A", "hold_frames": 4, "modifiers": ["LEFT_CONTROL"]}` holds the key down for `hold_frames`
  frames (default 1) with the modifiers held down on every one of them.
- `"scroll": {"element": "Name", "amount": -3}`: moves the pointer to the element's centre (as `click`) and
  applies a mouse-wheel move of `amount` (positive: up) on one frame.
- `"drag": {"from": "Name" | [x, y], "to": "Name" | [x, y], "frames": 10}`: left press at `from`, the pointer
  moves linearly over `frames` frames (default 10), release at `to`. `[x, y]` is window pixels. The reply's
  `drag` is `{from, to, frames}`.
- Click safety: `click`, `type`, `scroll` and a `drag` from an element queue nothing and reply
  `{"element": name, "error": reason}` when a player could not do it: `hidden` (the element, an ancestor or
  its canvas is not visible), `ambiguous` (several shown elements of that name in different canvases, and
  no `scope`), `disabled` (it or an ancestor), `offscreen` (its centre is outside the window) or `occluded`
  (the topmost pointer hit at its centre, `UICanvas::PickElement`, is neither it nor a descendant: a modal
  or popup on top). `"scope": "AncestorName"` (click, type, scroll, drag) only considers elements below an
  ancestor of that name. A missing element is `"no such element"`.
- The reply's `input` says where each landed (`{element, screen, visible, enabled}`) or why not. Every
  reply carries `"protocol": 2` (2: scroll, drag, key objects, click safety, `scope`).
- `"scene": "<path>"` (above) is the direct scene change, without a game's own transition.
- `"window_size": [w, h]` pins the window to that size as `--window-size` does (the game's own
  `set_window_size` and fullscreen then do nothing), and `"locale": "<code>"` sets the language as
  `engine.loc.set_language` does. Both apply before this request's input (UI scenarios, `docs/PLAYTEST.md`).
- `"wait_until": {"scene": "<path>", "visible": "ElementName", "timeout_frames": 600}`: after the
  input, the outputs wait until the scene is open and the element is shown (both optional), then
  `wait_frames` more; the reply's `wait_until` is `{met, frames}`.

Every reply also has `scene` (the open scene's path) and `events`: the structured events (§17.5)
logged since the request came in. `"ui": true` adds `ui`, every named element of every visible
canvas that is shown: `{name, type, rect, opacity, text (as drawn, localized), enabled: false}` when
it applies; what is on screen, by numbers. For the visual tests' UI rules each entry also has
`canvas_size` (logical), `screen_rect` (window pixels, through its transform and the canvas scale),
`parent` and `parent_rect` (unless the parent is the root), `clipped: true` below a scroll view or a
`ClipChildren` ancestor, and for Text and Button `measured_width`, `fitted_font_size`, `font_size` (its
style's, when it has a style class or a custom style) and `missing_glyphs` (the characters its font
cannot draw).

`"ui_problems": true` adds `ui_problems`: the UI rules' findings on every shown canvas
(`UICanvas::Problems`, SPEC-IYIUI §17 "UI problems"), `[{kind, element, rect, detail}]`. Ask for it after
every UI change: an empty list means nothing overflows, clips, shrinks, misses a glyph or overlaps.

`python tools/agent_probe.py run --project <game> --request a.json [--request b.json] [-- <flags>]`
starts an isolated game (`--agent-run --hidden --no-hot-reload --user-data <temp>`), sends each request
in turn, quits it and prints `{ok, replies, events, errors, exit_code}`: exit 0 when every request was
answered and no error was logged, 1 when one was not answered, 2 when an error was logged. Stdout is that JSON and nothing else (messages go to stderr), so `json.loads` parses it.
Two tools build on these requests (`docs/TESTING.md` lists every check): `tools/playtest.py ui` plays a
UI scenario with asserts and a replayable trace (`docs/PLAYTEST.md`); `tools/visual_test.py` captures
scenario screens and compares them with approved baselines (`docs/VISUAL-TESTS.md`).

`"health": true` adds the engine's own view of the running game, for finding out why it
misbehaves without game code: `uptime_s`, `frames`; `frame_ms` (`avg`, `max`, `over_50ms` over
the last 240 frames); `window` (`focused`, `minimized`, `focus_changes` since start, `screen`,
`render`, `monitor_hz`); `scene_changes` (since start); `memory` (Windows: `private_mb`, `working_set_mb`, `peak_private_mb`,
`growth_mb_per_min` since the previous health reply; `null` elsewhere); `assets` (cache counts,
the render texture keys `name#WxH`, `gpu_mb_estimate`); `log` (`warnings`, `errors` and the
`recent` 40 Info+ lines, where other agents' requests and hot reloads show up).
`tools/agent_probe.py --project <game>` asks for it twice, 5 s apart, and prints it readably.

### 3.2 Crash and hang reports

**Crash report (every build, Windows; `src/CrashReport.cpp`).** An unhandled exception or
`std::terminate` writes into the run's `logs/` (the per-user root in a shipped game): `crash.dmp`
(a minidump for a debugger), `crash.txt` (what, where as module+offset, each thread's open profiler
section) and, appended, `crash_python.txt` (every Python thread's stack, from `faulthandler`). Then
a message box says where the report is and offers to open that folder; agent runs and `--hidden`
write the files only. Nothing is sent anywhere. Known limit: faulthandler also appends a stack for a
Windows exception some code handles itself (ctypes turns a bad read into `OSError`), so an entry
there without a `crash.txt` of the same time is not a crash. Off Windows there is no report yet.

**Hang watchdog (development builds only).** A thread that only observes (it never touches raylib,
actors or Python) writes `agentio/heartbeat.json` every 0.5 s: `frame`, `seconds_since_frame`,
`game_section` and `render_section` (each thread's innermost open profiler section), `private_mb`,
`working_set_mb`, `stalled`, `pid`. A game step that has not finished for 2 s is logged once
(`[watchdog] main loop stalled ...`) and again when it recovers; Python's stacks go to
`agentio/hang_stack.txt` when the game thread has not re-armed `faulthandler.dump_traceback_later`
for 2.5 s. Lockstep runs (`--agent-run --fixed-dt`) wait for their agent on purpose and start no
watchdog. When a request gets no reply, `tools/agent_probe.py` reads both files and adds `hang`:
a `verdict` (frozen and where, slow, or heartbeat stopped: ended, crashed or suspended), the
`heartbeat` and the last `python_stacks`.

### Finding the project when `--project` is absent

The engine is a console application, so double-clicking it used to flash a console
and vanish, which reads as "nothing happens". When no `--project` is given, look
for a `project.json` in this order and use the first that exists:

1. the directory containing the executable
2. the current working directory
3. `<executable directory>/game/`

Step 3 is what makes a packaged build work: the packager puts the game in `game/`
next to the exe, so the exe is double-clickable.

If none is found, print the usage message **and, when the process was started with
no arguments at all, wait for Enter before exiting** so the message is readable
when launched from a file manager. Exit code stays 1. Do not wait when arguments
were supplied but were invalid, that path is for scripts.
