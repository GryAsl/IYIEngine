"""Dynamic obstruction checks on any geo_world map, run inside the engine (engine.Nav):

    from nav import checks
    checks.run(w)           -> [{"name", "ok", ...}]   (w is restored to how it was)

bridge: a route over a bridge; the bridge destroyed gives a legitimate other route (or an honest
"route_failed"), repaired gives the first route back. collapse: a destroyed house spills debris
that closes the street to cars (route and edge width) and slows walkers. walker: a walker on its
way replans round a wreck dropped on its route. landing: a legal site, and another once a vehicle
parks on it. load: hundreds of route requests cost the game side little; the worker answers all.
traffic: 150 calm civilian cars drive between junctions for 300 s (a new goal on arrival); none may stand
still a minute (a jam, a locked junction, a car nobody overtakes). 10 player trucks among them are sent to
points off the road, as a player's right click does: each must stop within 3 m of the point (or the nearest
point it fits: 80% within 3 m). walkers (default 300) walk between open points 50 to 300 m apart; none may stall
a minute either.
classes: engine.nav_classes() is still what classes.json (read by offline tools) says.
"""

import collections
import json
import math
import os
import random
import time

import engine
from geo_world import world as geo

DEBRIS = 1   # engine.Nav extra-layer bit: debris round a collapsed building


def run(w):
    saved = geo.save_state(w)
    try:
        return [_bridge(w), _collapse(w), _walker(w), _landing(w), _load(w), traffic(w), _classes()]
    finally:
        geo.load_state(w, saved)


def _classes():
    with open(os.path.join(os.path.dirname(__file__), "..", "..", "classes.json")) as f:
        return {"name": "classes", "ok": json.load(f) == engine.nav_classes()}


def _route(nav, start, goal, cls, mode="road"):
    t = nav.request(start, goal, cls, mode)
    nav.poll()
    return nav.take_result(t)


def _bridge(w):
    """Every bridge with a road route over it, closed and reopened; a bridge with another way round
    is reported first (a river with one crossing honestly gives route_failed)."""
    nav = engine.Nav(w, wait=True)
    tried = []
    for r in (r for r in w["roads"] if r["bridge"]):
        e = nav.edge(r["id"])
        start, goal = w["nodes"][r["a"]], w["nodes"][r["b"]]
        first = _route(nav, start, goal, "truck")
        if not first["found"] or e not in first["edges"]:
            continue
        geo.set_condition(w, r["id"], "destroyed")
        nav.sync()
        closed = _route(nav, start, goal, "truck")
        failed = any(ev["type"] == "route_failed" for ev in nav.events())
        geo.set_condition(w, r["id"], "intact")
        nav.sync()
        again = _route(nav, start, goal, "truck")
        ok = (e not in closed["edges"] if closed["found"] else failed) and again["found"] and e in again["edges"]
        tried.append({"bridge": r["id"], "ok": ok, "first_m": round(first["cost"]), "reopened_m": round(again["cost"]),
                      "closed": round(closed["cost"]) if closed["found"] else "route_failed"})
    if not tried:
        return {"name": "bridge", "ok": False, "why": "no bridge with a road route over it"}
    tried.sort(key=lambda t: t["closed"] == "route_failed")
    return {"name": "bridge", "ok": all(t["ok"] for t in tried) and tried[0]["closed"] != "route_failed", "bridges": tried}


def _collapse(w):
    nav = engine.Nav(w, wait=True)
    for b in w["buildings"]:
        if b["kind"] != "house" or b["obb"][2] < 6:
            continue
        cx, cy = b["obb"][0], b["obb"][1]
        hit = nav.snap(cx, cy, 9.0)
        if not hit or w["roads"][hit[0]]["kind"] != "residential":
            continue
        e = hit[0]
        before = nav.edge_state(e)[1]
        geo.set_condition(w, b["id"], "destroyed")
        nav.sync()
        after = nav.edge_state(e)[1]
        debris = nav.obstacle("debris:" + b["id"])
        car_cells = [nav.cell_index(*p) for p in _route(nav, (cx - 60, cy), (cx + 60, cy), "car", "free")["points"]]
        on_debris = [i for i in car_cells if i is not None and nav.cell(i)[1] >> DEBRIS & 1]
        walkable = nav.passable("civilian", *_debris_point(nav, w, debris)) if debris else False
        geo.set_condition(w, b["id"], "intact")
        nav.sync()
        restored = nav.edge_state(e)[1] == before and nav.obstacle("debris:" + b["id"]) is None
        ok = bool(debris) and after < before and not on_debris and walkable and restored
        return {"name": "collapse", "ok": ok, "building": b["id"], "road_clear_m": [before, after], "restored": restored}
    return {"name": "collapse", "ok": False, "why": "no house by a narrow road"}


