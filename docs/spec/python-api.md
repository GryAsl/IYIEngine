# IYI Engine contract: The `engine` Python module (the whole script API)

Part of the engine contract; index and rules in [../SPEC.md](../SPEC.md). Section numbers are the contract's own.

## 12. Python API, the `engine` module

Built into the executable via `PYBIND11_EMBEDDED_MODULE(engine, m)`.
Split across `EngineBindings.cpp` (engine concepts) and `RaylibBindings.cpp`
(raylib types and thin forwards); each file exposes one
`void RegisterXxxBindings(pybind11::module_&)` function called from the embedded
module initializer in `PythonRuntime.cpp`.

Keep exactly one way to do each thing.

### 12.1 raylib types

Registered as pybind classes with public fields and readable `__repr__`:

- `Vector2(x=0, y=0)`, fields `x`, `y`
- `Vector3(x=0, y=0, z=0)`, fields `x`, `y`, `z`
- `Rectangle(x=0, y=0, width=0, height=0)`, fields `x`, `y`, `width`, `height`
- `Color(r=0, g=0, b=0, a=255)`, fields `r`, `g`, `b`, `a`
- `Camera2D(offset=Vector2(), target=Vector2(), rotation=0, zoom=1)`, fields
  `offset`, `target`, `rotation`, `zoom`
- `Camera3D(position, target, up=Vector3(0, 0, 1), fovy=45, projection=CAMERA_PERSPECTIVE)`, fields
  `position`, `target`, `up`, `fovy`, `projection` (`engine.CAMERA_PERSPECTIVE`: `fovy` in degrees;
  `engine.CAMERA_ORTHOGRAPHIC`: `fovy` = view height in world units). World space, z = height up.
- `Texture2D`, read-only `width`, `height`, `id` (no Python constructor)
- `Font`, read-only `base_size` (no Python constructor)
- `Sound`, `Music`, `Shader`, opaque handles, no Python constructor.
- `RenderTexture2D`, read-only `id` and `texture` (a `Texture2D`), no Python constructor.
  `Music` also gets a read/write `looping` and a read-only `time_length`.

`Vector2`/`Vector3`/`Rectangle`/`Color`/`Camera2D` must be default-constructible
and copyable from Python. Because `GameActor::WorldPos` is exposed with
`def_readwrite`, `actor.world_pos.x += 5` mutates the actor in place, verify
this works.

Named colors as module constants (raylib names, uppercase):
`WHITE BLACK GRAY DARKGRAY LIGHTGRAY RED MAROON GREEN LIME DARKGREEN BLUE
DARKBLUE SKYBLUE PURPLE VIOLET PINK YELLOW GOLD ORANGE BROWN BEIGE MAGENTA
BLANK RAYWHITE`.

Key constants as module `int` attributes (raylib values), covering:
`KEY_A`…`KEY_Z`, `KEY_ZERO`…`KEY_NINE`, `KEY_SPACE`, `KEY_ENTER`, `KEY_ESCAPE`,
`KEY_TAB`, `KEY_BACKSPACE`, `KEY_DELETE`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_UP`,
`KEY_DOWN`, `KEY_LEFT_SHIFT`, `KEY_RIGHT_SHIFT`, `KEY_LEFT_CONTROL`,
`KEY_RIGHT_CONTROL`, `KEY_LEFT_ALT`, `KEY_MINUS`, `KEY_EQUAL`, `KEY_KP_ADD`,
`KEY_KP_SUBTRACT`, `KEY_F1`…`KEY_F12`.
Mouse: `MOUSE_BUTTON_LEFT`, `MOUSE_BUTTON_RIGHT`, `MOUSE_BUTTON_MIDDLE`.
Gamepad: `GAMEPAD_BUTTON_LEFT_FACE_UP/DOWN/LEFT/RIGHT`,
`GAMEPAD_BUTTON_RIGHT_FACE_UP/DOWN/LEFT/RIGHT`,
`GAMEPAD_AXIS_LEFT_X`, `GAMEPAD_AXIS_LEFT_Y`, `GAMEPAD_AXIS_RIGHT_X`,
`GAMEPAD_AXIS_RIGHT_Y`.

### 12.2 `GameActor`

```text
actor.id             -> int          (read-only)
actor.name           -> str          (read-only)
actor.script         -> str          (read-only, "" for native actors)
actor.world_pos      -> Vector3      (read/write, mutable in place)
actor.active         -> bool         (read/write)
actor.visible        -> bool         (read/write)
actor.persistent     -> bool         (read/write)
actor.screen_space   -> bool         (read/write; True = draw as HUD, ignores camera)
actor.pending_destroy-> bool         (read-only)
actor.state          -> dict         (read-only attribute, mutable dict)
actor.destroy()      -> None
repr(actor)          -> "<GameActor 3 'Player'>"
# once the actor is removed: every attribute but id raises ReferenceError; repr still works
```

**Actor reference lifetime.** A dead actor held from Python (a module global,
another actor's `state`, a closure; `UICanvas` included) raises `ReferenceError` on
every attribute; only `id` and `repr` still work. Store the **id** and re-resolve it
each frame with `engine.find_actor_by_id(id)`, which returns `None` once the actor is
gone. The parked actor is freed once Python lets go of it.

### 12.3 Engine functions

```text
engine.spawn_actor(name="Actor", script="", x=0.0, y=0.0, z=0.0,
                   persistent=False, screen_space=False) -> GameActor
