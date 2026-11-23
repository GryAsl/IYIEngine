"""The game side of a geo_world package: load it, ask the world, change what can change.

The package is immutable imported geometry; the conditions of buildings, roads and bridges are the
mutable part, kept beside it in the same dict. One dict `w` per map (keep it in actor.state):

    w = world.load("assets/maps/jowhar")
    world.height(w, x, y)                       ground height, metres above w["frame"]["base_elevation"]
    world.to_lonlat(w, x, y) / from_lonlat(w, lon, lat)
    world.building(w, id) / building_at(w, x, y)
    world.road(w, id), w["nodes"], w["roads"]   road graph: edges between junctions and map-edge entries
    world.flags(w, x, y), w["grid"]             passability grid (bits below), conditions applied
    world.set_condition(w, id, state)           "intact" | "damaged" | "destroyed" (a building or road id) | "cleared"
                                                (a destroyed building whose rubble was taken away: its cells free)
    world.condition(w, id), road_open(w, id), changes_since(w, revision)
    world.set_damage(w, id, amount), damage(w, id)  a building's damage 0..1 (how far its stack crumbles; looks only)
    world.add_mark(w, ...), set_marks(w, marks)  ground marks (craters, tracks, rubble, blood; looks only)
    world.save_state(w) / load_state(w, data)
    world.cell_terrain(w)                       the engine.CellTerrain of the heights
    world.sight_grid(w), update_sight(w, g, rev) engine.SightGrid (line of sight, ray hits) kept in step
    world.blocker_heights(w)                    the sight blockers per grid cell (for a native SightGrid)
    world.foliage(w)                            the leaf bands per grid cell (SightGrid.set_foliage)
    world.blast(w, x, y, radius)                trees in the radius broken or burnt, a scorch left
    world.set_trees(w, tree_state, scorches)    replace the blast damage ({}, [] for a fresh scenario)
    w["walls"], w["trees"]                      overrides.json walls (WALL cells), single trees
                                                [x, y, height, crown radius, crown base, kind]
    w["hedges"], w["props"]                     compound hedges (WALL cells, leaves for sight), yard props
    w["tall_grass"], w["tufts"]                 tall-grass patches (hide a person lying down) and their tufts
    w["fields"]                                 farmland plots, one outer ring each (where farmers work)
    w["tree_state"], w["scorches"]              {tree index: "broken" | "burnt"}, [[x, y, radius]] (blasts)

World frame: metres, x east, y south (screen-down at rotation 0, as the engine's ground), z up.
Who decides what: navigation reads flags/roads and plans routes, combat calls set_condition,
the game decides what may be rebuilt. Format: ../../FORMAT.md.

The logic is C++ (libraries/geo_world/src/World.h, engine.geo_world); this module keeps the world dict that callers
read and the Python signatures. Without the engine (the offline tools geo_import.py and geo_zones.py) only the
package's static part loads, and the frame and raster helpers below run in Python (the importer's copy of the C++).
The self-check is the C++ test TestGeoWorld (tests/engine_tests.cpp).
"""

import json
import math
import sys
from array import array
from pathlib import Path

try:
    from engine import geo_world as _g
except ImportError:      # offline tools: no engine, no C++
    _g = None

FORMAT, VERSION = "geo_world", 1
WATER, STEEP, ROAD, BRIDGE, BUILDING, RUBBLE, WALL = 1, 2, 4, 8, 16, 32, 64   # grid cell bits
STATES = ("intact", "damaged", "destroyed", "cleared")
BUCKET = 64.0               # metres across a building lookup bucket
RUBBLE_HEIGHT = 1.0         # metres a destroyed building still blocks sight
GRASS_HEIGHT = 0.9          # metres of tall grass: hides a person lying down, not one standing
BROKEN_TRUNK = 0.4          # share of its height a tree snapped by a blast keeps
A, E2 = 6378137.0, 6.69437999014e-3   # WGS84 semi-major axis, first eccentricity squared


# --- frame: WGS84 <-> world metres ----------------------------------------------------------