def _debris_point(nav, w, debris):
    g = w["grid"]
    for i in debris["cells"]:
        if not nav.cell(i)[0] & geo.BUILDING:
            return g["origin"][0] + (i % g["columns"] + 0.5) * g["cell"], g["origin"][1] + (i // g["columns"] + 0.5) * g["cell"]
    return 0.0, 0.0


def _walker(w):
    nav = engine.Nav(w, wait=True)
    r = max((r for r in w["roads"] if r["kind"] in ("residential", "unclassified") and not r["bridge"]), key=lambda r: r["length"])
    start, goal = r["points"][0], r["points"][-1]
    p = nav.spawn_walker("w1", "infantry", *start)
    nav.walk("w1", goal)
    for _ in range(30):
        nav.step(1 / 30)
    if not p.path:
        return {"name": "walker", "ok": False, "why": "no first route"}
    mid, (ux, uy) = _ahead(p)
    nav.set_obstacle("wreck1", [(mid[0], mid[1], 14.0, 14.0, math.degrees(math.atan2(uy, ux)))], "blocked")
    for _ in range(30):
        nav.step(1 / 30)
    replanned = any(ev["type"] == "replan" and ev.get("walker") == "w1" for ev in nav.events())
    clear = nav.path_closed("infantry", p.path, max(p.i - 1, 0)) is None and bool(p.path)
    return {"name": "walker", "ok": replanned and clear, "replanned": replanned, "new_path_clear": clear}


def _ahead(p):
    """A point 40 m further along a walker's path, and the path's direction there."""
    pts, left = p.path[p.i:], 40.0
    for a, b in zip(pts, pts[1:]):
        d = math.hypot(b[0] - a[0], b[1] - a[1])
        if d >= left:
            return (a[0] + (b[0] - a[0]) * left / d, a[1] + (b[1] - a[1]) * left / d), ((b[0] - a[0]) / d, (b[1] - a[1]) / d)
        left -= d
    return pts[-1], (1.0, 0.0)


def _landing(w):
    nav = engine.Nav(w, wait=True)
    market = next((p for p in w["points"] if p["kind"] == "market"), w["points"][0])
    site = nav.landing_site(market["x"], market["y"])
    if site is None:
        return {"name": "landing", "ok": False, "why": "no site near the market"}
    nav.spawn_vehicle("parked", "truck", site[0], site[1])
    nav.park("parked")
    other = nav.landing_site(market["x"], market["y"])
    nav.spawn_heli("h1", market["x"] + 600, market["y"])
    target = nav.fly_to("h1", market["x"], market["y"])
    for _ in range(60 * 15):
        nav.step(1 / 15)
    landed = nav.helis["h1"].state == "landed"
    ok = other is not None and math.hypot(other[0] - site[0], other[1] - site[1]) >= engine.nav_classes()["heli"]["pad"] and landed
    return {"name": "landing", "ok": ok, "site": [round(v) for v in site], "after_parking": [round(v) for v in other] if other else None,
            "heli_target": [round(v) for v in target] if target else None, "landed": landed}


def _load(w):
    nav = engine.Nav(w, wait=False)
    nodes = [w["nodes"][n] for n in nav.node_ids]
    n = len(nodes)
    t0 = time.perf_counter()
    tickets = [nav.request(nodes[(k * 7919) % n], nodes[(k * 104729 + 13) % n], ("truck", "car", "civilian")[k % 3],
                            "free" if k % 3 == 2 else "road") for k in range(300)]
    issue_ms = (time.perf_counter() - t0) * 1000
    t1 = time.perf_counter()
    results = {}
    while nav.pending:
        nav.poll()
        time.sleep(0.001)
    nav.poll()
    wall_ms = (time.perf_counter() - t1) * 1000
    for t in tickets:
        r = nav.take_result(t)
        if r is not None:
            results[t] = r
    answered = len(results)
    return {"name": "load", "ok": answered == len(tickets) and issue_ms / len(tickets) < 1.0, "requests": len(tickets),
            "answered": answered, "found": sum(r["found"] for r in results.values()),
            "issue_ms_total": round(issue_ms, 1), "worker_wall_ms": round(wall_ms)}


def traffic(w, cars=150, seconds=300.0, seed=7, dt=0.1, trace=(), soldiers=10, walkers=300, wait=True):
    """Calm civilian traffic: cars between junctions where a car fits, 150 to 600 m apart, a new goal on arrival.
    stalled: units on their way that moved under 2 m in the last minute (sampled every second)."""
    rng = random.Random(seed)
    nav = engine.Nav(w, seed=seed, wait=wait)       # wait=False: the nav thread as in play (measures ms_worker; not reproducible)
    fits = {n for r in w["roads"] if r["width"] >= 3.5 for n in (r["a"], r["b"])}
    edge = max(max(abs(x), abs(y)) for x, y in w["nodes"].values()) - 5.0   # road ends on the map's border: where traffic leaves the map
    nodes = [n for n in nav.node_ids if n in fits and max(abs(w["nodes"][n][0]), abs(w["nodes"][n][1])) < edge]
    starts = []
    for n in rng.sample(nodes, len(nodes)):            # junctions 15 m apart
        x, y = w["nodes"][n]
        if len(starts) < cars and all(math.hypot(x - a, y - b) > 15.0 for a, b in starts):
            starts.append((x, y))
    for k, (x, y) in enumerate(starts):
        nav.spawn_vehicle(f"car{k}", rng.choice(("car", "car", "pickup", "truck")), x, y, rng.uniform(0, 2 * math.pi), side="civilian")
        nav.drive(f"car{k}", _goal(w, nodes, rng, x, y))
    for k in range(soldiers):                         # player trucks, each sent off the road
        x, y = w["nodes"][rng.choice(nodes)]
        nav.spawn_vehicle(f"army{k}", "truck", x, y, rng.uniform(0, 2 * math.pi), side="player")
        nav.drive(f"army{k}", _off_road(nav, w, nodes, rng, x, y))
    for k in range(walkers):                          # people between open points
        x, y = _open_point(nav, w, nodes, rng, "civilian")
        nav.spawn_walker(f"p{k}", "civilian", x, y, side="civilian")
        nav.walk(f"p{k}", _open_point(nav, w, nodes, rng, "civilian", x, y, 50.0, 300.0))
    seen, kinds, stalled_max, t0, traced, failed, misses, trips = {}, {}, 0, time.perf_counter(), [], [], [], 0
    miss_at = []
    walks, wseen, wstalled_max, worst, hits, fast, spent, worker = 0, {}, 0, 0.0, {}, 0, 0.0, 0.0
    for k in range(int(seconds / dt)):
        a = time.perf_counter()
        nav.step(dt)
        spent += time.perf_counter() - a
        worker += nav.worker_ms
        worst = max(worst, time.perf_counter() - a) if k > 10 else 0.0   # the first steps answer every first route
        for e in nav.events():
            kinds[e["type"]] = kinds.get(e["type"], 0) + 1
            if e["type"] == "collision":
                hit = ("building" if str(e["b"]).startswith("b:") else "vehicle") + (" army" if "army" in e["a"] + str(e["b"]) else "")
                if hit.startswith("building"):          # what the driver was doing
                    v = nav.vehicles[e["a"]]
                    hit += f" {v.state}/{v.mode}/{'pass' if abs(v.lat) > 0.1 else ''}"
                hits[hit] = hits.get(hit, 0) + 1
                fast += e["speed"] >= 3.0
            if e["type"] == "route_failed":
                failed.append(e)
            if any(e.get(key) in trace for key in ("vehicle", "walker", "a", "b")):
                traced.append(e)
            if e["type"] == "arrived":
                v = nav.vehicles[e["vehicle"]]
                if v.id.startswith("army"):
                    trips += 1
                    misses.append(round(math.hypot(v.goal[0] - v.x, v.goal[1] - v.y), 1))
                    if misses[-1] > 3.0:
                        miss_at.append([v.id, round(nav.time), misses[-1], round(math.hypot(v.goal[0] - v.path[-1][0], v.goal[1] - v.path[-1][1]), 1),
                                        v.mode, len(v.path), [round(c) for c in v.goal], nav.passable("truck", *v.goal)])
                    nav.drive(v.id, _off_road(nav, w, nodes, rng, v.x, v.y))
                else:
                    nav.drive(v.id, _goal(w, nodes, rng, v.x, v.y))
        if k % int(1 / dt) == 0:
            for v in nav.vehicles.values():
                if v.state == "stuck":                    # no way there: as a game does, another errand
                    nav.drive(v.id, _goal(w, nodes, rng, v.x, v.y))
                seen.setdefault(v.id, []).append((nav.time, v.x, v.y, v.state))
                if v.id in trace:
                    traced.append([v.id, round(nav.time), round(v.x, 1), round(v.y, 1), v.state, v.mode, round(v.vel, 1), round(v.s, 1),
                                   round(v.lat, 1), len(v.path), round(math.degrees(v.h)), v.why, _walker_ahead(nav, v)])
            stalled_max = max(stalled_max, len(_stalled(seen, nav.time)))
            for p in nav.walkers.values():
                if p.state == "idle":                 # there: walk on
                    walks += 1
                    nav.walk(p.id, _open_point(nav, w, nodes, rng, "civilian", p.x, p.y, 50.0, 300.0))
                if p.id in trace:
                    traced.append([p.id, round(nav.time), round(p.x, 1), round(p.y, 1), p.state, len(p.path), p.i,
                                   p.path[p.i] if p.i < len(p.path) else None, p.goal])
                wseen.setdefault(p.id, []).append((nav.time, p.x, p.y, "drive" if p.state in ("walk", "wait") else p.state))
            wstalled_max = max(wstalled_max, len(_stalled(wseen, nav.time)))
    ms = (time.perf_counter() - t0) * 1000
    stalled = _stalled(seen, nav.time)
    wstalled = _stalled(wseen, nav.time)
    return {"name": "traffic", "ok": not stalled and not wstalled and kinds.get("arrived", 0) >= len(starts) and trips >= soldiers
            and sum(m < 3.0 for m in misses) >= 0.8 * len(misses), "cars": len(starts), "walkers": walkers, "walks": walks,
            "walkers_stalled_end": len(wstalled), "walkers_stalled_max": wstalled_max,
            "walkers_stalled_at": [[i, round(x), round(y), nav.walkers[i].state, len(nav.walkers[i].path)] for i, x, y, st in wstalled[:8]],
            "ms_step_max": round(worst * 1000, 1), "collisions": hits, "collisions_3ms": fast, "soldier_trips": trips, "soldier_miss_m": max(misses, default=None),
            "soldiers": [[v.id, v.state, v.mode, round(v.vel, 1), v.goal and round(math.hypot(v.goal[0] - v.x, v.goal[1] - v.y)), len(v.path),
                          round(v.s), _brief(v)] for v in nav.vehicles.values() if v.id.startswith("army")],
            "misses": misses, "miss_at": miss_at[:10],
            "arrived": kinds.get("arrived", 0), "stalled_end": len(stalled), "stalled_max": stalled_max,
            "stalled_at": [_why(nav, vid) for vid, x, y, st in stalled[:12]],
            "events": kinds, "failed": failed[:6],
            "doing": collections.Counter(_doing(v) for v in nav.vehicles.values()).most_common(),
            "failed_most": collections.Counter(f"{e['cls']}/{e['mode']} {round(e['start'][0])},{round(e['start'][1])}" for e in failed).most_common(5), "ms_step": round(ms / (seconds / dt), 2), "ms_nav": round(spent * 1000 / (seconds / dt), 2), "ms_worker": round(worker / (seconds / dt), 2), **({"trace": traced[-300:]} if trace else {})}


def _off_road(nav, w, nodes, rng, x, y):
    """A point 15 to 30 m off a road near a junction 150 to 600 m away that a truck can reach from that junction
    (a right click on open ground, not into a walled yard)."""
    for _ in range(40):
        gx, gy = _goal(w, nodes, rng, x, y)
        a, d = rng.uniform(0, 2 * math.pi), rng.uniform(15.0, 30.0)
        p = (gx + math.cos(a) * d, gy + math.sin(a) * d)
        if nav.passable("truck", *p) and not nav.cell(nav.cell_index(*p))[0] & geo.ROAD:   # open ground, not a road
            r = _route(nav, (gx, gy), p, "truck", "free")
            if r and r["found"] and math.hypot(r["points"][-1][0] - p[0], r["points"][-1][1] - p[1]) < 2.0:
                return p
    return _goal(w, nodes, rng, x, y)


def _walker_ahead(nav, v):
    """The nearest walker within 10 m in front of v (trace): id, distance, state, path left."""
    best = None
    for p in nav.walkers.values():
        dx, dy = p.x - v.x, p.y - v.y
        d = math.hypot(dx, dy)
        if d < 10.0 and dx * math.cos(v.h) + dy * math.sin(v.h) > 0.0 and (best is None or d < best[1]):
            best = [p.id, round(d, 1), p.state, len(p.path) - p.i]
    return best


def _doing(v):
    """What a vehicle is doing, in one word (traffic's summary)."""
    if v.state != "drive":
        return v.state
    if abs(v.vel) > 0.5:
        return "moving"
    j = v.why["junction"]
    return "junction" if j and not j[1] else "queued" if v.why["queued"] > 0 else "blocked:" + (v.why["blocked_by"] or "?")[:6]


def _open_point(nav, w, nodes, rng, cls, x=None, y=None, near=0.0, far=1e9):
    """An open point for cls within [near, far] metres of (x, y) (anywhere when x is None), near a junction and reachable from it."""
    for _ in range(60):
        n = w["nodes"][rng.choice(nodes)]
        p = (n[0] + rng.uniform(-8, 8), n[1] + rng.uniform(-8, 8))     # beside a street
        if (nav.passable(cls, *p) and (x is None or near < math.hypot(p[0] - x, p[1] - y) < far)
                and (_route(nav, n, p, cls, "free") or {"found": True})["found"]):   # not a courtyard shut in by buildings
            return p
    return nav.open_near(cls, w["nodes"][rng.choice(nodes)])


def _goal(w, nodes, rng, x, y):
    near = [n for n in nodes if 150.0 < math.hypot(w["nodes"][n][0] - x, w["nodes"][n][1] - y) < 600.0]
    return w["nodes"][rng.choice(near or nodes)]


def _why(nav, vid):
    """A stalled car, what it says holds it, and the nearest vehicle in front of it (within 12 m)."""
    v = nav.vehicles[vid]
    ahead = None
    for o in nav.vehicles.values():
        dx, dy = o.x - v.x, o.y - v.y
        d = math.hypot(dx, dy)
        if o is not v and d < 12.0 and dx * math.cos(v.h) + dy * math.sin(v.h) > 0.3 * d and (ahead is None or d < ahead[1]):
            ahead = (o, d)
    out = [vid, round(v.x), round(v.y), v.state, v.cls, _brief(v)]
    if ahead:
        out.append(["ahead", ahead[0].id, round(ahead[1], 1), round(math.degrees(ahead[0].h - v.h)) % 360])
    chain, o = [], v
    while o.why["wait_for"] and len(chain) < 6:          # who it waits for, and who that one waits for
        o = nav.vehicles.get(o.why["wait_for"])
        if o is None or any(c[0] == o.id for c in chain):
            break
        chain.append([o.id, round(math.hypot(o.x - v.x, o.y - v.y), 1)] + _brief(o))
    return out + chain


def _brief(v):
    w, j = v.why, v.why["junction"]
    return [v.state, round(w["still"], 1), w["wait_for"], j and [j[0], j[1], round(j[2], 1)]]


def _stalled(seen, now):
    """Units on their way (drive, stuck, reverse) that never left a 2 m circle in the last 60 s."""
    out = []
    for vid, s in seen.items():
        t, x, y, st = s[-1]
        old = [p for p in s if p[0] <= now - 60.0]
        recent = [p for p in s if p[0] > now - 60.0]
        if old and all(p[3] in ("drive", "stuck", "reverse") and math.hypot(p[1] - old[-1][1], p[2] - old[-1][2]) < 2.0 for p in recent):
            out.append((vid, x, y, st))
    return out