engine.find_actor(name) -> GameActor | None
engine.find_actor_by_id(id) -> GameActor | None
engine.all_actors() -> list[GameActor]          # every live actor, spawn order
engine.change_scene(script_path) -> None        # deferred to end of frame
engine.toggle_fullscreen() -> None              # same borderless-windowed toggle as Alt+Enter
engine.is_fullscreen() -> bool                  # True while that borderless-windowed mode is on
engine.set_vsync(interval) -> None              # 0 off, 1 every refresh, 2 every second refresh; while on, the
                                                # frame limiter only guards at twice the paced rate
engine.set_target_fps(fps) -> None              # the frame limiter's rate, 0 unlimited (project.json "target_fps" at start); VSync wins while on
engine.get_monitor_refresh_rate() -> int        # refresh rate (Hz) of the monitor the window is on, 0 if unknown
engine.is_window_focused() -> bool              # False while another window has the keyboard focus (Alt+Tab)
engine.quit() -> None                           # requests app exit; takes effect at the end of Run()'s current frame
engine.set_close_intercept(enabled) -> None     # on: window close button / Alt+F4 no longer exit, they raise a close request
engine.take_close_request() -> bool             # True once per intercepted close; the game answers with engine.quit(). A second close while one is untaken exits anyway
engine.take_screenshot(path) -> None            # saves the next finished frame (world + HUD) as an image file

engine.profile_begin(name) -> None              # frame profiler: a section inside the running stage/hook
engine.profile_end() -> None                    # closes the innermost section this script opened
engine.profile_count(counter, n=1.0) -> None    # adds n to a counter of the innermost open section
engine.get_profile() -> list[tuple[str, float, float, float]]   # (name, avg_ms, max_ms, calls_per_frame)
engine.set_gpu_profiling(enabled) -> bool       # GPU time per section as a "gpu/..." tree (Profiler.h); False if unsupported

engine.set_camera(camera, vertical_scale=1.0) -> None   # Camera2D; vertical_scale: see below
engine.set_camera_3d(camera) -> None            # Camera3D: the world pass in 3D (see below); replaces set_camera's
engine.clear_camera() -> None
engine.set_background_color(color) -> None

engine.load_texture(rel_path) -> Texture2D | None
Texture2D.set_smooth(smooth=True) -> None       # bilinear filtering (a ground picture seen close); textures load point-filtered
engine.load_font(rel_path, font_size=32, glyph_text="", pixel=False) -> Font | None
    # pixel: a pixel font on its grid at font_size (ascender - descender in px); UI text then draws it only at
    # whole multiples of its native sizes (point-filtered, sharp)
engine.add_pixel_font_size(font, rel_path, font_size) -> bool   # another native size (file) of a pixel font
engine.load_shader(vs_rel_path, fs_rel_path) -> Shader | None   # a line `#include "name.glsl"` is replaced by that file
    # beside the shader (one level, through the resource store; a missing one fails the load)
engine.load_render_texture(name, width, height) -> RenderTexture2D | None
engine.load_sprite_stack(layer_paths) -> SpriteStack | None   # layers bottom first, packed into a shared texture page
engine.begin_texture_mode(target) -> bool       # False = refused (inside Drawing, already open, invalid)
engine.end_texture_mode() -> None
engine.set_postprocess_shader(shader, strength=1.0, radial_warp=0.0, world_only=False, text_mask=False) -> None  # Shader | None; None disables it

engine.project_path(rel_path="") -> str         # absolute path, for read-only project resolution
engine.user_data_path(subdir="") -> str         # per-user writable dir for saves/config/logs (§18.5)
engine.run_path(rel_path="") -> str             # this run's own folder: logs/, agentio/ (§18.5)
engine.is_shipping() -> bool                    # true in a packaged shipping build (§18.5)
```

An active postprocess shader receives the full world and screen-space UI image as
`texture0`, drawn through a screen-sized render texture; with `world_only` it receives the world
pass alone and the screen-space pass (the HUD) is drawn after it, unchanged (and `radial_warp`
then never moves UI hit tests). With `text_mask` the screen-space pass keeps `texture0`'s alpha at 1
except under text (`draw_text`, `draw_text_ex`, IYIUI text), where it drops by the glyph's coverage
(0 = fully text, also under later draws only partly covering it), so a shader can leave text out of
an effect; `uTextMask` (float) is 1 then, else 0 and the alpha means nothing. The engine supplies optional
`uTime` (float, seconds), `uResolution` (vec2, window pixels), and `uStrength`
(float, clamped to 0..1) uniforms when the shader declares them. `radial_warp` is an
optional 0..0.25 coefficient supplied as `uRadialWarp`; a shader sampling at
`centered * (1 + uRadialWarp * dot(centered, centered))` must pass the same value
so UI pointer hit tests use the matching source coordinate. The render texture
is recreated on window resize and is released when the effect is disabled or the
application exits. With no shader, drawing uses the original direct path. With a 3D camera
(`set_camera_3d`) the target has a depth texture (made only while a 3D camera is active; a 2D game's
target has no depth buffer, and the target is remade once when that changes), given to a shader that declares them as
`uDepth` (sampler2D, the world pass's 0..1 depth) and `uView` (vec4: near, far, 1 when
orthographic, distance from the camera to its target); without a 3D camera `uView` is all 0.

`load_render_texture` returns an engine-owned offscreen target, cached like fonts under
the key `name#WxH` (the same name at another size is a second target) and freed with the
other assets at shutdown. Fill it between `begin_texture_mode(target)` and
`end_texture_mode()` from `Start`/`Update`/`SceneStart`, then draw `target.texture` with
any texture call (use a negative source height: render textures are stored upside
down). `begin_texture_mode` is refused (returns `False`, warns once, category `render`)
inside `Drawing`, because raylib's texture mode would drop the `Camera2D` transform and
the postprocess target for the rest of the frame; a mode still open after `Update` is
closed before the frame draws. `end_texture_mode` without a matching begin is a no-op.

