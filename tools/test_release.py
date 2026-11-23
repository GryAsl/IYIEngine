"""Release pipeline unit tests. Run: python tools/test_release.py"""
import os
import sys
import tempfile
import zipfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release as R

GOOD_LOG = "m_python.Initialize returned true\nhot reload: disabled\n[INFO ] [startup] IYI_RUNTIME_READY\n"


class SteamAppId(unittest.TestCase):
    def test_shipping_rejects_0_and_480(self):
        for v in (0, 480, None):
            with self.assertRaises(R.ReleaseError) as c:
                R.check_steam_app_id({"steam_app_id": v}, True)
            self.assertIn("Spacewar", str(c.exception))

    def test_shipping_accepts_real_and_dev_accepts_480(self):
        R.check_steam_app_id({"steam_app_id": 12345}, True)
        R.check_steam_app_id({"steam_app_id": 480}, False)


class ProjectOverrides(unittest.TestCase):
    def test_overrides_load_and_must_be_an_object(self):
        with tempfile.TemporaryDirectory() as d:
            cfg_path = Path(d) / "release.demo.json"
            cfg_path.write_text('{"project_overrides": {"name": "Game Demo", "steam_app_id": 7}}', encoding="utf-8")
            self.assertEqual(R.load_release_config(cfg_path, {}).project_overrides, {"name": "Game Demo", "steam_app_id": 7})
            cfg_path.write_text('{"project_overrides": []}', encoding="utf-8")
            with self.assertRaises(R.ReleaseError):
                R.load_release_config(cfg_path, {})
            self.assertEqual(R.load_release_config(Path(d) / "missing.json", {}).project_overrides, {})


class RuntimeLog(unittest.TestCase):
    def test_good(self):
        R.validate_runtime_log(GOOD_LOG)

    def test_old_marker_not_enough(self):
        log = "m_python.Initialize returned true\nm_python.CallGameStart() completed.\nhot reload: disabled\n"
        with self.assertRaises(R.ReleaseError):
            R.validate_runtime_log(log)

    def test_broken_first_scenestart(self):
        # Engine suppresses the marker and logs the traceback when SceneStart() raises.
        log = ("m_python.Initialize returned true\nhot reload: disabled\n[ERROR] SceneStart failed\n"
               "Traceback (most recent call last):\n  File \"scene.py\", line 3\nNameError: x\n")
        with self.assertRaises(R.ReleaseError):
            R.validate_runtime_log(log)

    def test_error_with_marker_still_fails(self):
        with self.assertRaises(R.ReleaseError):
            R.validate_runtime_log(GOOD_LOG + "[ERROR] boom\n")


class PinnedDownload(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())
        self.entry = {"url": "http://x", "sha256": R.hashlib.sha256(b"good").hexdigest()}

    def test_cached_good(self):
        f = self.d / "a.zip"; f.write_bytes(b"good")
        R.ensure_pinned_download(f, self.entry)
        self.assertTrue(f.exists())

    def test_cached_corrupt_deleted_and_fails(self):
        f = self.d / "a.zip"; f.write_bytes(b"bad")
        with self.assertRaises(R.ReleaseError) as c:
            R.ensure_pinned_download(f, self.entry)
        self.assertIn("SHA-256 mismatch", str(c.exception))
        self.assertFalse(f.exists())

    def test_download_corrupt_never_lands(self):
        f = self.d / "a.zip"
        with mock.patch.object(R.urllib.request, "urlretrieve", lambda u, t: Path(t).write_bytes(b"bad")):
            with self.assertRaises(R.ReleaseError):
                R.ensure_pinned_download(f, self.entry)
        self.assertEqual(list(self.d.iterdir()), [])

    def test_download_good_atomic(self):
        f = self.d / "a.zip"
        with mock.patch.object(R.urllib.request, "urlretrieve", lambda u, t: Path(t).write_bytes(b"good")):
            R.ensure_pinned_download(f, self.entry)
        self.assertEqual(f.read_bytes(), b"good")
        self.assertEqual([p.name for p in self.d.iterdir()], ["a.zip"])


