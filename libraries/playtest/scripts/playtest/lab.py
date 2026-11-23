"""Balance Lab and batch runner: a study (seeds x policies x CEOs x modes x scenarios) run through
core.record in independent worker processes, then distributions, alerts and reports.

    study JSON -> jobs (expand) -> one process per job (tools/playtest.py run-job) -> results
    -> runs.csv, daily.csv, aggregates.json, alerts.json, failures.json, report.html, report.md, repros/

What is game-specific comes from optional adapter hooks (docs/PLAYTEST.md "Adapter"): FIXTURE, run_row,
COHORT_KEYS, cohort_stats, ALERTS + alerts, EXPLOITS, REPORT_COLUMNS, SERIES; without them the generic columns,
alerts and report remain.

Stdlib only. Reports are balance probes, not proofs of fun: matched seeds are not identical events.
"""

import csv
import html
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile
import time

from playtest import core

STUDY_KEYS = {
    "schema_version", "study", "mode", "scenario", "scenarios", "days", "max_ticks", "run_seeds", "policy_seeds",
    "policies", "ceos", "modes", "workers", "decision_interval_ticks", "hash_every_ticks", "snapshot_every_ticks",
    "stall_days", "keep", "verify_sample", "shrink", "overrides", "alerts", "wall_seconds_per_run", "faults",
    "description", "report", "runs",
}
ALERT_DEFAULTS = {  # the game adds its own thresholds (adapter ALERTS)
    "success_rate_low": 0.05,        # a cohort almost never survives the horizon
    "perf_drop": 0.2,                # ticks/s below the baseline by this share
}


# --- study -> jobs --------------------------------------------------------------------------------

def load_study(path: str) -> dict:
    study = core.read_json(path)
    unknown = set(study) - STUDY_KEYS
    if unknown:
        raise ValueError(f"unknown study keys {sorted(unknown)} (allowed: {sorted(STUDY_KEYS)})")
    if study.get("schema_version", 1) != 1:
        raise ValueError(f"study schema_version {study.get('schema_version')} not supported (1)")
    if study.get("mode", "headless") != "headless":
        raise ValueError("a study is headless; UI scenarios run with `playtest.py ui`")
    return study


def fixture(scenario: dict, adapter) -> str:
    return scenario.get("fixture", getattr(adapter, "FIXTURE", "start"))


def expand(study: dict, adapter) -> list:
    """Every run of the matrix as a self-contained job (also its repro recipe)."""
    unknown = set(study.get("alerts", {})) - set(ALERT_DEFAULTS) - set(getattr(adapter, "ALERTS", {}))
    if unknown:
        raise ValueError(f"unknown alert thresholds {sorted(unknown)}")
    policies = list(adapter.POLICIES) if study.get("policies", "all") == "all" else study["policies"]
    bad = [p for p in policies if p not in adapter.POLICIES]
    if bad:
        raise ValueError(f"unknown policies {bad}; this game has {sorted(adapter.POLICIES)}")
    seeds = study.get("run_seeds", {"start": 1, "count": 3})
    scenarios = study.get("scenarios") or [study.get("scenario", {})]
    config = {k: study[k] for k in ("decision_interval_ticks", "hash_every_ticks", "snapshot_every_ticks", "stall_days", "faults")
              if k in study}
    config["max_days"] = study.get("days", 30)
    config["max_ticks"] = study.get("max_ticks", config["max_days"] * 2000)
    config["wall_seconds"] = study.get("wall_seconds_per_run", 600)
    jobs = []
    for scenario in scenarios:
        for mode in study.get("modes", [None]):
            for ceo in study.get("ceos", [None]):
                sc = dict(scenario, **({"mode": mode} if mode else {}), **({"ceo": ceo} if ceo else {}))
                if study.get("overrides"):
                    sc["overrides"] = study["overrides"]
                for policy in policies:
                    for policy_seed in study.get("policy_seeds", [17]):
                        for seed in range(seeds["start"], seeds["start"] + seeds["count"]):
                            run_id = "-".join(str(x) for x in (fixture(sc, adapter), mode or "std", ceo or "anyceo",
                                                                policy, seed, policy_seed))
                            if "runs" in study and {"policy": policy, "seed": seed} not in study["runs"]:
                                continue  # a targeted subset of the matrix: only these (policy, seed) pairs
                            jobs.append({"id": run_id, "scenario": sc, "seed": seed, "policy": policy,
                                         "policy_seed": policy_seed, "config": config})
    return jobs