`set_camera`'s `vertical_scale` squashes the world pass along screen-vertical around
the camera offset, after rotation and zoom: a camera pitched down from the horizon by
angle a uses `sin(a)`. `1.0` (the default) takes the original `BeginMode2D` path
unchanged; it must be finite and > 0. Pass the same value to `get_world_to_screen_2d` /
`get_screen_to_world_2d`, which with `1.0` match raylib's own functions exactly.

`set_camera_3d` draws the world pass through a 3D camera given in world space with z = height
up: x, y are the ground and y runs down the screen as in the 2D view (that space is mirrored
against raylib's, so the engine draws through raylib's camera with z negated: `WorldCameraToGl`).
Depth testing is on and face culling off for the pass; the depth range follows the camera's
distance to its target. 2D draws in the pass land at z = 0. `get_screen_to_world_ray(position,
camera)` returns the ray `(origin, direction)` under a screen point in the same world space, and
`get_world_to_screen(position, camera)` the screen point of a world point (`None` behind the camera).
`StackLighting.draw_glow` turns the depth test off while it draws, so the glow lies over
everything as in the 2D view. `begin_mode_2d(camera)` / `end_mode_2d()` forward raylib's 2D mode for drawing world-space
things into a render texture between `begin_texture_mode` / `end_texture_mode` (not in `Drawing`).

