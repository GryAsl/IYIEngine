"""Offline: split a geo_world package into named neighbourhoods (zones.json, FORMAT.md "zones.json").

    python geo_zones.py <package dir>          write <package dir>/zones.json
    python geo_zones.py check <package dir>    load zones.json and assert it matches the package

Homes (house buildings) go to the nearest settlement point within SEED_RANGE; the rest are binned
into BIN-metre squares, and every busy square is a village of its own. Zones under MIN_HOMES merge
into the nearest one. Standard library only; the same package gives the same bytes.
"""

import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from geo_world import world as W  # noqa: E402

CELL = 50            # zone grid cell, m
REACH = 150.0        # a cell or a building joins the zone of the nearest home within this, m
SEED_RANGE = 700.0   # a home joins a settlement point at most this far away, m
BIN = 500.0          # leftover homes are binned into squares this wide, m
MIN_HOMES = 60       # a smaller zone merges into the nearest one
HOUSEHOLD = 6        # people per home (the estimate behind "people")
DUPLICATE = 25.0     # a point and a building of the same kind this close are one place, m
COMPASS = ["East", "South-East", "South", "South-West", "West", "North-West", "North", "North-East"]   # y is south


def _r(v):
    return round(v, 1)


def _centre(b):
    return b["obb"][0], b["obb"][1]


def _mean(homes):
    return sum(h[0] for h in homes) / len(homes), sum(h[1] for h in homes) / len(homes)


def _nearest(points, x, y):
    """Index of the (x, y, ...) point nearest to (x, y)."""
    return min(range(len(points)), key=lambda i: (points[i][0] - x) ** 2 + (points[i][1] - y) ** 2)


def _merge_small(groups):
    while len(groups) > 1:
        small = min(groups, key=lambda g: len(g["homes"]))
        if len(small["homes"]) >= MIN_HOMES:
            break
        groups.remove(small)
        groups[_nearest([_mean(g["homes"]) for g in groups], *_mean(small["homes"]))]["homes"] += small["homes"]
    return groups


def _names(groups):
    """The seed's OSM name; else '<nearest named zone> <direction>', else 'Village'. Unique."""
    named = [g for g in groups if g["name"]]
    used = {g["name"] for g in named}
    for g in sorted((g for g in groups if not g["name"]), key=lambda g: -len(g["homes"])):
        x, y = _mean(g["homes"])
        if named:
            n = named[_nearest([_mean(n["homes"]) for n in named], x, y)]
            nx, ny = _mean(n["homes"])
            base = f"{n['name']} {COMPASS[round(math.atan2(y - ny, x - nx) / (math.pi / 4)) % 8]}"
        else:
            base = "Village"
        name, k = base, 2
        while name in used:
            name, k = f"{base} {k}", k + 1
        g["name"] = name
        used.add(name)


