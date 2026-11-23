"""Lighting for sprite stacks drawn with engine.draw_sprite_stacks: sun shadows, contact shadows and a
light map, plus the camera pass that lights every stack texel at its own ground point.

The work is the engine's, in C++ (engine.StackLighting, IYIEngine StackLighting.h, where the passes,
the static cache and its patches are described) and the library's (libraries/sprite_stack/src/Fx.h:
shaders, light sprites, ground detail, marks, shared with native games); this module keeps the
library's Python API and forwards to them (s["fx"] is the C++ Fx). One actor owns a state dict `s` (its actor.state):
    Start:    fx.load(s, lights)                  {name: SpriteStack}: one-layer light sprites, origin = pivot
                                                  (light_sprites(): the library's "radial" and "cone")
    Update:   fx.render(s, items, layer_height, sun, ambient, lamps, lights[, static])
    Drawing:  ground -> fx.draw_ground(s) -> decals lying on the ground -> fx.draw_ground_light(s)
              [-> fx.draw_wet(s, patches, wetness, view_rotation, sky)]
              -> fx.draw_stacks(s, items, view_rotation, height_scale, layer_height)
              [-> fx.draw_glow(s, strength)]
    Any time: the look (set_shadow_tint, set_clouds, set_height_shade, set_wind); defaults: black
              shadows, no clouds, no height shade, no wind.

items are the frame's draw_sprite_stacks items: tuples (stack, x, y, z, rotation, tint[, scale[, lean]])
and SpriteStackLists, optionally with (..., scale, lean, build, sway): build 0..1 raises a stack layer
by layer, sway is how far it leans in the wind (set_wind); sun, ambient and lamps come from
sprite_stack.daylight (ambient a level or an (r, g, b) colour); lights = [(name, x, y, size, rotation,
power[, (r, g, b)])] in world space ("radial": size = diameter, "cone": size = beam length), power
scaled by lamps. static = (key, items, ids, lights): the stacks that never move (items a
SpriteStackList or a tuple list, ids one int per item or None) and their lights, read only when key
changes; items and lights are then only the moving ones. How the static sun shadows follow the sun
(set_static_sun): "smooth" (default: snapshots a quarter texel apart, cross-faded), "step" (a texel
at a time, the cheapest), "exact" (redrawn every frame).
"""

from engine import sprite_stack as _c

STATIC_SUN_MODES = ("smooth", "step", "exact")


def load(s, lights, ground_half=640, light_half=1536, light_scale=0.5, contact_strength=0.6,
         shadow_scale=1.0, contact_scale=1.0, sun_map=0, contact_map=0, light_map=0, mark_map=0, light_tops=False):
    """ground_half: the shadow and contact buffers cover +-ground_half world units, at shadow_scale
    and contact_scale texels per unit; light_half / light_scale: the light map's reach and texels
    per unit (light pools are soft). sun_map > 0 (a 3D camera): sun shadows from a depth map of that
    many texels square, drawn from the sun over the camera's area (render's ground and sun_area),
    instead of the flat sun buffer; draw the terrain with draw_terrain then. contact_map > 0 (with
    sun_map): the contact buffers are that many texels square over the camera's area instead (sharp
    up close), redrawn as the camera moves a step; contact_scale is unused then. light_map > 0:
    the light map that many texels square over the same camera area (light_half, light_scale unused).
    mark_map > 0 (with sun_map): ground marks (set_marks) that many texels square over that area.
    light_tops: draw_terrain lights its tops with the night light map too (a ground picture that never
    had draw_ground_light drawn into it)."""
    s["fx"] = _c.Fx(lights, ground_half=ground_half, light_half=light_half, light_scale=light_scale,
                    contact_strength=contact_strength, shadow_scale=shadow_scale, contact_scale=contact_scale,
                    sun_map=sun_map, contact_map=contact_map, light_map=light_map, mark_map=mark_map, light_tops=light_tops)


def light_sprites():
    """The library's light sprites for load: "radial" (a round pool, pivot in its middle) and "cone"
    (a beam, pivot at its apex, shining toward +x at rotation 0)."""
    return _c.light_sprites()


def at(x, y, rotation, lx, ly):
    """World point of a spot given in an object's own unrotated space around its pivot (x, y)."""
    return _c.at(x, y, rotation, lx, ly)


def set_static_sun(s, mode):
    """How the static stacks' sun shadows follow the sun: one of STATIC_SUN_MODES."""
    s["fx"].lighting.static_sun = mode


def static_sun(s):
    return s["fx"].lighting.static_sun


def render(s, items, layer_height, sun, ambient, lamps, lights, static=None, ground=None, sun_area=None):
    """Offscreen passes. Call from Update only. With a sun map: ground (engine.CellTerrain) and
    sun_area (engine.Rectangle, the world area the camera sees) say what the sun map covers."""
    s["fx"].lighting.render(items, layer_height, tuple(sun), ambient, lamps, lights, static, ground, sun_area)