`engine.CellTerrain(columns, rows, cell, origin, corners, water=[])` (`CellTerrain.h`) is ground of square
cells, cell (0, 0)'s top-left corner at world `origin`, each with its own height at its four
corners (`corners`: 4 per cell, row by row, top-left, top-right, bottom-right, bottom-left), so
cliffs between cells are exact. `water` (optional, one flag per cell in the same order) marks
water cells: a wall between two of them is a waterfall (its uv's x is 1 plus the wall's height),
which the terrain shader draws as falling water (streaks and foam moving with `set_wind`'s clock)
where the stream crosses it; flat water itself
is the game's, drawn into the ground picture (library `sprite_stack.water`). `.height(x, y)` interpolates the cell under a world point (0 off
the grid). `.draw(texture, area, wall, upside_down=True)` draws, with a 3D camera, the cells under
world rectangle `area`: their tops with `texture` laid over `area` by world x, y (a ground picture
drawn top-down, e.g. into a render texture), and a wall in colour `wall` wherever neighbouring
cells do not meet; `.draw` sends those cells' quads every call. `StackLighting.draw_terrain` and the
sun map draw it instead from GPU meshes it builds on first use (chunks of 32 x 32 cells, those
overlapping the area; the terrain shader maps the tops' world x, y onto `area` by its `uTerrainUv`),
so their cost no longer grows with the cells in view. A shader with a `uTerrainHeights` sampler
also gets the corner heights as a float texture (one RGBA texel per cell, on texture unit 15;
`uTerrainGrid` = origin x, y, cell, and w = 1 only for a mesh chunk where a corner rises at least a
unit above its neighbour's beyond the slope, else 0): the library's `terrain.fs` shades a cliff's foot
per pixel from it (no extra geometry), and skips that work where w is 0. Given as `ground` to `StackLighting.draw_stacks` or `Particles.draw` (with
`z_up=True`, a 3D camera's pass: layers at their real height instead of lifted toward screen-up),
it raises each item by the ground under its pivot (and with a `sun_map`, `StackLighting.render`'s `ground` is drawn into the sun's depth map:
see `StackLighting.h`), so items keep z as their height above their
ground, which is what the shadow, contact and light passes read.

`engine.NavPlanner(columns, rows, cell, origin)` (`Nav.h`) answers route searches on up to 4 search
threads (hardware threads less 3), so replanning many units never stalls the game or render side. It holds a passability grid
(per cell a base byte, e.g. geo_world's bits, and an extra byte of 8 dynamic bits such as a wreck
or debris: `.set_base(bytes)`, `.set_base_cells(cells, bytes)`, `.set_extra(cells, bit, on)`,
`.cell(i)` -> `(base, extra)`) and a road graph (`.set_graph(nodes, edges)`, nodes `(x, y)`, edges
`(a, b, length, kind, oneway, width)`; `.set_edge(edge, open, clear, hazard, delay)` closes, narrows,
endangers or slows one). `.find_grid(start, goal, costs, extra_costs, clearance=0, max_nodes=500000)`
is an 8-neighbour A* without corner cutting: a cell costs `costs[base] * product of extra_costs[bit]`
over its set extra bits (0 = closed), `clearance` cells of room are kept, and the path is string
pulled. `.find_graph(starts, goals, kind_costs, width=0, hazard_weight=1, max_nodes=200000)` is A*
from any start node to any goal node over open edges at least `width` wide, priced `length *
kind_costs[kind] * (1 + hazard * hazard_weight) + delay`. Both return a ticket; the search runs on a
snapshot of the grid and graph taken when it was asked, so later edits never race it.
`.poll(wait=False)` returns the finished results in ticket order (`wait` blocks, without the GIL,
until every request asked so far is done) as dicts `{ticket, found, cost, expanded, points, nodes,
edges}`; `.pending` counts the unanswered ones. `engine.Nav` (below) builds traffic, walkers and aircraft on it.

`engine.Nav(world, seed=1, wait=False)` (`NavSim.h`) is the dynamic navigation of a `geo_world` world dict: it loads the grid
and road graph into a planner, prices them per unit class (`engine.nav_classes()`; `nav_point_at(points, s)` is the point
s metres along a polyline) and reads the dict live (grid bits, conditions, `changes`), so a destroyed building or bridge closes
routes at the next `step`. Units are typed `engine.NavWalker`, `NavVehicle` and `NavHeli` in the dict-like views `nav.walkers`,
`.vehicles`, `.helis` (`len`, `in`, `[]`, `[]=`, `get`, `pop`, `keys`, `values`, `items`, iteration; insertion order; keys are
`str`). `.step(dt, focus=None)` runs it all once (`dt` capped at `Nav.MAX_DT`; units beyond `WALK_FAR` / `DRIVE_FAR` of `focus`
step every 4th frame; profiler sections `routes`, `vehicles`, `walkers`, `air`); `.update`, `.sync`, `.poll` are its parts.
Live (`wait=False`) `step` waits for the step in flight, applies the queued writes, publishes the result into the units the views
hold and starts the next step on its own thread: a unit read from Python is the last finished step, a field write changes it at once
and the live unit when the step ends, and every call that changes or asks the live state waits for the step first (`.join()` only
waits); `.worker_ms` is the last step's time there, `.counts` its unit counts. `NavVehicle.why` says what holds a vehicle.
Routes: `.request(start, goal, cls, mode, side, heading)` -> ticket, `.take_result(ticket)`. Map: `.passable`, `.cell_index`,
`.cell`, `.open_near`, `.snap`, `.path_closed`, `.edge`, `.edge_state`, `.landing_site`, `.set_obstacle` / `.clear_obstacle` /
.obstacle, `.report_hazard` / `.clear_hazard`. Units: `.spawn_walker`, `.walk`, `.gather`, `.wander`, `.spawn_vehicle`, `.drive`,
`.convoy`, `.disable`, `.park`, `.remove`, `.spawn_heli`, `.fly_to`. `.events()` drains the events (dicts with `type`, `time`),
`.emit` adds one. With `wait=True` every answer arrives on the next step and randomness is seeded per unit (`engine.PyRandom`),
so a run repeats exactly. The model, events and limits are in `libraries/nav/README.md`.

The living-world classes (`Life.h`, plain data, no game rules): `engine.Scheduler` (`at(id, time)`, `cancel`, `due(now, budget=64)`,
`items`) runs things now and then, earliest first; `engine.EventLog` (`limit`, `last`, `add(kind, x, y, time, cause=0, **data)` -> id,
`since(id)`, `near(x, y, radius, since=0, kind="")`, `get(id)`) records what happened and why; `engine.Facts` (`half_life`,
`tell(owner, kind, subject, x, y, time, confidence, source="")`, `near`, `about`, `forget`, `prune`, `all`) holds what each owner
believes, its confidence fading with time; `engine.Population(zones, nav, seed=1, walkers=400, cars=120)` keeps a town as numbers
per neighbourhood (`zones`: `engine.LifeZone` with `id`, `name`, `x`, `y`, `homes`, `people`, `markets`, `mosques`, `schools`,
`doors`, and read-write `fear`, `support`, `displaced`; built from a package's `zones.json`, geo_world `FORMAT.md`) plus a
pool of drawn people (`p0..`) and cars (`pc0..`) in `nav`. `.manage(nav, focus, hour)` -> `{out, want, cars, want_cars}`: calm
unpinned pool walkers farther than `Population.FAR` from the focus go indoors (state `hidden`) and come out at house doors within
`NEAR` of it (skipping doors with a vehicle within 15 m), as many as `.activity(zone, hour)` asks; pool cars leave the map far away and enter on roads near the edge of the view (not within 30 m of a vehicle). Event data (`EventLog.add(**data)`) may hold any JSON value.
`.summon(nav, n, x, y, radius=150)` brings pinned people (`.pin`, `.unpin`, `.pinned`; the pool leaves them alone), `.shock(x, y,
radius, fear)` frightens zones, `.step(hours)` moves the numbers, `.zone_at`, `.home(id)`, `.members`.

`engine.SightGrid()` (`Sight.h`) answers line of sight and rays over a terrain plus blockers, for
any game with heights and buildings. `.set_ground(columns, rows, cell, origin, heights)` takes
`(columns + 1) * (rows + 1)` corner heights (bilinear between them), `.set_blockers(columns, rows,
cell, origin, tops)` one top height per cell of its own grid (0 = nothing stands there; a building
cell holds the building's top), `.set_blocker_cells(cells, height)` changes some of them (a
collapse). Heights are any float32 buffer (`array('f')`, `bytes`). `.ground(x, y)`, `.top(x, y)`
(the higher of the two), `.visible(a, b)` (`Vector3`s; the last half metre before `b` is skipped, so
a target beside a wall still counts), `.set_foliage(lows, highs)` (per blocker cell, the absolute
height band of leaves over it; `highs` below `lows` = none) and `.foliage(a, b)` -> how many metres
of leaves the line crosses, or None when it is blocked (leaves conceal, the game decides how much),
`.ray(origin, direction, max_distance=100000)` -> the first
hit on the ground or a roof, or None (the cursor's world point), and `.visible_pairs(observers
[(x, y, z, range)], targets [(x, y, z)])` -> `[(observer, target, distance)]` for every visible pair
in range. It is plain C++ in `IYIEngineSim` (§2b), so a game's native module uses the same class
directly. Library `geo_world` builds one from a map package (`world.sight_grid`).

`set_shader_value` / `set_shader_texture` flush raylib's draw batch before setting the
uniform, so draws already submitted keep the values they were made with. A flush also
releases raylib's extra sampler slots, so set textures last: after `begin_shader_mode`
and every `set_shader_value`, right before the draws that sample them. Unknown uniform names are ignored.

> [!WARNING]
> **Audio API Removal (Breaking Change)**: The legacy flat audio functions (`engine.load_sound`, `engine.load_music`, `engine.play_sound`, `engine.stop_sound`, `engine.play_music`, `engine.stop_music`, `engine.pause_music`, `engine.resume_music`, `engine.set_master_volume`) have been **removed**. Audio is now managed exclusively through the `engine.audio` subsystem (§20).

`take_screenshot` reads the frame back right before `EndDrawing` swaps it (after the postprocess
pass, HUD included; the draw batch is flushed first so the last screen-space draws are in it)
and writes it with raylib's `ExportImage`; the extension picks the format.
`path` is used as given: pass an absolute path under `user_data_path()` (a shipping game never
writes into its install folder). The result is logged under `render`. Only the last request in a
frame is kept.

The frame profiler times CPU sections every frame and publishes one-second averages. Each thread
role keeps its own tree (`ProfileSetThread`), rooted at `game` and `render`; with the two roles on
two threads their times overlap, so the roots are never added up as one frame. The engine's own
sections are the frame stages, in order: under `game` `hot_reload`, `audio`, `start`,
`fixed_update`, `update`, `agent` and `scene`; under `render` `draw` (children `world`, `screen`)
and `present` (GPU submit, buffer swap and the frame-rate wait, so it absorbs idle time). Inside a
stage every actor hook that runs is a section named by its script path
(`game/update/scripts/actors/world.py`), UI canvases add `ui`, and a script nests its own sections
with `profile_begin(name)` / `profile_end()` (`game/update/scripts/actors/world.py/sim`). Engine C++ code times its own work with
`ProfileScope` wherever it is worth seeing, nested under whatever is open: every
`draw_sprite_stacks` call is a `sprite_stacks` section with `read` (tuples to C++), `cull`, `sort`
and `emit` (the quads), and a 3D `StackLighting` pass adds `instanced` (`stacks`, `draws`, `quads`) with `chunks` (static lists, below; count: chunks in view), under the hook or script section that drew. A section left open is closed
when its hook returns; a script can never close the engine's sections. `profile_count(counter, n)`
(C++ `ProfileCount`) adds to a named counter of the innermost open section, reported as a mean per
frame beside its time (agent channel `profile`, `--profile`): what a cost was spent on. `get_profile()` returns the last
window in tree order (each section followed by its children), the `game` tree first, then `render`
and its `gpu/render/...` rows (GPU marks are placed only in the render tree, the GL thread);
sections that did not run in it are absent. It is data only (rule 9): the `profiler_overlay` library draws it (F3), the
agent channel and `--profile` report it. Meant for per-frame stages, not inner loops.

`load_sprite_stack` returns an engine-owned `SpriteStack` cached per path list (see §12.4).

Note: only `actor.destroy()` destroys an actor, there is no
`engine.destroy_actor`. See §18 for `engine.user_data_path` and `engine.is_shipping`
contracts.

`engine.play` (Google Play: achievements, Saved Games, review, licence state) is in §24.5.

### 12.4 Thin raylib forwards

Drawing:

```text
engine.draw_texture(texture, x, y, tint=WHITE)
engine.draw_texture_v(texture, position, tint=WHITE)
engine.draw_texture_rec(texture, source, position, tint=WHITE)
engine.draw_texture_pro(texture, source, dest, origin, rotation, tint=WHITE)
engine.clear_background(color)
engine.begin_shader_mode(shader) / engine.end_shader_mode()   # pair them in the same hook
engine.set_shader_value(shader, name, value)      # value: list of 1-4 floats -> float/vec2/vec3/vec4
engine.set_shader_texture(shader, name, texture)  # extra sampler; set last, inside begin_shader_mode
engine.draw_sprite_stacks(items, view_rotation=0.0, height_scale=1.0, layer_height=1.0,
                          offset=Vector2(0, 0), tint=None, max_z=inf, sort=True,
                          ground_in_normal=False, ground_max_z=inf)
engine.SpriteStackList(items)                     # item tuples read once; an entry of draw_sprite_stacks' items
    .set(index, item)                             # replace one item (a building that changed); draws see it next call
engine.set_sprite_stack_culling(enabled)          # on by default; off only to measure what culling saves
engine.set_sprite_stack_instancing(enabled)       # on by default; off sends every stack down the per-quad path (A/B)
engine.StackLighting(light_sprites, ground_half, shadow_scale, contact_scale, light_half, light_scale,
                     contact_strength, shadow_fs, contact_fs, light_fs, sprite, blur,
                     wet=None, glow=None, sun_map=0, sun_depth=None, terrain=None, contact_map=0, light_map=0, mark_map=0, marks=None)   # stack lighting (below)
    .render(items, layer_height, sun, ambient, lamps, lights, static=None, ground=None, sun_area=None)   # Update only
    .draw_terrain(terrain, texture, area, wall, water=None, water_area=None)   # 3D camera: CellTerrain.draw lit (sun map, clouds, contact, wall lights); water: a sprite_stack.water map over world rect water_area, drawn per pixel
    .draw_ground() / .draw_ground_light() / .draw_stacks(items, view_rotation, height_scale, layer_height,
                                                         z_up=False, ground=None)
    .set_marks(marks, replace)                       # ground marks: [(kind, x, y, length, width, rotation, strength)]
    .draw_silhouettes(items, view_rotation, layer_height, opacity, ground=None)   # after draw_stacks, 3D camera
    .wash = [(x, y, radius, strength)]               # up to 4 rotor downwash points
    .draw_wet(patches, wetness, view_rotation, sky, eye=None) / .draw_glow(strength)   # world pass
    .static_sun                                   # "smooth" | "step" | "exact"
    .shadow_tint / .clouds / .height_shade / .wind   # the look (below)
engine.Particles(capacity=4096)                   # soft puffs in the world (below)
    .emit(x, y, z, vx, vy, vz, life, size, size_end, tint, drag=0.0, lift=0.0)
    .update(dt) / .draw(view_rotation=0.0, height_scale=0.0, z_up=False, ground=None) / .clear() / len()
engine.CellTerrain(columns, rows, cell, origin, corners, water=[])   # terrain for a 3D camera (above)
    .height(x, y) / .draw(texture, area, wall, upside_down=True)
engine.draw_rectangle(x, y, width, height, color)
engine.draw_rectangle_rec(rec, color)
engine.draw_rectangle_lines(x, y, width, height, color)
engine.draw_rectangle_lines_ex(rec, thickness, color)
engine.draw_circle(center_x, center_y, radius, color)
engine.draw_circle_v(center, radius, color)
engine.draw_sphere(center, radius, color)            # Vector3; with set_camera_3d (world z up)
engine.draw_billboards(texture, camera, centers, size, tint=WHITE)  # list[Vector3], camera-facing textured quads in 3D
engine.draw_circle_lines(center_x, center_y, radius, color)
engine.draw_line(start_x, start_y, end_x, end_y, color)
engine.draw_line_ex(start, end, thickness, color)
engine.draw_rectangle_pro(rec, origin, rotation, color)
engine.draw_rectangle_rounded(rec, roundness, segments, color)
engine.draw_rectangle_gradient_v(rec, top, bottom)
engine.draw_rectangle_gradient_h(rec, left, right)
engine.draw_rectangle_gradient_ex(rec, top_left, bottom_left, bottom_right, top_right)   # corners counter-clockwise
engine.draw_circle_gradient(center, radius, inner, outer)
engine.draw_triangle(v1, v2, v3, color)
engine.draw_triangle_fan(points, color)              # points: list[Vector2]
engine.draw_triangle_strip(points, color)
engine.draw_poly(center, sides, radius, rotation, color)
engine.draw_circle_sector(center, radius, start_angle, end_angle, segments, color)
engine.draw_ring(center, inner_radius, outer_radius, start_angle, end_angle, segments, color)
engine.draw_line_strip(points, color)
engine.draw_spline_linear(points, thick, color)    # Vector2s or (x, y) pairs, tuples or lists (faster); mitered joints
engine.draw_text(text, x, y, font_size, color)
engine.draw_text_ex(font, text, position, font_size, spacing, color)
engine.measure_text(text, font_size) -> int
engine.measure_text_ex(font, text, font_size, spacing) -> Vector2   # draw_text_ex's size; IYIUI text uses spacing 1.0
```

The shape forwards keep raylib's own rules: triangles, and the first triangle of a strip or
fan, must be counter-clockwise as seen on screen or raylib culls them; a strip needs at least
3 points, a spline or line strip at least 2 (fewer draws nothing).

raylib is built with `SUPPORT_SPLINE_MITERS`, so `draw_spline_linear` joins its segments with
miters: a curve drawn as a polyline keeps a clean outer edge. A turn sharper than about 120
degrees makes a long miter spike; draw such a corner as two polylines.

Sprite stacks are the one drawing call that is not a 1:1 raylib forward (`SpriteStack.h`). A
`SpriteStack` (from `engine.load_sprite_stack`) holds equally sized layers, bottom first, packed
into one block of a shared texture page (the AssetManager packs stacks into 8192 square pages,
256 MB each; one holds a whole town, so a list of many stacks switches texture rarely; a block larger than a
page gets its own texture); it has `texture`, `layers`, `width`, `height` (one layer, texels) and a
writable `origin`: the pivot as a `Vector3` in layer texels (`x`/`y`) and layers (`z`),
centre of the bottom layer by default, and a writable `layer_step` (default 1): layer heights per
layer, 2 for a stack loaded from every second layer file that keeps its full height (a far LOD).
Drawing skips layers on its own when they would land closer than `SpriteStackPass::LodPixels` (1)
pixels apart on the target (up to every `kMaxLodStep` = 8th; the top layer always draws), so a far
zoom costs fewer quads with no visible change. In the 3D passes of `StackLighting` (`draw_stacks`
with `z_up`, the sun map) opaque stacks go instanced instead (`DrawSpriteStacksInstanced`, the way
Unity or Unreal batch identical meshes): each stack's layers are a GPU mesh made on its first draw,
each item a small instance record, one draw per stack, and `lit_sprite.vs` does the placing, the
height shade, `build`, `lean`, `sway` and `damage`. The same LOD applies there: each mesh also
holds the layers of every 2nd, 4th and 8th step (the step rounded down to a power of two), and
instances are drawn per (stack, step). `damage` (0..1, the 11th tuple entry) crumbles a stack's
texels away in two-texel chunks, more of them higher up, and soots the rest; its sun shadow and
contact footprint keep the holes. It needs the instanced path (the per-quad path ignores it).
The game still gives one item per object, so a building that changes is one `SpriteStackList.set`.
There a `SpriteStackList` is a static index kept on the render side between frames: the list goes
over once, then only its `set` edits (more than a quarter of it plus 64 resend it whole); its items
sit in at most 64 x 64 chunks over the list's extent, each item's box with room for its lean and
sway, and their instance records are made once. A frame tests the chunks against the view; in a
chunk only partly in view each item is tested on its own box, and every item's LOD step is taken at
its pivot, so it draws what per-item culling would within a few percent. An edit moves one item
(chunk bounds only grow); a new ground or layer height rebuilds the index. Items with a see-through tint, and any pass whose shader
lacks the instance inputs, take the per-quad path below. `draw_sprite_stacks` draws a whole list per call, every
layer a quad in raylib's batch exactly as `DrawTexturePro` emits it, so Python decides *what* to
draw and C++ does the per-layer work. `items` are tuples `(stack, x, y, z, rotation, tint)`,
optionally followed by `scale`, `lean`, `build`, `sway` and `damage`: `x`/`y` place the pivot, `z` is its height above
the ground in world units, rotation and scale act around the pivot. Layer `i` is lifted toward
screen-up by `(z + (i - origin.z) * layer_height * layer_step * scale) * height_scale`: pass the
`Camera2D` rotation as `view_rotation` and `cot(pitch)` of a tilted camera as `height_scale`
(0 flattens every layer onto the footprint). `lean` adds degrees to `view_rotation` for one
stack. `build` (0..1, default 1) raises a stack layer by layer: bottom first, a few layers in
flight at once, each fading in as it settles from three layers above its place (`BuildLayer`).
`sway` is how many degrees a stack leans at full wind; only `StackLighting.draw_stacks` applies
wind (its `wind`), so shadow passes never sway. With `z_up` (`StackLighting.draw_stacks`, a 3D
camera) `height_scale` is not used: layers stand at their real height, `lean` and `sway` tilt the
stack in the world (lean toward screen-left by `view_rotation`, sway downwind) and the sort goes by
the camera's depth. With `sort` the list is drawn back to front for that view (ground distance
along the view and height, weighted by the pitch `height_scale` implies). For offscreen passes:
`offset` moves everything (a world-anchored buffer's origin), `tint` overrides every item's
tint (a shadow pass), `max_z` skips items above that height (footprints of grounded things), and
`sort=False` skips the sort. Every call culls: an item none of whose quads can land in the render
target (the screen through the camera in the world pass, the buffer in texture mode) is skipped
before sorting, tested on its footprint at the bottom and the top layer through rlgl's model-view
and projection, so games pass whole lists and never cull in Python. `engine.SpriteStackList(items)`
reads a list of item tuples once; placed in `items` as one entry, its items are copied in without
converting them again (stacks that rarely change: buildings, trees). With `ground_in_normal` every vertex carries the ground point
under it (world x, y without lift or offset) in its normal as `normalize(x, y, 1024)`, so a
vertex shader recovers it as `vertexNormal.xy / vertexNormal.z * 1024` and can light each texel
at its own ground position with the whole list still one batch; items above `ground_max_z`
get normal `(0, 0, -1)` instead ("no ground under me"). With one layer, `view_rotation=0`
and `height_scale` 1 a stack draws exactly what `draw_texture_pro` draws for the same source,
scaled dest, origin and rotation. `engine.StackLighting` (`StackLighting.h`) lights stacks: sun
shadows, contact shadows and a light map in world-fixed render textures filled by `render` in
Update, composited on the ground by `draw_ground` / `draw_ground_light` and applied per texel by
`draw_stacks` in the world pass. `static = (key, items, ids, lights)` gives the stacks that never
move apart (read only when `key` changes): their footprints are cached, their sun shadows redrawn
only as the sun moves (`static_sun`), and a change of a few ids patches only round them.
`ambient` is a level or an `(r, g, b)` colour (0..1; all 1 = daylight, no light map) and a light
may end with an `(r, g, b)` colour. The look, set any time: `shadow_tint` (a `Color` shadows and
contact occlusion blend toward), `clouds = (cover, strength, offset_x, offset_y, scale)` (value
noise in world space joining the sun shadows on the ground and shading the stacks, scaled by the
sun), `height_shade = (strength, reach)` (stack layers darkened toward the ground) and
`wind = (time, direction_degrees, strength)` (for items with `sway`). `draw_wet` draws
`(texture, source, dest)` patches as wet ground (darker, reflecting the light map in streaks
toward screen-down, a little `sky` by day; `eye`, a perspective camera's position, makes each
streak run along its own view ray); `draw_glow` adds the night's light map once more,
blurred, over the finished world pass (bloom without a screen pass). The `sprite_stack` library's
`fx` wraps it with the shaders it ships, and its `post` sets the world post shader.

`engine.Particles` (`Particles.h`) is a pool of soft round puffs simulated on the CPU: `emit`
one at a time (position, velocity, life in seconds, diameter at birth and death, tint whose alpha
is the most opaque it gets, `drag` slowing it, `lift` added to its vertical speed per second),
`update(dt)` once a frame (dead ones are dropped; none sinks below the ground; a full pool replaces
its oldest), `draw` in the world pass with the camera's rotation and `cot(pitch)` like the stacks:
each puff is a camera-facing disc lifted by its height. It is drawn over whatever came before it;
with `z_up` (a 3D camera) at its real height, facing the camera by its own right and up (the
rotation and pitch are then not used), depth tested but writing no depth.

With `contact_map` > 0 as well (and a ground and `sun_area` each frame), the contact buffers are
that many texels square over the camera's area instead of +-ground_half, so contact is sharp up
close: the area is a square in sqrt(2) steps of size on a grid of an eighth of it, and the static
footprints are redrawn (instanced, looking straight down) only when it steps; while nothing moves
and the static set holds, the contact is not redrawn at all. The sun map and the contact fade out
toward their edge, so a `sun_area` cut short (a shadow distance) ends softly; the sun map is padded
so its fade falls just past `sun_area`, never inside it. `light_map` > 0
puts the light map over the same camera area at that many texels (readers past its edge get its
edge texel). `fog = (colour, start, end)` hazes the lit stacks and terrain with their distance from
the eye (colour alpha: the most it covers; end <= start: none). `mark_map` > 0 with `marks` (the
library's marks.fs) keeps ground marks in a buffer of that many texels over the same camera area,
which `draw_terrain` lays over the ground picture: `set_marks(marks, replace)` replaces the list or
adds to it (kind 0 crater, 1 tyre track, 2 rubble, 3 blood, painted procedurally); the buffer is
repainted whole when the area steps or the list is replaced, otherwise only new marks are painted.
`draw_silhouettes` draws items flat in their tint only where something stands more than 3 world
units in front of them (each vertex pulled that far toward the eye, depth test GREATER, no depth
write), so units behind a building show through it but never over themselves. `wash` makes swaying
instanced stacks (trees, grass) lean away from up to 4 points, with the rotor's gusts.

With a sun map and a ground, `render` also keeps each contact footprint's ground height, and
`draw_terrain` draws the contact halo on the terrain only where it is about that high (a halo
stops at a cliff; `draw_ground` then skips contact); at the cliff itself a wall takes the same
shade at its own foot, fading up within its lowest 8 world units above the ground it meets, and
the ground strip below the wall fades out from it over about 9 world units; by night it lights
the cliff walls by the light map on their lower side. The flat sun buffers are not made when a
sun map is. The sun map's sun moves in 0.25 degree steps of bearing and height: between steps its
projection holds and the static casters' depth is copied from a cache instead of drawn again.

Input:

```text
# All input reads answer from this frame's FrameInput copy (§8a).
engine.is_key_down(key) / is_key_up(key) / is_key_pressed(key) / is_key_released(key)
engine.is_ime_composing() -> bool   # live Windows IME composition, False on other platforms
engine.get_pinch_zoom() -> float    # two-finger zoom factor this frame, 1 when none; the mouse is the fingers' midpoint
engine.get_key_pressed() -> int
engine.is_mouse_button_down(button) / is_mouse_button_pressed(button)
engine.is_mouse_button_released(button)
engine.get_mouse_position() -> Vector2
engine.get_mouse_delta() -> Vector2
engine.get_mouse_wheel_move() -> float
engine.get_mouse_wheel_move_v() -> Vector2   # x: sideways (touchpad), y: wheel
engine.is_gamepad_available(gamepad) -> bool
engine.is_gamepad_button_down(gamepad, button) -> bool
engine.is_gamepad_button_pressed(gamepad, button) -> bool
engine.get_gamepad_axis_movement(gamepad, axis) -> float
```

Collision (raylib only, no custom collision code anywhere):

```text
engine.check_collision_recs(rec1, rec2) -> bool
engine.check_collision_circles(c1, r1, c2, r2) -> bool
engine.check_collision_circle_rec(center, radius, rec) -> bool
engine.check_collision_point_rec(point, rec) -> bool
engine.check_collision_point_circle(point, center, radius) -> bool
engine.get_collision_rec(rec1, rec2) -> Rectangle
```

Time / screen / camera math:

```text
engine.get_frame_time() -> float
engine.get_time() -> float                       # this frame's clock, seconds; --fixed-dt: game time (§3)
engine.set_clipboard_text(text: str) -> None    # UTF-8, raylib SetClipboardText (a window request, §8a)
engine.get_clipboard_text() -> str              # UTF-8; only in a frame where Ctrl/Cmd+V was pressed, else ""
engine.set_display_cutout(use: bool) -> None    # mobile: draw under the notch/cutout; no-op on desktop
engine.get_fps() -> int
engine.get_screen_width() -> int
engine.get_screen_height() -> int
engine.get_world_to_screen_2d(position, camera, vertical_scale=1.0) -> Vector2
engine.get_screen_to_world_2d(position, camera, vertical_scale=1.0) -> Vector2
engine.get_screen_to_world_ray(position, camera) -> tuple[Vector3, Vector3]   # Camera3D: (origin, direction)
engine.get_world_to_screen(position, camera) -> Vector2 | None   # Camera3D, world z up; None behind it
engine.begin_mode_2d(camera) / engine.end_mode_2d()   # Camera2D, inside texture mode only
engine.set_window_size(width, height) -> None   # windowed: also recentres it; the getters follow later (§8a)
engine.set_window_title(title) -> None
engine.set_window_icon(rel_path, frame=0, frame_count=1) -> bool   # frame of a horizontal strip; call per frame change to animate
engine.get_monitor_resolutions() -> list[tuple[int, int]]
```

Nothing beyond this list in v1.
