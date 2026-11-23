"""Offline importer: a real place (DEM + OpenStreetMap) -> a geo_world package (FORMAT.md).

A region folder holds what goes in, all of it committed so a build is the same every time:
    region.json     name, centre lat/lon, half size, cell sizes, DEM zoom (and palette overrides)
    overrides.json  manual authoring on top of the sources (optional; FORMAT.md)
    osm.json.gz     the Overpass answer          } written by `fetch`, never edited by hand:
    dem/z_x_y.png   Terrarium elevation tiles    } fix data in overrides.json instead

    python geo_import.py fetch <region dir>              download the sources (internet)
    python geo_import.py build <region dir> <out dir>    write the package (offline)
    python geo_import.py check <region dir>              build twice, prove identical bytes

Standard library only. Nothing here runs in the game; world.py (the runtime) shares the frame and
rasterizing functions with it, so the package and the game agree on every cell.
"""

import argparse
import gzip
import hashlib
import itertools
import json
import math
import random
import struct
import sys
import tempfile
import urllib.parse
import urllib.request
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from geo_world import world as W  # noqa: E402

OVERPASS = "https://overpass-api.de/api/interpreter"
TERRARIUM = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
AGENT = "IYIEngine-geo_world-importer/1"
FETCH_MARGIN = 300.0        # metres fetched past the map edge, so ways crossing it are whole

ROAD_WIDTH = {"motorway": 12, "trunk": 10, "primary": 8, "secondary": 7, "tertiary": 6, "unclassified": 5,
              "residential": 5, "living_street": 4, "service": 3.5, "track": 3.5, "path": 1.5, "footway": 1.5,
              "cycleway": 1.5, "bridleway": 1.5, "steps": 1.5, "pedestrian": 4}
ROAD_SKIP = {"proposed", "construction", "platform", "abandoned", "razed", "corridor", "elevator", "bus_stop"}
FOOT = {"path", "footway", "cycleway", "bridleway", "steps", "pedestrian"}
PAVED = {"asphalt", "paved", "concrete", "concrete:plates", "paving_stones", "sett", "chipseal"}
WATERWAY_WIDTH = {"river": 15, "canal": 5, "stream": 3, "drain": 2, "ditch": 2}
BUILDING_KINDS = {
    "house": "yes house residential detached semidetached_house apartments terrace bungalow dormitory farm",
    "hut": "hut shed cabin roof shelter barn stable sty",
    "commercial": "commercial retail shop kiosk office supermarket hotel",
    "industrial": "industrial warehouse factory hangar garage garages storage_tank service",
    "worship": "mosque church temple religious cathedral chapel shrine",
    "school": "school university college kindergarten",
    "health": "hospital clinic",
    "government": "government public civic townhall police military fire_station prison"}
BUILDING_KINDS = {v: k for k, vs in BUILDING_KINDS.items() for v in vs.split()}
AMENITY_KINDS = {"place_of_worship": "worship", "school": "school", "college": "school", "university": "school",
                 "kindergarten": "school", "hospital": "health", "clinic": "health", "doctors": "health",
                 "police": "government", "townhall": "government", "courthouse": "government",
                 "marketplace": "market", "fuel": "fuel"}
CIVILIAN = {"market", "worship", "school", "health", "settlement"}
IMPORTANT = {"worship", "school", "health", "government"}
STOREY = 3.2                # metres per storey for heights from levels or estimates
WALL_HEIGHT = 2.2           # metres, an overrides.json wall without its own height
TREE_KINDS = {             # kind -> height range m, crown radius range m, crown base as a share of the height
    "acacia": ((4.0, 8.0), (2.5, 4.5), 0.55), "mango": ((6.0, 11.0), (3.0, 5.0), 0.3),
    "neem": ((6.0, 10.0), (2.5, 4.0), 0.35), "palm": ((8.0, 14.0), (2.0, 3.0), 0.85),
    "banana": ((2.5, 4.0), (1.2, 2.0), 0.3), "bush": ((1.0, 2.2), (0.7, 1.5), 0.0)}
VEGETATION = {              # region.json "vegetation" replaces any key; densities are trees per hectare
    "fade": 18.0,           # metres over which a wood thins out at its edge, and as far past it trees spill
    "clumps": 70.0,         # metres across the clumps and clearings that break up an even density
    "areas": {"wood": [110, {"mango": 3, "neem": 3, "acacia": 2, "palm": 1, "bush": 2}],
              "scrub": [60, {"bush": 5, "acacia": 2}]},
    "orchards": {"mango": 8.0, "banana": 3.5},             # row spacing m; each orchard grows one kind
    "open": [2.0, {"acacia": 3, "bush": 4}],               # bare land between everything else
    "riparian": [30, 60.0, {"palm": 2, "mango": 2, "neem": 2, "acacia": 1, "bush": 2}],   # at the water, reach m
    "yards": [10, {"neem": 3, "mango": 2, "palm": 2}],     # residential land
    "fields": 0.3}          # farmland keeps this share of the open and riparian trees
TREE_NODE = "mango"         # kind of an OSM natural=tree node
DENSITY_CELL, SAMPLE = 4.0, 3.0   # metres: the density field's cells, the stratified sampling pitch
MIN_AREA = 4.0              # m2: smaller footprints are dropped as noise
BLOCKED_FREE = 2.0          # m: a building that leaves a road less than this free (no car fits) is trimmed off it
CLEAR_CORE = 3.0            # m: what a trimmed building leaves clear round the road's centre line
TRIM_KEEP = 0.4             # a trim that would keep less of the footprint than this removes the building instead
AREA_TAGS = {               # (key, value) -> ground class, in paint order below
    ("landuse", "residential"): "residential", ("landuse", "commercial"): "residential",
    ("landuse", "retail"): "residential", ("landuse", "industrial"): "residential",
    ("landuse", "farmland"): "farmland", ("landuse", "farmyard"): "farmland", ("landuse", "allotments"): "farmland",
    ("landuse", "meadow"): "grass", ("landuse", "grass"): "grass", ("leisure", "park"): "grass",
    ("leisure", "pitch"): "grass", ("natural", "grassland"): "grass", ("natural", "scrub"): "scrub",
    ("landuse", "orchard"): "orchard", ("landuse", "plantation"): "orchard", ("landuse", "forest"): "wood",
    ("natural", "wood"): "wood", ("landuse", "cemetery"): "cemetery", ("amenity", "grave_yard"): "cemetery",
    ("natural", "sand"): "sand", ("natural", "bare_rock"): "sand", ("natural", "beach"): "sand",
    ("natural", "wetland"): "wetland", ("natural", "water"): "water", ("waterway", "riverbank"): "water",
    ("landuse", "reservoir"): "water", ("landuse", "basin"): "water"}
HOMESTEADS = {              # region.json "homesteads" replaces any key
    "hedge": 0.7,           # share of the outlying houses (few neighbours) that get a compound hedge
    "outlying": [18.0, 3],  # a house with at most this many others within this many metres is outlying
    "max_area": 250.0,      # m2: larger buildings get no hedge or yard props
    "yard": [3.0, 6.0],     # metres from the house's box to its hedge
    "gate": 4.0,            # metres of hedge left open toward the nearest road
    "kinds": {"zariba": 3, "cactus": 1}, "height": [1.4, 2.0],
    "props": [2.2, {"water_tank": 3, "laundry": 2, "charcoal": 2, "cart": 1, "goat_0": 1, "goat_1": 1, "goat_2": 1}]}   # mean per yard
TALL_GRASS = {              # region.json "tall_grass" replaces any key
    "classes": {"grass": 0.5, "scrub": 0.35, "wetland": 0.7},  # share of each class's land in patches
    "patch": [8.0, 26.0],   # patch radius range, metres
    "tufts": 1 / 30.0}      # tufts per m2 inside a patch (its height: geo_world world.GRASS_HEIGHT)
ROAD_WEAR = {"ruts": 0.9, "puddles": 45.0, "potholes": 35.0}   # rut offset from the edge m; metres of road per puddle (wet) / pothole
PALETTE = {"base": (190, 172, 132), "residential": (198, 182, 150), "farmland": (126, 146, 76),
           "grass": (150, 160, 98), "scrub": (140, 142, 92), "orchard": (112, 136, 72), "wood": (92, 116, 62),
           "cemetery": (172, 162, 132), "sand": (212, 192, 152), "wetland": (122, 142, 102), "water": (72, 102, 106),
           "yard": (176, 158, 124), "road_edge": (150, 128, 100), "unpaved": (178, 142, 106),
           "paved": (88, 88, 90), "path": (190, 166, 130), "soil": (178, 152, 114), "dry_grass": (172, 164, 108),
           "tall_grass": (124, 140, 74), "bund": (184, 164, 120), "rut": (150, 118, 88), "puddle": (96, 92, 78),
           "pothole": (62, 60, 58)}
