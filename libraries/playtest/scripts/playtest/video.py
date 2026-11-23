"""In-game side of `tools/playtest.py video` (docs/PLAYTEST.md "Failure video"): a bundle replayed in a live game.

The live game shows the state while this module steps it in core.replay's order (commands of tick N, then the game's
step). A game's trailer shot calls begin() once the run is up, switches off the game's own fixed update (the engine's dt
is a float32, the adapter's step is exact) and calls step() on every FixedUpdate30, recording meanwhile.
"""

import random

from playtest import core


def begin(adapter, folder: str, live: dict) -> dict:
    """Replace the live state (in place) by the bundle's initial state."""
    bundle = core.load_bundle(folder)
    random.seed(bundle["manifest"]["seed"])
    live.clear()
    live.update(adapter.restore(bundle["initial"]))
    run = {"bundle": bundle, "tick": 0, "end": max(bundle["hashes"]), "by_tick": {}}
    for a in bundle["actions"]:
        run["by_tick"].setdefault(a["tick"], []).append(a)
    return run


def step(run: dict, adapter, live: dict) -> bool:
    """One fixed update: checks the hash of the steps done so far, runs this tick's commands, takes the game's step.
    True once the recorded end is reached. Raises when the live state differs from the recording."""
    tick = run["tick"]
    if tick in run["bundle"]["hashes"] and core.digest(adapter.canonical(live)) != run["bundle"]["hashes"][tick]:
        raise RuntimeError(f"the live game diverged from the bundle at tick {tick}")
    if tick >= run["end"]:
        return True
    for a in sorted(run["by_tick"].get(tick, ()), key=lambda a: a["seq"]):
        action = a["action"]
        if action.get("type") == "__inject__":
            adapter.inject(live, {k: v for k, v in action.items() if k != "type"})
            continue
        _, failure = core._apply(adapter, live, action, tick, a["seq"], watch=not a["ok"])
        if failure:   # as in record: a failing command ends the tick before the game's step
            break
    else:
        adapter.tick(live)
    run["tick"] += 1
    return False
