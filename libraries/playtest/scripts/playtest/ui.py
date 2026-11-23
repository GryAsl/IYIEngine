"""UI playtester: a scenario of steps played through the real agent channel (SPEC §3.1) in an isolated,
hidden, lockstep game (agent_probe.launch with --fixed-dt --seed --unthrottled), with assertions, a
trace and a replay that compares the game's state hash at every step.

Scenario (JSON, schema_version 1): {"scenario", "mode": "ui", "run_seed", "viewport", "locale",
"max_steps", "timeout_seconds", "steps": [...], "artifacts": {"screenshots": "checkpoints"|"on_failure"|"all"}}

Steps (one key each):
    {"ui.click": {"name": "X", "scope": "Parent"}}   {"ui.type": {"element": "X", "text": "..."}}
    {"ui.key": "ESCAPE" | {"key", "hold_frames", "modifiers"}}   {"ui.scroll": {"element", "amount"}}
    {"ui.drag": {"from", "to", "frames"}}            {"wait.frames": 30}
    {"wait_until": {"scene", "visible", "timeout_frames"}}       engine-side wait
    {"wait_until": {"state": "stats.humans_captured", "op": ">=", "value": 1, "timeout_frames": 1500}}
    {"observe": {"include": ["ui", "gameplay", "diagnostics"]}}  {"screenshot": {"label": "x"}}
    {"assert": {"state": PATH, "op": OP, "value": V} | {"visible": NAME} | {"scene": PATH}
               | {"text": {"element": NAME, "contains": S}} | {"no_errors": true}
               | {"ui_problems": {"kinds": [...], "ignore": [{"kind", "element"} globs]}}   none on screen:
                 the visual tests' UI rules (tools/visual_test.py ui_problems, docs/VISUAL-TESTS.md)
    {"fixture": "early_game"}          labelled setup (the game's test_state key)
    {"project.action": {...}}          a domain command, labelled as such, never a UI click
    {"simulation.step": N}             N fixed steps of the live run (test session only)
    {"request": {...}}                 a raw agent-channel request

State predicates use the game's `read_state` key and are an allowlisted comparison, never eval.
Every reply carries the game's `playtest_hash` (its canonical state hash) for replay.
"""

import json
import os
import shutil
import sys
import time

from playtest import core

STEP_KEYS = {"ui.click", "ui.type", "ui.key", "ui.scroll", "ui.drag", "wait.frames", "wait_until", "observe",
             "screenshot", "assert", "fixture", "project.action", "simulation.step", "request"}
SCENARIO_KEYS = {"schema_version", "scenario", "mode", "run_seed", "viewport", "locale", "steps", "artifacts",
                 "max_steps", "timeout_seconds", "description", "scene"}
OPS = {"==": lambda a, b: a == b, "!=": lambda a, b: a != b, ">": lambda a, b: a is not None and a > b,
       ">=": lambda a, b: a is not None and a >= b, "<": lambda a, b: a is not None and a < b,
       "<=": lambda a, b: a is not None and a <= b, "contains": lambda a, b: a is not None and b in a,
       "exists": lambda a, b: a is not None}
TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "tools")


def load(path: str) -> dict:
    sc = core.read_json(path)
    bad = set(sc) - SCENARIO_KEYS
    if bad:
        raise ValueError(f"unknown scenario keys {sorted(bad)}")
    if sc.get("mode", "ui") != "ui" or sc.get("schema_version", 1) != 1:
        raise ValueError("a UI scenario has mode 'ui' and schema_version 1")
    for i, step in enumerate(sc.get("steps", [])):
        if not isinstance(step, dict) or len(step) != 1 or next(iter(step)) not in STEP_KEYS:
            raise ValueError(f"step {i}: one key of {sorted(STEP_KEYS)}, got {step!r}")
        kind, body = next(iter(step.items()))
        if kind in ("assert", "wait_until") and "state" in body and body.get("op", "==") not in OPS:
            raise ValueError(f"step {i}: op must be one of {sorted(OPS)}")
    if len(sc.get("steps", [])) > sc.get("max_steps", 200):
        raise ValueError(f"{len(sc['steps'])} steps exceed max_steps {sc.get('max_steps', 200)}")
    return sc