SEASONS = {                 # extra ground pictures: season -> palette changes (region.json "seasons" replaces it)
    "dry": {"farmland": (178, 162, 112), "grass": (178, 168, 110), "scrub": (162, 152, 102), "orchard": (128, 138, 80),
            "wood": (108, 118, 70), "wetland": (142, 146, 104), "dry_grass": (194, 178, 122),
            "tall_grass": (186, 166, 104), "bund": (196, 176, 132)}}


# --- PNG (8-bit RGB / RGBA, no interlace) ---------------------------------------------------

def read_png(data):
    """(width, height, bytes per pixel, pixel bytes) of an 8-bit RGB or RGBA PNG."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat = 8, []
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body, pos = data[pos + 8:pos + 8 + n], pos + 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
    if depth != 8 or ctype not in (2, 6) or interlace:
        raise ValueError(f"unsupported PNG (depth {depth}, colour type {ctype}, interlace {interlace})")
    bpp = 3 if ctype == 2 else 4
    stride, raw = w * bpp, zlib.decompress(b"".join(idat))
    out, prev = bytearray(), bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + prev[i]) & 255
            elif f == 3:
                line[i] = (line[i] + (a + prev[i]) // 2) & 255
            elif f == 4:
                p = a + prev[i] - c
                pa, pb, pc = abs(p - a), abs(p - prev[i]), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else prev[i] if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, bpp, bytes(out)


def write_png(path, w, h, pixels, bpp=3):
    stride = w * bpp
    raw = b"".join(b"\x00" + pixels[y * stride:(y + 1) * stride] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2 if bpp == 3 else 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


# --- fetch ----------------------------------------------------------------------------------

def _get(url, data=None):
    req = urllib.request.Request(url, data=data, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(req, timeout=600) as r:
        return r.read()


def _bbox(region, margin):
    """(south, west, north, east) of the map square plus margin metres."""
    f, r = W.frame(region["lat"], region["lon"]), region["half"] + margin
    west, north = W.unproject(f, -r, -r)
    east, south = W.unproject(f, r, r)
    return south, west, north, east


def _tile(lon, lat, z):
    """Global Web Mercator pixel (x, y) at zoom z (256 px tiles)."""
    n = 256 * 2 ** z
    return (lon + 180) / 360 * n, (1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n


def fetch(region_dir):
    region = json.loads((region_dir / "region.json").read_text())
    s, w, n, e = _bbox(region, FETCH_MARGIN)
    query = (f"[out:json][timeout:300][bbox:{s:.6f},{w:.6f},{n:.6f},{e:.6f}];"
             "(way[highway];way[building];relation[building];way[landuse];relation[landuse];way[natural];"
             "relation[natural];way[waterway];relation[waterway];way[water];relation[water];way[leisure];"
             "way[amenity];node[amenity];node[place];);out body geom;")
    osm = _get(OVERPASS, urllib.parse.urlencode({"data": query}).encode())
    json.loads(osm)                                   # fail here, not at build, on an error page
    (region_dir / "osm.json.gz").write_bytes(gzip.compress(osm, mtime=0))
    z = region["dem_zoom"]
    (x0, y0), (x1, y1) = _tile(w, n, z), _tile(e, s, z)
    dem = region_dir / "dem"
    dem.mkdir(exist_ok=True)
    for tx in range(int(x0 // 256), int(x1 // 256) + 1):
        for ty in range(int(y0 // 256), int(y1 // 256) + 1):
            (dem / f"{z}_{tx}_{ty}.png").write_bytes(_get(TERRARIUM.format(z=z, x=tx, y=ty)))
    print(f"fetched {len(osm) // 1024} KB of OSM and the DEM tiles into {region_dir}")


# --- build: terrain -------------------------------------------------------------------------

def _heights(region, f, region_dir, issues):
    """Vertex heights (columns + 1) x (rows + 1), row by row from the north-west corner, in metres
    above the returned base elevation (the lowest vertex, rounded down)."""
    z, half, cell = region["dem_zoom"], region["half"], region["cell"]
    tiles = {}
    for p in sorted((region_dir / "dem").glob(f"{z}_*.png")):
        _, tx, ty = p.stem.split("_")
        _, _, bpp, px = read_png(p.read_bytes())
        tiles[int(tx), int(ty)] = (bpp, px)

    def elevation(gx, gy):
        bpp, px = tiles[gx // 256, gy // 256]
        i = ((gy % 256) * 256 + gx % 256) * bpp
        return px[i] * 256 + px[i + 1] + px[i + 2] / 256 - 32768

    n = round(2 * half / cell)
    out = []
    for r in range(n + 1):
        for c in range(n + 1):
            px, py = _tile(*W.unproject(f, -half + c * cell, -half + r * cell), z)
            gx, gy = math.floor(px - 0.5), math.floor(py - 0.5)
            u, v = px - 0.5 - gx, py - 0.5 - gy
            top = elevation(gx, gy) * (1 - u) + elevation(gx + 1, gy) * u
            out.append(top * (1 - v) + (elevation(gx, gy + 1) * (1 - u) + elevation(gx + 1, gy + 1) * u) * v)
    for _ in range(region.get("dem_smooth", 0)):     # 3x3 box passes against DEM speckle (region.json)
        prev = out[:]
        for r in range(n + 1):
            for c in range(n + 1):
                near = [prev[rr * (n + 1) + cc] for rr in range(max(0, r - 1), min(n, r + 1) + 1)
                        for cc in range(max(0, c - 1), min(n, c + 1) + 1)]
                out[r * (n + 1) + c] = sum(near) / len(near)
    if max(out) - min(out) > 3000:
        issues.append({"kind": "dem", "note": "height range over 3 km: a missing or bad tile?"})
    base = math.floor(min(out))
    return n, base, [round(h - base, 3) for h in out]


# --- build: OSM -----------------------------------------------------------------------------

def _rings(parts):
    """Closed rings joined from way geometries (lists of (lon, lat)); one that stays open is dropped."""
    parts, rings = [list(p) for p in parts if len(p) >= 2], []
    while parts:
        ring = parts.pop(0)
        while ring[0] != ring[-1]:
            for i, p in enumerate(parts):
                if p[0] == ring[-1] or p[-1] == ring[-1]:
                    ring += p[1:] if p[0] == ring[-1] else p[-2::-1]
                    parts.pop(i)
                    break
            else:
                ring = None
                break
        if ring:
            rings.append(ring[:-1])
    return rings


def _geometry(el):
    return [(p["lon"], p["lat"]) for p in el.get("geometry", ()) if p]


def _areas(el):
    """(outer rings, inner rings) in (lon, lat) of a closed way or a multipolygon relation;
    (None, None) if not an area."""
    if el["type"] == "way":
        g = _geometry(el)
        return ([g[:-1]], []) if len(g) >= 4 and g[0] == g[-1] else (None, None)
    if el["tags"].get("type") != "multipolygon":
        return None, None
    outer = _rings(_geometry(m) for m in el["members"] if m["type"] == "way" and m.get("role", "outer") in ("outer", ""))
    inner = _rings(_geometry(m) for m in el["members"] if m["type"] == "way" and m.get("role") == "inner")
    return (outer, inner) if outer else (None, None)


def _signed_area(ring):
    return sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(ring, ring[1:] + ring[:1])) / 2


def _self_intersects(ring):
    n = len(ring)
    for i in range(n):
        a, b = ring[i], ring[(i + 1) % n]
        for j in range(i + 2, n):
            if i == 0 and j == n - 1:
                continue
            if _cross(a, b, ring[j], ring[(j + 1) % n]):
                return True
    return False


def _cross(a, b, c, d):
    """Proper intersection of segments ab and cd (touching ends do not count)."""
    def side(p, q, r):
        return (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0])
    d1, d2, d3, d4 = side(c, d, a), side(c, d, b), side(a, b, c), side(a, b, d)
    return d1 * d2 < 0 and d3 * d4 < 0


def _obb(ring):
    """Minimum-area oriented box: (centre x, centre y, length, width, angle in degrees of the
    length axis), length >= width."""
    best = None
    for a, b in zip(ring, ring[1:] + ring[:1]):
        ang = math.atan2(b[1] - a[1], b[0] - a[0])
        ca, sa = math.cos(ang), math.sin(ang)
        us, vs = [p[0] * ca + p[1] * sa for p in ring], [-p[0] * sa + p[1] * ca for p in ring]
        area = (max(us) - min(us)) * (max(vs) - min(vs))
        if best is None or area < best[0] - 1e-6:
            best = (area, ang, min(us), max(us), min(vs), max(vs))
    _, ang, u0, u1, v0, v1 = best
    cu, cv = (u0 + u1) / 2, (v0 + v1) / 2
    cx, cy = cu * math.cos(ang) - cv * math.sin(ang), cu * math.sin(ang) + cv * math.cos(ang)
    length, width, deg = u1 - u0, v1 - v0, math.degrees(ang)
    if width > length:
        length, width, deg = width, length, deg + 90
    return [round(cx, 2), round(cy, 2), round(length, 2), round(width, 2), round(deg % 180, 2)]


def _boxes(ring, obb, max_boxes=4, cover=0.92):
    """Oriented boxes (as _obb) covering a footprint: the obb itself when the footprint nearly
    fills it, else up to max_boxes rectangles greedily cut from it on a grid along the obb's axes
    (an L, U or T shaped building), so no box reaches over a neighbour."""
    cx, cy, length, width, deg = obb
    if abs(_signed_area(ring)) >= 0.9 * length * width:
        return [obb]
    ang = math.radians(deg)
    ca, sa = math.cos(ang), math.sin(ang)
    local = [((x - cx) * ca + (y - cy) * sa, -(x - cx) * sa + (y - cy) * ca) for x, y in ring]
    res = max(0.5, min(length, width) / 12)
    nu, nv = max(1, round(length / res)), max(1, round(width / res))
    ru, rv = length / nu, width / nv
    cells = {(i, j) for i in range(nu) for j in range(nv)
             if W.inside([local], -length / 2 + (i + 0.5) * ru, -width / 2 + (j + 0.5) * rv)}
    total, out = len(cells), []
    while cells and len(out) < max_boxes and 1 - len(cells) / total < cover:
        best = None                                   # largest all-filled rectangle (histogram method)
        heights = [0] * nv
        for i in range(nu):
            heights = [heights[j] + 1 if (i, j) in cells else 0 for j in range(nv)]
            stack = []
            for j in range(nv + 1):
                h = heights[j] if j < nv else 0
                start = j
                while stack and stack[-1][1] >= h:
                    start, sh = stack.pop()
                    if best is None or sh * (j - start) > best[0]:
                        best = (sh * (j - start), i - sh + 1, i, start, j - 1)
                stack.append((start, h))
        area, i0, i1, j0, j1 = best
        if area * ru * rv < MIN_AREA:
            break
        cells -= {(i, j) for i in range(i0, i1 + 1) for j in range(j0, j1 + 1)}
        u, v = -length / 2 + (i0 + i1 + 1) / 2 * ru, -width / 2 + (j0 + j1 + 1) / 2 * rv
        bl, bw, bdeg = (i1 - i0 + 1) * ru, (j1 - j0 + 1) * rv, deg
        if bw > bl:
            bl, bw, bdeg = bw, bl, deg + 90
        out.append([round(cx + u * ca - v * sa, 2), round(cy + u * sa + v * ca, 2), round(bl, 2), round(bw, 2), round(bdeg % 180, 2)])
    return out or [obb]


def _number(text):
    try:
        return float(str(text).split()[0].replace(",", "."))
    except (ValueError, IndexError):
        return None


def _clean(f, lonlat):
    """World ring from (lon, lat): no repeated points, counter-clockwise on screen (y down)."""
    ring = []
    for lon, lat in lonlat:
        p = W.project(f, lon, lat)
        if not ring or math.dist(p, ring[-1]) > 0.05:
            ring.append(p)
    if len(ring) > 1 and math.dist(ring[0], ring[-1]) <= 0.05:
        ring.pop()
    if _signed_area(ring) < 0:
        ring.reverse()
    return ring


def _buildings(f, half, elements, overrides, issues):
    out = []
    candidates = [(f"b:w{el['id']}", [g[:-1]] if (g := _geometry(el))[:1] == g[-1:] else None, el["tags"])
                  for el in elements if el["type"] == "way" and el["tags"].get("building", "no") != "no"]
    for el in elements:
        if el["type"] == "relation" and el["tags"].get("building", "no") != "no":
            outer, _ = _areas(el)
            if outer is None:
                issues.append({"kind": "building_dropped", "id": f"b:r{el['id']}", "note": "relation without closed outer ring"})
                continue
            if len(outer) > 1 or any(m.get("role") == "inner" for m in el["members"]):
                issues.append({"kind": "building_simplified", "id": f"b:r{el['id']}", "note": "outer rings only, holes ignored"})
            candidates += [(f"b:r{el['id']}.{k}", [ring], el["tags"]) for k, ring in enumerate(outer)]
    for add in overrides.get("add_buildings", []):
        candidates.append((add["id"], [[tuple(p) for p in add["lonlat"]]], add.get("tags", {"building": "yes"})))
    edits = overrides.get("buildings", {})
    for bid, rings, tags in candidates:
        edit = edits.get(bid, {})
        if edit.get("remove"):
            continue
        if not rings or len(rings[0]) < 3:
            issues.append({"kind": "building_dropped", "id": bid, "note": "not a closed ring"})
            continue
        ring = _clean(f, rings[0])
        if len(ring) < 3 or abs(_signed_area(ring)) < MIN_AREA:
            issues.append({"kind": "building_dropped", "id": bid, "note": "degenerate or under 4 m2"})
            continue
        if _self_intersects(ring):
            issues.append({"kind": "building_dropped", "id": bid, "at": [round(v, 1) for v in ring[0]], "note": "self-intersecting outline"})
            continue
        cx, cy = sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring)
        if max(abs(cx), abs(cy)) >= half:
            continue                                  # centre off the map: not ours
        kind = BUILDING_KINDS.get(tags.get("building"), AMENITY_KINDS.get(tags.get("amenity")))
        if kind not in IMPORTANT and tags.get("amenity") in AMENITY_KINDS and AMENITY_KINDS[tags["amenity"]] in IMPORTANT:
            kind = AMENITY_KINDS[tags["amenity"]]
        if kind is None:
            issues.append({"kind": "building_type_unknown", "id": bid, "note": f"building={tags.get('building')} -> house"})
            kind = "house"
        out.append({"id": bid, "kind": kind, "tags": tags, "ring": ring, "edit": edit})
    return out


def _finish_buildings(raw, issues):
    """Heights, boxes and the final dicts, after amenity points have refined the kinds."""
    out, estimated = [], 0
    for b in raw:
        tags, edit = b["tags"], b["edit"]
        kind = edit.get("kind", b["kind"])
        levels = edit.get("levels") or _number(tags.get("building:levels"))
        height = edit.get("height") or _number(tags.get("height"))
        source = "override" if "height" in edit or "levels" in edit else "height" if height else "levels" if levels else "estimate"
        levels = max(1, round(levels or (height / STOREY if height else 1)))
        height = height or levels * STOREY
        estimated += source == "estimate"
        out.append({"id": b["id"], "kind": kind, "important": edit.get("important", kind in IMPORTANT),
                    "levels": levels, "height": round(height, 2), "height_source": source,
                    "name": tags.get("name:en", tags.get("name", "")), "obb": (obb := _obb(b["ring"])),
                    "boxes": _boxes(b["ring"], obb),
                    "footprint": [[round(x, 2), round(y, 2)] for x, y in b["ring"]]})
    issues.append({"kind": "heights_estimated", "count": estimated, "note": f"{STOREY} m per storey, 1 storey"})
    return out


def _clip(points, half):
    """Pieces of a polyline [(node id or None, x, y)] inside the square +-half; a point where
    a piece crosses the edge has node id None."""
    pieces, cur = [], None
    for (na, ax, ay), (nb, bx, by) in zip(points, points[1:]):
        t0, t1, dx, dy = 0.0, 1.0, bx - ax, by - ay
        for p, q in ((-dx, ax + half), (dx, half - ax), (-dy, ay + half), (dy, half - ay)):
            if p == 0:
                if q < 0:
                    t0, t1 = 1.0, 0.0
            elif p < 0:
                t0 = max(t0, q / p)
            else:
                t1 = min(t1, q / p)
        if t0 > t1:
            cur = None
            continue
        if cur is None or t0 > 0:
            cur = [(na, ax, ay) if t0 == 0 else (None, ax + t0 * dx, ay + t0 * dy)]
            pieces.append(cur)
        cur.append((nb, bx, by) if t1 == 1 else (None, ax + t1 * dx, ay + t1 * dy))
        if t1 < 1:
            cur = None
    return [p for p in pieces if len(p) >= 2]


def _roads(f, half, elements, overrides, issues):
    edits = overrides.get("roads", {})
    ways = []
    for el in elements:
        t = el["tags"]
        if el["type"] != "way" or "highway" not in t or t["highway"] in ROAD_SKIP or t.get("area") == "yes":
            continue
        edit = edits.get(f"w{el['id']}", {})
        if edit.get("remove"):
            continue
        g = el.get("geometry") or []
        if len(g) < 2 or not all(g):
            issues.append({"kind": "road_dropped", "id": f"w{el['id']}", "note": "missing geometry"})
            continue
        ways.append((el, edit, [(nid, *W.project(f, p["lon"], p["lat"])) for nid, p in zip(el["nodes"], g)]))
    uses = {}
    for el, _, pts in ways:
        for nid in set(n for n, _, _ in pts):
            uses[nid] = uses.get(nid, 0) + 1
        for nid in el["nodes"][1:-1]:                 # a way passing a node twice ends there too
            if el["nodes"].count(nid) > 1:
                uses[nid] = 2
    nodes, roads = {}, []
    for el, edit, pts in ways:
        t, k = el["tags"], 0
        kind = edit.get("kind", t["highway"])
        width = edit.get("width") or _number(t.get("width")) or ROAD_WIDTH.get(kind, 3.0)
        surface = t.get("surface")
        paved = surface in PAVED if surface else kind in ("motorway", "trunk", "primary")
        for p, piece in enumerate(_clip(pts, half)):
            cut = [0] + [i for i in range(1, len(piece) - 1) if uses.get(piece[i][0], 0) > 1] + [len(piece) - 1]
            for a, b in zip(cut, cut[1:]):
                part = piece[a:b + 1]
                ends = []
                for side, (nid, x, y) in (("a", part[0]), ("b", part[-1])):
                    key = f"n{nid}" if nid is not None else f"e{el['id']}.{p}{side}"
                    nodes[key] = [round(x, 2), round(y, 2)]
                    ends.append(key)
                points = [[round(x, 2), round(y, 2)] for _, x, y in part]
                roads.append({"id": f"w{el['id']}.{k}", "a": ends[0], "b": ends[1], "kind": kind, "width": width,
                              "paved": paved, "bridge": edit.get("bridge", t.get("bridge", "no") != "no"),
                              "tunnel": t.get("tunnel", "no") != "no", "layer": int(_number(t.get("layer")) or 0),
                              "oneway": t.get("oneway") == "yes", "name": t.get("name:en", t.get("name", "")),
                              "length": round(sum(math.dist(u, v) for u, v in zip(points, points[1:])), 2), "points": points})
                k += 1
    _crossings(roads, issues)
    return nodes, roads


def _half_plane(ring, nx, ny, c):
    """The part of ring where nx*x + ny*y >= c (Sutherland-Hodgman, one edge)."""
    out = []
    for p, q in zip(ring, ring[1:] + ring[:1]):
        dp, dq = p[0] * nx + p[1] * ny - c, q[0] * nx + q[1] * ny - c
        if dp >= 0:
            out.append(p)
        if (dp >= 0) != (dq >= 0):
            t = dp / (dp - dq)
            out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
    return out


def _clear_streets(raw, roads, issues):
    """Buildings that close a road (OSM outlines drawn over a street; less than BLOCKED_FREE metres left
    between them at some point) lose the strip that lies within CLEAR_CORE / 2 of the road's centre line;
    one whose middle is past the centre line, or that would keep under TRIM_KEEP of itself, is removed. Footpaths
    and bridges are left alone. Deterministic: buildings and roads in their import order."""
    bucket = 20.0
    near = {}
    for b in raw:
        xs, ys = [p[0] for p in b["ring"]], [p[1] for p in b["ring"]]
        for i in range(int(min(xs) // bucket), int(max(xs) // bucket) + 1):
            for j in range(int(min(ys) // bucket), int(max(ys) // bucket) + 1):
                near.setdefault((i, j), []).append(b)
    cuts = {}                                          # building id -> [(nx, ny, c)] half-planes to keep
    for r in roads:
        if r["kind"] in FOOT or r["bridge"] or r["tunnel"]:
            continue
        hw = r["width"] / 2
        for (ax, ay), (bx, by) in zip(r["points"], r["points"][1:]):
            seg = math.hypot(bx - ax, by - ay)
            if seg < 1e-6:
                continue
            ux, uy = (bx - ax) / seg, (by - ay) / seg
            for k in range(int(seg) + 1):
                x, y = ax + ux * k, ay + uy * k
                here = near.get((int(x // bucket), int(y // bucket)))
                if not here:
                    continue
                reach, who = [0.0, 0.0], {}
                for s, side in enumerate((1, -1)):     # walk in from each edge, 0.25 m at a time
                    for step in range(int(hw * 4)):
                        off = (hw - step * 0.25) * side
                        hit = [b for b in here if W.inside([b["ring"]], x - uy * off, y + ux * off)]
                        if not hit:
                            break
                        reach[s] = step * 0.25 + 0.25
                        for b in hit:
                            who[b["id"]] = side
                if r["width"] - reach[0] - reach[1] < BLOCKED_FREE:
                    for bid, side in who.items():     # keep the side of the line (normal (-uy, ux) * side) beyond the core
                        nx, ny = -uy * side, ux * side
                        cuts.setdefault(bid, []).append((nx, ny, nx * ax + ny * ay + CLEAR_CORE / 2))
    keep = []
    for b in raw:
        planes = cuts.get(b["id"])
        if not planes:
            keep.append(b)
            continue
        ring, before = b["ring"], abs(_signed_area(b["ring"]))
        cx, cy = sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring)
        if any(nx * cx + ny * cy < c - CLEAR_CORE / 2 for nx, ny, c in set(planes)):
            issues.append({"kind": "building_off_road", "id": b["id"], "at": [round(cx, 1), round(cy, 1)], "note": "stood on the road: removed"})
            continue
        for nx, ny, c in sorted(set(planes)):
            ring = _half_plane(ring, nx, ny, c) if len(ring) >= 3 else ring
        if len(ring) < 3 or abs(_signed_area(ring)) < max(MIN_AREA, TRIM_KEEP * before):
            issues.append({"kind": "building_off_road", "id": b["id"], "at": [round(cx, 1), round(cy, 1)], "note": "mostly on the road: removed"})
            continue
        issues.append({"kind": "building_trimmed", "id": b["id"], "at": [round(cx, 1), round(cy, 1)],
                       "note": f"{round(before - abs(_signed_area(ring)), 1)} m2 off the road"})
        keep.append({**b, "ring": ring})
    raw[:] = keep


def _crossings(roads, issues):
    """Roads that cross on the same level without a shared node: reported, not joined (an
    unmapped junction, or a missing bridge tag); overrides.json can fix either."""
    buckets = {}
    for r in roads:
        level = (r["bridge"], r["tunnel"], r["layer"])
        for a, b in zip(r["points"], r["points"][1:]):
            for i in range(math.floor(min(a[0], b[0]) / 50), math.floor(max(a[0], b[0]) / 50) + 1):
                for j in range(math.floor(min(a[1], b[1]) / 50), math.floor(max(a[1], b[1]) / 50) + 1):
                    buckets.setdefault((i, j), []).append((r["id"], level, a, b))
    seen = set()
    for segs in buckets.values():
        for i, (ra, la, a, b) in enumerate(segs):
            for rb, lb, c, d in segs[i + 1:]:
                if ra != rb and la == lb and (min(ra, rb), max(ra, rb)) not in seen and _cross(a, b, c, d):
                    seen.add((min(ra, rb), max(ra, rb)))
                    issues.append({"kind": "road_crossing_without_junction", "id": f"{min(ra, rb)} x {max(ra, rb)}",
                                   "at": [round((a[0] + b[0]) / 2, 1), round((a[1] + b[1]) / 2, 1)]})


def _points(f, half, elements, buildings, roads, nodes, overrides):
    out = []
    by_bucket = {}
    for b in buildings:
        for key in W._bucket_keys(b["ring"]):
            by_bucket.setdefault(key, []).append(b)
    for el in elements:
        t = el["tags"]
        kind = AMENITY_KINDS.get(t.get("amenity"))
        if el["type"] == "node" and t.get("place") in ("city", "town", "village", "hamlet", "suburb", "neighbourhood", "quarter", "isolated_dwelling"):
            kind = "settlement"
        if not kind:
            continue
        if el["type"] == "node":
            x, y = W.project(f, el["lon"], el["lat"])
        else:
            g = _geometry(el)
            if not g:
                continue
            ring = [W.project(f, *p) for p in g]
            x, y = sum(p[0] for p in ring) / len(ring), sum(p[1] for p in ring) / len(ring)
        if max(abs(x), abs(y)) >= half:
            continue
        ref = next((b for b in by_bucket.get((math.floor(x / W.BUCKET), math.floor(y / W.BUCKET)), ())
                    if W.inside([b["ring"]], x, y)), None)
        if ref and kind in IMPORTANT and ref["kind"] not in IMPORTANT:
            ref["kind"] = kind                         # a mosque mapped as a node inside its building
        out.append({"id": f"p:{el['type'][0]}{el['id']}", "kind": kind, "x": round(x, 2), "y": round(y, 2),
                    "name": t.get("name:en", t.get("name", "")), "civilian": kind in CIVILIAN,
                    "ref": ref["id"] if ref else None})
    for r in roads:
        if r["bridge"]:
            mx, my = r["points"][len(r["points"]) // 2]
            out.append({"id": f"p:bridge:{r['id']}", "kind": "bridge", "x": mx, "y": my, "name": r["name"],
                        "civilian": False, "ref": r["id"]})
        for end in (r["a"], r["b"]):
            if end.startswith("e") and r["kind"] not in FOOT:
                x, y = nodes[end]
                out.append({"id": f"p:entry:{end}", "kind": "entry", "x": x, "y": y, "name": r["name"],
                            "civilian": False, "ref": r["id"]})
    drop = set(overrides.get("remove_points", []))
    out = [p for p in out if p["id"] not in drop]
    for p in overrides.get("points", []):
        x, y = W.project(f, *p["lonlat"])
        out.append({"id": p["id"], "kind": p["kind"], "x": round(x, 2), "y": round(y, 2), "name": p.get("name", ""),
                    "civilian": p.get("civilian", p["kind"] in CIVILIAN), "ref": p.get("ref")})
    return out


# --- build: grid and ground picture ---------------------------------------------------------

def _water(f, elements):
    """Water polygons (list of rings) and waterway lines (points, width) in world metres."""
    polys, lines = [], []
    for el in elements:
        t = el["tags"]
        if AREA_TAGS.get(("natural", t.get("natural"))) == "water" or t.get("waterway") == "riverbank" \
                or t.get("landuse") in ("reservoir", "basin") or ("water" in t and el["type"] != "node"):
            outer, inner = _areas(el)
            if outer:
                polys.append([_clean(f, r) for r in outer + inner])
        elif el["type"] == "way" and t.get("waterway") in WATERWAY_WIDTH:
            pts = [W.project(f, *p) for p in _geometry(el)]
            if len(pts) >= 2:
                lines.append((pts, _number(t.get("width")) or WATERWAY_WIDTH[t["waterway"]]))
    return polys, lines


def _walls(f, overrides):
    """overrides.json walls (OSM has none here): [{"id", "points" (metres), "height"}]."""
    return [{"id": o["id"], "points": [list(W.project(f, *p)) for p in o["lonlat"]], "height": o.get("height", WALL_HEIGHT)}
            for o in overrides.get("walls", ())]


def _hash01(i, j, seed):
    n = (i * 374761393 + j * 668265263 + seed * 1442695041) & 0xFFFFFFFF
    n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
    return (n ^ (n >> 16)) / 4294967296.0


def _noise(x, y, seed):
    """Smooth value noise in 0..1, about one feature per unit (deterministic, no global state)."""
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b, c, d = _hash01(ix, iy, seed), _hash01(ix + 1, iy, seed), _hash01(ix, iy + 1, seed), _hash01(ix + 1, iy + 1, seed)
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy


def _field(g, fn):
    """fn(x, y) at every cell centre of grid g, row by row."""
    (ox, oy), c = g["origin"], g["cell"]
    return [fn(ox + (k + 0.5) * c, oy + (r + 0.5) * c) for r in range(g["rows"]) for k in range(g["columns"])]


def _blur(v, cols, rows, radius):
    """Box blur twice (close to a Gaussian), radius in cells; outside the grid counts as 0."""
    w = 2 * radius + 1
    for _ in range(2):
        out = [0.0] * len(v)
        for y in range(rows):
            pre = list(itertools.accumulate(v[y * cols:(y + 1) * cols], initial=0.0))
            for x in range(cols):
                out[y * cols + x] = (pre[min(cols, x + radius + 1)] - pre[max(0, x - radius)]) / w
        v, out = out, [0.0] * len(v)
        for x in range(cols):
            pre = list(itertools.accumulate(v[x::cols], initial=0.0))
            for y in range(rows):
                out[y * cols + x] = (pre[min(rows, y + radius + 1)] - pre[max(0, y - radius)]) / w
        v = out
    return v


def _distance(mask, cols, rows, cell, cap):
    """Metres from every cell to the nearest masked one (chamfer, two passes), at most cap."""
    d, diag = [0.0 if m else cap for m in mask], cell * math.sqrt(2)
    for order in (range(len(d)), range(len(d) - 1, -1, -1)):
        sign = 1 if order.step == 1 else -1
        for i in order:
            if d[i] == 0.0:
                continue
            x, v = i % cols, d[i]
            j = i - sign * cols                        # the row already passed
            if 0 <= j < len(d):
                v = min(v, d[j] + cell)
                if 0 <= x - 1 and j - 1 >= 0:
                    v = min(v, d[j - 1] + diag)
                if x + 1 < cols and j + 1 < len(d):
                    v = min(v, d[j + 1] + diag)
            k = i - sign                               # the cell before on this row
            if 0 <= k < len(d) and k // cols == i // cols:
                v = min(v, d[k] + cell)
            d[i] = v
    return d


def _landuse(f, elements):
    """{ground class: [rings, ...]} of the OSM land use areas, and the elements' ids beside them."""
    out = {}
    for el in elements:
        cls = next((AREA_TAGS[(k, v)] for k, v in el["tags"].items() if (k, v) in AREA_TAGS), None)
        if cls:
            outer, inner = _areas(el)
            if outer:
                out.setdefault(cls, []).append(([_clean(f, r) for r in outer + inner], f"{el['type']}{el['id']}"))
    return out


