"""Visual regression tests for any IYIEngine game (docs/VISUAL-TESTS.md).

    python tools/visual_test.py run     --project ../HumanResources [--scenario market,settings] [--category menus]
                                        [--matrix default|full|<name>] [--changed] [--visible] [--jobs N]
    python tools/visual_test.py compare --project ../HumanResources [--run <id>]    # re-judge a run, no game
    python tools/visual_test.py approve --project ../HumanResources --case 'market/*' [--case ...] | --all
    python tools/visual_test.py reject  --project ../HumanResources --case 'market/*'
    python tools/visual_test.py report  --project ../HumanResources [--run <id>]    # rebuild HTML, print summary

A scenario (`<project>/visual_tests/scenarios/<name>.py`) drives an isolated, hidden, deterministic game
through the agent channel (SPEC §3.1) and captures checkpoints:

    def run(t):
        t.click("MarketButton")
        t.wait_until(visible="MarketScreen")
        t.capture("market")

Each capture is compared (IYIEngine --compare-images: changed regions, SSIM, masks) with its approved
baseline in `visual_tests/baselines/<environment>/`, and its UI is checked by the structural rules below.
Baselines change only through `approve`. Exit code: 0 all passed, 1 a case did not pass, 2 a case errored
(the game or the scenario failed). Stdlib only.
"""

import argparse
import concurrent.futures
import fnmatch
import glob
import importlib.util
import itertools
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import agent_probe  # noqa: E402  the agent channel's client (one implementation)
import visual_report  # noqa: E402

