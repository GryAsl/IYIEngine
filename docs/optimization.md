# Optimization backlog

Measured opportunities that are not done yet. Each entry says where the cost is, what was
measured, the idea and its risk. Measurements: isolated agent runs (`tools/agent_probe.py run`,
`profile`), 120 fps cap, avg ms per frame unless noted. Done work is recorded in the game's own
docs (HumanResources `docs/chart_performance.md`).

- **HR Company cash chart while easing** (partly done 2026-11-15, HR `docs/chart_performance.md`):
  `Charts.company` peaks at about 2.1 ms (avg 0.49); the rest is building the 2,017 points in
  Python. Idea: envelope the targets per pixel column before easing (about 2 per column). Risk:
  the eased line changes shape slightly while it glides.
- **TransportationCo route lines** still build `engine.Vector2(*p)` per point for
  `draw_spline_linear` (frozen project, left as is). Synthopia passes its `(x, y)` pairs directly
  since 2026-11-15 (lanes, road preview, bus line, road query, routes, tile lines), and its
  cursor tip follows the mouse on `translation` (it re-laid out the HUD on every frame of a road
  drag). `draw_spline_linear` takes any `(x, y)` sequence, tuple or list (a JSON path).

## Done (2026-11-14): layout re-solves, hot reload, charts

Measured in HR (isolated agent runs, 120 fps cap). The trigger: panning Investments dropped to
34 fps because `draw/screen/ui/layout/solve` took about 19 ms on every frame.

- **`RectTransform::Translation` / `element.translation`** (SPEC-IYIUI §3): a visual offset applied
  after scale and rotation, like `scale`. It is not in the layout-cache signature, so changing it
  every frame lays nothing out again; drawing, culling, clipping and hit testing follow it through
  `ResolvedTransform`. `element.translate_to(x, y, duration, ease)` tweens it. `anchored_position`
  written with a new value each frame re-solves the whole canvas: use `translation` for pans,
  slides and floats (an element whose parent does not lay it out and whose `resolved_rect` nobody
  positions things from; HR's screen shake stays on `anchored_position` for that reason).
- **Layout solve without repeated subtrees** (`IYIUILayout.cpp`): a container's shown children
  (not scroll views) get only their own rect in `ResolveElementContent`, since `ApplyContainerLayout`
  places them and resolves their content anyway, and `FitHeightAt` measures children without
  resolving their content. Nested `fit_height` stacks resolved each subtree up to 4x per level.
  HR draw pass: 75,942 -> 5,963 `ResolveElementLayout` calls for 3,305 elements, a miss
  14.5-19 ms -> about 3 ms. The `solve` section reports `resolves` and `elements` counts.
- **Hot reload scan** (`PythonRuntime.cpp ScriptFiles`): the write time comes from the directory
  listing and the path is made relative by text, instead of `fs::relative` and `last_write_time`
  per file. The 0.25 s poll: max 25 -> 2.3 ms, avg 0.81 -> 0.07 ms (development builds only).
- **`draw_spline_linear` takes `(x, y)` tuples** as well as `Vector2`s: a Python `Vector2` per
  point cost about 1 us. HR `Chart.line`: avg 1.15 -> 0.48 ms, max 6.2 -> about 2.5 ms.
- **HR** moved per-frame offsets to `translation`: Investments pan (and zoom, already `scale`),
  new-mail row slide, retention card drop and payout rise, money / level floats, chart labels
  (the live label follows the easing line), the tutorial card slide (`translate_to`).
  Idle Investments, Operations and Market screens with time running: no re-solves.

Proof of "same result": every shown element's on-screen rect from the old engine + old HR against
the new pair, on three test states (`ceo_hired`, `early_game`, `ceo_delegation`), every screen,
Investments zoom and pan: about 5,000 rects, 0 different. Market and Company frames: 576 colour
regions each, 0 different. Native tests cover nested fit stacks, translation hit testing and
`translate_to`.

Known limits: `ui_problems` and `resolved_rect` see an element without its translation (a panned
Investments card that is off view is not reported as clipped). `UICanvas::Problems()` crashes on a
headless `UIButton` with no font (seen in `engine_tests`; the test avoids it).

## Done (2026-11-07)

- IYIUI: an element wholly outside the innermost clip draws nothing, nor its children when it clips
  them (`DrawElement`, `OutsideClip`); unscaled, unrotated elements record no matrix; text the fit
  measured inside its box gets no scissor; alpha-0 backgrounds are not drawn; consecutive texts share
  one text-mask blend (`RecordText`, `RecordState`); `ResolvedText` formats only when the key, args
  or `LocalizationManager::Revision()` change; a text memo keeps its `FontForText` answers while
  `AssetManager::FontGeneration()` is unchanged, and its line widths until the next refit.
  HR testrun (Operations): `game/draw/screen/ui` 0.67 -> 0.44 ms, `render/draw/screen` 1.03 -> 0.48 ms;
  visual tests (default and cjk matrices) identical to the previous engine.
- Assets: font files are read once for their atlases (last two kept), glyph images are not kept after
  the atlas is built, pak reads seek to the chunk offset `ScanArchive` recorded, the postprocess
  target has a depth texture only while a 3D camera is active.
- Release: the Windows runtime skips modules no game imports (ssl, sqlite3, asyncio, msi,
  multiprocessing, expat, elementtree, winsound, `python.cat`): about 4.4 MB.
- HumanResources: Investments refreshes only when what its cards read changed (or once a second);
  the cash chart redraws on `cash_tick` or the money label, one to two points per pixel column; every
  stock read goes through `capture_harvest._stock`; a save copies only the log and simulation (no
  deepcopy) and is written gzip (day-86 save: 187 KB -> 24 KB, 5 ms).

## Known limits

- The font byte cache holds up to two font files (about 21 MB with a CJK font) until `UnloadAll`.
- The stdlib zip is not repacked; `_hashlib` and libcrypto stay (hashlib is used).
- The Load list still parses each save once per session (cached by mtime); saves are small now.
