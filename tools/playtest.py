"""AI playtesting, deterministic replay, time-travel debugging and the Balance Lab (docs/PLAYTEST.md).

    python tools/playtest.py run     --project ../HumanResources [--scenario s.json] [--policy coverage] [--seed 1] [--days 30]
    python tools/playtest.py study   --project ../HumanResources --study s.json [--dry-run] [--baseline aggregates.json]
    python tools/playtest.py verify  <bundle> [--runs 2]
    python tools/playtest.py debug   <bundle> [--do "failure; diff; show economy.money"]   (no --do: interactive)
    python tools/playtest.py shrink  <bundle>
    python tools/playtest.py compare <bundle-a> <bundle-b>
    python tools/playtest.py fork    <bundle> --tick N --out <folder>
    python tools/playtest.py video   <bundle> [--out file.mp4]   (IYITrailer take of the bundle replayed in the real game)
    python tools/playtest.py ui      run|replay --project ../HumanResources --scenario ui.json | --trace <trace.jsonl>
    python tools/playtest.py selfcheck

Headless commands load the game's adapter (<project>/devtools/adapter.py, or --adapter) and run its
real fixed step without a window, audio or Steam. Nothing starts a game unless you run `ui`.
Every command prints JSON on stdout. Exit codes: 0 ok, 1 usage/config error, 3 a run failed,
4 a replay diverged or did not reproduce, 5 a UI assertion failed.
"""

import argparse
import datetime
import importlib.util
import json
import os
import platform
import shutil
import subprocess
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(TOOLS, "..", "libraries", "playtest", "scripts"))

from playtest import core, debugger, lab  # noqa: E402