def frame(lat, lon):
    """The local frame at (lat, lon): metres per radian of latitude (m) and of longitude (n)."""
    s = math.sin(math.radians(lat))
    return {"lat": lat, "lon": lon, "m": A * (1 - E2) / (1 - E2 * s * s) ** 1.5,
            "n": A / math.sqrt(1 - E2 * s * s) * math.cos(math.radians(lat))}
# shortcut: equirectangular round the centre (cm-level error within a few km); maps tens of km
# across need a transverse Mercator here.


def project(f, lon, lat):
    return math.radians(lon - f["lon"]) * f["n"], -math.radians(lat - f["lat"]) * f["m"]


def unproject(f, x, y):
    return f["lon"] + math.degrees(x / f["n"]), f["lat"] - math.degrees(y / f["m"])


# --- rasterizing onto a grid {"cell", "columns", "rows", "origin"} --------------------------

def polygon_spans(g, rings):
    """(row, first column, last column) runs of the cells whose centres lie inside the polygon
    (rings: outer and holes, even-odd)."""
    c, (ox, oy), cols = g["cell"], g["origin"], g["columns"]
    ys = [p[1] for r in rings for p in r]
    r0 = max(0, math.ceil((min(ys) - oy) / c - 0.5))
    r1 = min(g["rows"] - 1, math.floor((max(ys) - oy) / c - 0.5))
    edges = [(r[i - 1], r[i]) for r in rings for i in range(len(r))]
    for row in range(r0, r1 + 1):
        y = oy + (row + 0.5) * c
        xs = sorted(a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]) for a, b in edges if (a[1] <= y) != (b[1] <= y))
        for x0, x1 in zip(xs[::2], xs[1::2]):
            c0, c1 = max(0, math.ceil((x0 - ox) / c - 0.5)), min(cols - 1, math.floor((x1 - ox) / c - 0.5))
            if c0 <= c1:
                yield row, c0, c1


def polygon_cells(g, rings):
    cols = g["columns"]
    return [row * cols + col for row, c0, c1 in polygon_spans(g, rings) for col in range(c0, c1 + 1)]


def building_cells(g, b):
    """The cells a building blocks: those whose centres lie in its footprint, or for a hut smaller
    than a cell the one under its centre."""
    c, (ox, oy) = g["cell"], g["origin"]
    return polygon_cells(g, [b["footprint"]]) or [int((b["obb"][1] - oy) // c) * g["columns"] + int((b["obb"][0] - ox) // c)]


def line_cells(g, points, width):
    """Cells whose centres lie within width / 2 of the polyline, sorted."""
    c, (ox, oy), cols, rows = g["cell"], g["origin"], g["columns"], g["rows"]
    half, out = width / 2, set()
    for (ax, ay), (bx, by) in zip(points, points[1:]):
        dx, dy = bx - ax, by - ay
        ll = dx * dx + dy * dy or 1e-9
        for row in range(max(0, math.floor((min(ay, by) - half - oy) / c)), min(rows, math.ceil((max(ay, by) + half - oy) / c))):
            py = oy + (row + 0.5) * c
            for col in range(max(0, math.floor((min(ax, bx) - half - ox) / c)), min(cols, math.ceil((max(ax, bx) + half - ox) / c))):
                px = ox + (col + 0.5) * c
                t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / ll))
                if (ax + t * dx - px) ** 2 + (ay + t * dy - py) ** 2 <= half * half:
                    out.add(row * cols + col)
    return sorted(out)


def yard_open(g, bits, points):
    """True when a walker gets from the free cells inside a ring (a hedge, its gap closed) out past it:
    8 neighbours, no corner cutting (as nav)."""
    cols, (ox, oy), c = g["columns"], g["origin"], g["cell"]
    free = lambda i: not bits[i] & (WALL | BUILDING | WATER)
    mx, my = sum(p[0] for p in points) / len(points), sum(p[1] for p in points) / len(points)
    reach = max(math.hypot(p[0] - mx, p[1] - my) for p in points) + 2 * c
    todo = [i for i in polygon_cells(g, [points]) if free(i)]
    seen = set(todo)
    while todo:
        r, q = divmod(todo.pop(), cols)
        if math.hypot(ox + (q + 0.5) * c - mx, oy + (r + 0.5) * c - my) > reach:
            return True
        for dr, dq in ((-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)):
            j = (r + dr) * cols + q + dq
            if j not in seen and free(j) and free(r * cols + q + dq) and free((r + dr) * cols + q):
                seen.add(j)
                todo.append(j)
    return False


