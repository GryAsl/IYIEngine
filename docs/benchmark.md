# Benchmark: COIN, 2026-10-29

Where the frame goes in COIN (the heaviest game on the engine) and what to optimise first, in three
parts: game thread, render thread, GPU. The numbers come from the existing tools and scenarios only:
COIN's `tools/perf_check.py --launch` and the agent channel (`profile`, `"render": {"gpu": true}`)
on the `patrol`, `town` and `stress` scenarios.

## Setup

- AMD Radeon 780M (iGPU, shared memory), 1920x1080 window, fps cap 120, no vsync, sun map 8192
  (COIN's default `shadow_map`), engine and COIN at commit `9f2ae109`.
- Game started with COIN's `agentio/launch.ps1`, window visible. Each shot waited 2.5 s after a
  camera move (one whole profiler window).
- Caveat: a HumanResources game from another session was open during the runs (about 0.3 of a core
  and some GPU), so absolute numbers may be a few % pessimistic. The ratios hold.

Status 2026-11-06: a dated snapshot. COIN's game code is still Python (its C++ port is planned in COIN
`docs/cpp-port-plan.md`), so the Python-side items below still apply; nothing here has been implemented.

Reading the profile: the game and render threads run one frame apart (SPEC §8a), so a frame costs
`max(game, render, GPU)`, never the sum. `game/wait` high means the render side is the limit;
`render/wait` high means the game thread is. `gpu/render` (the root) spans the whole render frame,
waits included: the GPU's real work is the sum of its children (`offscreen` + `draw` + `present`).

## Results

| Shot | fps | game/update | game/wait | render/wait | GPU work (offscreen + draw) | Limit |
|---|---|---|---|---|---|---|
| idle, town (zoom 1) | 47.7 | 2.4 | 18.3 | 0.0 | 7.9 + 11.0 | GPU |
| idle, street (zoom 7) | 55.9 | 1.9 | 15.8 | 0.0 | 7.4 + 8.6 | GPU |
| idle, horizon | 46.5 | 2.3 | 18.9 | 0.0 | 21.5 total | GPU |
| patrol, close | 52.1 | 17.4 | 0.6 | 12.2 | 8.1 + 8.7 | game + GPU |
| town scenario, wide | 48.9 | 16.5 | 1.3 | 8.7 | 8.1 + 11.0 | game + GPU |
| stress, close 20 s | 43.9 | 20.1 | 0.0 | 18.4 | 8.3 + 10.4 | game |
| stress, town 20 s | 40.0 | 22.1 | 1.2 | 16.3 | 24.0 total | game + GPU |
| stress, night | 58.7 | 15.1 | 0.0 | 14.4 | 1.6 + 11.1 | game |
| stress, clock fast (4x) | 40.5 | 22.3 | 0.0 | 19.5 | 9.4 + 11.1 | game |
| stress, close 180 s | 39.7 | 19.9 | 0.0 | 19.9 | 25.0 total | game |

All times in ms. Stress after 180 s matches 20 s (823 people, no growth): no leak, no creeping cost.
No new log problems in any shot.

`perf_check.py` against `tools/perf_baseline.json` fails, but mostly because the world grew since
the baseline: stacks 22k → 35k and quads 129k → 202k in the town shot (living scenery), and the sun
map went to 8192 (GPU 2.1 → 7.3 ms). Nav got much cheaper (stress `nav` 17 → 2.5 ms); combat got
dearer (0.7 → 7.7 ms). Save a new baseline (`--save`) once the work below is decided, so the next
regression is visible.

**The headline:** both sides are at about 20 to 25 ms, so fixing one alone buys little. Idle, the
GPU is the limit; with any scenario running, the game thread is, and the GPU is right behind it.
The four biggest items, in order: sun map (GPU, about 7 ms everywhere), combat glue (game, about 7 ms),
`world.py/self` (game, 6 to 9 ms, unprofiled), terrain (GPU, 4 to 6 ms, never culled).

## Game thread

| Row (stress close) | avg ms | max ms |
|---|---|---|
| `game/update` | 20.1 | 32.2 |
| `world.py/self` (no section) | 7.9 | 13.3 |
| `battle/combat` (823 people) | 7.9 | 13.9 |
| `battle/nav` | 2.8 | 5.4 |
| `battle/ai` | 0.8 | 1.7 |
| `game/draw/screen` overlay.py | 1.6 | 3.0 |

1. **Combat's Python glue every frame** (`COIN/scripts/game/combat.py` `step`, `_apply`). Each frame
   it builds five Python lists of 823 entries (a dict lookup and a branch per person), then
   `core.people()` returns 823 tuples of 13 fields (about 11k Python objects) only to be read back
   by `person()`. The cost scales with people, not with fighting: the town scenario (418 people,
   little shooting) still pays 6.7 ms. Fixes, cheapest first:
   - call `core.people()` only when someone reads it (lazy, or once per detect tick, 0.2 s), or
     return only the rows that changed;
   - pass positions as one buffer (nav already holds them in C++: read them there through
     IYIEngineSim instead of rebuilding lists in Python);
   - step combat at a lower rate for people far from the camera (as nav does with focus).