def dry_run(study: dict, adapter) -> dict:
    jobs = expand(study, adapter)
    days = study.get("days", 30)
    return {"runs": len(jobs), "days_per_run": days, "game_days": len(jobs) * days,
            "workers": study.get("workers", 1), "policies": sorted({j["policy"] for j in jobs}),
            "first_jobs": jobs[:3]}


# --- one job, in its own process ------------------------------------------------------------------

def peak_memory_mb():
    """This process's peak working set (Windows) or max RSS (POSIX); None if unknown."""
    try:
        if sys.platform == "win32":
            import ctypes
            from ctypes import wintypes

            class PMC(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
                    (n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                                   "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                                   "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
            pmc = PMC()
            pmc.cb = ctypes.sizeof(PMC)
            k32 = ctypes.windll.kernel32
            k32.GetCurrentProcess.restype = wintypes.HANDLE
            k32.K32GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
            if not k32.K32GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb):
                return None
            return round(pmc.PeakWorkingSetSize / 2**20, 1)
        import resource
        return round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / (2**20 if sys.platform == "darwin" else 1024), 1)  # bytes on macOS, KiB on Linux
    except Exception:
        return None


def run_job(adapter, job: dict, out_dir: str, keep: str = "failures", shrink: bool = True, manifest_extra=None) -> dict:
    """Record one job; keep its bundle when it failed (or always); confirm a failure by replaying
    it, then shrink it. Returns the run summary row."""
    cpu = time.process_time()
    rec = core.record(adapter, job["scenario"], job["seed"], job["policy"], job["policy_seed"], job["config"],
                      manifest_extra=dict(manifest_extra or {}, run_id=job["id"]))
    rep = rec["report"]
    result = {"job": job, "report": {k: v for k, v in rep.items() if k != "daily"}, "daily": rep["daily"],
              "cpu_seconds": round(time.process_time() - cpu, 3), "peak_memory_mb": peak_memory_mb(), "bundle": None}
    failed = rep["status"] == "fail"
    if keep == "all" or (failed and keep != "none"):
        folder = core.save_bundle(os.path.join(out_dir, "bundles", job["id"]), rec)
        result["bundle"] = folder
        result["bundle_bytes"] = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(folder) for f in fs)
        if failed and rep["failure"]["kind"] != "timeout":
            bundle = core.load_bundle(folder)
            check = core.verify(adapter, bundle, runs=1)
            result["reproducible"] = check["reproducible"]
            if shrink and check["reproducible"]:
                small = core.shrink(adapter, bundle)
                core._write(os.path.join(folder, "repro.jsonl"), core._jsonl(small["actions"]))
                result["shrunk"] = {k: small[k] for k in ("original", "minimal", "confirmed", "replays")}
            core.write_json(os.path.join(folder, "report.json"), dict(rep, verify=check, shrunk=result.get("shrunk")))
            _write_report_md(folder, job, rep, result)
    return result


def _write_report_md(folder, job, rep, result):
    f = rep["failure"]
    lines = [f"# Failure: {f['signature']}", "", f"- run: `{job['id']}` (seed {job['seed']}, policy {job['policy']}, "
             f"policy seed {job['policy_seed']})", f"- scenario: `{json.dumps(job['scenario'])}`",
             f"- tick {f['tick']}, day {rep['days']}: {f['detail']}",
             f"- reproducible: {result.get('reproducible')}; shrunk: {result.get('shrunk')}", "",
             "Reproduce and inspect:", "", "```bash", f"python tools/playtest.py verify {folder}",
             f"python tools/playtest.py debug {folder} --jump failure", "```"]
    if f.get("traceback"):
        lines += ["", "```", f["traceback"].strip()[-3000:], "```"]
    core._write(os.path.join(folder, "report.md"), "\n".join(lines) + "\n")


# --- running a study ------------------------------------------------------------------------------