def inside(rings, x, y):
    """Even-odd point in polygon."""
    hit = False
    for r in rings:
        for (ax, ay), (bx, by) in zip(r, r[1:] + r[:1]):
            if (ay <= y) != (by <= y) and x < ax + (y - ay) * (bx - ax) / (by - ay):
                hit = not hit
    return hit


# --- loading --------------------------------------------------------------------------------

def _read(path):
    """A package file: the engine's resources in the game (packed or not), else the disk."""
    eng = sys.modules.get("engine")
    raw = eng._read_resource(path) if eng and hasattr(eng, "_read_resource") else None
    return raw if raw is not None else Path(path).read_bytes()


def load(path):
    """The package in folder `path` (project-relative in the game) as a fresh world dict, every
    condition intact."""
    raw = _read(f"{path}/world.json")
    w = json.loads(raw)
    if w.get("format") != FORMAT or w.get("version") != VERSION:
        raise ValueError(f"{path}: not a {FORMAT} v{VERSION} package")
    w["path"] = path
    for k in ("walls", "trees", "hedges", "props", "tall_grass", "tufts", "fields"):   # packages built before them
        w.setdefault(k, [])
    w["building_ids"] = {b["id"]: b for b in w["buildings"]}
    w["road_ids"] = {r["id"]: r for r in w["roads"]}
    if _g is None:
        return w
    t, g = w["terrain"], w["grid"]
    heights = _read(f"{path}/{t['file']}")
    w["grid_bits"] = bytearray(_read(f"{path}/{g['file']}"))   # the C++ world writes into this
    w["_cpp"] = _g.World(path, raw, heights, w["grid_bits"])    # checks the sizes
    w["heights"] = array("f", heights)   # little-endian host, as the C++ world
    w["condition"], w["damage"], w["revision"], w["changes"] = {}, {}, 0, []
    w["tree_state"], w["scorches"], w["blasts"] = {}, [], []   # blasts: [(revision, x, y, radius)]
    w["marks"], w["marks_gen"] = [], 0   # [kind, x, y, length, width, rotation, strength, time]
    return w


def zones(w):
    """The package's zones.json (FORMAT.md) parsed, None when it has none."""
    try:
        return json.loads(_read(f"{w['path']}/zones.json"))
    except FileNotFoundError:
        return None


def _bucket_keys(points):
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    return [(i, j) for i in range(math.floor(min(xs) / BUCKET), math.floor(max(xs) / BUCKET) + 1)
            for j in range(math.floor(min(ys) / BUCKET), math.floor(max(ys) / BUCKET) + 1)]


# --- queries --------------------------------------------------------------------------------

def to_lonlat(w, x, y):
    return _g.to_lonlat(w, x, y)


def from_lonlat(w, lon, lat):
    return _g.from_lonlat(w, lon, lat)


def height(w, x, y):
    """Ground height at world (x, y): the DEM vertices interpolated bilinearly (clamped at the edge,
    the same surface engine.CellTerrain draws)."""
    return _g.height(w, x, y)


def flags(w, x, y):
    """Grid bits under world (x, y) with the current conditions; None off the map."""
    return _g.flags(w, x, y)


def building(w, bid):
    return w["building_ids"][bid]


def building_at(w, x, y):
    """The id of the building whose footprint holds (x, y), or None."""
    return _g.building_at(w, x, y)


def buildings_near(w, x, y, radius):
    """Ids of the buildings whose footprint is within `radius` metres of (x, y) (holds it, or an edge is that close)."""
    return _g.buildings_near(w, x, y, radius)


def road(w, rid):
    return w["road_ids"][rid]


def condition(w, oid):
    return w["condition"].get(oid, "intact")


def road_open(w, rid):
    return condition(w, rid) != "destroyed"


def changes_since(w, revision):
    """[(revision, id, state)] after `revision` (w["revision"] is the latest): what navigation
    and the renderer have not seen yet."""
    return [c for c in w["changes"] if c[0] > revision]


# --- mutable conditions ---------------------------------------------------------------------