def build(w):
    homes = [(*_centre(b), b["id"]) for b in w["buildings"] if b["kind"] == "house"]
    seeds = [(p["x"], p["y"], p["name"]) for p in w["points"] if p["kind"] == "settlement"]
    groups = {}                                   # key -> {"name", "homes": [(x, y, id)]}
    for h in homes:
        i = _nearest(seeds, h[0], h[1]) if seeds else None
        if i is not None and math.hypot(seeds[i][0] - h[0], seeds[i][1] - h[1]) <= SEED_RANGE:
            g = groups.setdefault(("s", i), {"name": seeds[i][2], "homes": []})
        else:
            g = groups.setdefault(("v", int(h[0] // BIN), int(h[1] // BIN)), {"name": "", "homes": []})
        g["homes"].append(h)
    groups = _merge_small([groups[k] for k in sorted(groups)])
    _names(groups)
    groups.sort(key=lambda g: g["name"])
    zone_of = {h[2]: i for i, g in enumerate(groups) for h in g["homes"]}

    reach = {}                                    # REACH-metre buckets of homes
    for h in homes:
        reach.setdefault((int(h[0] // REACH), int(h[1] // REACH)), []).append(h)

    def zone_at(x, y):
        near = [h for i in range(-1, 2) for j in range(-1, 2) for h in reach.get((int(x // REACH) + i, int(y // REACH) + j), ())]
        h = min(near, key=lambda h: (h[0] - x) ** 2 + (h[1] - y) ** 2, default=None)
        return zone_of[h[2]] if h and math.hypot(h[0] - x, h[1] - y) <= REACH else None

    zones = []
    for i, g in enumerate(groups):
        x, y = _mean(g["homes"])
        zones.append({"id": f"z{i}", "name": g["name"], "x": _r(x), "y": _r(y), "buildings": [], "homes": len(g["homes"]),
                      "people": len(g["homes"]) * HOUSEHOLD, "markets": [], "mosques": [], "schools": []})
    for b in w["buildings"]:
        z = zone_of.get(b["id"], zone_at(*_centre(b)))
        if z is not None:
            zones[z]["buildings"].append(b["id"])
    for key, point_kind, building_kind in (("markets", "market", None), ("mosques", "worship", "worship"), ("schools", "school", "school")):
        found = [(p["x"], p["y"]) for p in w["points"] if p["kind"] == point_kind]
        for b in w["buildings"]:
            c = _centre(b)
            if b["kind"] == building_kind and all(math.hypot(c[0] - p[0], c[1] - p[1]) >= DUPLICATE for p in found):
                found.append(c)
        for x, y in found:
            z = zone_at(x, y)
            zones[_nearest([(q["x"], q["y"]) for q in zones], x, y) if z is None else z][key].append([_r(x), _r(y)])
    for z in zones:
        for key in ("markets", "mosques", "schools"):
            z[key].sort()

    half = w["frame"]["half"]
    n = int(2 * half // CELL)
    grid = []
    for r in range(n):
        for c in range(n):
            z = zone_at(-half + (c + 0.5) * CELL, -half + (r + 0.5) * CELL)
            grid.append(-1 if z is None else z)
    return {"format": "geo_zones", "version": 1, "package": w["name"], "source_hash": w["sources"]["hash"], "cell": CELL,
            "origin": [-half, -half], "columns": n, "rows": n, "grid": grid, "zones": zones}


def check(path):
    w = W.load(path)
    z = json.loads((Path(path) / "zones.json").read_text(encoding="utf-8"))
    assert z["format"] == "geo_zones" and z["version"] == 1 and z["package"] == w["name"] and z["source_hash"] == w["sources"]["hash"]
    assert len(z["grid"]) == z["columns"] * z["rows"] and z["columns"] * z["cell"] == 2 * w["frame"]["half"]
    ids = [q["id"] for q in z["zones"]]
    assert ids == [f"z{i}" for i in range(len(ids))] and len({q["name"] for q in z["zones"]}) == len(ids)
    owned = set(z["grid"])
    assert owned <= set(range(-1, len(ids)))
    seen = set()
    for i, q in enumerate(z["zones"]):
        assert i in owned, f"{q['name']} owns no grid cell"
        assert all(b in w["building_ids"] for b in q["buildings"]) and not seen & set(q["buildings"]), f"{q['name']}: bad building ids"
        seen.update(q["buildings"])
    print(f"{path}/zones.json: {len(ids)} zones, {len(seen)} buildings: ok")


def main(path):
    z = build(W.load(path))
    out = Path(path) / "zones.json"
    out.write_text(json.dumps(z, separators=(",", ":"), sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    for q in z["zones"]:
        print(f"{q['id']:>4} {q['name']:<34} homes {q['homes']:>5} people {q['people']:>6} markets {len(q['markets'])} mosques {len(q['mosques'])} schools {len(q['schools'])}")
    print(f"{len(z['zones'])} zones -> {out}")
    check(path)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["check"] and len(args) == 2:
        check(args[1])
    elif len(args) == 1:
        main(args[0])
    else:
        sys.exit(__doc__)
