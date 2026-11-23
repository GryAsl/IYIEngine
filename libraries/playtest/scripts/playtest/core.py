"""Deterministic playtest core: record a run as initial state + ordered commands at ticks + state
hashes, replay it, seek to any tick (restore a checkpoint, replay forward), shrink a failure.

Engine-generic and stdlib only. A game supplies an adapter module (docs/PLAYTEST.md "Adapter"):

    TICK_HZ, create(scenario, seed), snapshot(state), restore(snap), tick(state),
    actions(state), execute(state, action) -> (ok, reason), observe(state), canonical(state),
    check(state) -> [{"id", "detail"}], metrics(state), day(state), finished(state) -> reason | None,
    progress(state), POLICIES {name: choose(obs, legal, rng, memory) -> [action]},
    optional inject(state, spec) and features(state) -> set of feature-state names (coverage)

Tick order (also the replay order): commands queued for tick N run in `seq` order, then the game's
fixed step N, then checks; the state "at tick N" is the state after N steps.
"""

import copy
import gzip
import hashlib
import json
import os
import random
import time
import traceback

FORMAT, SCHEMA = "iyi-replay", 1


# --- canonical form, hash, diff ------------------------------------------------------------------

def canonical_json(value) -> str:
    """Sorted keys, no spaces, floats as repr (exact round trip); sets and tuples are refused/listed."""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=True, default=_refuse)


def _refuse(value):
    raise TypeError(f"not canonical JSON: {type(value).__name__}")


def digest(value) -> str:
    return hashlib.sha256(canonical_json(value).encode()).hexdigest()


def first_diff(a, b, path="$"):
    """The first JSON path where a and b differ, with both values (None when equal)."""
    if type(a) is not type(b) and not (isinstance(a, (int, float)) and isinstance(b, (int, float))):
        return {"path": path, "a": a, "b": b}
    if isinstance(a, dict):
        for key in sorted(set(a) | set(b), key=str):
            if key not in a or key not in b:
                return {"path": f"{path}.{key}", "a": a.get(key, "<missing>"), "b": b.get(key, "<missing>")}
            found = first_diff(a[key], b[key], f"{path}.{key}")
            if found:
                return found
        return None
    if isinstance(a, list):
        for i, (x, y) in enumerate(zip(a, b)):
            found = first_diff(x, y, f"{path}[{i}]")
            if found:
                return found
        return None if len(a) == len(b) else {"path": f"{path}.length", "a": len(a), "b": len(b)}
    return None if a == b else {"path": path, "a": a, "b": b}


def diff_all(a, b, path="$", limit=50, out=None):
    """Every differing leaf path (up to `limit`): the inspector's before/after view."""
    out = [] if out is None else out
    if len(out) >= limit:
        return out
    if isinstance(a, dict) and isinstance(b, dict):
        for key in sorted(set(a) | set(b), key=str):
            diff_all(a.get(key, "<missing>"), b.get(key, "<missing>"), f"{path}.{key}", limit, out)
    elif isinstance(a, list) and isinstance(b, list) and len(a) == len(b):
        for i, (x, y) in enumerate(zip(a, b)):
            diff_all(x, y, f"{path}[{i}]", limit, out)
    elif a != b:
        out.append({"path": path, "before": a, "after": b})
    return out


# --- bundle files ---------------------------------------------------------------------------------

