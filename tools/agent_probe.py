"""Asks a running game (development build) how it is doing, through the agent channel (SPEC §3.1).

    python tools/agent_probe.py --project ../MyGame                 # health twice, 5 s apart
    python tools/agent_probe.py --project ../MyGame --profile --screenshot
    python tools/agent_probe.py --project ../MyGame --json         # the raw replies
    python tools/agent_probe.py run --project ../MyGame --request a.json [--request b.json]

`run` starts its own isolated game (--agent-run --hidden --no-hot-reload, a fresh --user-data folder
holding its saves, settings, logs and agentio), sends each request in turn, quits the game and
prints {"ok", "replies", "events", "exit_code"} as JSON. Exit code 0: every request was answered and
no error was logged; 1: a request got no reply or the game died; 2: an error was logged.

Prints frame times, window focus, process memory and its growth, cached render textures, log
warnings and the last log lines (where other agents' requests and hot reloads show up). No reply
means the game is not running, is frozen, or is a shipping build. Stdlib only.
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time


def ask(agentio, request, timeout, process=None):
    """Writes the request and waits for its reply; None on timeout or when the process ended."""
    reply_path = os.path.join(agentio, request["id"] + ".json")
    if os.path.exists(reply_path):
        os.remove(reply_path)
    temp = os.path.join(agentio, "request.json.tmp")
    with open(temp, "w", encoding="utf-8") as f:
        json.dump(request, f)
    os.replace(temp, os.path.join(agentio, "request.json"))
    end = time.time() + timeout
    while time.time() < end:
        if os.path.exists(reply_path):
            try:
                with open(reply_path, encoding="utf-8") as f:
                    return json.load(f)
            except (PermissionError, json.JSONDecodeError):
                pass   # Windows: the engine (or a scanner) still holds the new file; read it next turn
        if process is not None and process.poll() is not None:
            return None
        time.sleep(0.1)
    return None


def hang_view(agentio):
    """Why a request got no reply, from the watchdog's files (SPEC §3.2): a verdict, the heartbeat and
    the last Python stacks dumped at a stall."""
    view = {"verdict": "no heartbeat: not a development build, or the engine never got going", "heartbeat": None}
    try:
        path = os.path.join(agentio, "heartbeat.json")
        age = time.time() - os.path.getmtime(path)
        with open(path, encoding="utf-8") as f:
            beat = json.load(f)
    except (OSError, json.JSONDecodeError):
        return view
    view["heartbeat"] = beat
    where = f"game thread in '{beat.get('game_section')}', render thread in '{beat.get('render_section')}'"
    if age > 3.0:
        view["verdict"] = f"heartbeat stopped {age:.0f} s ago: the process ended, crashed (logs/crash.txt) or is suspended"
    elif beat.get("stalled"):
        view["verdict"] = f"frozen {beat.get('seconds_since_frame', 0):.1f} s: {where}"
    else:
        view["verdict"] = "not frozen, still turning frames: slow or busy; ask again with a longer --timeout or --profile"
    try:
        with open(os.path.join(agentio, "hang_stack.txt"), encoding="utf-8", errors="replace") as f:
            dumps = f.read().strip()
        view["python_stacks"] = dumps[dumps.rfind("Thread 0x"):] if "Thread 0x" in dumps else dumps[-4000:]
    except OSError:
        pass
    return view


def launch(project, exe, visible=False, engine_args=(), user_files=None):
    """An isolated game (own --user-data, hidden unless `visible`): (process, agentio, user_data).
    `user_files` {relative path: JSON value} are written into the user data first (a settings.json)."""
    # Folders a killed launcher left behind (its game quits on its own: WatchLauncher); no run lasts a day.
    for old in glob.glob(os.path.join(tempfile.gettempdir(), "iyi_agent_*")):
        try:
            if time.time() - os.path.getmtime(old) > 86400:
                shutil.rmtree(old, ignore_errors=True)
        except OSError:
            pass   # a run beside this one just ended and removed its folder
    user_data = tempfile.mkdtemp(prefix="iyi_agent_")
    agentio = os.path.join(user_data, "agentio")
    os.makedirs(agentio)
    for rel, content in (user_files or {}).items():
        path = os.path.join(user_data, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(content, f)
    command = [os.path.abspath(exe), "--project", os.path.abspath(project), "--agent-run", "--no-hot-reload",
               "--user-data", user_data] + ([] if visible else ["--hidden"]) + list(engine_args)
    return subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL), agentio, user_data


def finish(process, agentio, user_data):
    """Quits the game (asks, then kills after 30 s): {exit_code, events, errors} of the whole run."""
    if process.poll() is None:
        ask(agentio, {"id": "_quit", "quit": True}, 15.0, process)
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
    out = {"exit_code": process.wait(), "events": []}
    events_path = os.path.join(user_data, "logs", "events.jsonl")
    if os.path.exists(events_path):   # the whole run, startup included
        with open(events_path, encoding="utf-8", errors="replace") as f:
            out["events"] = [json.loads(line) for line in f if line.strip()]
    out["errors"] = [e for e in out["events"] if e.get("kind") == "error"]
    return out


def run(argv):
    """`run`: an isolated game per call, the requests in order, JSON out (see the module doc)."""
    parser = argparse.ArgumentParser(prog="agent_probe.py run")
    parser.add_argument("--project", default=".")
    parser.add_argument("--request", action="append", default=[], help="a request .json file (repeatable)")
    parser.add_argument("--timeout", type=float, default=60.0, help="seconds per request")
    parser.add_argument("--exe", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build",
                                                      "ninja", "IYIEngine.exe"))
    parser.add_argument("--visible", action="store_true", help="show the window (no --hidden)")
    parser.add_argument("--keep", action="store_true", help="keep the user-data folder and print its path")
    parser.add_argument("engine_args", nargs="*", help="after --: more engine flags, e.g. -- --fixed-dt 0.0333 --seed 1")
    args = parser.parse_args(argv)
    if not os.path.exists(os.path.join(args.project, "project.json")):
        sys.exit(f"no project.json in {args.project}")
    requests = []
    for i, path in enumerate(args.request):
        with open(path, encoding="utf-8") as f:
            request = json.load(f)
        request.setdefault("id", f"r{i}")
        requests.append(request)
    process, agentio, user_data = launch(args.project, args.exe, args.visible, args.engine_args)
    result = {"ok": True, "replies": [], "events": [], "exit_code": 0}
    try:
        for request in requests:
            reply = ask(agentio, request, args.timeout, process)
            if reply is None:
                result.update(ok=False, error=f"no reply to '{request['id']}'" + (" (the game ended)" if process.poll() is not None else ""),
                              hang=hang_view(agentio))
                break
            result["replies"].append(reply)
            result["events"] += reply.get("events", [])
    finally:   # Ctrl+C or an error too: the game quits and its folder goes
        result.update(finish(process, agentio, user_data))
        if args.keep:
            result["user_data"] = user_data
        else:
            shutil.rmtree(user_data, ignore_errors=True)
    print(json.dumps(result, indent=1))
    return 1 if not result["ok"] else 2 if result["errors"] else 0


def report(reply, top):
    """The reply as a few readable lines."""
    h = reply.get("health") or {}
    f, w, m, a = h.get("frame_ms", {}), h.get("window", {}), h.get("memory") or {}, h.get("assets", {})
    lines = [f"fps {reply.get('fps', 0):.1f}   frame ms avg {f.get('avg', 0):.1f} max {f.get('max', 0):.1f}"
             f"   over 50 ms: {f.get('over_50ms', 0)}/{f.get('frames', 0)}   uptime {h.get('uptime_s', 0):.0f} s",
             f"window focused={w.get('focused')} minimized={w.get('minimized')} focus changes={w.get('focus_changes')}"
             f" screen={w.get('screen')} render={w.get('render')} monitor {w.get('monitor_hz')} Hz"]
    if m:
        growth = m.get("growth_mb_per_min")
        lines.append(f"memory private {m['private_mb']:.0f} MB (peak {m['peak_private_mb']:.0f}), working set"
                     f" {m['working_set_mb']:.0f} MB, growth "
                     + ("n/a (first sample)" if growth is None else f"{growth:+.1f} MB/min"))
    lines.append(f"assets ~{a.get('gpu_mb_estimate', 0):.0f} MB GPU: {a.get('textures')} textures,"
                 f" {a.get('sprite_stacks')} sprite stacks, render textures {a.get('render_textures')}")
    log = h.get("log", {})
    lines.append(f"log {log.get('warnings')} warnings, {log.get('errors')} errors; last lines:")
    lines += ["  " + line for line in log.get("recent", [])[-12:]]
    if reply.get("profile"):
        lines.append(f"slowest profiler sections (avg ms, max ms, calls/frame):")
        for s in sorted(reply["profile"], key=lambda s: -s["avg_ms"])[:top]:
            lines.append(f"  {s['avg_ms']:8.2f} {s['max_ms']:8.2f} {s['calls']:6.1f}  {s['name']}")
    if reply.get("game") is not None:
        lines.append("game: " + json.dumps(reply["game"])[:400])
    if reply.get("screenshot"):
        lines.append("screenshot: " + reply["screenshot"])
    return "\n".join(lines)


def main():
    if sys.argv[1:2] == ["run"]:
        sys.exit(run(sys.argv[2:]))
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--project", default=".", help="the game's folder (holds project.json)")
    parser.add_argument("--id", default="probe", help="names the reply files in agentio/")
    parser.add_argument("--interval", type=float, default=5.0, help="seconds between the two samples (0: one)")
    parser.add_argument("--profile", action="store_true", help="add the slowest profiler sections")
    parser.add_argument("--top", type=int, default=12, help="profiler sections to show")
    parser.add_argument("--screenshot", action="store_true", help="save agentio/<id>.png")
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--force", action="store_true", help="replace a request that is already waiting")
    parser.add_argument("--json", action="store_true", help="print the raw replies")
    args = parser.parse_args()

    agentio = os.path.join(args.project, "agentio")
    if not os.path.exists(os.path.join(args.project, "project.json")):
        sys.exit(f"no project.json in {args.project}")
    os.makedirs(agentio, exist_ok=True)
    waiting = os.path.join(agentio, "request.json")
    if os.path.exists(waiting) and not args.force:
        sys.exit("agentio/request.json is already waiting: another agent is using this game, or the game is"
                 " not running / frozen. --force replaces it.")

    samples = [args.id + "_1", args.id] if args.interval > 0 else [args.id]
    for i, rid in enumerate(samples):
        last = i == len(samples) - 1
        request = {"id": rid, "health": True, "profile": args.profile and last, "screenshot": args.screenshot and last}
        reply = ask(agentio, request, args.timeout)
        if reply is None:
            sys.exit(f"no reply in {args.timeout:.0f} s: " + json.dumps(hang_view(agentio), indent=1))
        if not last:
            time.sleep(args.interval)
            continue
        print(json.dumps(reply, indent=1) if args.json else report(reply, args.top))


if __name__ == "__main__":
    main()