ENGINE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(ENGINE_DIR, "build", "ninja", "IYIEngine.exe")
DEFAULT_CONFIG = {
    "engine_args": ["--fixed-dt", "0.0333333", "--seed", "1", "--unthrottled"],
    "options": {},                       # axis -> value -> {user-data file: JSON to merge into it}
    "matrices": {"default": {"size": ["1920x1080"]}},
    "thresholds": {"tolerance": 12, "tile": 16, "tile_min": 4, "min_region_pixels": 24, "min_ssim": 0.99},
    "ignore": [],                        # structural problems that are intended: {"kind", "element"} globs
    "timeout": 60,
}
DEFAULT_JOBS = max(1, min(4, (os.cpu_count() or 1) // 2))   # each game is a GPU + CPU process; measured: see docs/VISUAL-TESTS.md
PASS, FAIL, STRUCTURAL, MISSING, SIZE, ERROR, REJECTED = (
    "PASS", "FAIL", "STRUCTURAL_FAIL", "MISSING_BASELINE", "RESOLUTION_MISMATCH", "ERROR", "REJECTED")


class ScenarioError(Exception):
    pass


# ---- structural rules, on the engine's "ui" dump (ported from HumanResources' agent_probe.py) ----------

def ui_problems(ui):
    """Fallback for runtimes before the engine's "ui_problems" (UICanvas::Problems is the source; keep the
    two alike). [{kind, element, rect, detail}] for the shown elements of one `ui` reply (SPEC §3.1); `rect` is the
    element's bounds in canvas units. A rule whose field an older engine's dump lacks is skipped."""
    by_name = {e["name"]: e for e in ui}
    problems = []

    def add(kind, e, detail):
        problems.append({"kind": kind, "element": e["name"], "rect": e["rect"], "detail": detail})

    parents = {c.get("parent") for c in ui if c.get("visible", True)}
    for e in ui:
        x, y, w, h = e["rect"]
        if w <= 0 or h <= 0:
            if e.get("type") != "Panel" or e["name"] in parents:  # an empty panel draws nothing (engine rule)
                add("ZERO_SIZE", e, f"size {w:.1f} x {h:.1f}")
            continue
        canvas = e.get("canvas_size")
        if canvas and not e.get("clipped") and (x + w <= 0 or y + h <= 0 or x >= canvas[0] or y >= canvas[1]):
            add("OFFSCREEN", e, f"entirely outside the canvas {canvas[0]:.0f} x {canvas[1]:.0f}")
        if e.get("parent_rect") and not e.get("clipped"):   # inside a scroll view or a clipping parent: intended
            px, py, pw, ph = e["parent_rect"]
            over = max(px - x, py - y, x + w - px - pw, y + h - py - ph)
            if over > 0.5:
                add("OVERFLOW", e, f"extends {over:.1f} past parent '{e.get('parent')}' {e['parent_rect']}")
        if e.get("measured_width", 0) > w + 0.5:
            add("TEXT_CLIP", e, f"text {e['measured_width']:.1f} wide in a {w:.1f} box")
        if e.get("font_size") and e.get("fitted_font_size", e["font_size"]) < e["font_size"] - 0.01:
            add("FONT_SHRUNK", e, f"font {e['fitted_font_size']:.1f} below requested {e['font_size']:.1f}")
        if e.get("missing_glyphs"):
            add("UNSUPPORTED_GLYPH", e, f"no glyph for {e['missing_glyphs']!r}")
    # Two enabled buttons sharing clickable pixels: a player cannot tell which one a click hits.
    buttons = [e for e in ui if e["type"] == "Button" and e.get("enabled", True) and e["rect"][2] > 0 and e["rect"][3] > 0]
    for a, b in itertools.combinations(buttons, 2):
        ra, rb = _hit_rect(by_name, a), _hit_rect(by_name, b)
        ix, iy = min(ra[2], rb[2]) - max(ra[0], rb[0]), min(ra[3], rb[3]) - max(ra[1], rb[1])
        if ix > 1 and iy > 1 and not _is_ancestor(by_name, a, b) and not _is_ancestor(by_name, b, a):
            add("BUTTON_OVERLAP", a, f"overlaps button '{b['name']}' by {ix:.0f} x {iy:.0f}")
    return problems


def _hit_rect(by_name, e):
    """(x0, y0, x1, y1) of the part of `e` a click reaches, as the engine hit-tests it: cut by the scroll
    views around it and by its outermost clipping ancestor (the parent of the highest `clipped` element).
    shortcut: a clipping Panel between those is not seen (the dump has no ClipChildren); dump it if one matters."""
    x0, y0, w, h = e["rect"]
    x1, y1 = x0 + w, y0 + h
    seen = set()
    while e.get("clipped") and e.get("parent") in by_name and e["parent"] not in seen:
        seen.add(e["parent"])
        e = by_name[e["parent"]]
        if e["type"] == "ScrollView" or not e.get("clipped"):
            px, py, pw, ph = e["rect"]
            x0, y0, x1, y1 = max(x0, px), max(y0, py), min(x1, px + pw), min(y1, py + ph)
    return x0, y0, x1, y1


def _is_ancestor(by_name, a, b):
    seen = set()
    while b.get("parent") and b["parent"] not in seen:
        seen.add(b["parent"])
        if b["parent"] == a["name"]:
            return True
        b = by_name.get(b["parent"], {})
    return False


def reply_problems(reply):
    """The engine's own findings (UICanvas::Problems, reply "ui_problems"); a runtime older than them gets
    ui_problems() on its "ui" dump."""
    return reply["ui_problems"] if "ui_problems" in reply else ui_problems(reply.get("ui") or [])


def problem_key(p):
    return f"{p['kind']}:{p['element']}"


# ---- configuration and scenarios ------------------------------------------------------------------------

def load_config(project):
    config = json.loads(json.dumps(DEFAULT_CONFIG))
    path = os.path.join(project, "visual_tests", "config.json")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            user = json.load(f)
        for key, value in user.items():
            config[key] = {**config[key], **value} if key == "thresholds" else value
    return config


def load_scenarios(project):
    out = {}
    folder = os.path.join(project, "visual_tests", "scenarios")
    if folder not in sys.path:
        sys.path.insert(0, folder)   # scenarios may share helpers (_name.py) by import
    for path in sorted(glob.glob(os.path.join(folder, "*.py"))):
        name = os.path.splitext(os.path.basename(path))[0]
        if name.startswith("_"):
            continue
        spec = importlib.util.spec_from_file_location(f"visual_scenario_{name}", path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        out[name] = module
    return out


def configs_for(config, matrix):
    """Every combination of the matrix's axes, as {"size": "WxH", axis: value, ...} in a stable order."""
    axes = config["matrices"].get(matrix)
    if axes is None:
        raise SystemExit(f"no matrix '{matrix}' (have: {', '.join(config['matrices'])})")
    names = list(axes)
    return [dict(zip(names, values)) for values in itertools.product(*(axes[n] for n in names))]


def config_label(cfg):
    return "_".join(str(cfg[k]) for k in cfg)


def changed_files(project):
    """Paths (relative to the project, '/' separated) git reports as changed or new; None without git."""
    try:
        out = subprocess.run(["git", "status", "--porcelain", "--untracked-files=all", "."], cwd=project,
                             capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    prefix = subprocess.run(["git", "rev-parse", "--show-prefix"], cwd=project, capture_output=True,
                            text=True).stdout.strip()
    files = [line[3:].split(" -> ")[-1].strip('"') for line in out.splitlines()]
    return [f[len(prefix):] if f.startswith(prefix) else f for f in files]


def git_version(path):
    try:
        rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=path, capture_output=True, text=True,
                             check=True).stdout.strip()
        dirty = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=path,
                               capture_output=True, text=True).stdout.strip()
        return rev + ("+dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


# ---- one isolated game per scenario and configuration ------------------------------------------------------

class Test:
    """What a scenario's run(t) drives. Every call is one agent-channel request (SPEC §3.1); a request
    whose input did not land, whose wait was not met or whose game hook raised fails the scenario."""

    def __init__(self, agentio, process, out_dir, case_prefix, config, cfg, timeout):
        self.config = cfg                # {"size": "1920x1080", "lang": "en", ...}: this run's matrix entry
        self._agentio, self._process, self._out, self._prefix = agentio, process, out_dir, case_prefix
        self._project_config, self._timeout, self._n = config, timeout, 0
        self.captures, self.log_problems = [], []

    def send(self, **request):
        self._n += 1
        request["id"] = f"r{self._n}"
        reply = agent_probe.ask(self._agentio, request, self._timeout, self._process)
        if reply is None:
            raise ScenarioError(f"no reply to {json.dumps(request)}"
                                + (" (the game ended)" if self._process.poll() is not None else ""))
        self.log_problems += [line for line in (reply.get("log") or {}).get("lines", [])]
        for kind, landed in (reply.get("input") or {}).items():
            if isinstance(landed, dict) and landed.get("error"):
                raise ScenarioError(f"{kind} {landed.get('element')}: {landed['error']}")
        if reply.get("wait_until") and not reply["wait_until"].get("met"):
            raise ScenarioError(f"wait_until {request.get('wait_until')} not met")
        if isinstance(reply.get("game"), dict) and reply["game"].get("error"):
            raise ScenarioError(f"game: {reply['game']['error']}")
        return reply

    def game(self, **keys):
        """The game's own AgentRequest keys (test states, snapshots); returns the reply's `game`."""
        return self.send(**keys).get("game")

    def scene(self, path, **more):
        return self.send(scene=path, **more)

    def click(self, element, scope=None, wait_frames=2):
        return self.send(click=element, wait_frames=wait_frames, **({"scope": scope} if scope else {}))

    def type(self, element, text):
        return self.send(type={"element": element, "text": text})

    def key(self, key, wait_frames=2):
        return self.send(key=key, wait_frames=wait_frames)

    def wait(self, frames):
        return self.send(wait_frames=frames)

    def wait_until(self, visible=None, scene=None, timeout_frames=600, wait_frames=2):
        cond = {k: v for k, v in (("visible", visible), ("scene", scene)) if v}
        return self.send(wait_until={**cond, "timeout_frames": timeout_frames}, wait_frames=wait_frames)

    def wait_stable(self, frames=10, tries=12):
        """Until two frames `frames` apart are the same image (a transition has ended); fails after `tries`."""
        self.send(screenshot=True, wait_frames=frames)
        for _ in range(tries):
            reply = self.send(screenshot=True, wait_frames=frames, compare=f"r{self._n}")
            if (reply.get("compare") or {}).get("bbox", 1) is None:
                return reply
        raise ScenarioError(f"the screen did not settle within {tries * frames} frames")

    def capture(self, name, mask=(), ignore=(), wait_frames=2):
        """A checkpoint: the frame and its UI. `mask`: [x, y, w, h] window pixels or element names (dynamic
        content); `ignore`: structural problems intended here, {"kind", "element"} globs."""
        if not re.fullmatch(r"[\w-]+", name):
            raise ScenarioError(f"capture name must be letters, digits, _ and -: {name!r}")
        reply = self.send(screenshot=True, ui=True, ui_problems=True, wait_frames=wait_frames)
        shot = os.path.join(self._agentio, f"r{self._n}.png")
        if not os.path.exists(shot):
            raise ScenarioError(f"capture {name}: no screenshot written")
        case = f"{self._prefix}/{name}@{config_label(self.config)}"
        png = os.path.join(self._out, case + ".png")
        os.makedirs(os.path.dirname(png), exist_ok=True)
        shutil.move(shot, png)
        ui = reply.get("ui") or []
        size = (reply.get("image") or {}).get("size") or [int(v) for v in self.config["size"].split("x")]
        masks = []
        for m in mask:
            if isinstance(m, str):
                hit = next((e for e in ui if e["name"] == m), None)
                if hit is None or "screen_rect" not in hit:
                    raise ScenarioError(f"mask '{m}': not shown, or the engine's ui dump has no screen_rect")
                masks.append(hit["screen_rect"])
            else:
                masks.append(list(m))
        self.captures.append({"case": case, "scenario": self._prefix, "capture": name, "config": self.config,
                              "image": case + ".png", "size": size, "masks": masks, "ignore": list(ignore),
                              "scene": reply.get("scene"), "problems": reply_problems(reply),
                              "ui_fields": sorted({k for e in ui for k in e})})
        with open(os.path.join(self._out, case + ".ui.json"), "w", encoding="utf-8") as f:
            json.dump(ui, f, indent=1)
        return reply


def user_files(config, cfg):
    """Each axis value's files ({"settings.json": {...}}) merged, for the fresh user-data folder."""
    files = {}
    for axis, value in cfg.items():
        for rel, content in ((config["options"].get(axis) or {}).get(str(value)) or {}).items():
            files.setdefault(rel, {}).update(content)
    return files


def renderer_from_log(user_data):
    path = os.path.join(user_data, "logs", "latest.log")
    if os.path.exists(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            match = re.search(r"> Renderer: (.+)", f.read())
            if match:
                return match.group(1).strip()
    return "unknown"


def run_scenario(args, config, name, module, cfg, out_dir):
    """Runs one scenario under one configuration; returns (captures, error, renderer, log_problems)."""
    flags = ["--window-size", cfg["size"]] + config["engine_args"]
    process, agentio, user_data = agent_probe.launch(args.project, args.exe, args.visible, flags, user_files(config, cfg))
    t = Test(agentio, process, out_dir, name, config, cfg, config["timeout"])
    error = None
    try:
        t.send(wait_frames=2)   # the game is up and its entry scene has run a frame
        module.run(t)
    except ScenarioError as exc:
        error = str(exc)
    except Exception as exc:  # a bug in the scenario itself
        error = f"{type(exc).__name__}: {exc}"
    finally:
        agent_probe.finish(process, agentio, user_data)
        renderer = renderer_from_log(user_data)
        shutil.rmtree(user_data, ignore_errors=True)
    return t.captures, error, renderer, t.log_problems


# ---- judging ------------------------------------------------------------------------------------------

def environment_name(renderer):
    return re.sub(r"[^a-z0-9]+", "-", f"{platform.system()}-{renderer}".lower()).strip("-")


def baseline_paths(project, env, case):
    base = os.path.join(project, "visual_tests", "baselines", env, case)
    return base + ".png", base + ".json"


def judge(args, report):
    """Compares every capture of the run with its baseline (one --compare-images job) and sets statuses."""
    run_dir, project, th = report["run_dir"], report["project"], report["thresholds"]
    pairs, cases = [], []
    for case in report["cases"]:
        if case["status"] in (ERROR, REJECTED):
            continue
        png, meta = baseline_paths(project, report["environment"], case["case"])
        case["baseline"] = os.path.relpath(png, run_dir) if os.path.exists(png) else None
        accepted = []
        if os.path.exists(meta):
            with open(meta, encoding="utf-8") as f:
                accepted = [problem_key(p) for p in json.load(f).get("problems", [])]
        ignore = report["ignore"] + case.get("ignore", [])
        case["new_problems"] = [p for p in case["problems"] if problem_key(p) not in accepted
                                and not any(fnmatch.fnmatch(p["kind"], i.get("kind", "*"))
                                            and fnmatch.fnmatch(p["element"], i.get("element", "*")) for i in ignore)]
        if case["baseline"] is None:
            case.update(status=MISSING, reasons=["no approved baseline: check the capture, then approve it"])
            continue
        case["diff"] = case["case"] + ".diff.png"
        pairs.append({"baseline": png, "current": os.path.join(run_dir, case["image"]),
                      "diff": os.path.join(run_dir, case["diff"]), "masks": case["masks"],
                      "tolerance": th["tolerance"], "tile": th["tile"], "tile_min": th["tile_min"]})
        cases.append(case)
    if pairs:
        job = os.path.join(run_dir, "compare_job.json")
        with open(job, "w", encoding="utf-8") as f:
            json.dump({"pairs": pairs, "out": job + ".out"}, f)
        code = subprocess.run([os.path.abspath(args.exe), "--compare-images", job], stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode
        if code != 0 or not os.path.exists(job + ".out"):
            raise SystemExit(f"--compare-images failed (exit {code}): is {args.exe} a current development build?")
        with open(job + ".out", encoding="utf-8") as f:
            results = json.load(f)["results"]
        os.remove(job)
        os.remove(job + ".out")
    for case, result in zip(cases, results if pairs else []):
        case["compare"] = result
        reasons = []
        if result.get("error"):
            reasons.append(result["error"])
        elif not result.get("size_match"):
            case.update(status=SIZE, reasons=[f"baseline {result['baseline_size']} vs current "
                                              f"{result['current_size']}: approve a baseline for this size"])
            continue
        else:
            big = [r for r in result["regions"] if r["pixels"] >= th["min_region_pixels"]]
            if big:
                reasons.append(f"{len(big)} changed region(s), {sum(r['pixels'] for r in big)} px")
            if result["ssim"] < th["min_ssim"]:
                reasons.append(f"SSIM {result['ssim']:.4f} < {th['min_ssim']}")
        visual = bool(reasons)
        reasons += [f"{p['kind']} {p['element']}: {p['detail']}" for p in case["new_problems"]]
        case.update(status=FAIL if visual else STRUCTURAL if reasons else PASS, reasons=reasons)
    for case in report["cases"]:   # no baseline to compare with, but the UI can still be broken
        if case["status"] == MISSING and case["new_problems"]:
            case["reasons"] += [f"{p['kind']} {p['element']}: {p['detail']}" for p in case["new_problems"]]
    return report


def finish(report):
    run_dir = report["run_dir"]
    with open(os.path.join(run_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    visual_report.write_html(report, os.path.join(run_dir, "report.html"))
    width = max([len(c["case"]) for c in report["cases"]] + [10])
    for c in report["cases"]:
        print(f"{c['case']:<{width}}  {c['status']}" + (f"  ({c['reasons'][0]})" if c.get("reasons") else ""))
    counts = {}
    for c in report["cases"]:
        counts[c["status"]] = counts.get(c["status"], 0) + 1
    print(" ".join(f"{k}={v}" for k, v in sorted(counts.items())) or "no cases")
    print("report:", os.path.join(run_dir, "report.html"))
    if any(c["status"] == ERROR for c in report["cases"]):
        return 2
    return 0 if all(c["status"] == PASS for c in report["cases"]) else 1


# ---- commands -------------------------------------------------------------------------------------------

def cmd_run(args):
    project = os.path.abspath(args.project)
    config = load_config(project)
    scenarios = load_scenarios(project)
    if args.scenario:
        wanted = args.scenario.split(",")
        missing = [s for s in wanted if s not in scenarios]
        if missing:
            raise SystemExit(f"no scenario {missing} (have: {', '.join(scenarios)})")
        scenarios = {k: v for k, v in scenarios.items() if k in wanted}
    if args.category:
        scenarios = {k: v for k, v in scenarios.items() if getattr(v, "CATEGORY", "") in args.category.split(",")}
    if args.changed:
        files = changed_files(project)
        if files is None:
            raise SystemExit("--changed needs the project in a git repository")
        scenarios = {k: v for k, v in scenarios.items()   # no WATCH: the scenario always runs
                     if not hasattr(v, "WATCH") or any(fnmatch.fnmatch(f, w) for f in files for w in v.WATCH)}
    if not scenarios:
        raise SystemExit("no scenario selected")
    run_id = time.strftime("%Y%m%d-%H%M%S")
    run_dir = os.path.join(project, "visual_tests", "results", run_id)
    os.makedirs(run_dir)
    report = {"run": run_id, "run_dir": run_dir, "project": project, "project_name": os.path.basename(project),
              "project_version": git_version(project), "engine_version": git_version(ENGINE_DIR),
              "matrix": args.matrix, "thresholds": config["thresholds"], "ignore": config["ignore"],
              "command": " ".join(sys.argv), "cases": [], "environment": None, "renderer": None}
    jobs = [(name, module, cfg) for name, module in scenarios.items()
            for cfg in configs_for(config, (getattr(module, "MATRIX", None) if args.matrix == "default" else None) or args.matrix)]

    def one(job):
        name, module, cfg = job
        started = time.time()
        result = run_scenario(args, config, name, module, cfg, run_dir)
        print(f"ran {name} [{config_label(cfg)}] in {time.time() - started:.1f} s" + (f": {result[1]}" if result[1] else ""), flush=True)
        return result

    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:   # games are isolated processes; map keeps job order
        results = list(pool.map(one, jobs))
    for (name, module, cfg), (captures, error, renderer, log_problems) in zip(jobs, results):
        report["renderer"] = report["renderer"] or renderer
        for c in captures:
            c["log_problems"] = log_problems
            c["status"] = None
        report["cases"] += captures
        if error:
            report["cases"].append({"case": f"{name}/@{config_label(cfg)}", "scenario": name, "config": cfg,
                                    "status": ERROR, "reasons": [error], "problems": [],
                                    "log_problems": log_problems,
                                    "repro": f"{name} after {len(captures)} capture(s)"})
    report["environment"] = args.env or environment_name(report["renderer"] or "unknown")
    with open(os.path.join(project, "visual_tests", "results", "latest.txt"), "w") as f:
        f.write(run_id)
    return finish(judge(args, report))


def load_run(args):
    results = os.path.join(os.path.abspath(args.project), "visual_tests", "results")
    run_id = args.run
    if not run_id:
        latest = os.path.join(results, "latest.txt")
        if not os.path.exists(latest):
            raise SystemExit("no run yet: visual_test.py run first")
        with open(latest) as f:
            run_id = f.read().strip()
    path = os.path.join(results, run_id, "report.json")
    if not os.path.exists(path):
        raise SystemExit(f"no report in {os.path.dirname(path)}")
    with open(path, encoding="utf-8") as f:
        report = json.load(f)
    report["run_dir"] = os.path.dirname(path)
    return report


def selected(report, args):
    if not args.case and not getattr(args, "all", False):
        raise SystemExit("name the cases (--case 'scenario/capture@config', globs allowed) or pass --all")
    out = [c for c in report["cases"] if getattr(args, "all", False) or any(fnmatch.fnmatch(c["case"], p) for p in args.case)]
    if not out:
        raise SystemExit("no case matches")
    return out


def cmd_approve(args):
    report = load_run(args)
    approved = 0
    for case in selected(report, args):
        if case["status"] in (ERROR, REJECTED) or "image" not in case:
            print(f"skip {case['case']}: {case['status']}, nothing to approve")
            continue
        png, meta = baseline_paths(report["project"], report["environment"], case["case"])
        os.makedirs(os.path.dirname(png), exist_ok=True)
        shutil.copyfile(os.path.join(report["run_dir"], case["image"]), png)
        with open(meta, "w", encoding="utf-8") as f:   # its problems become the accepted ones
            json.dump({"case": case["case"], "approved": time.strftime("%Y-%m-%d %H:%M:%S"), "run": report["run"],
                       "environment": report["environment"], "renderer": report["renderer"],
                       "engine_version": report["engine_version"], "project_version": report["project_version"],
                       "config": case["config"], "size": case["size"], "problems": case["problems"]}, f, indent=1)
        print(f"approved {case['case']} ({case['status']})")
        approved += 1
    return finish(judge(args, report)) if approved else 1


def cmd_reject(args):
    report = load_run(args)
    for case in selected(report, args):
        if "image" in case and os.path.exists(os.path.join(report["run_dir"], case["image"])):
            os.remove(os.path.join(report["run_dir"], case["image"]))   # cannot be approved any more
        case.update(status=REJECTED, reasons=["rejected: fix the cause and run again"])
        print(f"rejected {case['case']}")
    return finish(report)


def cmd_compare(args):
    report = load_run(args)
    config = load_config(report["project"])
    report.update(thresholds=config["thresholds"], ignore=config["ignore"])
    for c in report["cases"]:
        if c["status"] not in (ERROR, REJECTED):
            c["status"] = None
    return finish(judge(args, report))


def cmd_report(args):
    return finish(load_run(args))


def main(argv=None):
    parser = argparse.ArgumentParser(prog="visual_test.py", description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("run", "compare", "approve", "reject", "report"):
        p = sub.add_parser(name)
        p.add_argument("--project", default=".")
        p.add_argument("--exe", default=DEFAULT_EXE)
        if name == "run":
            p.add_argument("--scenario", help="comma-separated scenario names")
            p.add_argument("--category", help="comma-separated CATEGORY values")
            p.add_argument("--matrix", default="default", help="a matrix of visual_tests/config.json")
            p.add_argument("--changed", action="store_true", help="only scenarios whose WATCH globs match git changes")
            p.add_argument("--jobs", type=int, default=DEFAULT_JOBS, help="games run at once (default %(default)s)")
            p.add_argument("--visible", action="store_true", help="show the windows (no --hidden)")
            p.add_argument("--env", help="baseline environment name (default: from the platform and GPU)")
        else:
            p.add_argument("--run", help="a run id under visual_tests/results (default: the latest)")
        if name in ("approve", "reject"):
            p.add_argument("--case", action="append", default=[], help="case id or glob (repeatable)")
        if name == "approve":
            p.add_argument("--all", action="store_true", help="every case of the run")
    args = parser.parse_args(argv)
    if not os.path.exists(os.path.join(args.project, "project.json")):
        raise SystemExit(f"no project.json in {args.project}")
    return {"run": cmd_run, "compare": cmd_compare, "approve": cmd_approve, "reject": cmd_reject,
            "report": cmd_report}[args.command](args)


if __name__ == "__main__":
    sys.exit(main())
