"""Time-travel debugger over a replay bundle: rewind = restore the nearest checkpoint and replay
forward (core.seek), never reverse execution. Plain functions over a session dict the caller owns,
shared by the CLI (`playtest.py debug`) and a game's dev-only panel.

Commands (each returns a JSON-able dict):
    seek N | step [n] | back [n] | next | prev | failure | start | end
    show PATH | obs | metrics | check | rng | diff | actions [n] | where | fork FOLDER
"""

from playtest import core

COMMANDS = ("seek N, step [n], back [n], next, prev, failure, start, end, show PATH, obs, metrics, check, rng, diff, "
            "actions [n], where, fork FOLDER")


def open_session(adapter, folder: str) -> dict:
    bundle = core.load_bundle(folder)
    session = {"adapter": adapter, "bundle": bundle, "tick": 0, "state": None, "prev": None,
               "end": max(bundle["hashes"]), "action_ticks": sorted({a["tick"] for a in bundle["actions"]})}
    goto(session, 0)
    return session


def goto(session: dict, tick: int) -> dict:
    a, b = session["adapter"], session["bundle"]
    tick = max(0, min(int(tick), session["end"]))
    result = core.seek(a, b, tick)
    session["prev"] = core.seek(a, b, tick - 1)["state"] if tick > 0 else None
    session["state"], session["tick"] = result["state"], result["tick"]
    session["mismatch"], session["failure"] = result["mismatch"], result["failure"]
    return where(session)


def where(session: dict) -> dict:
    a, s = session["adapter"], session["state"]
    t = session["tick"]
    return {"tick": t, "day": a.day(s), "end": session["end"], "hash": core.digest(a.canonical(s)),
            "recorded_hash": session["bundle"]["hashes"].get(t), "mismatch": session["mismatch"],
            "failure_here": session["failure"],
            "commands_before": [x for x in session["bundle"]["actions"] if x["tick"] == t - 1]}


def _dig(value, path: str):
    for part in path.split("."):
        if isinstance(value, list) and part.lstrip("-").isdigit():
            value = value[int(part)] if -len(value) <= int(part) < len(value) else None
        else:
            value = value.get(part) if isinstance(value, dict) else None
    return value


def failure_tick(session: dict):
    f = session["bundle"]["report"].get("failure")
    return f["tick"] if f and f.get("tick") is not None else None


def command(session: dict, line: str) -> dict:
    """One debugger command; unknown ones answer with the list."""
    a = session["adapter"]
    word, _, arg = line.strip().partition(" ")
    arg = arg.strip()
    t = session["tick"]
    ticks = session["action_ticks"]
    if word == "seek":
        return goto(session, int(arg))
    if word == "step":
        return goto(session, t + int(arg or 1))
    if word == "back":
        return goto(session, t - int(arg or 1))
    if word == "next":  # the state just after the next recorded command
        return goto(session, next((x + 1 for x in ticks if x + 1 > t), session["end"]))
    if word == "prev":
        return goto(session, max([x + 1 for x in ticks if x + 1 < t] or [0]))
    if word == "start":
        return goto(session, 0)
    if word == "end":
        return goto(session, session["end"])
    if word == "failure":
        ft = failure_tick(session)
        if ft is None:
            return {"error": "this bundle recorded no failure"}
        out = goto(session, ft - 1)  # the tick before the failure: inspect, then `step`
        out["failure"] = session["bundle"]["report"]["failure"]
        return out
    if word == "show":
        return {"path": arg, "value": _dig(session["state"], arg) if arg else None}
    if word == "obs":
        return a.observe(session["state"])
    if word == "metrics":
        return a.metrics(session["state"])
    if word == "check":
        return {"violations": a.check(session["state"])}
    if word == "rng":
        rng = getattr(a, "rng_streams", None)
        return rng(session["state"]) if rng else {"error": "adapter has no rng_streams()"}
    if word == "diff":
        if session["prev"] is None:
            return {"diff": [], "note": "tick 0 has no previous tick"}
        return {"from": t - 1, "to": t, "diff": core.diff_all(a.canonical(session["prev"]), a.canonical(session["state"]))}
    if word == "actions":
        n = int(arg or 10)
        acts = session["bundle"]["actions"]
        before = [x for x in acts if x["tick"] < t][-n:]
        after = [x for x in acts if x["tick"] >= t][:n]
        return {"before": before, "after": after}
    if word == "where":
        return where(session)
    if word == "fork":
        return {"forked": core.fork(session["bundle"], t, arg)}
    return {"error": f"unknown command {word!r}", "commands": COMMANDS}
