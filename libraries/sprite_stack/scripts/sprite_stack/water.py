"""Flowing water as toon bands, drawn per pixel by terrain.fs (engine StackLighting.draw_terrain's
water map): flat colour bands by depth, a wobbling foam line along the shore, thin wave lines
drifting with the flow, chunky foam on rapids and below falls. Still water (no flow) keeps slow
waves, so a lake or a sea is the same map. Waterfalls down the terrain's walls are terrain.fs's too
(engine.CellTerrain's water flags).

The map holds one texel per grid cell: rg the flow (|f| <= 0.7, a foam cell's flow is stretched to
1, so foam needs no channel of its own), b the signed distance from the cell's centre to the shore
in cells (+ in water up to DEEP, - on land down to -LAND). terrain.fs blends 4x4 texels (Catmull-Rom
for the shore, which keeps a 1-cell stream 1 cell wide; a B-spline for the flow), so the shore is a
smooth curve, not cell squares. Into the ground picture water.draw lays
a flat sand beach under the water and just past its shore: terrain.fs reads the picture there as
lights, overlays and tool previews (picture / SAND) and tints the water with them.

The caller owns a state dict s:
    Start:   water.load(s)
    Update:  water.bake(s, key, cells, columns, rows, cliffs)   redrawn only when key changes; texture mode,
             so outside the ground picture's own pass
             then inside the ground picture's 2D camera, before the roads: water.draw(s, origin, cell)
    Drawing: fx.draw_terrain(s, terrain, picture, area, wall, *water.terrain_map(s, origin, cell))
cells = {(cx, cy): (fx, fy, foam)} on a grid of columns x rows cells: the flow in cells per unit of
time (|f| <= 1, y down as on the ground) and foam 0..1. cliffs: dry cells beside the water on
another level (a cliff top or foot), kept out of the shore so it does not climb a wall. origin is the world point of cell (0, 0)'s
top-left corner, cell the world units across a cell.
"""

import math

try:
    import engine
except ImportError:            # the self-check at the bottom runs without the engine
    engine = None

SHADERS = "assets/sprite_stack/shaders/"
DEEP = 4                   # cells from the shore at which the water is deepest (terrain.fs, water.fs: keep equal)
LAND = 2                   # cells of shore distance kept on land (likewise)
CALM = 0.7                 # the flow map's speed at |f| = 1; foam cells are stretched to 1


def load(s):
    s["water"] = {"shader": engine.load_shader(SHADERS + "wet_ground.vs", SHADERS + "water.fs"), "key": None}


def shore(cells, cliffs=()):
    """{cell: signed distance in cells from its centre to the shore}: + in water (to the nearest
    land centre - 0.5, at most DEEP), - on land within LAND of water except cliffs; other cells are
    not listed (the map's -LAND)."""
    offs = sorted(((dx, dy) for dx in range(-DEEP - 1, DEEP + 2) for dy in range(-DEEP - 1, DEEP + 2)),
                  key=lambda o: o[0] * o[0] + o[1] * o[1])

    def nearest(c, wet):                   # distance to the nearest cell that is (not) water
        return next((math.hypot(*o) for o in offs if ((c[0] + o[0], c[1] + o[1]) in cells) != wet), DEEP + 1.0)

    out = {c: min(DEEP, nearest(c, True) - 0.5) for c in cells}
    land = {(c[0] + o[0], c[1] + o[1]) for c in cells for o in offs if math.hypot(*o) <= LAND + 1} - cells.keys() - set(cliffs)
    out.update((c, max(-LAND, 0.5 - nearest(c, False))) for c in land)
    return {c: d for c, d in out.items() if d > -LAND}


def bake(s, key, cells, columns, rows, cliffs=()):
    """The water map (the module's docstring): one texel per cell."""
    w = s["water"]
    if w["key"] == key:
        return
    if not cells:
        w.update(key=key, box=None)
        return
    rt = engine.load_render_texture(f"water_flow_{columns}x{rows}", columns, rows)
    if not engine.begin_texture_mode(rt):
        return                                         # try again next frame
    engine.clear_background(engine.Color(128, 128, 0, 255))
    for c, d in shore(cells, cliffs).items():
        fx, fy, foam = cells.get(c, (0.0, 0.0, 0.0))
        k = CALM if foam < 0.5 else 1.0 / max(1e-6, math.hypot(fx, fy))
        engine.draw_rectangle(c[0], c[1], 1, 1, engine.Color(round(127.5 + 127.5 * fx * k), round(127.5 + 127.5 * fy * k),
                                                             round((d + LAND) / (LAND + DEEP) * 255), 255))
    engine.end_texture_mode()
    xs, ys = [c[0] for c in cells], [c[1] for c in cells]
    w.update(key=key, rt=rt, grid=(columns, rows), box=(min(xs) - LAND, min(ys) - LAND, max(xs) + LAND + 1, max(ys) + LAND + 1))


def draw(s, origin, cell):
    """The beach under the water and just past its shore, into the ground picture (its 2D camera)."""
    w = s.get("water")
    if not w or not w.get("box"):
        return
    shader, (x0, y0, x1, y1), (columns, rows) = w["shader"], w["box"], w["grid"]
    engine.begin_shader_mode(shader)
    engine.set_shader_value(shader, "uOrigin", [origin[0], origin[1]])
    engine.set_shader_value(shader, "uCell", [cell])
    engine.draw_texture_pro(w["rt"].texture, engine.Rectangle(0, 0, columns, rows),
                            engine.Rectangle(origin[0] + x0 * cell, origin[1] + y0 * cell, (x1 - x0) * cell, (y1 - y0) * cell),
                            engine.Vector2(0, 0), 0.0, engine.Color(255, 255, 255, 255))
    engine.end_shader_mode()


def terrain_map(s, origin, cell):
    """(texture, world rectangle) of the water map for fx.draw_terrain, or (None, None) with no water."""
    w = s.get("water")
    if not w or not w.get("box"):
        return None, None
    columns, rows = w["grid"]
    return w["rt"].texture, engine.Rectangle(origin[0], origin[1], columns * cell, rows * cell)


if __name__ == "__main__":
    strip = {(x, y): (1.0, 0.0, 0.0) for x in range(-20, 20) for y in range(9)}   # 9 rows of water
    d = shore(strip)
    assert [d.get((0, y)) for y in range(-3, 12)] == [None, -1.5, -0.5, 0.5, 1.5, 2.5, 3.5, 4, 3.5, 2.5, 1.5, 0.5, -0.5, -1.5, None], d
    one = shore({(0, 0): (0.0, 0.0, 0.0)})
    assert one[(0, 0)] == 0.5 and one[(1, 0)] == -0.5 and abs(one[(1, 1)] - (0.5 - math.sqrt(2))) < 1e-9 and (2, 2) not in one
    assert (1, 0) not in shore({(0, 0): (0.0, 0.0, 0.0)}, {(1, 0)})       # a cliff beside it: no shore there
    print("water self-check OK")