def _pick(rng, weights):
    return rng.choices(sorted(weights), [weights[k] for k in sorted(weights)])[0]


def _tree(rng, kinds, kind, x, y, narrow=1.0):
    """[x, y, height, crown radius, crown base, kind]; `narrow` < 1 keeps sizes near the middle of the range."""
    (h0, h1), (r0, r1), base = kinds[kind]
    m = (1 - narrow) / 2
    h, r = h0 + (h1 - h0) * rng.uniform(m, 1 - m), r0 + (r1 - r0) * rng.uniform(m, 1 - m)
    return [round(x, 1), round(y, 1), round(h, 1), round(r, 1), round(h * base, 1), kind]


def _trees(region, f, half, elements, landuse, water, grid, bits):
    """Single trees [x, y, height, crown radius, crown base, kind] (VEGETATION, TREE_KINDS), never on a
    building, road, water or wall cell:
    - a density field on DENSITY_CELL cells: wood and scrub areas, blurred so they thin out over
      `fade` metres at the edge and spill as far past it; riparian trees falling off with the distance
      to water; yard trees on residential land; sparse open-land trees, fewer on farmland; all times
      a clump noise, so there are thickets and clearings. Sampled once per SAMPLE metres (stratified,
      jittered over the whole cell: no lattice shows), the kind drawn from whichever part dominates.
    - orchards in rows along their own long axis, a kind per orchard, a few gaps;
    - OSM natural=tree nodes.
    Deterministic: one seeded generator in a fixed order, and noise from coordinates."""
    veg = {**VEGETATION, **region.get("vegetation", {})}
    kinds = {**TREE_KINDS, **{k: tuple(tuple(x) if isinstance(x, list) else x for x in v) for k, v in region.get("tree_kinds", {}).items()}}
    blocked = W.BUILDING | W.ROAD | W.WATER | W.WALL
    out, near = [], {}

    def free(x, y, r):
        if not (-half <= x < half and -half <= y < half):
            return False
        if bits[int((y + half) // grid["cell"]) * grid["columns"] + int((x + half) // grid["cell"])] & blocked:
            return False
        bx, by = int(x // 4), int(y // 4)
        return all(math.hypot(x - ox, y - oy) >= max(1.5, 0.45 * (r + orr))
                   for k in ((bx + i, by + j) for i in (-2, -1, 0, 1, 2) for j in (-2, -1, 0, 1, 2)) for ox, oy, orr in near.get(k, ()))

    def add(t):
        if free(t[0], t[1], t[3]):
            out.append(t)
            near.setdefault((int(t[0] // 4), int(t[1] // 4)), []).append((t[0], t[1], t[3]))

    rng = random.Random(f"vegetation{region['name']}")
    for el in elements:                                # OSM single trees first: they are real
        t = el["tags"]
        if el["type"] == "node" and t.get("natural") == "tree":
            x, y = W.project(f, el["lon"], el["lat"])
            tree = _tree(rng, kinds, TREE_NODE, x, y)
            if _number(t.get("height")):              # the tagged height, the crown base in proportion
                tree[2], tree[4] = _number(t["height"]), round(tree[4] * _number(t["height"]) / tree[2], 1)
            add(tree)
    orchard_cells = set()
    g = {"cell": DENSITY_CELL, "columns": round(2 * half / DENSITY_CELL), "rows": round(2 * half / DENSITY_CELL), "origin": [-half, -half]}
    for rings, oid in landuse.get("orchard", ()):     # rows along the orchard's long axis
        orng = random.Random(oid)
        kind = _pick(orng, {k: 1 for k in veg["orchards"]})
        sp = veg["orchards"][kind]
        cx, cy, length, width, deg = _obb(rings[0])
        ca, sa = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        for i in range(int(length / sp / 2) + 1):
            for u in {i * sp, -i * sp}:
                for j in range(int(width / sp / 2) + 1):
                    for v in {j * sp, -j * sp}:
                        x, y = cx + u * ca - v * sa + orng.uniform(-0.4, 0.4), cy + u * sa + v * ca + orng.uniform(-0.4, 0.4)
                        if orng.random() > 0.07 and W.inside(rings, x, y):
                            add(_tree(orng, kinds, kind, x, y, narrow=0.5))
        orchard_cells.update(W.polygon_cells(g, rings))

    cols, rows, n = g["columns"], g["rows"], g["columns"] * g["rows"]
    radius = max(1, round(veg["fade"] / (2 * DENSITY_CELL)))
    parts = []                                         # (per-hectare field, kind weights)
    for cls, (per_ha, mix) in veg["areas"].items():
        mask = [0.0] * n
        for rings, _ in landuse.get(cls, ()):
            for i in W.polygon_cells(g, rings):
                mask[i] = 1.0
        parts.append(([per_ha * v for v in _blur(mask, cols, rows, radius)], mix))
    cover = [min(1.0, sum(p[0][i] / veg["areas"][c][0] for p, c in zip(parts, veg["areas"]))) for i in range(n)]
    water_mask = [0] * n
    for rings in water[0]:
        for i in W.polygon_cells(g, rings):
            water_mask[i] = 1
    for pts, width in water[1]:
        for i in W.line_cells(g, pts, width):
            water_mask[i] = 1
    rip_ha, reach, rip_mix = veg["riparian"]
    to_water = _distance(water_mask, cols, rows, DENSITY_CELL, 4 * reach)
    fields, homes = [0] * n, [0.0] * n
    for rings, _ in landuse.get("farmland", ()):
        for i in W.polygon_cells(g, rings):
            fields[i] = 1
    for rings, _ in landuse.get("residential", ()):
        for i in W.polygon_cells(g, rings):
            homes[i] = 1.0
    homes = _blur(homes, cols, rows, radius)
    open_ha, open_mix = veg["open"]
    yard_ha, yard_mix = veg["yards"]
    for i in range(n):
        keep = (1.0 - cover[i]) * (veg["fields"] if fields[i] else 1.0)
        homes[i] = yard_ha * homes[i] * (1.0 - cover[i])
        to_water[i] = rip_ha * math.exp(-to_water[i] / reach) * keep
        cover[i] = open_ha * keep
    parts += [(to_water, rip_mix), (homes, yard_mix), (cover, open_mix)]
    clumps = _field(g, lambda x, y: _noise(x / veg["clumps"], y / veg["clumps"], 11) * 0.65 + _noise(x / veg["clumps"] * 3, y / veg["clumps"] * 3, 12) * 0.35)

    steps = round(2 * half / SAMPLE)
    for row in range(steps):
        for col in range(steps):
            x, y = -half + (col + rng.random()) * SAMPLE, -half + (row + rng.random()) * SAMPLE
            i = min(rows - 1, int((y + half) // DENSITY_CELL)) * cols + min(cols - 1, int((x + half) // DENSITY_CELL))
            if i in orchard_cells:
                continue
            c = clumps[i]
            mult = 0.15 + 1.7 * (0 if c < 0.3 else 1 if c > 0.7 else (3 - 2 * (c - 0.3) / 0.4) * ((c - 0.3) / 0.4) ** 2)
            dens = [p[0][i] for p in parts]
            total = sum(dens) * mult
            if rng.random() < total * SAMPLE * SAMPLE / 10000.0:
                mix = parts[max(range(len(parts)), key=dens.__getitem__)][1]
                add(_tree(rng, kinds, _pick(rng, mix), x, y))
    return sorted(out)


def _homesteads(region, half, buildings, roads, g, bits):
    """Compound hedges and yard props of the outlying houses (HOMESTEADS). A hedge rings the house's
    box `yard` metres out, wavy, open for `gate` metres toward the nearest road; its cells become WALL
    (closed to movement; geo_world counts it as leaves for sight). A hedge that would cross a road,
    water, a building or another hedge is not built. Props stand in the yard on free cells.
    Returns (hedges [{"points", "height", "kind", "building"}], props [[x, y, z, rotation, kind]])."""
    cfg = {**HOMESTEADS, **region.get("homesteads", {})}
    reach, most = cfg["outlying"]
    near = {}
    for b in buildings:
        near.setdefault((int(b["obb"][0] // reach), int(b["obb"][1] // reach)), []).append(b["obb"])
    road_pts = {}                                      # road points every 5 m, in 32 m buckets
    for r in roads:
        for (ax, ay), (bx, by) in zip(r["points"], r["points"][1:]):
            k = max(1, int(math.hypot(bx - ax, by - ay) / 5))
            for t in range(k + 1):
                x, y = ax + (bx - ax) * t / k, ay + (by - ay) * t / k
                road_pts.setdefault((int(x // 32), int(y // 32)), []).append((x, y))
    hedges, props, taken = [], [], set()
    blocked = W.ROAD | W.WATER | W.BUILDING | W.WALL
    for b in buildings:
        cx, cy, length, width, deg = b["obb"]
        if b["kind"] != "house" or length * width > cfg["max_area"]:
            continue
        rng = random.Random(b["id"])
        others = sum(1 for k in ((int(cx // reach) + i, int(cy // reach) + j) for i in (-1, 0, 1) for j in (-1, 0, 1))
                     for o in near.get(k, ()) if 0 < math.hypot(o[0] - cx, o[1] - cy) <= reach)
        if others > most:
            continue
        m = rng.uniform(*cfg["yard"])
        ca, sa = math.cos(math.radians(deg)), math.sin(math.radians(deg))
        hl, hw = length / 2 + m, width / 2 + m
        ring = _wavy([(cx + u * ca - v * sa, cy + u * sa + v * ca) for u, v in ((-hl, -hw), (hl, -hw), (hl, hw), (-hl, hw))],
                     zlib.crc32(b["id"].encode()) % 1000, amp=0.6, step=2.0)
        if rng.random() < cfg["hedge"]:
            pts = [p for k in ((int(cx // 32) + i, int(cy // 32) + j) for i in (-2, -1, 0, 1, 2) for j in (-2, -1, 0, 1, 2)) for p in road_pts.get(k, ())]
            gx, gy = min(pts, key=lambda p: math.hypot(p[0] - cx, p[1] - cy)) if pts else (cx + ca, cy + sa)
            start = min(range(len(ring)), key=lambda i: math.hypot(ring[i][0] - gx, ring[i][1] - gy))
            line = ring[start:] + ring[:start] + [ring[start]]     # closed, starting at the gate
            for _ in range(2):                                 # cut gate/2 off each end, then flip back
                left = cfg["gate"] / 2
                while len(line) > 2:
                    (ax, ay), (bx, by) = line[0], line[1]
                    s = math.hypot(bx - ax, by - ay)
                    if s > left:
                        line[0] = (ax + (bx - ax) * left / s, ay + (by - ay) * left / s)
                        break
                    left -= s
                    line.pop(0)
                line.reverse()
            cells = set(W.line_cells(g, line, g["cell"]))
            own = set(W.building_cells(g, b))
            wall = cells - own - set(W.line_cells(g, [line[-1], line[0]], g["cell"]))   # the gate's own cells stay open
            if not cells & taken and not any(bits[i] & blocked for i in cells - own):
                for i in wall:
                    bits[i] |= W.WALL
                if W.yard_open(g, bits, line):
                    taken |= cells
                    hedges.append({"points": [[round(x, 2), round(y, 2)] for x, y in line], "height": round(rng.uniform(*cfg["height"]), 1),
                                   "kind": _pick(rng, cfg["kinds"]), "building": b["id"]})
                else:                                  # the gate faces a neighbour's wall: no hedge
                    for i in wall:
                        bits[i] &= ~W.WALL
        mean, mix = cfg["props"]
        for _ in range(int(mean * rng.random() * 2 + 0.5)):
            a, d = rng.uniform(0, math.tau), rng.uniform(0.5, 0.85)
            u, v = math.cos(a) * (length / 2 + m * d), math.sin(a) * (width / 2 + m * d)   # round the turned house
            props.append([round(cx + u * ca - v * sa, 2), round(cy + u * sa + v * ca, 2), 0.0, rng.randrange(360), _pick(rng, mix)])

    def free(x, y):
        col, row = int((x + half) // g["cell"]), int((y + half) // g["cell"])
        return 0 <= col < g["columns"] and 0 <= row < g["rows"] and not bits[row * g["columns"] + col] & blocked
    return hedges, [p for p in props if free(p[0], p[1])]     # after every hedge: a later one may cross an earlier yard


def _tall_grass(region, half, landuse, g, bits):
    """Tall-grass patches (TALL_GRASS) on grass, scrub and wetland land, and the tufts drawn in them.
    Returns (patches [ring], tufts [[x, y]]); a tuft never stands on a road, water, building or wall."""
    cfg = {**TALL_GRASS, **region.get("tall_grass", {})}
    patches, tufts = [], []
    blocked = W.ROAD | W.WATER | W.BUILDING | W.WALL
    for cls, share in sorted(cfg["classes"].items()):
        for rings, oid in landuse.get(cls, ()):
            rng = random.Random(f"grass{oid}")
            x0, y0 = min(p[0] for p in rings[0]), min(p[1] for p in rings[0])
            x1, y1 = max(p[0] for p in rings[0]), max(p[1] for p in rings[0])
            area, want = 0.0, share * abs(_signed_area(rings[0]))
            for _ in range(400):
                if area >= want:
                    break
                cx, cy, r = rng.uniform(x0, x1), rng.uniform(y0, y1), rng.uniform(*cfg["patch"])
                if not W.inside(rings, cx, cy):
                    continue
                ring = _wavy([(cx + math.cos(a) * r * rng.uniform(0.7, 1.2), cy + math.sin(a) * r * rng.uniform(0.7, 1.2))
                              for a in (k * math.tau / 10 for k in range(10))], rng.randrange(1000), amp=r * 0.2, step=r * 0.3)
                patches.append(ring)
                area += math.pi * r * r
                for _ in range(int(math.pi * r * r * cfg["tufts"])):
                    x, y = rng.uniform(cx - r, cx + r), rng.uniform(cy - r, cy + r)
                    col, row = int((x + half) // g["cell"]), int((y + half) // g["cell"])
                    if 0 <= col < g["columns"] and 0 <= row < g["rows"] and not bits[row * g["columns"] + col] & blocked \
                            and W.inside([ring], x, y) and W.inside(rings, x, y):
                        tufts.append([round(x, 2), round(y, 2)])
    return [[[round(x, 2), round(y, 2)] for x, y in ring] for ring in patches], sorted(tufts)


def _offset(pts, d):
    """The polyline moved d metres to its left (per-segment normals, averaged at the joints)."""
    out = []
    for i, (x, y) in enumerate(pts):
        a, b = pts[max(0, i - 1)], pts[min(len(pts) - 1, i + 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        n = math.hypot(dx, dy) or 1.0
        out.append((x - dy / n * d, y + dx / n * d))
    return out


def _grid(region, n, heights, buildings, roads, water, issues):
    half, cell, gc = region["half"], region["cell"], region["grid"]
    g = {"cell": gc, "columns": round(2 * half / gc), "rows": round(2 * half / gc), "origin": [-half, -half]}
    bits = bytearray(g["columns"] * g["rows"])
    k, steep_deg, steep = round(cell / gc), region.get("steep_deg", 30), 0
    for r in range(n):
        for c in range(n):
            i = r * (n + 1) + c
            dx = (heights[i + 1] + heights[i + n + 2] - heights[i] - heights[i + n + 1]) / (2 * cell)
            dy = (heights[i + n + 1] + heights[i + n + 2] - heights[i] - heights[i + 1]) / (2 * cell)
            if math.degrees(math.atan(math.hypot(dx, dy))) > steep_deg:
                steep += 1
                for rr in range(r * k, r * k + k):
                    for cc in range(c * k, c * k + k):
                        bits[rr * g["columns"] + cc] |= W.STEEP
    polys, lines = water
    for rings in polys:
        for i in W.polygon_cells(g, rings):
            bits[i] |= W.WATER
    for pts, width in lines:
        for i in W.line_cells(g, pts, width):
            bits[i] |= W.WATER
    for r in roads:
        for i in W.line_cells(g, r["points"], r["width"]):
            bits[i] |= W.ROAD | (W.BRIDGE if r["bridge"] else 0)
    for b in buildings:
        for i in W.building_cells(g, b):
            bits[i] |= W.BUILDING
    issues.append({"kind": "steep_cells", "count": steep, "note": f"terrain cells over {steep_deg} degrees"})
    return g, bits


def _wall_bits(g, bits, walls):
    for wl in walls:                                   # a grid cell wide, so a thin wall never slips between centres
        for i in W.line_cells(g, wl["points"], g["cell"]):
            bits[i] |= W.WALL


def _wavy(ring, seed, amp=3.0, step=4.0):
    """The ring densified every `step` metres, each point moved up to `amp` metres by smooth noise:
    a natural edge instead of OSM's straight one."""
    out = []
    for a, b in zip(ring, ring[1:] + ring[:1]):
        k = max(1, int(math.hypot(b[0] - a[0], b[1] - a[1]) / step))
        for t in range(k):
            x, y = a[0] + (b[0] - a[0]) * t / k, a[1] + (b[1] - a[1]) * t / k
            out.append((x + (_noise(x / 20, y / 20, seed) - 0.5) * 2 * amp, y + (_noise(x / 20, y / 20, seed + 1) - 0.5) * 2 * amp))
    return out


def _patches(half, seed, every, chance, radii):
    """Blobs ([ring]) scattered over the map: in each `every`-metre cell one with `chance`."""
    rng, out = random.Random(seed), []
    for gy in range(int(-half // every), int(half // every)):
        for gx in range(int(-half // every), int(half // every)):
            if rng.random() < chance:
                cx, cy, r = (gx + rng.random()) * every, (gy + rng.random()) * every, rng.uniform(*radii)
                ring = [(cx + math.cos(a) * r * rng.uniform(0.7, 1.2), cy + math.sin(a) * r * rng.uniform(0.7, 1.2))
                        for a in (k * math.tau / 10 for k in range(10))]
                out.append([_wavy(ring, seed, amp=r * 0.2, step=r * 0.3)])
    return out


def _ground(region, landuse, buildings, roads, water, trees, hedges=(), tall=(), season=None):
    """The ground picture (RGB): land use with natural edges, dry-grass and bare-soil patches, crop
    rows on farmland in plots between earth bunds, tall-grass patches, a gentle tone variation, shade
    under tree crowns, then water, building yards (the whole hedged compound) and roads crisp on top:
    unpaved ones with wheel ruts (and puddles in them in the wet season, season None), paved ones
    with potholes. `season`: a SEASONS palette over the region's. Stored south row first, the way a render texture is, since that is how
    the sprite_stack terrain pass reads it."""
    half, tx = region["half"], region.get("texel", 1.0)
    seasons = region.get("seasons", SEASONS)
    pal = {k: bytes(v) for k, v in {**PALETTE, **{k: tuple(v) for k, v in region.get("palette", {}).items()},
                                    **{k: tuple(v) for k, v in seasons.get(season, {}).items()}}.items()}
    g = {"cell": tx, "columns": round(2 * half / tx), "rows": round(2 * half / tx), "origin": [-half, -half]}
    cols = g["columns"]
    img = bytearray(pal["base"] * (cols * g["rows"]))

    def fill(rings, colour):
        for row, c0, c1 in W.polygon_spans(g, rings):
            img[(row * cols + c0) * 3:(row * cols + c1 + 1) * 3] = colour * (c1 - c0 + 1)

    def rows_fill(rings, colour, deg, spacing, plot):   # crop rows: alternate stripes of two tones, earth bunds round plots
        dark = bytes(int(c * 0.93) for c in colour)
        ca, sa = math.cos(math.radians(deg + 90)), math.sin(math.radians(deg + 90))
        for row, c0, c1 in W.polygon_spans(g, rings):
            y = -half + (row + 0.5) * tx
            for c in range(c0, c1 + 1):
                x = -half + (c + 0.5) * tx
                u, v = x * ca + y * sa, y * ca - x * sa
                img[(row * cols + c) * 3:(row * cols + c) * 3 + 3] = \
                    pal["bund"] if u % plot < 0.8 or v % (2 * plot) < 0.8 else dark if int(u // spacing) % 2 else colour
        for r in rings:
            line(r + r[:1], 1.0, pal["bund"])

    def line(pts, width, colour):
        for i in W.line_cells(g, pts, width):
            img[i * 3:i * 3 + 3] = colour

    for rings in _patches(half, 21, 90.0, 0.35, (10.0, 30.0)):   # dry grass on the bare land
        fill(rings, pal["dry_grass"])
    for cls in ["residential", "farmland", "grass", "wetland", "scrub", "orchard", "wood", "cemetery", "sand"]:
        for rings, oid in landuse.get(cls, ()):
            natural = cls in ("grass", "scrub", "wood", "wetland")
            wavy = [_wavy(r, 31) for r in rings] if natural else rings
            if natural:                                # a ragged half-tone fringe: the land changes over metres, not at a line
                fill([_wavy(r, 33, amp=10.0, step=6.0) for r in rings], bytes((a + b) // 2 for a, b in zip(pal[cls], pal["base"])))
            if cls == "farmland":
                h = _hash01(len(oid), sum(map(ord, oid)), 5)
                rows_fill(rings, pal[cls], _obb(rings[0])[4], 2.0 + h * 2.0, 24.0 + h * 20.0)
            else:
                fill(wavy, pal[cls])
        if cls == "grass":                             # bare soil showing through grass and scrub, under the woods
            for rings in _patches(half, 22, 120.0, 0.3, (6.0, 18.0)):
                fill(rings, pal["soil"])
        if cls == "scrub":                             # tall grass over grass, wetland and scrub, under the woods
            for ring in tall:
                fill([ring], pal["tall_grass"])
    block = max(1, round(DENSITY_CELL / tx))           # a gentle tone variation, constant over DENSITY_CELL squares
    luts = [bytes(min(255, int(v * (0.93 + 0.14 * k / 15))) for v in range(256)) for k in range(16)]
    fg = {"cell": block * tx, "columns": -(-cols // block), "rows": -(-g["rows"] // block), "origin": [-half, -half]}
    tone = _field(fg, lambda x, y: min(15, int(16 * (_noise(x / 45, y / 45, 41) * 0.6 + _noise(x / 11, y / 11, 42) * 0.4))))
    for row in range(g["rows"]):
        frow = (row // block) * fg["columns"]
        for k in range(fg["columns"]):
            a, b = (row * cols + k * block) * 3, (row * cols + min(cols, (k + 1) * block)) * 3
            img[a:b] = img[a:b].translate(luts[tone[frow + k]])
    for x, y, h, r, base, kind in trees:               # shade under the crown, darkest at the trunk
        rr = 0.8 * r
        for row in range(max(0, int((y - rr + half) / tx)), min(g["rows"], int((y + rr + half) / tx) + 1)):
            for c in range(max(0, int((x - rr + half) / tx)), min(cols, int((x + rr + half) / tx) + 1)):
                d = math.hypot(-half + (c + 0.5) * tx - x, -half + (row + 0.5) * tx - y) / rr
                if d < 1:
                    k, i = 1 - 0.22 * (1 - d), (row * cols + c) * 3
                    img[i:i + 3] = bytes(int(v * k) for v in img[i:i + 3])
    polys, lines = water
    for rings in polys:
        fill(rings, pal["water"])
    for pts, width in lines:
        line(pts, width, pal["water"])
    for hd in hedges:                                  # a hedged compound is swept earth
        fill([hd["points"]], pal["yard"])
    for b in buildings:
        fill([b["footprint"]], pal["yard"])
    ranked = sorted(roads, key=lambda r: (r["kind"] not in FOOT, r["paved"], r["width"]))
    for r in ranked:
        if r["kind"] not in FOOT:
            line(r["points"], r["width"] + 1.5, pal["road_edge"])
    wear = {**ROAD_WEAR, **region.get("road_wear", {})}
    for r in ranked:
        line(r["points"], r["width"], pal["path"] if r["kind"] in FOOT else pal["paved"] if r["paved"] else pal["unpaved"])
        if r["kind"] in FOOT:
            continue
        rng, every = random.Random(r["id"]), wear["potholes"] if r["paved"] else wear["puddles"]
        if not r["paved"]:                             # two wheel ruts
            for side in (-1, 1):
                line(_offset(r["points"], side * (r["width"] / 2 - wear["ruts"])), 0.6, pal["rut"])
        if r["paved"] or season is None:               # potholes; puddles in the ruts in the wet season
            for (ax, ay), (bx, by) in zip(r["points"], r["points"][1:]):
                for _ in range(int(math.hypot(bx - ax, by - ay) / every + rng.random())):
                    t, side = rng.random(), rng.choice((-1, 1)) * (r["width"] / 2 - wear["ruts"]) * (0.3 if r["paved"] else 1)
                    x, y = _offset([(ax, ay), (bx, by)], side)[0]
                    x, y = x + (bx - ax) * t, y + (by - ay) * t
                    rr = rng.uniform(0.4, 1.0) if r["paved"] else rng.uniform(0.6, 1.6)
                    fill([[(x + math.cos(k * 0.785) * rr * rng.uniform(0.7, 1.2), y + math.sin(k * 0.785) * rr * rng.uniform(0.7, 1.2))
                           for k in range(8)]], pal["pothole"] if r["paved"] else pal["puddle"])
    return g, img


# --- build ----------------------------------------------------------------------------------

def build(region_dir, out_dir):
    region = json.loads((region_dir / "region.json").read_text())
    over_path = region_dir / "overrides.json"
    overrides = json.loads(over_path.read_text()) if over_path.exists() else {}
    sha = hashlib.sha256()
    for p in [region_dir / "region.json", over_path, region_dir / "osm.json.gz", *sorted((region_dir / "dem").glob("*.png"))]:
        if p.exists():
            sha.update(p.name.encode() + p.read_bytes())
    osm = json.loads(gzip.decompress((region_dir / "osm.json.gz").read_bytes()))
    elements = sorted(osm["elements"], key=lambda e: (e["type"], e["id"]))
    for el in elements:
        el.setdefault("tags", {})
    f, half, issues = W.frame(region["lat"], region["lon"]), region["half"], []

    n, base, heights = _heights(region, f, region_dir, issues)
    raw = _buildings(f, half, elements, overrides, issues)
    nodes, roads = _roads(f, half, elements, overrides, issues)
    _clear_streets(raw, roads, issues)
    points = _points(f, half, elements, raw, roads, nodes, overrides)
    buildings = _finish_buildings(raw, issues)
    for p in points:                                   # a named place with no building near: OSM gap
        if p["kind"] == "settlement" and not any(abs(b["obb"][0] - p["x"]) < 300 and abs(b["obb"][1] - p["y"]) < 300 for b in buildings):
            issues.append({"kind": "settlement_without_buildings", "id": p["id"], "at": [p["x"], p["y"]],
                           "note": f"{p['name']}: no building within 300 m; add them in overrides.json"})
    water = _water(f, elements)
    grid, bits = _grid(region, n, heights, buildings, roads, water, issues)
    walls = _walls(f, overrides)
    _wall_bits(grid, bits, walls)
    landuse = _landuse(f, elements)
    hedges, props = _homesteads(region, half, buildings, roads, grid, bits)
    trees = _trees(region, f, half, elements, landuse, water, grid, bits)
    tall, tufts = _tall_grass(region, half, landuse, grid, bits)
    seasons = sorted(region.get("seasons", SEASONS))

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "heights.bin").write_bytes(struct.pack(f"<{len(heights)}f", *heights))
    (out_dir / "grid.bin").write_bytes(bytes(bits))
    for k in [None, *seasons]:                         # one picture in memory at a time (8000 square at 0.5 m)
        ground, img = _ground(region, landuse, buildings, roads, water, trees, hedges, tall, k)
        stride = ground["columns"] * 3
        write_png(out_dir / (f"ground_{k}.png" if k else "ground.png"), ground["columns"], ground["rows"],
                  b"".join(img[r * stride:(r + 1) * stride] for r in range(ground["rows"] - 1, -1, -1)))
        del img
    package = {
        "format": W.FORMAT, "version": W.VERSION, "name": region["name"], "title": region.get("title", region["name"]),
        "frame": {"lat": region["lat"], "lon": region["lon"], "half": half, "base_elevation": base,
                  "axes": "metres; x east, y south, z up from base_elevation", "projection": "equirectangular at the centre (WGS84 radii)"},
        "terrain": {"cell": region["cell"], "columns": n, "rows": n, "origin": [-half, -half], "file": "heights.bin"},
        "grid": {**grid, "file": "grid.bin", "bits": {"water": W.WATER, "steep": W.STEEP, "road": W.ROAD,
                                                       "bridge": W.BRIDGE, "building": W.BUILDING, "rubble": W.RUBBLE,
                                                       "wall": W.WALL}},
        "ground": {"file": "ground.png", "area": [-half, -half, 2 * half, 2 * half],
                   "seasons": {k: f"ground_{k}.png" for k in seasons}},
        "sources": {"osm": "OpenStreetMap contributors, ODbL", "osm_timestamp": osm.get("osm3s", {}).get("timestamp_osm_base", ""),
                    "dem": f"Terrarium tiles z{region['dem_zoom']} (Mapzen/AWS open data)", "hash": sha.hexdigest()},
        "nodes": dict(sorted(nodes.items())), "roads": roads, "buildings": buildings, "points": points,
        "walls": walls, "trees": trees, "hedges": hedges, "props": props, "tall_grass": tall, "tufts": tufts,
        "fields": [[[round(x, 1), round(y, 1)] for x, y in rings[0]] for rings, _ in landuse.get("farmland", ())], "issues": issues}
    (out_dir / "world.json").write_text(json.dumps(package, separators=(",", ":"), ensure_ascii=False), encoding="utf-8")
    counts = {}
    for i in issues:
        counts[i["kind"]] = counts.get(i["kind"], 0) + i.get("count", 1)
    print(f"{out_dir}: {len(buildings)} buildings, {len(roads)} road edges, {len(nodes)} nodes, {len(points)} points, "
          f"{len(walls)} walls, {len(trees)} trees, "
          f"heights {base} m + 0..{max(heights):.1f} m")
    for k, v in sorted(counts.items()):
        print(f"  {k}: {v}")


def check(region_dir):
    """Acceptance: the same sources give byte-identical packages."""
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        digests = []
        for k in range(2):
            out = Path(tmp) / str(k)
            build(region_dir, out)
            digests.append({p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(out.iterdir())})
        assert digests[0] == digests[1], f"builds differ: {digests}"
        print("deterministic:", {k: v[:12] for k, v in digests[0].items()})


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("command", choices=("fetch", "build", "check"))
    ap.add_argument("region", type=Path)
    ap.add_argument("out", type=Path, nargs="?")
    a = ap.parse_args()
    if a.command == "fetch":
        fetch(a.region)
    elif a.command == "build":
        if not a.out:
            ap.error("build needs an out dir")
        build(a.region, a.out)
    else:
        check(a.region)


if __name__ == "__main__":
    main()