def _request(step: dict, i: int) -> dict:
    """The agent-channel request for one step (asserts and state waits are client-side)."""
    kind, body = next(iter(step.items()))
    req = {"id": f"s{i:03d}", "playtest_hash": True, "wait_frames": 2}
    if kind == "ui.click":
        req.update(click=body["name"], **({"scope": body["scope"]} if body.get("scope") else {}))
    elif kind == "ui.type":
        req["type"] = body
    elif kind == "ui.key":
        req["key"] = body
    elif kind == "ui.scroll":
        req["scroll"] = body
    elif kind == "ui.drag":
        req["drag"] = body
    elif kind == "wait.frames":
        req["wait_frames"] = max(2, int(body))
    elif kind == "wait_until":
        if "state" in body:  # polled client-side; the first reply must carry the value too
            req["read_state"] = [body["state"]]
        else:
            req["wait_until"] = body
    elif kind == "observe":
        inc = body.get("include", ["ui", "gameplay"])
        req.update(ui="ui" in inc, health="diagnostics" in inc, playtest_observe="gameplay" in inc,
                   playtest_legal="gameplay" in inc)
    elif kind == "screenshot":
        req.update(screenshot=True, id=f"s{i:03d}_{body.get('label', 'shot')}")
    elif kind == "fixture":
        req["test_state"] = body
    elif kind == "project.action":
        req["playtest_action"] = body
    elif kind == "simulation.step":
        req["playtest_step"] = int(body)
    elif kind == "request":
        req.update(body)
    elif kind == "assert":
        if "state" in body:
            req["read_state"] = [body["state"]]
        if "visible" in body or "text" in body or "ui_problems" in body:
            req["ui"] = True
        if "ui_problems" in body:
            req["ui_problems"] = True
    return req


def _ui_element(reply: dict, name: str):
    return next((e for e in reply.get("ui") or [] if e.get("name") == name), None)


def _check(a: dict, reply: dict, errors: list) -> dict:
    """An assertion against the reply: {pass, expected, actual}."""
    if "state" in a:
        actual = ((reply.get("game") or {}).get("read_state") or {}).get(a["state"])
        return {"pass": OPS[a.get("op", "==")](actual, a.get("value")), "expected": [a.get("op", "=="), a.get("value")], "actual": actual}
    if "visible" in a:
        shown = _ui_element(reply, a["visible"]) is not None
        return {"pass": shown, "expected": f"{a['visible']} visible", "actual": "visible" if shown else "not shown"}
    if "scene" in a:
        return {"pass": reply.get("scene") == a["scene"], "expected": a["scene"], "actual": reply.get("scene")}
    if "text" in a:
        el = _ui_element(reply, a["text"]["element"]) or {}
        return {"pass": a["text"]["contains"] in (el.get("text") or ""), "expected": a["text"], "actual": el.get("text")}
    if "ui_problems" in a:
        import fnmatch
        import visual_test   # tools/, on sys.path through tools/playtest.py
        rule = a["ui_problems"] or {}
        found = [p for p in visual_test.reply_problems(reply)
                 if p["kind"] in rule.get("kinds", [p["kind"]]) and not any(
                     fnmatch.fnmatch(p["kind"], i.get("kind", "*")) and fnmatch.fnmatch(p["element"], i.get("element", "*"))
                     for i in rule.get("ignore", []))]
        return {"pass": not found, "expected": "no UI problems", "actual": found[:10]}
    if a.get("no_errors"):
        return {"pass": not errors, "expected": "no errors logged", "actual": errors[:5]}
    raise ValueError(f"unknown assertion {a}")


def _input_problem(reply: dict):
    """A refused click/type/scroll/drag (hidden, disabled, occluded, ...): a structured failure."""
    for key, entry in (reply.get("input") or {}).items():
        if isinstance(entry, dict) and entry.get("error"):
            return {"input": key, **entry}
    return None