def run_study(study: dict, adapter, out_dir: str, project: str, tool: str, baseline: str | None = None,
              manifest_extra=None, log=print) -> dict:
    """Every job in a worker process (`workers` at once, each with a hard timeout), then reports."""
    jobs = expand(study, adapter)
    os.makedirs(os.path.join(out_dir, "jobs"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "repros"), exist_ok=True)
    core.write_json(os.path.join(out_dir, "manifest.json"), {"study": study, "jobs": len(jobs), **(manifest_extra or {})})
    workers, pending, running, results = max(1, int(study.get("workers", 1))), list(jobs), {}, []
    timeout = study.get("wall_seconds_per_run", 600) * 3 + 60 + (240 if study.get("shrink", True) else 0)  # + verify and shrink
    started = time.perf_counter()
    while pending or running:
        while pending and len(running) < workers:
            job = pending.pop(0)
            job_path = os.path.join(out_dir, "jobs", job["id"] + ".json")
            core.write_json(job_path, dict(job, keep=study.get("keep", "failures"), shrink=study.get("shrink", True)))
            core.write_json(os.path.join(out_dir, "repros", job["id"] + ".json"), job)
            cmd = [sys.executable, tool, "run-job", "--project", project, "--job", job_path, "--out", out_dir]
            errfile = tempfile.TemporaryFile("w+", encoding="utf-8", errors="replace")  # a pipe could fill and stall the worker
            running[job["id"]] = (subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=errfile), time.perf_counter(), job, errfile)
        time.sleep(0.05)
        for run_id, (proc, t0, job, errfile) in list(running.items()):
            if proc.poll() is None and time.perf_counter() - t0 < timeout:
                continue
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            errfile.seek(0)
            err = errfile.read()
            errfile.close()
            path = os.path.join(out_dir, "jobs", run_id + ".result.json")
            if os.path.isfile(path):
                results.append(core.read_json(path))
            else:  # the worker died or hung: still a result, with its stderr tail
                kind = "timeout" if time.perf_counter() - t0 >= timeout else "crash"
                results.append({"job": job, "daily": [], "bundle": None, "report": {
                    "status": "fail", "days": 0, "ticks": 0, "final": {}, "commands": {},
                    "failure": {"kind": kind, "id": "worker", "signature": f"{kind}:worker", "tick": None,
                                "detail": err.strip()[-2000:] or f"exit {proc.returncode}"}}})
            del running[run_id]
            log(f"[{len(results)}/{len(jobs)}] {run_id}: {results[-1]['report']['status']}")
    wall = time.perf_counter() - started
    verified = _verify_sample(study, adapter, results, out_dir)
    return write_reports(study, adapter, results, out_dir, wall, verified, baseline)


def _verify_sample(study, adapter, results, out_dir):
    """Record `verify_sample` passing runs again with bundles and replay them: a determinism probe."""
    n = int(study.get("verify_sample", 1))
    out = []
    for r in [r for r in results if r["report"]["status"] != "fail"][:n]:
        job = r["job"]
        rec = core.record(adapter, job["scenario"], job["seed"], job["policy"], job["policy_seed"], job["config"])
        folder = core.save_bundle(os.path.join(out_dir, "bundles", job["id"] + "-verify"), rec)
        v = core.verify(adapter, core.load_bundle(folder), runs=2)
        same_as_worker = core.digest(rec["report"]["final"]) == core.digest(r["report"]["final"])
        out.append({"run": job["id"], "bundle": folder, "reproducible": v["reproducible"] and same_as_worker,
                    "mismatch": next((x["mismatch"] for x in v["runs"] if x["mismatch"]), None)})
    return out


# --- statistics -----------------------------------------------------------------------------------

def quantile(values, q):
    v = sorted(values)
    if not v:
        return None
    pos = (len(v) - 1) * q
    lo, hi = math.floor(pos), math.ceil(pos)
    return v[lo] + (v[hi] - v[lo]) * (pos - lo)


def describe(values) -> dict:
    v = [x for x in values if isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)]
    if not v:
        return {"n": 0}
    return {"n": len(v), "mean": round(statistics.fmean(v), 2), "median": round(quantile(v, 0.5), 2),
            "p10": round(quantile(v, 0.1), 2), "p90": round(quantile(v, 0.9), 2), "min": min(v), "max": max(v)}


def wilson(k: int, n: int, z: float = 1.96) -> dict:
    """Proportion k/n with its 95% Wilson interval."""
    if n == 0:
        return {"k": 0, "n": 0, "rate": None, "low": None, "high": None}
    p = k / n
    d = 1 + z * z / n
    c = (p + z * z / (2 * n)) / d
    h = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return {"k": k, "n": n, "rate": round(p, 3), "low": round(max(0, c - h), 3), "high": round(min(1, c + h), 3)}