def draw_terrain(s, terrain, texture, area, wall, water=None, water_area=None):
    """World pass, with a 3D camera: the terrain (engine.CellTerrain) with `texture` laid over
    world rectangle `area` (a render texture), sun-map shadows, clouds and contact on it, walls in
    `wall` (lit by the light map at night). water, water_area: the map sprite_stack.water bakes and
    the world rectangle it covers (water.terrain_map), drawn per pixel on the ground."""
    s["fx"].lighting.draw_terrain(terrain, texture, area, wall, water, water_area)


def set_ground_detail(s, strength=0.15, wobble=0.5):
    """draw_terrain's tops get grain finer than the ground picture (strength: brightness change, 0 =
    off, the default) and the picture's edges wobble up to `wobble` world units, so a coarse picture
    does not show its texels up close. Needs a sun map (load)."""
    s["fx"].set_ground_detail(strength, wobble)


def draw_ground(s):
    """World pass, after the flat ground: contact shadows and sun shadows (with a sun map and a
    ground, neither: draw_terrain does both)."""
    s["fx"].lighting.draw_ground()


def draw_ground_light(s):
    """World pass, after everything flat on the ground and before the stacks."""
    s["fx"].lighting.draw_ground_light()


def draw_wet(s, patches, wetness, view_rotation, sky, eye=None):
    """World pass, after draw_ground_light: patches = [(texture, source, dest)] of paved ground (its
    alpha = how much is there) shown wet: darker, reflecting the night's lights in streaks toward the
    viewer and, by day, a little of the sky colour `sky` (engine.Color). wetness 0..1. eye: a
    perspective camera's position (engine.Vector3, z up), so each streak runs along its own view
    ray; None for a 2D or orthographic camera."""
    s["fx"].lighting.draw_wet(patches, wetness, view_rotation, sky, eye)


def draw_glow(s, strength):
    """World pass, after the stacks: the night's light pools added again, blurred wide, as a glow
    round the lights (bloom from the light map). strength 0..1; nothing by day."""
    s["fx"].lighting.draw_glow(strength)


def set_marks(s, marks, gen):
    """Ground marks on the terrain (load's mark_map): marks = [(kind 0 crater | 1 track | 2 rubble |
    3 blood, x, y, length, width, rotation, strength, ...)], a list that only grows while gen stays
    the same (geo_world w["marks"], w["marks_gen"]). Call every frame: only what changed is sent."""
    s["fx"].set_marks(marks, gen)


def set_wash(s, points):
    """Rotor downwash: up to 4 (x, y, radius, strength 0..1); swaying instanced stacks lean away."""
    s["fx"].lighting.wash = list(points)


def draw_silhouettes(s, items, view_rotation, layer_height, opacity=0.55, ground=None):
    """After draw_stacks (3D camera): items drawn flat in their tint only where something stands
    well in front of them, so units behind a building show through it."""
    s["fx"].lighting.draw_silhouettes(items, view_rotation, layer_height, opacity, ground)


def set_fog(s, colour, start, end):
    """Distance haze on the lit stacks and terrain (3D camera): engine.Color (alpha: the most it
    covers, daylight.haze), from start to end world units from the eye; end <= start turns it off."""
    s["fx"].lighting.fog = (colour, start, end)


def set_shadow_tint(s, colour):
    """engine.Color shadows and contact occlusion blend toward (daylight.shadow_tint)."""
    s["fx"].lighting.shadow_tint = colour


def set_clouds(s, cover, strength, offset, scale=900.0):
    """Cloud shade on the ground and stacks: cover 0 (clear) .. 1 (overcast), strength relative to
    a sun shadow, offset (x, y) how far the field has drifted (world units), scale the cloud size."""
    s["fx"].lighting.clouds = (cover, strength, offset[0], offset[1], scale)


def set_height_shade(s, strength, reach):
    """Darken stack layers toward the ground: strength at the foot, gone `reach` world units up."""
    s["fx"].lighting.height_shade = (strength, reach)


def set_wind(s, time, direction, strength):
    """Wind for stacks with sway: time in seconds (any clock), the world direction it blows toward
    in degrees, strength 0..1. Only the drawn stacks sway; their shadows stay put."""
    s["fx"].lighting.wind = (time, direction, strength)


def draw_stacks(s, items, view_rotation, height_scale, layer_height, z_up=False, ground=None):
    """World pass: the stacks, back to front; by night every texel is lit at its own ground point.
    z_up: a 3D camera (engine.set_camera_3d), which ignores height_scale: layers stand at their
    real height, lean and sway tilt them in the world, and the order is by the camera's depth;
    ground: an engine.CellTerrain that raises each stack by the ground under it (its z stays the
    height above that ground)."""
    s["fx"].lighting.draw_stacks(items, view_rotation, height_scale, layer_height, z_up, ground)