def play(sc: dict, project: str, out: str, exe=None, timeout=60.0, expect_hashes=None) -> dict:
    """Run the steps in a fresh isolated game; write the failure package under `out`."""
    sys.path.insert(0, os.path.abspath(TOOLS))
    import agent_probe
    exe = exe or os.path.join(TOOLS, "..", "build", "ninja", "IYIEngine.exe")
    seed = int(sc.get("run_seed", 1))
    flags = ["--fixed-dt", "0.0333333", "--seed", str(seed), "--unthrottled"]
    os.makedirs(os.path.join(out, "screenshots"), exist_ok=True)
    process, agentio, user_data = agent_probe.launch(project, exe, False, flags)
    shots = sc.get("artifacts", {}).get("screenshots", "on_failure")
    trace, observations, steps, errors = [], [], [], []
    failure, started = None, time.perf_counter()
    budget = float(sc.get("timeout_seconds", 600))
    try:
        if sc.get("scene"):
            agent_probe.ask(agentio, {"id": "scene", "scene": sc["scene"], "wait_frames": 4}, timeout, process)
        for i, step in enumerate(sc.get("steps", [])):
            kind, body = next(iter(step.items()))
            req = _request(step, i)
            if shots == "all":
                req["screenshot"] = True
            reply, waited = agent_probe.ask(agentio, req, timeout, process), 0
            if reply is not None and kind == "wait_until" and "state" in body:  # poll the game's state, frame-locked
                limit = int(body.get("timeout_frames", 600))
                while reply is not None:
                    actual = ((reply.get("game") or {}).get("read_state") or {}).get(body["state"])
                    if OPS[body.get("op", "==")](actual, body.get("value")) or waited >= limit:
                        break
                    waited += 30
                    reply = agent_probe.ask(agentio, dict(req, id=f"s{i:03d}_w{waited}", read_state=[body["state"]],
                                                          wait_frames=30), timeout, process)
            if reply is None:
                failure = {"step": i, "kind": "no_reply", "detail": "the game did not answer" +
                           (" (it exited)" if process.poll() is not None else "")}
                break
            errors += [l for l in (reply.get("log") or {}).get("lines", []) if "[error]" in l.lower() or " error " in l.lower()]
            game = reply.get("game") or {}
            entry = {"step": i, "kind": kind, "request": {k: v for k, v in req.items() if k != "playtest_hash"},
                     "scene": reply.get("scene"), "input": reply.get("input"), "wait_until": reply.get("wait_until"),
                     "game": {k: v for k, v in game.items() if k not in ("playtest_hash", "playtest_observe", "playtest_legal")},
                     "hash": game.get("playtest_hash"), "new_problems": (reply.get("log") or {}).get("new_problems"),
                     "protocol": reply.get("protocol")}
            shot = reply.get("screenshot") and os.path.join(agentio, reply["screenshot"])  # relative to agentio
            if shot and os.path.isfile(shot):
                dest = os.path.join(out, "screenshots", os.path.basename(shot))
                shutil.copyfile(shot, dest)
                entry["screenshot"] = dest
            if kind == "observe":
                observations.append({"step": i, "ui": reply.get("ui"), "gameplay": game.get("playtest_observe"),
                                     "legal": game.get("playtest_legal"), "health": reply.get("health")})
            trace.append(entry)
            result = {"step": i, "kind": kind, "pass": True}
            problem = _input_problem(reply)
            if problem:
                result.update({"pass": False, "reason": "input_refused", "detail": problem})
            elif kind == "assert":
                result.update(_check(body, reply, errors))
            elif kind == "wait_until":
                met = (reply.get("wait_until") or {}).get("met")
                if "state" in body:
                    actual = (game.get("read_state") or {}).get(body["state"])
                    met = OPS[body.get("op", "==")](actual, body.get("value"))
                    result["actual"] = actual
                result.update({"pass": bool(met), "frames": waited or (reply.get("wait_until") or {}).get("frames")})
            elif kind == "project.action":
                pa = game.get("playtest_action") or {}
                result.update({"pass": bool(pa.get("ok")) and not pa.get("violations"), "label": "domain_action", "detail": pa})
            elif kind == "simulation.step" and (game.get("playtest_step") or {}).get("violations"):
                result.update({"pass": False, "detail": game["playtest_step"]["violations"]})
            elif game.get("error"):
                result.update({"pass": False, "detail": game["error"]})
            if expect_hashes is not None and i < len(expect_hashes) and expect_hashes[i] != entry["hash"]:
                result.update({"pass": False, "reason": "replay_divergence", "expected_hash": expect_hashes[i], "actual_hash": entry["hash"]})
            steps.append(result)
            if not result["pass"]:
                failure = dict(result)
                break
            if time.perf_counter() - started > budget:
                failure = {"step": i, "kind": "timeout", "detail": f"scenario exceeded {budget} s"}
                break
        if failure and process.poll() is None and shots != "never":
            shot = agent_probe.ask(agentio, {"id": "failure", "screenshot": True, "ui": True, "wait_frames": 2}, timeout, process)
            if shot and shot.get("screenshot") and os.path.isfile(os.path.join(agentio, shot["screenshot"])):
                failure["screenshot"] = os.path.join(out, "screenshots", "failure.png")
                shutil.copyfile(os.path.join(agentio, shot["screenshot"]), failure["screenshot"])
    finally:
        end = agent_probe.finish(process, agentio, user_data)
        shutil.rmtree(user_data, ignore_errors=True)
    errors += [json.dumps(e) for e in end["errors"]]
    manifest = {"format": "iyi-ui-trace", "schema": 1, "mode": "ui_interaction", "scenario": sc, "project": os.path.abspath(project),
                "engine_flags": flags, "fixtures_used": any("fixture" in s for s in sc.get("steps", [])),
                "domain_actions_used": any("project.action" in s or "simulation.step" in s for s in sc.get("steps", [])),
                "viewport_requested": sc.get("viewport"), "locale_requested": sc.get("locale"),
                "note": "viewport and locale are recorded, not enforced, by the agent channel"}
    report = {"ok": failure is None and not errors, "mode": "ui_interaction", "failure": failure, "steps": steps,
              "errors": errors[:50], "exit_code": end["exit_code"], "seconds": round(time.perf_counter() - started, 1),
              "out": out}
    core.write_json(os.path.join(out, "manifest.json"), manifest)
    core._write(os.path.join(out, "trace.jsonl"), core._jsonl(trace))
    core._write(os.path.join(out, "observations.jsonl"), core._jsonl(observations))
    core._write(os.path.join(out, "errors.txt"), "\n".join(errors) + ("\n" if errors else ""))
    core.write_json(os.path.join(out, "report.json"), report)
    core._write(os.path.join(out, "report.html"), _html(manifest, report))
    return report