def first_day(daily, test):
    return next((row["day"] for row in daily if test(row)), None)


def run_row(r: dict, adapter) -> dict:
    """A runs.csv row: the generic columns, then the adapter's `run_row(result)` game columns."""
    job, rep = r["job"], r["report"]
    return {
        "run": job["id"], "policy": job["policy"], "seed": job["seed"], "policy_seed": job["policy_seed"],
        "fixture": fixture(job["scenario"], adapter), "status": rep["status"], "end_reason": rep.get("end_reason"),
        "failure": (rep.get("failure") or {}).get("signature"), "days": rep.get("days"),
        "accepted": rep.get("commands", {}).get("accepted"), "rejected": rep.get("commands", {}).get("rejected"),
        "features": len(rep.get("features", [])),
        "wall_seconds": rep.get("wall_seconds"), "cpu_seconds": r.get("cpu_seconds"),
        "ticks_per_second": rep.get("ticks_per_second"), "peak_memory_mb": r.get("peak_memory_mb"),
        "bundle_bytes": r.get("bundle_bytes"), "bundle": r.get("bundle"),
        **(adapter.run_row(r) if hasattr(adapter, "run_row") else {}),
    }


def cohorts(rows, adapter) -> dict:
    keys = getattr(adapter, "COHORT_KEYS", ("policy", "fixture"))
    out = {}
    for row in rows:
        out.setdefault(" / ".join(str(row[k] or "-") for k in keys), []).append(row)
    return out


def aggregate(rows, adapter) -> dict:
    """Per cohort: survival and failure rates, generic distributions, then the adapter's `cohort_stats(rows)`."""
    out = {}
    for name, rs in cohorts(rows, adapter).items():
        out[name] = {
            "runs": len(rs),
            "survived_horizon": wilson(sum(r["status"] == "ok" for r in rs), len(rs)),
            "failed": wilson(sum(r["status"] == "fail" for r in rs), len(rs)),
            **{k: describe(r[k] for r in rs) for k in ("features", "ticks_per_second")},
            **(adapter.cohort_stats(rs) if hasattr(adapter, "cohort_stats") else {}),
        }
    return out


# --- alerts ---------------------------------------------------------------------------------------

def alerts(study, rows, agg, results, adapter, baseline=None) -> list:
    t = {**ALERT_DEFAULTS, **getattr(adapter, "ALERTS", {}), **study.get("alerts", {})}
    out = []

    def add(kind, cohort, observed, threshold, example, context):
        out.append({"kind": kind, "cohort": cohort, "observed": observed, "threshold": threshold,
                    "example": {k: example.get(k) for k in ("run", "seed", "policy", "ceo", "days", "bundle")} if example else None,
                    "repro": f"repros/{example['run']}.json" if example else None, "context": context})

    groups = cohorts(rows, adapter)
    for name, a in agg.items():
        s = a["survived_horizon"]
        if s["rate"] is not None and s["rate"] <= t["success_rate_low"] and s["n"] >= 3:
            add("near_zero_success", name, s, t["success_rate_low"], groups[name][0], "almost no run reached the horizon")
    if hasattr(adapter, "alerts"):
        adapter.alerts(rows, groups, agg, t, add)
    # failures, grouped (exploits, soft locks, crashes)
    for sig, group in group_failures(results).items():
        first = group[0]
        add(_kind_of(sig, adapter), sig, len(group), 1, run_row(first, adapter), first["report"]["failure"].get("detail", ""))
    if baseline:
        base = core.read_json(baseline)
        for name, a in agg.items():
            old = (base.get(name) or {}).get("ticks_per_second", {}).get("median")
            new = a["ticks_per_second"].get("median")
            if old and new and new < old * (1 - t["perf_drop"]):
                add("performance_regression", name, new, old * (1 - t["perf_drop"]), None, f"baseline {old} ticks/s")
    return out


def _kind_of(signature: str, adapter) -> str:
    kind = signature.split(":")[0]
    if signature.startswith(tuple(getattr(adapter, "EXPLOITS", ()))):
        return "economy_exploit"
    return {"softlock": "progress_plateau", "invariant": "invariant", "exception": "crash", "timeout": "timeout",
            "crash": "crash", "rejected_mutation": "rejected_command_changed_state"}.get(kind, kind)


