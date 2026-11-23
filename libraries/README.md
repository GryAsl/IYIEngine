# Engine libraries

Optional Python + asset folders a game uses by listing them in its `project.json` (paths are
relative to the project):

```json
"libraries": ["../IYIEngine/libraries/sprite_stack", "../IYIEngine/libraries/profiler_overlay"]
```

A library has the project layout and is laid under the project in development: its `scripts/`
folder follows the project's on `sys.path` (`from sprite_stack import fx`), its files fill in any
project-relative path the project lacks (`engine.load_shader("", "assets/sprite_stack/shaders/...")`,
actor scripts such as `scripts/profiler_overlay/actor.py`), hot reload watches it, and
`tools/release.py` packs it into the game. The project always wins over a library. Keep a
library's files under its own name (`scripts/<name>/`, `assets/<name>/`). Contract: `docs/spec/project.md` §2.

A library's runtime logic may be C++ in `libraries/<lib>/src/`, built into `IYIEngineCore`; its
Python module is then a thin wrapper over that C++, so Python games and native games (SPEC §2c)
share one implementation. Development-only parts (offline tools, checks) stay Python.

Libraries are the only way to share code between games (the engine puts just the project on
`sys.path`). They hold no game-specific code (hard rule 8) and follow the game script
rules: plain functions, state in a dict the caller owns, never in module globals. A library carries
Python, assets and its own engine-side C++ (`src/`); a game's own C++ built into a native module is
project-scoped under `<project>/native/`, never a library (`docs/spec/project.md` §2b).

