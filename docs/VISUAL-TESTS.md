# Visual regression tests (development only)

Which screens of a game changed, where, and did its UI break, in one command:

```bat
py -3.14 tools\visual_test.py run --project ..\HumanResources
```

Each scenario runs in its own isolated, hidden, lockstep game (SPEC §3.1) once per configuration. Its
captures are compared with approved baselines and checked by UI rules. Output: `report.html` and
`report.json` in the run folder. Exit 0: all passed, 1: a case did not, 2: a game or scenario failed.
Nothing of it runs in a normal game or a shipping build. All automated checks: `docs/TESTING.md`.

## Commands (all take `--project <game>`)

| Command | Does |
|---|---|
| `run [--scenario a,b] [--category c] [--changed] [--matrix <name>] [--jobs N] [--visible] [--env <name>]` | capture and judge; `--jobs N` games at once (default min(4, cores/2)) |
| `approve --case "<glob>"` (repeatable) or `--all` | the latest run's captures become baselines |
| `reject --case "<glob>"` | deletes the capture: it stays failed until fixed |
| `compare` / `report` `[--run <id>]` | judge again without the game / rebuild the HTML |

A case is `<scenario>/<capture>@<config>`, e.g. `early_game/market@1920x1080_en_dark`. A new screen starts
as `MISSING_BASELINE`: look at it in the report, then approve it. `--changed` runs the scenarios whose
`WATCH` globs match a file git reports as changed (a scenario without `WATCH` always runs).

## A game's files

```text
<game>/visual_tests/
  config.json          engine flags, options, matrices, thresholds, ignore
  scenarios/<name>.py  one scenario per file (_name.py: shared helpers)
  baselines/<env>/     approved PNG + JSON (local, git-ignored; <env> = platform + GPU from the log)
  results/<run>/       captures, .ui.json, .diff.png, report.html/json (local, git-ignored)
```

```python
import _hr

CATEGORY = "gameplay"            # --category
WATCH = ["scripts/game/ui/*"]    # --changed


def run(t):
    t.game(test_state="early_game")                      # the game's own AgentRequest keys
    t.click("NavMarketButton")                           # by name, through the player's input path
    t.wait_until(visible="MarketScreen", wait_frames=30)
    t.capture("market", mask=["ClockText"], ignore=[{"kind": "OVERFLOW", "element": "Chart*"}])
```

Also `t.scene`, `t.type`, `t.key`, `t.wait`, `t.wait_stable` (until two frames match), `t.send(**request)`
and `t.config` (this run's matrix entry). A click a player could not make, an unmet wait or a game error
ends the scenario as `ERROR` at that step.

`config.json` `options` maps an axis value to user-data files merged in before the game starts, so it
opens in that language or theme as for a player: `"lang": {"tr": {"settings.json": {"language": "tr"}}}`.
The `size` axis is `--window-size WxH` (the game's own resizes are ignored). `matrices` name the
combinations; `default` runs unless `--matrix` names another. Effects that alter the whole picture
(post-process shaders, screen shake) stay off through an option in every matrix, so a diff shows only the
UI; one small matrix turns them on to check the effects themselves (HR: axis `fx`, matrix `fx`).

## Judging

`IYIEngine --compare-images` (`ImageCompare.h`, no window): a pixel changed when a channel moved more than
`tolerance` (12); changed pixels group into 16 px tiles, a tile under `tile_min` (4) changed pixels is
noise (antialiasing); connected tiles are a region. A block SSIM on luma catches wide subtle shifts.
Masks count for nothing.

| Status | When |
|---|---|
| `PASS` | no region of `min_region_pixels` (24) or more, SSIM at least `min_ssim` (0.99), no new UI problem |
| `FAIL` | a visual change past those thresholds |
| `STRUCTURAL_FAIL` | the image matches, but the UI has a problem the baseline did not |
| `MISSING_BASELINE` / `RESOLUTION_MISMATCH` | no baseline / a baseline of another size (never compared) |
| `ERROR` / `REJECTED` | the run failed at a step / rejected by hand |

UI rules, from the engine since 2026-11-12 (each capture asks for `"ui_problems"`, SPEC-IYIUI §17
"UI problems"; an older runtime falls back to `visual_test.ui_problems` on the `ui` dump): `ZERO_SIZE` (not an empty panel),
`OFFSCREEN`, `OVERFLOW` (outside its parent, unless clipped by a scroll view or `ClipChildren`),
`TEXT_CLIP`, `FONT_SHRUNK`, `UNSUPPORTED_GLYPH`, `TEXT_OVERLAP` (engine only: sibling texts whose glyphs
cross), `BUTTON_OVERLAP` (on the clickable part only: a
button cut by every clipping ancestor, as the engine hit-tests; a button under a later
`blocks_input_behind` panel, a modal's backdrop, is not clickable and not counted; overlaps are per
canvas). The same list shows live in red with `canvas.debug_draw = True`. Approving accepts the current problems; later runs fail only on new ones. The UI
playtester asserts the same rules: `{"assert": {"ui_problems": {...}}}` (`docs/PLAYTEST.md`).

## Repeatable runs

`--fixed-dt 1/30 --seed 1 --unthrottled` plus lockstep: the frames between two requests are always the
same, and `engine.get_time()` and the shader `uTime` are game time. A game stays repeatable when its
animations read `dt` or `engine.get_time()` (never `time.monotonic()`) and decorative randomness is seeded
from `engine.run_seed()`. The window is hidden but real: a GL context is needed (no true headless).

## Status (2026-11-07)

Done: the tool and its tests (`tools/test_visual_test.py`), `ImageCompare` (also the agent `compare`),
`--window-size`, the frame clock, the `ui` dump's layout fields, the playtester assertion. HumanResources:
matrices `default`, `full` (1280x720, 1920x1080, 1280x1024 x EN/TR x dark/light), `cjk`, `small` with CRT/VHS/shake off, `fx` (1920x1080 EN dark) with them on; 22
captures per configuration covering every screen and the main popups; 352 cases with effects off plus
22 with them on, baselines re-approved 2026-11-07 (`default` and `fx` re-run with 0 changed pixels). First finding, fixed: Investments' unused finance slots sat off screen.

Known limits: `FONT_SHRUNK` is common where a game auto-shrinks text (HR: about 90 elements, accepted in
the baselines). `screen_rect` is window pixels, so masks by element name are off on high-DPI displays.
 `--jobs` runs scenario/configuration games in parallel (each is an isolated process, so results do not depend on it): HR `default` took 34.9 s with `--jobs 1`, 14.8 s with 4 and 16.2 s with 8 (16 logical cores, 0 changed pixels in all three), so 4 is the default; `full` (about 4 minutes sequentially) not re-timed.