def group_failures(results) -> dict:
    """One entry per unique bug: (kind/id, location) signature -> the runs that hit it."""
    out = {}
    for r in results:
        f = r["report"].get("failure")
        if f:
            out.setdefault(f["signature"], []).append(r)
    return out


# --- reports --------------------------------------------------------------------------------------

def _csv(path, rows):
    rows = list(rows)
    if not rows:
        core._write(path, "")
        return
    keys = list(dict.fromkeys(k for r in rows for k, v in r.items() if not isinstance(v, (dict, list))))
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=keys, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)


def write_reports(study, adapter, results, out_dir, wall, verified, baseline=None) -> dict:
    rows = [run_row(r, adapter) for r in results]
    agg = aggregate(rows, adapter)
    al = alerts(study, rows, agg, results, adapter, baseline)
    failures = [{"signature": sig, "count": len(g), "kind": _kind_of(sig, adapter), "runs": [x["job"]["id"] for x in g][:20],
                 "example": g[0]["report"]["failure"], "bundle": g[0].get("bundle"),
                 "reproducible": g[0].get("reproducible"), "shrunk": g[0].get("shrunk")}
                for sig, g in group_failures(results).items()]
    days = sum(r["days"] or 0 for r in rows)
    cpu = sum(r["cpu_seconds"] or 0 for r in rows)
    summary = {
        "study": study.get("study"), "runs": len(rows), "simulated_days": days,
        "status": {s: sum(r["status"] == s for r in rows) for s in ("ok", "ended", "fail")},
        "commands": {"accepted": sum(r["accepted"] or 0 for r in rows), "rejected": sum(r["rejected"] or 0 for r in rows)},
        "run_ends": {e: sum(r["end_reason"] == e for r in rows) for e in sorted({r["end_reason"] for r in rows if r["end_reason"]})},
        "unique_failures": len(failures), "reproducible_failures": sum(bool(f["reproducible"]) for f in failures),
        "failure_kinds": {k: sum(f["count"] for f in failures if f["kind"] == k) for k in sorted({f["kind"] for f in failures})},
        "unique_feature_states": len(set().union(*(set(r["report"].get("features", [])) for r in results))) if results else 0,
        "replay_divergence": sum(not v["reproducible"] for v in verified), "determinism_checks": verified,
        "cpu_seconds_per_1000_days": round(cpu / days * 1000, 2) if days else None,
        "wall_seconds": round(wall, 1), "peak_memory_mb": max((r["peak_memory_mb"] or 0 for r in rows), default=None),
        "bundle_bytes_total": sum(r["bundle_bytes"] or 0 for r in rows),
        "alerts": len(al), "out": out_dir,
    }
    _csv(os.path.join(out_dir, "runs.csv"), rows)
    _csv(os.path.join(out_dir, "daily.csv"), ({"run": r["job"]["id"], "policy": r["job"]["policy"], **d} for r in results for d in r["daily"]))
    core.write_json(os.path.join(out_dir, "aggregates.json"), agg)
    core.write_json(os.path.join(out_dir, "alerts.json"), al)
    core.write_json(os.path.join(out_dir, "failures.json"), failures)
    core.write_json(os.path.join(out_dir, "summary.json"), summary)
    core._write(os.path.join(out_dir, "report.html"), _html(study, summary, agg, al, failures, results, adapter))
    core._write(os.path.join(out_dir, "report.md"), _md(summary, al, failures))
    return summary


def _md(summary, al, failures) -> str:
    lines = [f"# Study {summary['study']}", "", "Balance probe, not proof of fun. " +
             ", ".join(f"{k}: {v}" for k, v in summary.items() if not isinstance(v, (dict, list))), "", "## Unique failures", ""]
    lines += [f"- `{f['signature']}` x{f['count']} ({f['kind']}), reproducible {f['reproducible']}, shrunk {f['shrunk']}, "
              f"bundle `{f['bundle']}`" for f in failures] or ["- none"]
    lines += ["", "## Alerts", ""]
    lines += [f"- **{a['kind']}** {a['cohort']}: observed {a['observed']}, threshold {a['threshold']}; {a['context']}"
              f" (repro `{a['repro']}`)" for a in al] or ["- none"]
    return "\n".join(lines) + "\n"