def _html(manifest, report) -> str:
    import html
    rows = "".join(f"<tr><td>{s['step']}</td><td>{html.escape(s['kind'])}</td><td>{'pass' if s['pass'] else 'FAIL'}</td>"
                   f"<td><code>{html.escape(json.dumps({k: v for k, v in s.items() if k not in ('step', 'kind', 'pass')}, default=str))[:300]}</code></td></tr>"
                   for s in report["steps"])
    shot = report["failure"] and report["failure"].get("screenshot")
    return (f"<!doctype html><meta charset='utf-8'><title>UI playtest</title><h1>{html.escape(str(manifest['scenario'].get('scenario')))}"
            f" ({'ok' if report['ok'] else 'FAILED'})</h1><p>ui_interaction; fixtures used: {manifest['fixtures_used']}; "
            f"domain actions used: {manifest['domain_actions_used']}</p><table border=1>{rows}</table>"
            + (f"<h2>Failure</h2><pre>{html.escape(json.dumps(report['failure'], indent=1, default=str))}</pre>" if report["failure"] else "")
            + ("<img src='screenshots/failure.png' width=960>" if shot else ""))


def _out_dir(project, name, given):
    import datetime
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    return given or os.path.join(os.path.abspath(project), "artifacts", "playtests", f"{stamp}-{name}")


def run(args) -> dict:
    if not args.scenario:
        raise ValueError("ui run needs --scenario")
    sc = load(args.scenario)
    return play(sc, args.project, _out_dir(args.project, sc.get("scenario", "ui"), args.out), args.exe, args.timeout)


def replay(args) -> dict:
    """The same steps again in a fresh game; the first step whose state hash differs fails."""
    if not args.trace:
        raise ValueError("ui replay needs --trace <run folder or trace.jsonl>")
    folder = args.trace if os.path.isdir(args.trace) else os.path.dirname(args.trace)
    manifest = core.read_json(os.path.join(folder, "manifest.json"))
    hashes = [json.loads(l)["hash"] for l in core._read(os.path.join(folder, "trace.jsonl")).splitlines() if l.strip()]
    project = args.project if args.project != "." else manifest["project"]
    out = _out_dir(project, str(manifest["scenario"].get("scenario", "ui")) + "-replay", args.out)
    result = play(manifest["scenario"], project, out, args.exe, args.timeout, expect_hashes=hashes)
    result["replayed"] = folder
    return result
