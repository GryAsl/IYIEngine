"""Dedupe and rate limits over a plain dict the caller owns (transient: never put it in a save)."""

DEDUPE_CAP = 4096


def new() -> dict:
    return {"seen": {}, "last": {}, "recent": [], "count": 0}


def seen(state: dict, key: str) -> bool:
    """True if `key` was recorded before; records it otherwise. Oldest keys go past DEDUPE_CAP."""
    ids = state["seen"]
    if key in ids:
        return True
    ids[key] = None
    if len(ids) > DEDUPE_CAP:
        del ids[next(iter(ids))]
    return False


def allow(state: dict, user: str, now: float, cooldown: float, per_minute: int, cap: int) -> bool:
    """One accepted action per `user` per `cooldown` seconds (inf: once per session), `per_minute`
    across everyone, `cap` in total. Records the action only when it is allowed."""
    recent = [t for t in state["recent"] if now - t < 60.0]
    last = state["last"].get(user)
    if state["count"] >= cap or len(recent) >= per_minute or (last is not None and now - last < cooldown):
        state["recent"] = recent
        return False
    state["recent"] = recent + [now]
    state["last"][user] = now
    state["count"] += 1
    return True