class GameTestsStage(unittest.TestCase):
    def _pipe(self, skip=False):
        d = Path(tempfile.mkdtemp())
        (d / "tools").mkdir()
        (d / "tools" / "verify.py").write_text("", encoding="utf-8")
        p = R.ReleasePipeline(d, d, skip_game_tests=skip)
        return p

    def test_failure_aborts_before_build(self):
        p = self._pipe()
        with mock.patch.object(p, "stage_preflight"), mock.patch.object(R.subprocess, "call", return_value=3), \
                mock.patch.object(p, "stage_clean_staging") as clean, \
                mock.patch.object(p, "cleanup_secrets_and_temps"), mock.patch("builtins.print"):
            self.assertEqual(p.run(), 1)
            clean.assert_not_called()

    def test_failure_message_has_command_and_code(self):
        p = self._pipe()
        with mock.patch.object(R.subprocess, "call", return_value=3), mock.patch("builtins.print"):
            with self.assertRaises(R.ReleaseError) as c:
                p.stage_game_tests()
        self.assertIn("exit code 3", str(c.exception))
        self.assertIn("verify.py", str(c.exception))
        self.assertIn("full", str(c.exception))

    def test_skip_flag_skips(self):
        p = self._pipe(skip=True)
        with mock.patch.object(p, "stage_preflight"), mock.patch.object(R.subprocess, "call") as call, \
                mock.patch.object(p, "stage_clean_staging", side_effect=R.ReleaseError("stop")), \
                mock.patch.object(p, "cleanup_secrets_and_temps"), mock.patch("builtins.print"), \
                mock.patch.object(sys, "stderr"):
            p.run()
            call.assert_not_called()


class PlatformTable(unittest.TestCase):
    def test_manifest_pins_every_platform(self):
        for py in R.PYTHON_VERSIONS:
            for name in (n for n in R.PLATFORMS if n != "android"):
                pin = R.load_runtime_pin(name, py)
                self.assertRegex(pin["sha256"], r"^[0-9a-f]{64}$")
                self.assertTrue(pin["url"].startswith("https://"))
                self.assertTrue(R.runtime_archive_name(pin).endswith((".zip", ".tar.gz")))
                self.assertTrue(pin["version"].startswith(py + "."))
                tag = py if name != "windows" else py.replace(".", "")
                self.assertIn(tag, pin["pylib"])
                self.assertIn(tag, pin["stdlib"])
        self.assertEqual(R.load_runtime_pin("windows")["pylib"], "python314.dll")

    def test_layouts(self):
        out = Path("/rel/Game-x")
        w = R.package_layout(R.PLATFORMS["windows"], out, "Game")
        self.assertEqual((w.exe, w.data, w.libs, w.app), (out / "Game.exe", out / "Game_Data", out, None))
        l = R.package_layout(R.PLATFORMS["linux"], out, "Game")
        self.assertEqual((l.exe, l.data, l.libs), (out / "Game", out / "Game_Data", out))
        m = R.package_layout(R.PLATFORMS["macos"], out, "Game")
        c = out / "Game.app" / "Contents"
        self.assertEqual((m.exe, m.data, m.libs, m.app),
                         (c / "MacOS" / "Game", c / "Resources" / "Game_Data", c / "Frameworks", out / "Game.app"))

    def test_lib_names(self):
        win = R.runtime_lib_names(R.PLATFORMS["windows"], {"pylib": "python314.dll"})
        self.assertEqual(win[0], "python314.dll")
        self.assertIn("vcruntime140.dll", win)

    def test_python_version_choice(self):
        d = Path(tempfile.mkdtemp())
        new, old = R.ReleasePipeline(d, d), R.ReleasePipeline(d, d, python="3.11")
        self.assertEqual(new.pin["version"][:5], "3.14.")
        self.assertEqual(old.pin["version"][:5], "3.11.")
        self.assertEqual(new.shipping_dir.name, "shipping")
        self.assertEqual(old.shipping_dir.name, "shipping-py311")
        self.assertEqual(R.ReleasePipeline(d, d, raylib="5.5").shipping_dir.name, "shipping-rl55")
        self.assertEqual(R.ReleasePipeline(d, d, python="3.11", raylib="5.5").shipping_dir.name, "shipping-py311-rl55")
        with self.assertRaises(R.ReleaseError):
            R.ReleasePipeline(d, d, raylib="6.0")
        with self.assertRaises(R.ReleaseError):
            R.ReleasePipeline(d, d, python="3.12")
        self.assertEqual(R.runtime_lib_names(R.PLATFORMS["linux"], {"pylib": "libpython3.11.so.1.0"}),
                         ["libpython3.11.so.1.0"])

    def test_running_interpreter_must_match_the_target(self):
        # The .pyc files are marshalled by the running interpreter, so packaging for another version
        # is refused (the packaged engine would fail to load them).
        d = Path(tempfile.mkdtemp())
        p = R.ReleasePipeline(d, d)
        p.python = "%d.%d" % sys.version_info[:2]
        p._require_target_interpreter()                       # same version: no error
        with mock.patch.object(sys, "version_info", (2, 7, 0, "final", 0)):
            with self.assertRaises(R.ReleaseError) as c:
                p._require_target_interpreter()
        self.assertIn("2.7", str(c.exception))

    def test_non_host_platform_refused(self):
        other = next(p for p in R.PLATFORMS if p != R.host_platform())
        with mock.patch.object(sys, "argv", ["release.py", "--platform", other]), mock.patch.object(sys, "stderr"):
            self.assertEqual(R.main(), 1)

    def test_no_steam_lets_480_pass(self):
        d = Path(tempfile.mkdtemp())
        p = R.ReleasePipeline(d, d, no_steam=True)
        p.config.steam.prepare = True
        self.assertFalse(p.steam_enabled)
        R.check_steam_app_id({"steam_app_id": 480}, p.steam_enabled)
        p.no_steam = False
        self.assertTrue(p.steam_enabled)

    def test_steam_lib_path_per_platform(self):
        d = Path(tempfile.mkdtemp())
        for name, rel in (("windows", "win64/steam_api64.dll"), ("linux", "linux64/libsteam_api.so"),
                          ("macos", "osx/libsteam_api.dylib")):
            p = R.ReleasePipeline(d, d, platform=name)
            sdk = (d.parent / "steamworks_sdk_165" / "sdk").resolve()
            self.assertEqual(p._steam_lib_path(), sdk / "redistributable_bin" / rel)