def _write(path: str, text: str) -> None:
    """Atomic: a file that exists is complete."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    temp = path + ".tmp"
    opener = gzip.open if path.endswith(".gz") else open
    with opener(temp, "wt", encoding="utf-8") as f:
        f.write(text)
    os.replace(temp, path)


def _read(path: str) -> str:
    opener = gzip.open if path.endswith(".gz") else open
    with opener(path, "rt", encoding="utf-8") as f:
        return f.read()


def write_json(path: str, value) -> None:
    _write(path, json.dumps(value, indent=1, sort_keys=True, default=str))


def read_json(path: str):
    return json.loads(_read(path))


def _jsonl(rows) -> str:
    return "".join(canonical_json(r) + "\n" for r in rows)


def save_bundle(folder: str, rec: dict) -> str:
    """manifest.json, initial_state.json.gz, actions.jsonl, hashes.jsonl, checkpoints/, report.json."""
    write_json(os.path.join(folder, "manifest.json"), rec["manifest"])
    _write(os.path.join(folder, "initial_state.json.gz"), canonical_json(rec["initial"]))
    _write(os.path.join(folder, "actions.jsonl"), _jsonl(rec["actions"]))
    _write(os.path.join(folder, "hashes.jsonl"), _jsonl({"tick": t, "hash": h} for t, h in sorted(rec["hashes"].items())))
    index = []
    for tick, snap in sorted(rec["checkpoints"].items()):
        name = f"checkpoints/{tick:09d}.json.gz"
        _write(os.path.join(folder, name), canonical_json(snap))
        index.append({"tick": tick, "file": name, "hash": rec["hashes"].get(tick)})
    write_json(os.path.join(folder, "checkpoints.json"), index)
    write_json(os.path.join(folder, "report.json"), rec["report"])
    return folder


def load_bundle(folder: str) -> dict:
    """The recording back; refuses another format or a newer schema with a readable reason."""
    folder = os.path.abspath(folder)
    path = os.path.join(folder, "manifest.json")
    if not os.path.isfile(path):
        raise ValueError(f"not a replay bundle (no manifest.json): {folder}")
    manifest = read_json(path)
    if manifest.get("format") != FORMAT or not isinstance(manifest.get("schema"), int) or manifest["schema"] > SCHEMA:
        raise ValueError(f"unsupported replay: format {manifest.get('format')!r} schema {manifest.get('schema')!r} "
                         f"(this tool reads {FORMAT} schema <= {SCHEMA})")
    lines = lambda name: [json.loads(l) for l in _read(os.path.join(folder, name)).splitlines() if l.strip()]
    checkpoints = {}
    for entry in read_json(os.path.join(folder, "checkpoints.json")):
        file = os.path.normpath(os.path.join(folder, entry["file"]))
        if not file.startswith(folder + os.sep):  # a trace never names a path outside its own folder
            raise ValueError(f"checkpoint path escapes the bundle: {entry['file']!r}")
        checkpoints[entry["tick"]] = file
    return {
        "folder": folder, "manifest": manifest,
        "initial": json.loads(_read(os.path.join(folder, "initial_state.json.gz"))),
        "actions": lines("actions.jsonl"),
        "hashes": {r["tick"]: r["hash"] for r in lines("hashes.jsonl")},
        "checkpoints": checkpoints,  # tick -> file, loaded on demand
        "report": read_json(os.path.join(folder, "report.json")) if os.path.isfile(os.path.join(folder, "report.json")) else {},
    }


# --- recording a run ------------------------------------------------------------------------------

DEFAULTS = {
    "max_ticks": 30000, "max_days": None, "decision_interval_ticks": 10, "hash_every_ticks": 30,
    "snapshot_every_ticks": 900, "stall_days": 20, "wall_seconds": 600, "faults": [],
}


def _signature(kind: str, ident: str, extra: str = "") -> str:
    return f"{kind}:{ident}" + (f"@{extra}" if extra else "")


def _exception_failure(exc: BaseException, tick: int, where: str) -> dict:
    frames = traceback.extract_tb(exc.__traceback__)
    last = frames[-1] if frames else None
    at = f"{os.path.basename(last.filename)}:{last.lineno}" if last else "?"
    return {"kind": "exception", "id": type(exc).__name__, "detail": f"{exc!r} in {where}", "tick": tick,
            "signature": _signature("exception", type(exc).__name__, at), "traceback": traceback.format_exc()}


def _execute(adapter, state, action):
    """execute() returns (ok, reason) or (ok, reason, oracle violations)."""
    res = adapter.execute(state, action)
    return res[0], res[1], list(res[2]) if len(res) > 2 else []


def _invariant(found, tick):
    if not found:
        return None
    v = found[0]
    return {"kind": "invariant", "id": v["id"], "detail": v["detail"], "tick": tick,
            "signature": _signature("invariant", v["id"]), "all": found[:20]}


def _apply(adapter, state, action, tick, seq, watch=True):
    """Run one command; a rejected one must leave the canonical state as it was (`watch`: check that;
    a replay only pays for it on commands the recording rejected)."""
    before = digest(adapter.canonical(state)) if watch else None
    ok, reason, found = _execute(adapter, state, action)
    entry = {"tick": tick, "seq": seq, "action": action, "ok": bool(ok), "reason": reason}
    failure = _invariant(found, tick + 1)
    if failure is None and watch and not ok and digest(adapter.canonical(state)) != before:
        failure = {"kind": "rejected_mutation", "id": action.get("type", "?"), "tick": tick,
                   "detail": f"rejected {action} still changed the state",
                   "signature": _signature("rejected_mutation", action.get("type", "?"))}
    return entry, failure


def _stalled(adapter, state, memo, day, stall_days, tick):
    """Soft lock check at a day change, shared by record and replay: memo = [last progress, day it changed]."""
    progress = adapter.progress(state)
    if progress != memo[0]:
        memo[:] = [progress, day]
    elif stall_days and day - memo[1] >= stall_days:
        return {"kind": "softlock", "id": "no_progress", "tick": tick,
                "detail": f"no progress for {day - memo[1]} days (since day {memo[1]})",
                "signature": _signature("softlock", "no_progress")}


def record(adapter, scenario: dict, seed: int, policy: str, policy_seed: int, config: dict | None = None,
           manifest_extra: dict | None = None, choose=None) -> dict:
    """Play one run with a policy and record it. `choose` overrides the adapter's policy (e.g. an LLM
    client); whatever it returns is recorded, so the replay never needs it again."""
    cfg = dict(DEFAULTS, **(config or {}))
    choose = choose or adapter.POLICIES[policy]
    random.seed(seed)  # cosmetic randomness the game leaves on the global generator
    rng = random.Random(policy_seed)
    memory = {}  # the policy's own notes for this run
    features = getattr(adapter, "features", lambda s: ())
    state = adapter.create(scenario, seed)
    initial = adapter.snapshot(state)
    seen = set(features(state))
    hashes = {0: digest(adapter.canonical(state))}
    checkpoints = {0: initial}
    actions, daily, failure, end_reason = [], [], None, None
    accepted = rejected = 0
    faults = {f["tick"]: f for f in cfg["faults"]}
    day = adapter.day(state)
    stall = [adapter.progress(state), day]
    stall_days = 0 if getattr(choose, "expects_stall", False) else cfg["stall_days"]
    started = time.perf_counter()
    tick = 0
    while tick < cfg["max_ticks"]:
        tick_found = []
        try:
            if tick in faults:  # labelled fault injection (acceptance demo), recorded like a command
                entry = {"tick": tick, "seq": -1, "action": {"type": "__inject__", **faults[tick]}, "ok": True, "reason": "fault"}
                adapter.inject(state, faults[tick])
                actions.append(entry)
            if tick % cfg["decision_interval_ticks"] == 0:
                obs = dict(adapter.observe(state), scenario=scenario)
                for seq, action in enumerate(choose(obs, adapter.actions(state), rng, memory) or []):
                    entry, failure = _apply(adapter, state, action, tick, seq)
                    actions.append(entry)
                    accepted += entry["ok"]
                    rejected += not entry["ok"]
                    if failure:
                        break
            if failure is None:
                tick_found = adapter.tick(state) or []
        except Exception as exc:
            failure = _exception_failure(exc, tick, "command" if tick in {a["tick"] for a in actions[-5:]} else "tick")
        tick += 1
        if failure is None:
            failure = _invariant(tick_found + adapter.check(state), tick)
        if failure is None and (tick % cfg["hash_every_ticks"] == 0 or (actions and actions[-1]["tick"] == tick - 1)):
            hashes[tick] = digest(adapter.canonical(state))
        if tick % cfg["snapshot_every_ticks"] == 0:
            checkpoints[tick] = adapter.snapshot(state)
        now_day = adapter.day(state)
        if now_day != day:
            seen.update(features(state))
            day = now_day
            daily.append(dict(adapter.metrics(state), day=day))
            failure = failure or _stalled(adapter, state, stall, day, stall_days, tick)
        if failure:
            break
        end_reason = adapter.finished(state)
        if end_reason or (cfg["max_days"] is not None and day >= cfg["max_days"]):
            break
        if time.perf_counter() - started > cfg["wall_seconds"]:
            failure = {"kind": "timeout", "id": "wall_seconds", "tick": tick, "signature": "timeout:wall_seconds",
                       "detail": f"run exceeded {cfg['wall_seconds']} s of wall time"}
            break
    hashes[tick] = digest(adapter.canonical(state))
    checkpoints[tick] = adapter.snapshot(state)
    wall = time.perf_counter() - started
    final = dict(adapter.metrics(state), day=adapter.day(state))
    report = {
        "status": "fail" if failure else ("ended" if end_reason else "ok"),
        "end_reason": end_reason, "failure": failure, "ticks": tick, "days": final["day"],
        "commands": {"accepted": accepted, "rejected": rejected},
        "wall_seconds": round(wall, 3), "ticks_per_second": round(tick / wall, 1) if wall else None,
        "final": final, "daily": daily, "features": sorted(seen),
    }
    manifest = {
        "format": FORMAT, "schema": SCHEMA, "tick_hz": adapter.TICK_HZ, "seed": seed, "policy": policy,
        "policy_seed": policy_seed, "scenario": scenario, "config": cfg,
        "adapter_schema": getattr(adapter, "SCHEMA", 1), "mode": "headless_simulation",
        "expects_stall": stall_days == 0 and bool(cfg["stall_days"]),
        **(manifest_extra or {}),
    }
    return {"manifest": manifest, "initial": initial, "actions": actions, "hashes": hashes,
            "checkpoints": checkpoints, "report": report}


# --- replay, verify, seek -------------------------------------------------------------------------

def _checkpoint(bundle: dict, tick: int):
    snap = bundle["checkpoints"].get(tick)
    return read_json(snap) if isinstance(snap, str) else copy.deepcopy(snap)


def replay(adapter, bundle: dict, until: int | None = None, start: int = 0, actions=None, verify=True, on_tick=None):
    """Restore the checkpoint at `start` and replay to `until` (default: the recorded end).
    Returns {state, tick, mismatch, failure}: mismatch is the first hash or command result that
    differs from the recording; failure is a check/exception hit while replaying."""
    actions = bundle["actions"] if actions is None else actions
    end = until if until is not None else max(bundle["hashes"])
    random.seed(bundle["manifest"]["seed"])
    state = adapter.restore(bundle["initial"] if start == 0 else _checkpoint(bundle, start))
    by_tick = {}
    for a in actions:
        by_tick.setdefault(a["tick"], []).append(a)
    tick, mismatch, failure = start, None, None
    stall_days = 0 if bundle["manifest"].get("expects_stall") else bundle["manifest"].get("config", {}).get("stall_days", 0)
    day = adapter.day(state)
    stall = [adapter.progress(state), day]
    while tick < end:
        tick_found = []
        try:
            for a in sorted(by_tick.get(tick, ()), key=lambda a: a["seq"]):
                if a["action"].get("type") == "__inject__":
                    adapter.inject(state, {k: v for k, v in a["action"].items() if k != "type"})
                    continue
                entry, found = _apply(adapter, state, a["action"], tick, a["seq"], watch=not a["ok"])
                failure = failure or found
                if verify and entry["ok"] != a["ok"] and mismatch is None:
                    mismatch = {"tick": tick, "kind": "command_result", "action": a["action"],
                                "recorded": a["ok"], "replayed": entry["ok"], "reason": entry["reason"]}
                if found:  # as in record: a failing command ends the tick before the game's step
                    break
            if failure is None:
                tick_found = adapter.tick(state) or []
        except Exception as exc:
            failure = _exception_failure(exc, tick, "replay")
            tick += 1
            break
        tick += 1
        if on_tick:
            on_tick(state, tick)
        if failure is None:
            failure = _invariant(tick_found + adapter.check(state), tick)
        if verify and mismatch is None and tick in bundle["hashes"]:
            if digest(adapter.canonical(state)) != bundle["hashes"][tick]:
                mismatch = {"tick": tick, "kind": "hash"}
        if (now_day := adapter.day(state)) != day:
            day = now_day
            failure = failure or _stalled(adapter, state, stall, day, stall_days, tick)
        if failure or (verify and mismatch):
            break
    return {"state": state, "tick": tick, "mismatch": mismatch, "failure": failure}


def locate_divergence(adapter, bundle: dict) -> dict | None:
    """Replay; at the first hash mismatch, add the last matching tick and the first differing JSON
    path between the recorded and the replayed state at the next recorded checkpoint."""
    m = replay(adapter, bundle)["mismatch"]
    if not m or m["kind"] != "hash":
        return m
    m["previous_good_tick"] = max(t for t in bundle["hashes"] if t < m["tick"])
    at = min(t for t in bundle["checkpoints"] if t >= m["tick"])
    replayed = replay(adapter, bundle, until=at, verify=False)["state"]
    m["checkpoint_tick"] = at
    m["first_diff"] = first_diff(adapter.canonical(adapter.restore(_checkpoint(bundle, at))), adapter.canonical(replayed))
    return m


def compare(adapter, a: dict, b: dict) -> dict:
    """Two recordings of the same start: the first tick whose hashes differ, then both states
    seeked there and their first differing path."""
    common = sorted(set(a["hashes"]) & set(b["hashes"]))
    bad = next((t for t in common if a["hashes"][t] != b["hashes"][t]), None)
    if bad is None:
        return {"same": True, "ticks_compared": len(common)}
    good = max([t for t in common if t < bad] or [0])
    for t in range(good + 1, bad + 1):  # exact tick: replay both a tick at a time
        sa, sb = seek(adapter, a, t)["state"], seek(adapter, b, t)["state"]
        found = first_diff(adapter.canonical(sa), adapter.canonical(sb))
        if found:
            return {"same": False, "first_divergent_tick": t, "first_diff": found,
                    "actions_a": [x for x in a["actions"] if x["tick"] == t - 1],
                    "actions_b": [x for x in b["actions"] if x["tick"] == t - 1]}
    return {"same": False, "first_divergent_tick": bad, "first_diff": None}


def verify(adapter, bundle: dict, runs: int = 2) -> dict:
    """Replay `runs` times; every run must match every recorded hash and command result, and
    reproduce the recorded failure signature (if any)."""
    want = (bundle["report"].get("failure") or {}).get("signature")
    out = {"runs": [], "ok": True}
    for _ in range(runs):
        r = replay(adapter, bundle, verify=True)
        got = (r["failure"] or {}).get("signature")
        if r["mismatch"] and r["mismatch"]["kind"] == "hash":
            r["mismatch"] = locate_divergence(adapter, bundle)
        entry = {"end_tick": r["tick"], "final_hash": digest(adapter.canonical(r["state"])),
                 "mismatch": r["mismatch"], "failure_signature": got}
        out["runs"].append(entry)
        out["ok"] &= r["mismatch"] is None and got == want
    out["reproducible"] = out["ok"] and len({e["final_hash"] for e in out["runs"]}) == 1
    out["expected_failure"] = want
    return out


def seek(adapter, bundle: dict, tick: int) -> dict:
    """The state at `tick`: latest checkpoint at or before it, then forward replay."""
    tick = max(0, min(tick, max(bundle["hashes"])))
    start = max(t for t in list(bundle["checkpoints"]) + [0] if t <= tick)
    return replay(adapter, bundle, until=tick, start=start, verify=True)


def fork(bundle: dict, tick: int, folder: str) -> str:
    """A new, separately labelled experimental bundle: the recording up to `tick`, nothing after.
    The original is never rewritten."""
    rec = {
        "manifest": dict(bundle["manifest"], forked_from={"bundle": bundle["folder"], "tick": tick}, experimental=True),
        "initial": bundle["initial"],
        "actions": [a for a in bundle["actions"] if a["tick"] < tick],
        "hashes": {t: h for t, h in bundle["hashes"].items() if t <= tick},
        "checkpoints": {t: _checkpoint(bundle, t) for t in bundle["checkpoints"] if t <= tick},
        "report": {"status": "fork", "ticks": tick},
    }
    return save_bundle(folder, rec)


# --- shrinking a failure --------------------------------------------------------------------------

def reproduces(adapter, bundle: dict, actions: list, signature: str) -> bool:
    end = (bundle["report"].get("failure") or {}).get("tick", max(bundle["hashes"])) + 1
    r = replay(adapter, bundle, until=end, actions=actions, verify=False)
    return (r["failure"] or {}).get("signature") == signature


def shrink(adapter, bundle: dict, max_replays: int = 400, max_seconds: float = 180.0) -> dict:
    """Delta debugging (ddmin) over the recorded commands: keep only removals that still produce
    the same failure signature from the same initial state, at the same ticks."""
    signature = (bundle["report"].get("failure") or {}).get("signature")
    if not signature:
        raise ValueError("the bundle has no failure to shrink")
    keep = [a for a in bundle["actions"] if a["ok"] or a["action"].get("type") == "__inject__"]
    replays, deadline = 0, time.perf_counter() + max_seconds
    if not reproduces(adapter, bundle, keep, signature):  # rejected commands mattered after all
        keep = list(bundle["actions"])
    n = 2
    while len(keep) >= 2 and replays < max_replays and time.perf_counter() < deadline:
        size = max(1, len(keep) // n)
        reduced = False
        for i in range(0, len(keep), size):
            candidate = keep[:i] + keep[i + size:]
            replays += 1
            if reproduces(adapter, bundle, candidate, signature):
                keep, n, reduced = candidate, max(n - 1, 2), True
                break
            if replays >= max_replays or time.perf_counter() > deadline:
                break
        if not reduced:
            if size == 1:
                break
            n = min(len(keep), n * 2)
    return {"signature": signature, "original": len(bundle["actions"]), "minimal": len(keep),
            "replays": replays, "complete": replays < max_replays and time.perf_counter() < deadline, "actions": keep, "confirmed": reproduces(adapter, bundle, keep, signature)}