| Library | What it gives a game |
|---|---|
| `sprite_stack` | `fx`: sun shadows, contact shadows and a light map for stacks drawn with `engine.draw_sprite_stacks`, and the camera pass that lights each stack texel at its own ground point (a thin wrapper over the engine's C++ `engine.StackLighting`, with the shaders); also the look: shadow tint, clouds, height shade, wind sway, wet ground, night glow, ground grain and wobbling edges finer than the ground picture (`set_ground_detail`). `daylight`: hour -> sun direction/length/strength, ambient level and colour, lamp level, shadow tint, morning mist, colour grade. `post`: one world-only postprocess shader (grade, mist, tilt-shift, rain streaks, heat haze, vignette, grain). `water`: a per-cell water map (flow, shore distance) that `terrain.fs` draws per pixel as toon water (depth bands, ribbons and wave lines riding the flow, a wobbling shore foam line, chunky foam on rapids, stream-wide waterfalls with the beach and bank running down the walls beside them; still water for a lake or a sea), plus the sand beach it lays into the ground picture. Used by TransportationCo, SpriteStacking, Synthopia and COIN. |
| `geo_world` | Real places as prebuilt map packages (`FORMAT.md`). `tools/geo_import.py` (offline, standard library only) turns a region folder (DEM tiles and OpenStreetMap, both committed, plus manual `overrides.json`) into a deterministic package: DEM heights, road graph, building footprints with heights and covering boxes, compound hedges and yard props round outlying houses, tall-grass patches and tufts, single trees of six kinds (from tree nodes, orchard rows and a density field over wood, scrub, water banks, yards and open ground that fades out at area edges), hand-entered walls, points (settlements, markets, bridges, map-edge entries), a passability grid and a ground picture per season (field bunds, ruts, puddles and potholes). `world`: loads it in the game and answers world queries (height, lon/lat, building at a point, grid bits, roads); building, road and bridge conditions update the grid without regenerating anything and go into save games; builds its `engine.CellTerrain`; `sight_grid` / `update_sight` build and update an `engine.SightGrid` (line of sight, the cursor ray) from the terrain, the tops of buildings, walls and tree trunks (`blocker_heights`) and the leaf bands of crowns, hedges and tall grass (`foliage`); `blast` breaks and burns trees and leaves a scorch, and sight is redone only round it. Used by COIN. |
| `nav` | Dynamic navigation and traffic on a `geo_world` map: the C++ `engine.Nav` (vehicles with lanes, junctions, convoys and doctrines; walkers with crowds, flee and shelter; helicopters that pick a legal landing site), on `engine.NavPlanner` route searches; the library itself holds `classes.json` (the movement class table, equal to `engine.nav_classes()`, for offline tools) and `checks.run` (dynamic obstruction proofs on any map). Walker `steady` (under orders: no running for cover) and `pace` (a wounded walker's speed); a walker stuck on a corner routes again. Overview, events and limits: `nav/README.md`. Used by COIN. |
| `rts_select` | `select`: RTS selection over things the game puts on screen: a click or a drag box from the mouse (`drag`), world points to screen items (`project`, through `engine.get_world_to_screen`), the pick under the cursor by tier then distance (`pick`), boxed ids (`in_box`), replace or toggle the selection (`apply`), control groups with a double recall (`group`). The game decides tiers and what may be boxed. Used by COIN. |
| `playtest` | Development only: deterministic replay of a game's real fixed step and commands (`core`: record, hash, first diff, verify, seek, shrink), the time-travel `debugger`, the Balance Lab (`lab`), the UI playtester (`ui`) and an optional Claude policy (`llm`); driven by `tools/playtest.py` and a game's `devtools/adapter.py` (`docs/PLAYTEST.md`). Stdlib only. Used by HumanResources. |
| `game_kit` | Small pure-Python helpers for management games: `rng` (seeded mulberry32 streams in a dict the save owns: `random`, `uniform`, `randint`, `choice`, `weighted`, `derive`), `files` (crash-safe `write_atomic` / `write_json_atomic`: temp file, fsync, replace), `keys` (rebindable shortcuts over a game-owned action table: `resolve`, `rebind`, `label`, `held`), `warn` (`warn_once`), `world` (shared country list: position, language, continent region, `map_point`; the Natural Earth `assets/game_kit/world_map.png`, rebuilt by `tools/make_map.py`). Engine-side UI parts (import only from UI code): `ui` (layout helpers `place` / `frac` / `fill` / `cell` / `band` / `foot` / `corner`; `stack` (vertical, horizontal or wrapping rows, `fit`), `card`, `window` + `update_windows` (movable card with a close box), `panel`, `text` (`fit=True`: as tall as its text), `button`, `set_text`, `loc`, `money`) and `theme` (skins "office" and "retro_os"; the type scale `TYPE` used as `style(..., role="body")`, `base=` for a copy of another class; `apply(canvas, text_scale, font, extra, skin, pixel_fonts, pixel_em)`, where `extra(theme, scale)` adds a game's own style classes). Prefer these over hand-placed rows and numeric sizes (SPEC-IYIUI "Toolbox"). Used by the management games built on the engine (Human Resources and others). |
| `streaming` | Desktop only (list it in `"desktop_libraries"` too, so Android, iOS and web builds leave it out). Twitch (EventSub WebSocket), YouTube Live (chat polling) and StreamElements tips as one normalized event: `new_session`, `start` / `stop` / `forget` a provider (OAuth in the browser, tokens in the OS credential vault), `status`, `poll` on the game thread; `events` (event fields, `!mail` / `!human` parsing, `clean_text` for viewer text), `limits` (dedupe, cooldowns). Worker threads never touch the game. Stdlib only. `libraries/streaming/README.md`. Used by HumanResources. |
| `net` | Multiplayer (SPEC §25): C++ in `src/` (host-authoritative replication with per-client delta, messages, inputs, interpolation, host migration over ENet, Steam or loopback), compiled only with `IYI_NET` (AUTO: on when this folder exists; `tools/release.py` turns it on only for games that list this library). Python `engine.net` exists only for projects that list it; `net_predict` is the client-side prediction helper. Desktop only (`"desktop_libraries"` too). Test game `tests/net_example`. |
| `profiler_overlay` | `scripts/profiler_overlay/actor.py`: an F3 on-screen table of the engine frame profiler (F3 cycles: threads summary, game and render windows, off; F4 toggles GPU timing) (`engine.get_profile`). Spawn it after the HUD. |

Self-checks: the runtime C++ (daylight, select, the profiler table, geo_world's world) is checked by
the engine tests (`build\ninja\IYIEngineTests.exe`); what stays Python checks itself without the engine:

```bat
python libraries\sprite_stack\scripts\sprite_stack\water.py
python libraries\playtest\scripts\playtest\selfcheck.py
python libraries\game_kit\scripts\game_kit\rng.py
python libraries\game_kit\scripts\game_kit\files.py
python libraries\streaming\scripts\streaming\selfcheck.py
```