class NativeChecks(unittest.TestCase):
    """Pure parsers for ldd / otool output; the real tools only run in release validation on the native host."""

    def test_ldd(self):
        libs = Path("/pkg/Game")
        ok = "\tlibpython3.11.so.1.0 => /pkg/Game/libpython3.11.so.1.0 (0x1)\n\tlibc.so.6 => /lib/libc.so.6 (0x2)\n"
        R.check_ldd_output(ok, libs)
        with self.assertRaises(R.ReleaseError):
            R.check_ldd_output(ok + "\tlibasound.so.2 => not found\n", libs)
        with self.assertRaises(R.ReleaseError):
            R.check_ldd_output("\tlibpython3.11.so.1.0 => /usr/lib/libpython3.11.so.1.0 (0x1)\n", libs)

    def test_otool(self):
        good = ("Game:\n\t@rpath/libpython3.11.dylib (compatibility version 3.11.0)\n"
                "\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0)\n"
                "\t/System/Library/Frameworks/Cocoa.framework/Cocoa (compatibility version 1.0.0)\n")
        R.check_otool_output(good)
        for bad in ("/opt/homebrew/lib/libx.dylib", "/Users/runner/x.dylib", "/install/lib/libpython3.11.dylib"):
            with self.assertRaises(R.ReleaseError):
                R.check_otool_output("Game:\n\t" + bad + " (c)\n")
        R.check_otool_output("m.so:\n\t/install/lib/libpython3.11.dylib (c)\n", strict=False)

    def test_smoke_env_strips_python(self):
        d = Path(tempfile.mkdtemp())
        (d / "py").mkdir()
        (d / "py" / "python3").write_text("")
        (d / "ok").mkdir()
        env = {"PYTHONHOME": "x", "PYTHONPATH": "y", "LD_LIBRARY_PATH": "z", "HOME": "h",
               "PATH": os.pathsep.join([str(d / "py"), str(d / "ok")])}
        self.assertEqual(R.smoke_env(env), {"HOME": "h", "PATH": str(d / "ok")})


