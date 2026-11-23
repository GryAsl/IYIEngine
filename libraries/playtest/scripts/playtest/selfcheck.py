"""Self-check of the playtest core against a 30-line fake game (no engine, no real game):

    python libraries/playtest/scripts/playtest/selfcheck.py

Proves record -> replay twice with identical hashes, seek forward/back, a changed command changes
the hash, a rejected command that mutates and a soft lock are caught and replay-verified, an injected fault is found, shrunk and
reproduced, and an unknown schema is refused.
"""

import copy
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from playtest import core, video  # noqa: E402


class Fake:
    """Coins grow each tick; `buy` spends 5 coins for a machine; rng is a state-held LCG."""
    TICK_HZ, SCHEMA = 30, 1

    @staticmethod
    def create(scenario, seed):
        return {"t": 0, "coins": scenario.get("coins", 0), "machines": 0, "rng": seed}

    snapshot = staticmethod(copy.deepcopy)
    restore = staticmethod(copy.deepcopy)

    @staticmethod
    def tick(s):
        s["rng"] = (s["rng"] * 1103515245 + 12345) & 0xFFFFFFFF
        s["coins"] += 1 + s["machines"] + (s["rng"] >> 31)
        s["t"] += 1

    @staticmethod
    def actions(s):
        return [{"type": "buy"}] if s["coins"] >= 5 else []

    @staticmethod
    def execute(s, a):
        if a["type"] == "buy" and s["coins"] >= 5:
            s["coins"] -= 5
            s["machines"] += 1
            return True, None
        if a["type"] == "leaky":  # a buggy command: rejected, yet it changed the state
            s["coins"] += 1
        return False, "cannot afford"

    canonical = staticmethod(lambda s: s)
    check = staticmethod(lambda s: [{"id": "negative_coins", "detail": str(s["coins"])}] if s["coins"] < 0 else [])
    metrics = staticmethod(lambda s: {"coins": s["coins"], "machines": s["machines"]})
    day = staticmethod(lambda s: s["t"] // 30)
    finished = staticmethod(lambda s: "done" if s["machines"] >= 40 else None)
    progress = staticmethod(lambda s: (s["coins"], s["machines"]))

    @staticmethod
    def inject(s, spec):
        s["coins"] = -100

    POLICIES = {"greedy": lambda obs, legal, rng, memory: legal[:1] if rng.random() < 0.5 else [],
                "leaky": lambda obs, legal, rng, memory: [{"type": "leaky"}]}
    observe = staticmethod(lambda s: dict(s))


class Stuck(Fake):
    """Nothing it does counts as progress: a soft lock after `stall_days`."""
    progress = staticmethod(lambda s: 0)
    POLICIES = {"idle": lambda obs, legal, rng, memory: []}


def main() -> int:
    cfg = {"max_ticks": 2000, "snapshot_every_ticks": 300, "decision_interval_ticks": 7}
    rec = core.record(Fake, {}, seed=3, policy="greedy", policy_seed=9, config=cfg)
    with tempfile.TemporaryDirectory() as tmp:
        bundle = core.load_bundle(core.save_bundle(os.path.join(tmp, "ok"), rec))
        v = core.verify(Fake, bundle, runs=3)
        assert v["reproducible"], v
        end = rec["report"]["ticks"]
        for t in (end, 450, 17, 451):  # forward, back, back, forward again
            assert core.digest(core.seek(Fake, bundle, t)["state"]) == core.digest(
                core.replay(Fake, bundle, until=t, verify=False)["state"]), t
        assert core.seek(Fake, bundle, 450)["state"]["t"] == 450

        changed = copy.deepcopy(bundle)
        first = next(a for a in changed["actions"] if a["ok"])
        changed["actions"].remove(first)
        m = core.replay(Fake, changed)["mismatch"]
        assert m and m["tick"] > first["tick"], m
        cmp = core.compare(Fake, bundle, core.load_bundle(core.save_bundle(
            os.path.join(tmp, "b"), core.record(Fake, {}, seed=3, policy="greedy", policy_seed=10, config=cfg))))
        assert not cmp["same"] and cmp["first_diff"]["path"].startswith("$."), cmp

        leak = core.record(Fake, {}, seed=1, policy="leaky", policy_seed=1, config=cfg)
        assert leak["report"]["failure"]["kind"] == "rejected_mutation", leak["report"]["failure"]

        assert core.verify(Fake, core.load_bundle(core.save_bundle(os.path.join(tmp, "leak"), leak)))["reproducible"]
        stuck = core.record(Stuck, {}, seed=1, policy="idle", policy_seed=1, config=dict(cfg, stall_days=3, max_ticks=5000))
        assert stuck["report"]["failure"]["signature"] == "softlock:no_progress", stuck["report"]["failure"]
        assert core.verify(Stuck, core.load_bundle(core.save_bundle(os.path.join(tmp, "stuck"), stuck)))["reproducible"]

        bad = core.record(Fake, {}, seed=5, policy="greedy", policy_seed=2, config=dict(cfg, faults=[{"tick": 200, "what": "negative"}]))
        assert bad["report"]["failure"]["signature"] == "invariant:negative_coins", bad["report"]["failure"]
        fb = core.load_bundle(core.save_bundle(os.path.join(tmp, "fail"), bad))
        assert core.verify(Fake, fb, runs=2)["reproducible"]
        small = core.shrink(Fake, fb)
        assert small["confirmed"] and small["minimal"] == 1 and small["actions"][0]["action"]["type"] == "__inject__", small["minimal"]
        # a live game's state, stepped by video (failure bundles: up to the failure; the second one fails
        # in a command, before the tick)
        for bundle in (fb, core.load_bundle(os.path.join(tmp, "leak"))):
            live = {}
            run = video.begin(Fake, bundle["folder"], live)
            while not video.step(run, Fake, live):
                pass
            assert core.digest(live) == core.digest(core.replay(Fake, bundle, until=run["end"], verify=False)["state"]), "video replay"
        out = core.fork(fb, 150, os.path.join(tmp, "fork"))
        assert core.load_bundle(out)["manifest"]["experimental"]

        core.write_json(os.path.join(tmp, "ok", "manifest.json"), dict(rec["manifest"], schema=99))
        try:
            core.load_bundle(os.path.join(tmp, "ok"))
            raise AssertionError("schema 99 accepted")
        except ValueError:
            pass
    print("playtest core self-check: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