def _svg_lines(series: dict, width=640, height=180) -> str:
    """A daily metric's median per day, one polyline per cohort."""
    pts = [v for s in series.values() for v in s if v is not None]
    if not pts:
        return ""
    lo, hi = min(pts + [0]), max(pts + [1])
    n = max(len(s) for s in series.values())
    colours = ["#2b6cb0", "#c05621", "#2f855a", "#9b2c2c", "#6b46c1", "#b7791f", "#2c7a7b", "#4a5568"]
    zero = height + lo / (hi - lo) * height
    out = [f'<svg viewBox="0 0 {width} {height + 20}" width="{width}" height="{height + 20}">',
           f'<line x1="0" y1="{zero:.1f}" x2="{width}" y2="{zero:.1f}" stroke="#ccc"/>']
    for i, (name, s) in enumerate(series.items()):
        xy = " ".join(f"{j / max(1, n - 1) * width:.1f},{height - (v - lo) / (hi - lo) * height:.1f}" for j, v in enumerate(s) if v is not None)
        out.append(f'<polyline fill="none" stroke="{colours[i % len(colours)]}" stroke-width="1.5" points="{xy}"><title>{html.escape(name)}</title></polyline>')
    out.append(f'<text x="0" y="{height + 15}" font-size="11">min {lo:,.0f} / max {hi:,.0f}, {n} days</text></svg>')
    return "".join(out)


def _html(study, summary, agg, al, failures, results, adapter) -> str:
    esc = lambda v: html.escape(str(v))
    metric, label = getattr(adapter, "SERIES", (None, None))  # (daily metric, chart label); none: no chart
    by_cohort = {}
    for r in results if metric else ():
        key = f"{r['job']['policy']} / {r['job']['scenario'].get('ceo') or '-'}"
        for d in r["daily"]:
            by_cohort.setdefault(key, {}).setdefault(d["day"], []).append(d.get(metric))
    series = {k: [quantile([x for x in days.get(day, []) if x is not None], 0.5) for day in range(1, max(days) + 1)]
              for k, days in by_cohort.items() if days}
    chart = f"<h2>Median {esc(label)} per day</h2>{_svg_lines(series)}" if metric else ""
    # (header, aggregates key, stats joined by " / ")
    columns = getattr(adapter, "REPORT_COLUMNS", (("failed", "failed", ("rate",)), ("ticks/s", "ticks_per_second", ("median",))))
    head = f"<th>{esc(' / '.join(getattr(adapter, 'COHORT_KEYS', ('policy', 'fixture'))))}</th><th>n</th><th>survived [95%]</th>" + \
        "".join(f"<th>{esc(h)}</th>" for h, _, _ in columns)
    rows = "".join(
        f"<tr><td>{esc(name)}</td><td>{a['runs']}</td><td>{esc(a['survived_horizon']['rate'])} [{esc(a['survived_horizon']['low'])}-{esc(a['survived_horizon']['high'])}]</td>"
        + "".join(f"<td>{' / '.join(esc(a[key].get(st)) for st in stats)}</td>" for _, key, stats in columns) + "</tr>"
        for name, a in agg.items())
    alerts_html = "".join(f"<li><b>{esc(a['kind'])}</b> {esc(a['cohort'])}: observed {esc(a['observed'])}, threshold "
                          f"{esc(a['threshold'])}. {esc(a['context'])} <code>{esc(a['repro'])}</code></li>" for a in al) or "<li>none</li>"
    fails = "".join(f"<li><code>{esc(f['signature'])}</code> x{f['count']} reproducible={esc(f['reproducible'])} "
                    f"shrunk={esc(f['shrunk'])}<br><code>{esc(f['bundle'])}</code></li>" for f in failures) or "<li>none</li>"
    return f"""<!doctype html><meta charset="utf-8"><title>{esc(study.get('study'))}</title>
<style>body{{font:14px system-ui;margin:24px;max-width:1100px}}table{{border-collapse:collapse}}td,th{{border:1px solid #ddd;padding:3px 6px;text-align:right}}td:first-child{{text-align:left}}</style>
<h1>Study {esc(study.get('study'))}</h1><p>headless_simulation; balance probe, not proof of fun; matched seeds are not identical events.</p>
<pre>{esc(json.dumps({k: v for k, v in summary.items() if k != 'determinism_checks'}, indent=1))}</pre>
{chart}
<h2>Cohorts</h2><table><tr>{head}</tr>{rows}</table>
<h2>Alerts</h2><ul>{alerts_html}</ul><h2>Unique failures</h2><ul>{fails}</ul>"""