def load_adapter(project: str, adapter_path: str | None = None):
    path = os.path.abspath(adapter_path or os.path.join(project, "devtools", "adapter.py"))
    if not os.path.isfile(path):
        raise SystemExit(_fail(1, f"no playtest adapter at {path} (docs/PLAYTEST.md 'Adapter')"))
    spec = importlib.util.spec_from_file_location("playtest_game_adapter", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _git(folder: str) -> str:
    try:
        rev = subprocess.run(["git", "-C", folder, "rev-parse", "--short", "HEAD"], capture_output=True, text=True, timeout=10).stdout.strip()
        dirty = subprocess.run(["git", "-C", folder, "status", "--porcelain"], capture_output=True, text=True, timeout=10).stdout.strip()
        return (rev or "unknown") + ("+dirty" if dirty else "")
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def build_info(project: str, adapter) -> dict:
    return {"game": getattr(adapter, "GAME", os.path.basename(os.path.abspath(project))), "project": os.path.abspath(project),
            "build": {"engine": _git(os.path.join(TOOLS, "..")), "game": _git(project)},
            "runtime": {"python": platform.python_version(), "platform": platform.platform(), "machine": platform.machine()}}


def _fail(code: int, message: str) -> int:
    print(json.dumps({"ok": False, "error": message}))
    return code


def _out(value, code=0) -> int:
    print(json.dumps(value, indent=1, default=str))
    return code


def _default_out(project: str, name: str) -> str:
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    return os.path.join(os.path.abspath(project), "artifacts", "playtest", f"{stamp}-{name}")


def _project_of(bundle_folder: str) -> str:
    """The game a bundle belongs to: the project it recorded, else the project folder around it."""
    try:
        recorded = core.read_json(os.path.join(bundle_folder, "manifest.json")).get("project")
    except (OSError, ValueError):
        recorded = None
    if recorded and os.path.isfile(os.path.join(recorded, "project.json")):
        return recorded
    folder = os.path.abspath(bundle_folder)
    while folder != os.path.dirname(folder):
        if os.path.isfile(os.path.join(folder, "project.json")):
            return folder
        folder = os.path.dirname(folder)
    raise SystemExit(_fail(1, f"{bundle_folder} is not inside a game project: pass --project"))


def _bundle_adapter(args):
    project = args.project or _project_of(args.bundle)
    adapter = load_adapter(project, args.adapter)
    bundle = core.load_bundle(args.bundle)
    here = build_info(project, adapter)["build"]
    warn = None if bundle["manifest"].get("build") == here else {
        "recorded_build": bundle["manifest"].get("build"), "current_build": here,
        "note": "builds differ: a mismatch may be a code change, not nondeterminism (replay is same-build only)"}
    return project, adapter, bundle, warn


def _policy(adapter, name: str):
    if name == "llm":
        from playtest import llm
        return llm.make_policy(adapter)
    if name not in adapter.POLICIES:
        raise SystemExit(_fail(1, f"unknown policy {name!r}; this game has {sorted(adapter.POLICIES)} and 'llm'"))
    return adapter.POLICIES[name]


# --- commands -------------------------------------------------------------------------------------

def cmd_run(args) -> int:
    adapter = load_adapter(args.project, args.adapter)
    scenario = core.read_json(args.scenario) if args.scenario else {}
    if args.fixture:
        scenario["fixture"] = args.fixture
    if args.ceo:
        scenario["ceo"] = args.ceo
    config = dict(scenario.pop("config", {}), max_days=args.days, decision_interval_ticks=args.decision_ticks)
    if args.inject_tick is not None:
        config["faults"] = [{"tick": args.inject_tick, "what": args.inject}]
    choose = _policy(adapter, args.policy)
    rec = core.record(adapter, scenario, args.seed, args.policy, args.policy_seed, config,
                      manifest_extra=build_info(args.project, adapter), choose=choose)
    folder = core.save_bundle(args.out or _default_out(args.project, f"{args.policy}-s{args.seed}"), rec)
    report = {k: v for k, v in rec["report"].items() if k != "daily"}
    return _out({"ok": report["status"] != "fail", "bundle": folder, **report}, 3 if report["status"] == "fail" else 0)


def cmd_run_job(args) -> int:
    """Internal: one study job in its own process; writes <out>/jobs/<id>.result.json."""
    adapter = load_adapter(args.project, args.adapter)
    job = core.read_json(args.job)
    result = lab.run_job(adapter, job, args.out, keep=job.get("keep", "failures"), shrink=job.get("shrink", True),
                         manifest_extra=build_info(args.project, adapter))
    core.write_json(os.path.join(args.out, "jobs", job["id"] + ".result.json"), result)
    return 0


def cmd_study(args) -> int:
    adapter = load_adapter(args.project, args.adapter)
    try:
        study = lab.load_study(args.study)
        if args.dry_run:
            return _out(lab.dry_run(study, adapter))
    except ValueError as exc:
        return _fail(1, str(exc))
    out = args.out or _default_out(args.project, "study-" + str(study.get("study", "unnamed")))
    summary = lab.run_study(study, adapter, out, os.path.abspath(args.project), os.path.abspath(__file__), args.baseline,
                            manifest_extra=build_info(args.project, adapter), log=lambda m: print(m, file=sys.stderr))
    return _out(dict(summary, report=os.path.join(out, "report.html")), 3 if summary["unique_failures"] else 0)


def cmd_verify(args) -> int:
    _, adapter, bundle, warn = _bundle_adapter(args)
    result = core.verify(adapter, bundle, runs=args.runs)
    return _out(dict(result, build_warning=warn, bundle=bundle["folder"]), 0 if result["reproducible"] else 4)


def cmd_shrink(args) -> int:
    _, adapter, bundle, _ = _bundle_adapter(args)
    small = core.shrink(adapter, bundle)
    path = os.path.join(bundle["folder"], "repro.jsonl")
    core._write(path, core._jsonl(small["actions"]))
    return _out(dict({k: v for k, v in small.items() if k != "actions"}, repro=path), 0 if small["confirmed"] else 4)


def cmd_compare(args) -> int:
    args.bundle = args.a
    _, adapter, a, _ = _bundle_adapter(args)
    return _out(core.compare(adapter, a, core.load_bundle(args.b)))


def cmd_fork(args) -> int:
    _, _, bundle, _ = _bundle_adapter(args)
    return _out({"forked": core.fork(bundle, args.tick, args.out)})


def cmd_debug(args) -> int:
    project, adapter, _, warn = _bundle_adapter(args)
    session = debugger.open_session(adapter, args.bundle)
    if args.do:
        out = [{"command": c.strip(), "result": debugger.command(session, c)} for c in args.do.split(";") if c.strip()]
        return _out({"build_warning": warn, "results": out})
    print(json.dumps(debugger.where(session), default=str))
    print(f"commands: {debugger.COMMANDS}, quit", file=sys.stderr)
    while True:
        try:
            line = input(f"[tick {session['tick']}] > ")
        except EOFError:
            return 0
        if line.strip() in ("q", "quit", "exit"):
            return 0
        if line.strip():
            try:
                print(json.dumps(debugger.command(session, line), indent=1, default=str))
            except (ValueError, KeyError) as exc:
                print(json.dumps({"error": repr(exc)}))


def cmd_video(args) -> int:
    """The bundle replayed in the real game and recorded by IYITrailer: the project's shot
    marketing/shots/playtest_replay.py (docs/PLAYTEST.md "Failure video") reads the bundle from the environment."""
    bundle = core.load_bundle(args.bundle)
    out = os.path.join(bundle["folder"], "video")
    seconds = max(bundle["hashes"]) / 30 + 60   # real time plus the run-up and the final encode
    done = subprocess.run([sys.executable, os.path.join(TOOLS, "trailer.py"), "render", "--project", args.project or _project_of(args.bundle),
                           "--shot", "playtest_replay", "--out", out, "--timeout", str(seconds), "--json"],
                          env=dict(os.environ, IYI_PLAYTEST_BUNDLE=bundle["folder"]), capture_output=True, text=True)
    try:
        report = json.loads(done.stdout)
    except ValueError:
        return _fail(1, done.stdout.strip() or done.stderr.strip())
    mp4 = report["output"].get("mp4")
    if mp4 and args.out:
        shutil.copyfile(mp4, args.out)
        mp4 = args.out
    return _out({"ok": report["ok"], "video": mp4, "seconds": (report.get("video") or {}).get("seconds"),
                 "bytes": mp4 and os.path.getsize(mp4), "errors": report["errors"]}, 0 if report["ok"] else 3)


def cmd_ui(args) -> int:
    from playtest import ui
    try:
        result = ui.replay(args) if args.action == "replay" else ui.run(args)
    except ValueError as exc:
        return _fail(1, str(exc))
    return _out(result, 0 if result["ok"] else 5)


def cmd_selfcheck(args) -> int:
    from playtest import selfcheck
    return selfcheck.main()


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add(name, fn, bundle=False):
        s = sub.add_parser(name)
        s.set_defaults(fn=fn)
        s.add_argument("--project", default=None if bundle else ".")
        s.add_argument("--adapter", default=None, help="adapter file (default <project>/devtools/adapter.py)")
        if bundle:
            s.add_argument("bundle")
        return s

    s = add("run", cmd_run)
    s.add_argument("--scenario")
    s.add_argument("--fixture")
    s.add_argument("--ceo")
    s.add_argument("--policy", default="coverage")
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--policy-seed", type=int, default=17)
    s.add_argument("--days", type=int, default=30)
    s.add_argument("--decision-ticks", type=int, default=10)
    s.add_argument("--inject", default="warehouse_overflow", help="fault name for --inject-tick (acceptance demo)")
    s.add_argument("--inject-tick", type=int)
    s.add_argument("--out")
    s = add("run-job", cmd_run_job)
    s.add_argument("--job", required=True)
    s.add_argument("--out", required=True)
    for name in ("study", "batch"):
        s = add(name, cmd_study)
        s.add_argument("--study", required=True)
        s.add_argument("--dry-run", action="store_true")
        s.add_argument("--baseline")
        s.add_argument("--out")
    add("verify", cmd_verify, bundle=True).add_argument("--runs", type=int, default=2)
    add("shrink", cmd_shrink, bundle=True)
    add("debug", cmd_debug, bundle=True).add_argument("--do", help="';'-separated debugger commands, JSON out")
    s = add("fork", cmd_fork, bundle=True)
    s.add_argument("--tick", type=int, required=True)
    s.add_argument("--out", required=True)
    add("video", cmd_video, bundle=True).add_argument("--out", help="copy the .mp4 here (default <bundle>/video/)")
    s = sub.add_parser("compare")
    s.set_defaults(fn=cmd_compare)
    s.add_argument("a")
    s.add_argument("b")
    s.add_argument("--project")
    s.add_argument("--adapter")
    s = sub.add_parser("ui")
    s.set_defaults(fn=cmd_ui)
    s.add_argument("action", choices=("run", "replay"))
    s.add_argument("--project", default=".")
    s.add_argument("--scenario")
    s.add_argument("--trace")
    s.add_argument("--out")
    s.add_argument("--exe")
    s.add_argument("--timeout", type=float, default=60.0)
    sub.add_parser("selfcheck").set_defaults(fn=cmd_selfcheck)
    args = p.parse_args(argv)
    try:
        return args.fn(args)
    except ValueError as exc:  # unreadable bundle, bad schema, bad scenario
        return _fail(1, str(exc))


if __name__ == "__main__":
    sys.exit(main())