class AndroidRelease(unittest.TestCase):
    def test_config(self):
        cfg = R.parse_android_config({"android": {"application_id": "com.example.hr", "app_name": "HR",
                                                    "version_code": 8, "version_name": "2.1"}}, {})
        self.assertEqual(cfg, {"application_id": "com.example.hr", "app_name": "HR",
                               "version_code": 8, "version_name": "2.1", "play_games_app_id": "",
                               "license_public_key": "", "play_achievement_ids": {}})
        with self.assertRaisesRegex(R.ReleaseError, "application_id"):
            R.parse_android_config({}, {"name": "HR"})
        for key, value in (("play_games_app_id", 1), ("license_public_key", None),
                           ("play_achievement_ids", {"FIRST_HARVEST": 1})):
            with self.subTest(key=key), self.assertRaisesRegex(R.ReleaseError, key):
                R.parse_android_config({"android": {"application_id": "com.example.hr", key: value}}, {})
        for ids in ({"FIRST_HARVEST": ""}, {"": "CgkI"}, {"A=B": "CgkI"}, {"A": "C=gkI"}):
            with self.subTest(ids=ids), self.assertRaisesRegex(R.ReleaseError, "play_achievement_ids"):
                R.parse_android_config({"android": {"application_id": "com.example.hr",
                                                    "play_achievement_ids": ids}}, {})

    def test_launcher_icons(self):
        from PIL import Image
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            source = root / "icon.ico"
            Image.new("RGBA", (256, 256), "red").save(source, sizes=[(32, 32), (256, 256)])
            R.generate_android_icons(source, root / "res")
            for density, size in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                                  ("xxhdpi", 144), ("xxxhdpi", 192)):
                folder = root / "res" / f"mipmap-{density}"
                with Image.open(folder / "iyi_launcher.png") as icon:
                    self.assertEqual(icon.size, (size, size))
                with Image.open(folder / "iyi_launcher_foreground.png") as icon:
                    self.assertEqual(icon.size, (size * 9 // 4,) * 2)
            self.assertTrue((root / "res" / "mipmap-anydpi-v26" / "iyi_launcher.xml").is_file())

    def test_alignment_parser(self):
        R.check_android_load_alignment("LOAD 0x0 0x0 0x0 0x1000 0x1000 R E 0x4000\n")
        with self.assertRaises(R.ReleaseError):
            R.check_android_load_alignment("LOAD 0x0 0x0 0x0 0x1000 0x1000 R E 0x1000\n")
        with self.assertRaises(R.ReleaseError):
            R.check_android_load_alignment("There are no program headers\n")

    def test_rres_must_be_stored(self):
        with tempfile.TemporaryDirectory() as d:
            apk = Path(d) / "app.apk"
            with zipfile.ZipFile(apk, "w") as z:
                z.writestr("assets/game.rres", b"packed", compress_type=zipfile.ZIP_STORED)
            R.check_android_rres_stored(apk)
            with zipfile.ZipFile(apk, "w") as z:
                z.writestr("assets/game.rres", b"packed", compress_type=zipfile.ZIP_DEFLATED)
            with self.assertRaises(R.ReleaseError):
                R.check_android_rres_stored(apk)


class FakePackage(unittest.TestCase):
    """Runs the Linux/macOS-shaped assembly steps against a fake runtime on any host."""

    def _pipe(self, plat):
        d = Path(tempfile.mkdtemp())
        p = R.ReleasePipeline(d, d, platform=plat)
        p.config.output_name = "Game"
        p.output_path = d / "out"
        p.layout = R.package_layout(p.plat, p.output_path, "Game")
        root = d / "python"
        stdlib = root / p.pin["stdlib"]
        (stdlib / "site-packages").mkdir(parents=True)
        (stdlib / "os.py").write_text("")
        (stdlib / "site-packages" / "junk.py").write_text("")
        (root / "lib" / p.pin["pylib"]).write_text("lib")
        p.python_root = root
        for x in (p.layout.exe.parent, p.layout.data, p.layout.libs):
            x.mkdir(parents=True, exist_ok=True)
        return p

    def test_posix_runtime_install(self):
        for plat in ("linux", "macos"):
            p = self._pipe(plat)
            with mock.patch("builtins.print"):
                p._install_posix_runtime()
            L = p.layout
            self.assertTrue((L.data / "runtime" / p.pin["stdlib"] / "os.py").exists())
            self.assertFalse((L.data / "runtime" / p.pin["stdlib"] / "site-packages").exists())
            self.assertTrue((L.libs / p.pin["pylib"]).exists())

    def test_macos_plist_and_signing(self):
        p = self._pipe("macos")
        p.project_data = {"publisher": "GryCo", "version": "2.0"}
        calls = []
        with mock.patch.object(R, "run_tool", lambda cmd, env=None: calls.append(cmd) or "Game:\n"), \
                mock.patch("builtins.print"):
            p._finish_macos()
        import plistlib
        info = plistlib.loads((p.layout.app / "Contents" / "Info.plist").read_bytes())
        self.assertEqual(info["CFBundleExecutable"], "Game")
        self.assertEqual(info["CFBundleIdentifier"], "com.gryco.game")
        self.assertEqual(calls[-1][:3], ["codesign", "--force", "--deep"])
        self.assertIn(str(p.layout.app), calls[-1])


class ThirdPartyNotices(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())
        (self.d / "LICENSE").write_text("zlib text\n", encoding="utf-8")
        (self.d / "lib.h").write_text("/* x\n*   LICENSE: MIT\n*\n*   Copyright (c) A\n*   Permission granted\n**********/\n", encoding="utf-8")
        (self.d / "mono.h").write_text("// head\n// This file is dual-licensed\n//\n// BSD or CC0\n#ifndef X\n", encoding="utf-8")

    def test_license_text_whole_file_and_header_block(self):
        self.assertEqual(R.license_text(self.d / "LICENSE"), "zlib text")
        self.assertEqual(R.license_text(self.d / "lib.h", "LICENSE: MIT", "*****"), "LICENSE: MIT\n\n  Copyright (c) A\n  Permission granted")
        self.assertEqual(R.license_text(self.d / "mono.h", "dual-licensed", "#ifndef"), "This file is dual-licensed\n\nBSD or CC0")
        self.assertEqual(R.license_text(self.d / "lib.h", "no such marker", "*****"), "")

    def test_build_and_check(self):
        sources = [("raylib", self.d / "LICENSE", "", ""), ("rres", self.d / "lib.h", "LICENSE: MIT", "*****")]
        text = R.build_notices(sources, crt=True)
        self.assertIn("raylib\n======\n\nzlib text", text)
        self.assertIn("Microsoft Visual C++ runtime", text)
        R.check_notices(text, sources, crt=True)
        with self.assertRaises(R.ReleaseError) as c:
            R.check_notices(text.replace("rres\n====", "rr"), sources, crt=True)
        self.assertIn("rres", str(c.exception))

    def test_missing_text_is_an_error_not_invented(self):
        with self.assertRaises(R.ReleaseError) as c:
            R.build_notices([("pl_mpeg", self.d / "nope", "", "")], crt=False)
        self.assertIn("pl_mpeg", str(c.exception))

    def test_sources_by_mode(self):
        names = lambda lic: [n for n, *_ in R.notice_sources(self.d, self.d, lic)]
        self.assertEqual(names(None), ["raylib", "GLFW (built into raylib)", "nlohmann/json", "pl_mpeg", "rres", "Monocypher"])
        self.assertEqual(names(self.d / "L")[-2:], ["CPython", "pybind11"])


class NativeMode(unittest.TestCase):
    def test_log_needs_no_python_marker(self):
        log = "hot reload: disabled\n[INFO ] [startup] IYI_RUNTIME_READY\n"
        R.validate_runtime_log(log, python=False)
        with self.assertRaises(R.ReleaseError):
            R.validate_runtime_log(log)
        with self.assertRaises(R.ReleaseError):
            R.validate_runtime_log(log + "[ERROR] x\n", python=False)

    def test_shipping_exe_is_the_one_in_its_tree(self):
        d = Path(tempfile.mkdtemp())
        p = R.ReleasePipeline(d, d, platform="windows")
        self.assertEqual(p.shipping_exe(), p.shipping_dir / "IYIEngine.exe")
        p.native, p.cmake_dir = True, d / "src"
        bin_dir = p.shipping_dir / "bin"
        bin_dir.mkdir(parents=True)
        self.assertEqual(p.shipping_dir, d / "src" / "build" / "shipping")
        with self.assertRaises(R.ReleaseError):
            p.shipping_exe()
        (bin_dir / "game.exe").write_bytes(b"")
        (bin_dir / "game.pdb").write_bytes(b"")
        self.assertEqual(p.shipping_exe(), bin_dir / "game.exe")


class TrailerExcluded(unittest.TestCase):
    def test_scripts(self):
        R.check_no_trailer_scripts([("scripts/a.py", "import engine\ntext = 'marketing budget'\n")])
        for text in ("from marketing import hr_bridge\n", "  import marketing.shots\n", "engine.trailer.start()\n"):
            with self.assertRaises(R.ReleaseError):
                R.check_no_trailer_scripts([("scripts/b.py", text)])

    def test_exe(self):
        R.check_no_trailer_exe(b"MZ... --frames --project ...")
        with self.assertRaises(R.ReleaseError):
            R.check_no_trailer_exe(b"MZ... --trailer-shot ...")


if __name__ == "__main__":
    unittest.main()
