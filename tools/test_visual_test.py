"""Tests of the visual regression tool itself (tools/visual_test.py, docs/VISUAL-TESTS.md), without a game:
synthetic PNGs through the real IYIEngine --compare-images, a fake run judged and approved.

    python tools/test_visual_test.py
"""

import json
import os
import shutil
import struct
import sys
import tempfile
import threading
import time
import unittest
import zlib
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import visual_test as vt  # noqa: E402

W, H = 320, 180


def write_png(path, pixels):
    """pixels: rows of (r, g, b)."""
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in pixels)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", len(pixels[0]), len(pixels), 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def screen(button=(40, 40, 80, 30), w=W, h=H, noise=None, extra=None):
    """A dark screen with a light gradient band and one 'button'."""
    rows = [[(20, 22, 30 + x // 8) for x in range(w)] for _ in range(h)]
    for y in range(h * 2 // 3, h * 2 // 3 + 20):
        for x in range(w):
            rows[y][x] = (x // 2, 100, 160)
    if button:
        bx, by, bw, bh = button
        for y in range(by, by + bh):
            for x in range(bx, bx + bw):
                rows[y][x] = (200, 60, 60) if (x - bx) % 79 and (y - by) % 29 else (255, 255, 255)
    if noise:
        noise(rows)
    if extra:
        extra(rows)
    return rows


class Args:
    def __init__(self, project, **kw):
        self.project, self.exe, self.run, self.case, self.all = project, vt.DEFAULT_EXE, None, [], False
        self.__dict__.update(kw)


@unittest.skipUnless(os.path.exists(vt.DEFAULT_EXE), "needs a development build of IYIEngine.exe")
class VisualTestTool(unittest.TestCase):
    def setUp(self):
        self.project = tempfile.mkdtemp(prefix="iyi_vt_test_")
        with open(os.path.join(self.project, "project.json"), "w") as f:
            f.write("{}")
        self.run_dir = os.path.join(self.project, "visual_tests", "results", "r1")
        os.makedirs(self.run_dir)
        with open(os.path.join(self.project, "visual_tests", "results", "latest.txt"), "w") as f:
            f.write("r1")
        self.cases = []

    def tearDown(self):
        shutil.rmtree(self.project, ignore_errors=True)

    def add_case(self, name, current, baseline=None, masks=(), problems=(), accepted=()):
        case = f"s/{name}@{W}x{H}"
        os.makedirs(os.path.join(self.run_dir, "s"), exist_ok=True)
        write_png(os.path.join(self.run_dir, case + ".png"), current)
        if baseline is not None:
            png, meta = vt.baseline_paths(self.project, "test-env", case)
            os.makedirs(os.path.dirname(png), exist_ok=True)
            write_png(png, baseline)
            with open(meta, "w") as f:
                json.dump({"problems": list(accepted)}, f)
        self.cases.append({"case": case, "scenario": "s", "capture": name, "config": {"size": f"{W}x{H}"},
                           "image": case + ".png", "size": [W, H], "masks": list(masks), "ignore": [],
                           "problems": list(problems), "status": None, "scene": None})
        return case

    def judge(self):
        report = {"run": "r1", "run_dir": self.run_dir, "project": self.project, "project_name": "t",
                  "project_version": "x", "engine_version": "x", "matrix": "default", "environment": "test-env",
                  "renderer": "test", "thresholds": dict(vt.DEFAULT_CONFIG["thresholds"]), "ignore": [],
                  "command": "", "cases": self.cases}
        vt.judge(Args(self.project), report)
        vt.finish(report)
        return {c["capture"]: c for c in report["cases"]}

    def test_compare_cases(self):
        def aa_noise(rows):   # below the tolerance everywhere, plus a few lone antialiasing pixels past it
            for y in range(0, H, 3):
                for x in range(0, W, 5):
                    r, g, b = rows[y][x]
                    rows[y][x] = (min(255, r + 8), g, b)
            for x, y in ((10, 10), (200, 90), (300, 170)):
                rows[y][x] = (255, 255, 0)

        def clock(rows):      # dynamic content inside a masked area
            for y in range(5, 15):
                for x in range(250, 300):
                    rows[y][x] = (255, 255, 255)

        base = screen()
        self.add_case("identical", screen(), base)
        self.add_case("moved", screen(button=(60, 40, 80, 30)), base)
        self.add_case("missing_image", screen(button=None), base)
        self.add_case("aa_noise", screen(noise=aa_noise), base)
        self.add_case("masked", screen(extra=clock), base, masks=[[245, 0, 75, 20]])
        self.add_case("unmasked", screen(extra=clock), base)
        self.add_case("no_baseline", screen())
        self.add_case("other_size", screen(w=W // 2, h=H // 2), base)
        overflow = {"kind": "TEXT_CLIP", "element": "Title", "rect": [0, 0, 10, 10], "detail": "x"}
        self.add_case("structural", screen(), base, problems=[overflow])
        self.add_case("accepted", screen(), base, problems=[overflow], accepted=[overflow])
        got = self.judge()
        expect = {"identical": "PASS", "moved": "FAIL", "missing_image": "FAIL", "aa_noise": "PASS", "masked": "PASS",
                  "unmasked": "FAIL", "no_baseline": "MISSING_BASELINE", "other_size": "RESOLUTION_MISMATCH",
                  "structural": "STRUCTURAL_FAIL", "accepted": "PASS"}
        self.assertEqual({k: got[k]["status"] for k in expect}, expect)
        self.assertEqual(got["identical"]["compare"]["ssim"], 1.0)
        boxes = [r["box"] for r in got["moved"]["compare"]["regions"]]   # the old and the new edges changed
        self.assertTrue(min(b[0] for b in boxes) <= 40 and max(b[0] + b[2] for b in boxes) >= 140, boxes)
        self.assertTrue(os.path.exists(os.path.join(self.run_dir, got["moved"]["diff"])))
        self.assertTrue(os.path.exists(os.path.join(self.run_dir, "report.html")))

    def test_run_jobs_in_parallel_keeps_job_order(self):
        scenarios = os.path.join(self.project, "visual_tests", "scenarios")
        os.makedirs(scenarios)
        for name in "abc":
            with open(os.path.join(scenarios, name + ".py"), "w") as f:
                f.write("def run(t):\n    pass\n")
        live = peak = 0
        lock = threading.Lock()

        def fake_run(args, config, name, module, cfg, out_dir):   # the first job is the slowest, so order must come from the jobs
            nonlocal live, peak
            with lock:
                live += 1
                peak = max(peak, live)
            time.sleep(0.3 if name == "a" else 0.05)
            with lock:
                live -= 1
            return [], f"{name} failed", "test-renderer", []

        args = Args(self.project, scenario=None, category=None, changed=False, matrix="default", env=None, visible=False, jobs=3)
        with mock.patch.object(vt, "run_scenario", fake_run):
            self.assertEqual(vt.cmd_run(args), 2)
        self.assertEqual(peak, 3)
        report = vt.load_run(Args(self.project, run=None))
        self.assertEqual([c["case"] for c in report["cases"]], [f"{n}/@1920x1080" for n in "abc"])

    def test_approve_only_named_and_never_by_run(self):
        base = screen()
        moved = self.add_case("moved", screen(button=(60, 40, 80, 30)), base)
        other = self.add_case("other", screen(button=(60, 40, 80, 30)), base)
        self.judge()
        png_other = vt.baseline_paths(self.project, "test-env", other)[0]
        with open(png_other, "rb") as stream:
            before = stream.read()
        with self.assertRaises(SystemExit):   # no --case and no --all: refused
            vt.cmd_approve(Args(self.project))
        vt.cmd_approve(Args(self.project, case=["s/moved@*"]))
        report = vt.load_run(Args(self.project))
        status = {c["case"]: c["status"] for c in report["cases"]}
        self.assertEqual(status, {moved: "PASS", other: "FAIL"})
        with open(png_other, "rb") as stream:
            self.assertEqual(stream.read(), before)

    def test_reject_keeps_failing(self):
        case = self.add_case("moved", screen(button=(60, 40, 80, 30)), screen())
        self.judge()
        vt.cmd_reject(Args(self.project, case=[case]))
        vt.cmd_approve(Args(self.project, case=[case]))   # nothing left to approve
        report = vt.load_run(Args(self.project))
        self.assertEqual(report["cases"][0]["status"], "REJECTED")


class StructuralRules(unittest.TestCase):
    def test_rules(self):
        ui = [{"name": "Panel", "type": "Panel", "rect": [0, 0, 100, 100], "canvas_size": [1920, 1080]},
              {"name": "Label", "type": "Text", "rect": [10, 10, 50, 20], "parent": "Panel", "measured_width": 80,
               "font_size": 20, "fitted_font_size": 14},
              {"name": "Wide", "type": "Panel", "rect": [50, 10, 80, 20], "parent": "Panel", "parent_rect": [0, 0, 100, 100]},
              {"name": "Clipped", "type": "Panel", "rect": [50, 10, 80, 20], "parent_rect": [0, 0, 100, 100], "clipped": True},
              {"name": "Gone", "type": "Image", "rect": [2000, 10, 50, 20], "canvas_size": [1920, 1080]},
              {"name": "Flat", "type": "Image", "rect": [0, 0, 0, 20]},
              {"name": "A", "type": "Button", "rect": [0, 200, 100, 40]},
              {"name": "B", "type": "Button", "rect": [50, 210, 100, 40]},
              {"name": "C", "type": "Button", "rect": [0, 300, 100, 40], "enabled": False},
              {"name": "D", "type": "Button", "rect": [0, 300, 100, 40]},
              {"name": "List", "type": "ScrollView", "rect": [0, 500, 100, 50]},
              {"name": "Row", "type": "Button", "rect": [0, 520, 100, 60], "parent": "List", "clipped": True},
              {"name": "Below", "type": "Button", "rect": [0, 560, 100, 30]},
              {"name": "Glyph", "type": "Text", "rect": [0, 400, 100, 20], "missing_glyphs": "日"}]
        got = sorted(vt.problem_key(p) for p in vt.ui_problems(ui))
        self.assertEqual(got, ["BUTTON_OVERLAP:A", "FONT_SHRUNK:Label", "OFFSCREEN:Gone", "OVERFLOW:Wide",
                               "TEXT_CLIP:Label", "UNSUPPORTED_GLYPH:Glyph", "ZERO_SIZE:Flat"])

    def test_matrix(self):
        config = {"matrices": {"full": {"size": ["1280x720", "1920x1080"], "lang": ["en", "tr"]}}}
        cfgs = vt.configs_for(config, "full")
        self.assertEqual(len(cfgs), 4)
        self.assertEqual(vt.config_label(cfgs[1]), "1280x720_tr")


if __name__ == "__main__":
    unittest.main()