2. **`world.py/self` is 6 to 9 ms with no section.** Everything after the battle in `Update`
   (`units.items`, `buildings.items`, `ambient`, `downwash`, `lights`, `marks`, `weather`,
   `fx.render`'s arguments) is one opaque number. First step: wrap each in `engine.profile_begin` so
   the cost has a name; `units.items` (one item per walker and vehicle every frame) is the likely
   leader. Then only rebuild items that moved, or build them in C++ from nav's state.
3. **Native sight checks.** `Combat::Detect` tests every able soldier against every non-player
   within 200 m with a grid ray (`Sight.Visible`) five times a second; `ChooseTarget` scans all
   people per shooter. Fine at 823, quadratic past it. A uniform grid bucket of people (rebuilt once
   per step) would cut both scans to neighbours. Do this after (1), when native time shows on its own.
4. **Clock fast** runs the battle 1.39 times per frame (`clock.slices`) and costs +2.6 ms; the max
   reaches 39 ms (hitches). Steady sub-steps are fine; cap the work per frame instead of the slices.
5. Small: `overlay.py` 1.2 to 1.6 ms per frame for marks and badges (cache the text and layout per
   squad); the F3 overlay costs 0.4 to 1.0 ms while open (keep it closed when measuring).

## Render thread

The render thread is not the limit in any shot (it waits on the game side for 9 to 20 ms), but
two items set its spikes:

| Row | avg ms | max ms |
|---|---|---|
| `sprite_stacks/instanced/chunks` (idle town, 406 chunks) | 4.0 | 7.0 |
| `sprite_stacks/instanced` (idle town, 35k stacks, 811 draws) | 9.6 | 12.3 |
| `sun_map/static_depth` rebuild (every ~30 frames) | 0.5 | 23.7 |
| `sun_map/instanced/split` | 0.4 | 15.9 |

1. **Static sun depth rebuild spikes** (`StackLighting.cpp` `SunMapPass`): when the camera or the
   sun step changes the projection, every static caster is drawn again into the 8192 map, a 15 to
   25 ms frame. Spread the rebuild over several frames (tiles of the map) or keep the old cache
   until the new one is ready.
2. **Stack submission scales with stacks in view.** The whole-town shot submits 35k stacks in 811
   draws (9.6 ms on the render thread). More static batching per chunk (one draw per chunk and
   atlas, not per archetype) and a far LOD for small props (tufts, yard props skipped beyond a
   zoom) would cut both this and the GPU stack cost.
3. `render/present` is small in game-bound shots; in idle shots it is the GPU wait, not CPU work.

## GPU

| Row (stress close) | avg ms | Notes |
|---|---|---|
| `offscreen/sun_map` | 7.4 | about 7 ms in every daytime shot, 0 at night |
| `draw/world/sprite_stacks` | 4.8 | 5.8 to 6 ms in wide shots |
| `draw/world/terrain` | 3.9 | 256 of 256 chunks, 250k cells, at every zoom |
| `present` | 3.6 | post effects and the screen blit |
| `offscreen/contact/blur` | 0.7 | |
| `draw/world/particles` | 0.7 | |

1. **The sun map is the biggest single cost**, and it is fixed: 6.6 ms even on a quiet street with
   few moving casters. With the static cache, each frame copies the 8192x8192 static depth into
   the sun map (256 MB read and written on an iGPU sharing system memory) and then draws the moving
   casters. Night proves it: no sun map, 58.7 fps against 43.9. Options:
   - draw the moving casters into a small separate map round the camera (2048 is enough for
     units), and sample the static map directly instead of copying it;
   - or copy only the area under the moving casters (scissored blit);
   - or default to 4096 on integrated GPUs (`SHADOW_MAPS` already offers it: "about a quarter
     faster").
2. **Terrain is never culled** (`CellTerrain.cpp` `DrawCellTerrain`): chunks are tested against
   `area`, the same rectangle that maps the ground picture, and COIN passes the whole map
   (`world.py` `fx.draw_terrain(..., s["ground_area"], ...)`). Split the two: a separate cull
   rectangle (the camera's view, as `_sun_area` already computes). The terrain fragment shader is
   heavy too (sun map, light map, contact, marks, water, clouds, fog), so cut overdraw: draw terrain
   after the opaque stack tops where they cover it, or early depth.
3. **Sprite stacks** 4.8 to 6 ms: fragment cost of many overlapping layers. The same far LOD as in
   the render thread section (fewer layers per stack when zoomed out) helps here most.
4. `present` 2.5 to 4.7 ms: the post chain at full resolution. Bloom/glow at half resolution would
   save most of it.

## Order of work

1. Combat glue (game, about -5 ms with a scenario running).
2. Sun map copy (GPU, about -5 ms everywhere in daytime).
3. Sections in `world.py/self`, then the largest of them (game).
4. Terrain cull rectangle (GPU, -1 to -2 ms close up, little at zoom 1).
5. Static sun rebuild spread over frames (hitches).
6. Stack LOD and batching (render thread and GPU on wide shots).

After each: `py -3.14 tools\perf_check.py --launch` in COIN, alone on the machine, and a new
baseline once a step is kept.

## Known limits

- One machine (780M iGPU). On a discrete GPU the sun map copy is far cheaper and the game thread
  will be the only limit.
- Scenario fights differ run to run; combat and nav rows vary about 10%.
- Another game was open during these runs (see Setup).
