"""The HTML page of a visual test run (tools/visual_test.py, docs/VISUAL-TESTS.md). One self-contained
file beside the run's images: failures first, baseline / current / diff side by side, an overlay slider."""

import html
import json
import os

ORDER = ["ERROR", "FAIL", "STRUCTURAL_FAIL", "RESOLUTION_MISMATCH", "MISSING_BASELINE", "REJECTED", "PASS"]
COLOURS = {"PASS": "#2e7d32", "FAIL": "#c62828", "STRUCTURAL_FAIL": "#d84315", "RESOLUTION_MISMATCH": "#6a1b9a",
           "MISSING_BASELINE": "#f9a825", "ERROR": "#000", "REJECTED": "#555"}
STYLE = """body{font:14px system-ui,sans-serif;margin:20px;background:#f4f4f4;color:#222}
table{border-collapse:collapse;background:#fff}td,th{padding:4px 10px;border:1px solid #ddd;text-align:left}
.badge{color:#fff;padding:2px 8px;border-radius:3px;font-weight:600}
.case{background:#fff;margin:16px 0;padding:12px;border-left:6px solid #888}
.imgs{display:flex;gap:8px;flex-wrap:wrap}.imgs figure{margin:0;flex:1;min-width:300px}
.imgs img{width:100%;border:1px solid #ccc;image-rendering:pixelated}figcaption{font-weight:600}
.overlay{position:relative}.overlay img.top{position:absolute;left:0;top:0}
code{background:#eee;padding:1px 4px}details{margin-top:6px}"""


def _e(value):
    return html.escape(str(value))


def _case(c, report):
    status = c["status"]
    out = [f'<div class="case" id="{_e(c["case"])}" style="border-color:{COLOURS.get(status, "#888")}">',
           f'<h3><span class="badge" style="background:{COLOURS.get(status, "#888")}">{status}</span> {_e(c["case"])}</h3>',
           f'<div>config <code>{_e(json.dumps(c.get("config")))}</code> scene <code>{_e(c.get("scene"))}</code>']
    r = c.get("compare") or {}
    if r.get("size_match"):
        out.append(f' &middot; SSIM {r["ssim"]:.4f} (worst block {r["min_ssim"]:.3f}) &middot; changed '
                   f'{r["changed_pct"]:.3f}% ({r["changed_pixels"]} px) &middot; mean diff {r["mean_diff"]:.2f}')
    out.append("</div>")
    if c.get("reasons"):
        out.append("<ul>" + "".join(f"<li>{_e(x)}</li>" for x in c["reasons"]) + "</ul>")
    figures = [(label, c.get(key)) for label, key in (("Baseline", "baseline"), ("Current", "image"), ("Difference", "diff"))]
    figures = [(label, path) for label, path in figures if path]
    if figures:
        out.append('<div class="imgs">' + "".join(
            f'<figure><figcaption>{label}</figcaption><a href="{_e(path)}"><img loading="lazy" src="{_e(path)}"></a></figure>'
            for label, path in figures) + "</div>")
    if c.get("baseline") and c.get("image") and r.get("size_match"):
        out.append(f'<details><summary>Overlay (slide: baseline to current)</summary><input type="range" min="0" max="100" '
                   f'value="50" oninput="this.nextElementSibling.lastElementChild.style.opacity=this.value/100">'
                   f'<div class="overlay"><img src="{_e(c["baseline"])}" style="width:100%">'
                   f'<img class="top" src="{_e(c["image"])}" style="width:100%;opacity:.5"></div></details>')
    if r.get("regions"):
        out.append("<details><summary>Changed regions (image pixels)</summary><table><tr><th>x, y, w, h</th><th>pixels</th></tr>"
                   + "".join(f'<tr><td>{", ".join(str(int(v)) for v in g["box"])}</td><td>{g["pixels"]}</td></tr>'
                             for g in r["regions"]) + "</table></details>")
    problems = c.get("problems") or []
    if problems:
        new = {f"{p['kind']}:{p['element']}" for p in c.get("new_problems", problems)}
        out.append(f"<details{' open' if new else ''}><summary>UI problems ({len(new)} new, {len(problems)} total)</summary>"
                   "<table><tr><th></th><th>kind</th><th>element</th><th>rect (canvas units)</th><th>detail</th></tr>"
                   + "".join(f'<tr><td>{"NEW" if p["kind"] + ":" + p["element"] in new else "accepted"}</td>'
                             f'<td>{_e(p["kind"])}</td><td>{_e(p["element"])}</td><td>{_e(p["rect"])}</td><td>{_e(p["detail"])}</td></tr>'
                             for p in problems) + "</table></details>")
    if c.get("log_problems"):
        out.append("<details><summary>Log warnings and errors</summary><pre>" + _e("\n".join(c["log_problems"][:80])) + "</pre></details>")
    if status != "PASS":
        out.append(f'<div>reproduce: <code>python tools/visual_test.py run --project {_e(report["project"])} '
                   f'--scenario {_e(c.get("scenario"))}</code> &middot; approve: <code>python tools/visual_test.py approve '
                   f'--project {_e(report["project"])} --case "{_e(c["case"])}"</code></div>')
    out.append("</div>")
    return "".join(out)


def write_html(report, path):
    cases = sorted(report["cases"], key=lambda c: (ORDER.index(c["status"]) if c["status"] in ORDER else 0, c["case"]))
    rows = "".join(f'<tr><td><a href="#{_e(c["case"])}">{_e(c["case"])}</a></td><td><span class="badge" '
                   f'style="background:{COLOURS.get(c["status"], "#888")}">{c["status"]}</span></td>'
                   f'<td>{_e((c.get("reasons") or [""])[0])}</td></tr>' for c in cases)
    passed = sum(c["status"] == "PASS" for c in cases)
    head = (f'<h1>Visual tests: {_e(report["project_name"])}</h1><p>run <code>{_e(report["run"])}</code> &middot; '
            f'{passed}/{len(cases)} passed &middot; project {_e(report["project_version"])} &middot; engine '
            f'{_e(report["engine_version"])} &middot; environment <code>{_e(report["environment"])}</code> '
            f'({_e(report["renderer"])}) &middot; matrix {_e(report["matrix"])} &middot; thresholds '
            f'<code>{_e(json.dumps(report["thresholds"]))}</code></p>')
    page = (f"<!doctype html><meta charset=utf-8><title>Visual tests {_e(report['run'])}</title><style>{STYLE}</style>"
            f"{head}<table><tr><th>case</th><th>status</th><th>first reason</th></tr>{rows}</table>"
            + "".join(_case(c, report) for c in cases))
    with open(path, "w", encoding="utf-8") as f:
        f.write(page)
    return os.path.abspath(path)