def set_condition(w, oid, state):
    """Set a building's or road's condition; updates the grid (a destroyed building leaves RUBBLE,
    a destroyed bridge drops its BRIDGE bits) without touching the package. True if it changed."""
    return _g.set_condition(w, oid, state)


def damage(w, oid):
    return w["damage"].get(oid, 0.0)


def set_damage(w, oid, amount):
    """A building's damage 0..1 beside its condition: how far its stack crumbles (the renderer's
    business; no grid, sight or navigation change). Raises w["revision"] without a change entry.
    True if it changed."""
    return _g.set_damage(w, oid, float(amount))


def blast(w, x, y, radius):
    """A blast at (x, y): trees within half the radius snap (BROKEN_TRUNK of their height left, no
    crown), the rest within the radius burn bare (trunk and branches, no leaves); a scorch is left.
    Raises w["revision"] (update_sight then fixes the trunks and leaves) without a change entry, as
    no building, road or grid bit changes. Returns how many trees it changed."""
    return _g.blast(w, x, y, radius)


MARKS = ("crater", "track", "rubble", "blood")


def add_mark(w, kind, x, y, length, width, rotation=0.0, strength=1.0, time=0.0):
    """A ground mark (MARKS) of length x width metres turned `rotation` degrees, strength 0..1,
    made at game `time` (the game may fade or drop old ones with set_marks). Looks only: no grid,
    sight or revision change. w["marks"] only grows between set_marks calls, so a renderer can
    draw just the new tail."""
    _g.add_mark(w, MARKS.index(kind), x, y, length, width, rotation, strength, time)


def set_marks(w, marks):
    """Replace the marks (a save game's, a pruned list, or [] for a fresh scenario); bumps
    w["marks_gen"] so a renderer redraws them all."""
    _g.set_marks(w, marks)


def save_state(w):
    """The mutable part for a save game (plain JSON data)."""
    return _g.save_state(w)


def load_state(w, data):
    """Restore save_state's data on a loaded package. Returns the ids it does not know (a package
    rebuilt from newer sources), which are skipped."""
    return _g.load_state(w, data)


def set_trees(w, tree_state, scorches):
    """Replace the blast damage: a save game's, or ({}, []) to start a scenario on unharmed trees."""
    _g.set_trees(w, tree_state, scorches)


def cell_terrain(w):
    """engine.CellTerrain of the heights (corners shared between cells, so no cliffs). Build once."""
    import engine
    t = w["terrain"]
    return engine.CellTerrain(t["columns"], t["rows"], t["cell"], engine.Vector2(*t["origin"]), _g.terrain_corners(w))


def foliage(w, box=None):
    """(lows, highs): array('f') per grid cell of the band above the ground that hides without
    blocking (both 0 where there is none): the widest band the leafy tree crowns give it (low = crown
    base, high = tree top), widened over a compound hedge (from the ground to its height); where
    neither is, tall grass (to GRASS_HEIGHT). One band per cell: a crown over tall grass keeps the crown's.
    box (x0, y0, x1, y1): only the cells in it are redone, in the arrays of the last full call."""
    return _g.foliage(w, box)


def blocker_heights(w):
    """array('f') of grid["columns"] * grid["rows"] sight blocker heights above the ground: the
    tallest building on each cell (RUBBLE_HEIGHT where it is destroyed), wall or tree trunk, 0 where open."""
    return _g.blocker_heights(w)


def sight_grid(w):
    """engine.SightGrid of the ground, the blockers (conditions applied) and the leaves. Build once;
    keep it in step with update_sight."""
    import engine
    t, g = w["terrain"], w["grid"]
    s = engine.SightGrid()
    s.set_ground(t["columns"], t["rows"], t["cell"], engine.Vector2(*t["origin"]), w["heights"])
    s.set_blockers(g["columns"], g["rows"], g["cell"], engine.Vector2(*g["origin"]), blocker_heights(w))
    s.set_foliage(*foliage(w))
    return s


def update_sight(w, sight, revision):
    """Apply the building changes and blasts after `revision` to a sight grid (anything with
    set_blocker_cells(cells, height), and set_foliage for blasts: engine.SightGrid or a native
    module's). Returns w["revision"]."""
    return _g.update_sight(w, sight, revision)
