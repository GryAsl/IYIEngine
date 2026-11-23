#!/usr/bin/env python3
"""IYITrailer launcher (docs/IYITRAILER.md): records a game's scripted marketing shot without a human.

    python tools/trailer.py render   --project ../HumanResources --shot market_spike [--format mp4|gif|both]
                                     [--fps 25] [--resolution 1920x1080] [--no-audio] [--mpg] [--out DIR]
                                     [--visible] [--hot-reload] [--encoder auto|nvenc|amf|qsv|cpu]
    python tools/trailer.py list     --project ../HumanResources
    python tools/trailer.py validate --project ../HumanResources --shot market_spike

render runs the development engine with --trailer-shot / --trailer-config; the shot
(<project>/marketing/shots/<shot>.py, an actor script) prepares the game and calls engine.trailer.
The run is --agent-run, --no-hot-reload, --hidden and has its own temporary --user-data folder (the
player's settings and audio.json stay untouched). The engine writes the .mp4 (H.264 + AAC from the
game's own mix) and capture.json; this tool checks the file with ffprobe, measures loudness (LUFS,
true peak), makes the GIF (FFmpeg palettegen) and/or the .mpg (MPEG-1, BT.601) and writes result.json
next to it, with the project files whose mtime changed during the take and the shot's failed
expectations (<user-data>/expectations.json). The last stdout line is result.json's path (--json:
the report itself). Exit code 0 only on success.
"""

import argparse
import functools
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ENGINE_DIR = Path(__file__).resolve().parent.parent
CRF, PRESET = 18, "slow"   # the final encode; the live capture is lossless
KEEP_TAKES = 3             # newest takes kept per shot in the default output folder
ENGINE_EXE = ENGINE_DIR / "build" / "ninja" / ("IYIEngine.exe" if os.name == "nt" else "IYIEngine")
WINGET_FFMPEG = "~/AppData/Local/Microsoft/WinGet/Packages/Gyan.FFmpeg*/ffmpeg-*/bin/ffmpeg.exe"


def find_ffmpeg(explicit):
    """--ffmpeg, then IYI_FFMPEG, then PATH, then a winget install (it is not on PATH until a new shell)."""
    for candidate in (explicit, os.environ.get("IYI_FFMPEG"), shutil.which("ffmpeg")):
        if candidate:
            return str(Path(candidate).resolve()) if Path(candidate).exists() else candidate
    found = sorted(glob.glob(os.path.expanduser(WINGET_FFMPEG)))
    return found[-1] if found else None


def check_ffmpeg(ffmpeg):
    """Error text when FFmpeg is missing or lacks libx264 / aac, else None."""
    if not ffmpeg:
        return "FFmpeg not found: install it (winget install Gyan.FFmpeg), or pass --ffmpeg / set IYI_FFMPEG"
    try:
        out = subprocess.run([ffmpeg, "-hide_banner", "-encoders"], capture_output=True, text=True,
                             timeout=30).stdout
    except (OSError, subprocess.SubprocessError) as e:
        return f"FFmpeg '{ffmpeg}' does not run: {e}"
    missing = [enc for enc in ("libx264", "aac") if f" {enc} " not in out]
    return f"FFmpeg '{ffmpeg}' lacks encoder(s): {', '.join(missing)}" if missing else None


# H.264 arguments per encoder at quality q (CRF-like: lower is better). Hardware ones are tried in this
# order by "auto"; the CPU preset only applies to libx264.
ENCODERS = {
    "nvenc": lambda q, preset: ["-c:v", "h264_nvenc", "-preset", "p7", "-tune", "hq", "-rc", "vbr", "-cq", str(q), "-b:v", "0"],
    # amf: B-frames at q+4 cut the file about 23% at the same VMAF as flat CQP; QVBR measured worse (docs/VIDEO-ENCODING.md).
    "amf": lambda q, preset: ["-c:v", "h264_amf", "-quality", "quality", "-rc", "cqp", "-bf", "3",
                              "-qp_i", str(q), "-qp_p", str(q), "-qp_b", str(q + 4)],
    "qsv": lambda q, preset: ["-c:v", "h264_qsv", "-preset", "veryslow", "-global_quality", str(q)],
    "cpu": lambda q, preset: ["-c:v", "libx264", "-preset", preset, "-crf", str(q)],
}


@functools.cache
def pick_encoder(ffmpeg, choice="auto"):
    """(name, error): 'auto' = the first hardware encoder that encodes a test clip, else 'cpu'.
    FFmpeg builds list every encoder whether or not this machine has the GPU and driver, so each is tried."""
    if choice not in ("auto", *ENCODERS):
        return None, f"unknown encoder '{choice}' (auto, {', '.join(ENCODERS)})"
    for name in ([n for n in ENCODERS if n != "cpu"] if choice == "auto" else [choice]):
        test = [ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=s=256x256:d=0.2",
                "-vf", "format=yuv420p", *ENCODERS[name](18, "medium"), "-f", "null", "-"]
        try:
            if subprocess.run(test, capture_output=True, timeout=30).returncode == 0:
                return name, None
        except (OSError, subprocess.SubprocessError):
            pass
    return ("cpu", None) if choice == "auto" else (None, f"encoder '{choice}' does not work on this machine")


def shots(project):
    folder = project / "marketing" / "shots"
    return sorted(p.stem for p in folder.glob("*.py") if p.stem != "__init__")


def probe(ffmpeg, path):
    """ffprobe's streams of a file, or None when it is not a readable media file."""
    ffprobe = str(Path(ffmpeg).with_name("ffprobe" + Path(ffmpeg).suffix)) if os.sep in ffmpeg else "ffprobe"
    try:
        out = subprocess.run([ffprobe, "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)],
                             capture_output=True, text=True, timeout=60)
        return json.loads(out.stdout) if out.returncode == 0 else None
    except (OSError, subprocess.SubprocessError, ValueError):
        return None


def check_mp4(ffmpeg, path, width, height, audio):
    """Errors in the finished .mp4 (an empty list when it is what was asked for)."""
    info = probe(ffmpeg, path)
    if not info:
        return [f"ffprobe cannot read {path}"]
    streams = info.get("streams", [])
    video = [s for s in streams if s.get("codec_type") == "video"]
    sound = [s for s in streams if s.get("codec_type") == "audio"]
    errors = []
    if not video or video[0].get("codec_name") != "h264":
        errors.append("no H.264 video stream")
    elif (video[0].get("width"), video[0].get("height")) != (width, height):
        errors.append(f"video is {video[0].get('width')}x{video[0].get('height')}, expected {width}x{height}")
    if audio and (not sound or sound[0].get("codec_name") != "aac" or sound[0].get("channels") != 2):
        errors.append("no AAC stereo audio stream")
    return errors


def convert(ffmpeg, mp4, out, args, what):
    """FFmpeg `mp4` -> `out` with `args`; error text (the log is kept next to `out`) or None."""
    log = out.with_name(out.name + ".log")
    with open(log, "wb") as f:
        code = subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "warning", "-y", "-i", str(mp4), *args, str(out)],
                              stdout=f, stderr=subprocess.STDOUT).returncode
    if code == 0:
        log.unlink(missing_ok=True)
        return None
    return f"{what} conversion failed with FFmpeg exit code {code} (see {log})"


def make_gif(ffmpeg, mp4, gif, fps, width):
    return convert(ffmpeg, mp4, gif, ["-vf", f"fps={fps},scale={width}:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse",
                                      "-loop", "0"], "GIF")


def make_mpg(ffmpeg, mp4, mpg):
    """MPEG-1 + MP2 for engine.video. pl_mpeg decodes BT.601 and the master is BT.709: convert the matrix."""
    return convert(ffmpeg, mp4, mpg, ["-vf", "scale=in_color_matrix=bt709:out_color_matrix=bt601", "-c:v", "mpeg1video", "-q:v", "4",
                                      "-c:a", "mp2", "-b:a", "192k", "-ar", "48000", "-f", "mpeg"], "MPG")


def audio_levels(ffmpeg, mp4):
    """Integrated loudness (LUFS) and true peak (dBFS) of the file's audio (ebur128 summary), or None."""
    text = subprocess.run([ffmpeg, "-hide_banner", "-nostats", "-i", str(mp4), "-vn", "-af", "ebur128=peak=true", "-f", "null", "-"],
                          capture_output=True, text=True).stderr
    summary = text[text.rfind("Summary:"):]
    lufs, peak = (re.search(pattern + r"\s+(-?inf|-?[\d.]+)", summary) for pattern in ("I:", "Peak:"))
    return {"integrated_lufs": float(lufs[1]), "true_peak_dbfs": float(peak[1])} if lufs and peak else None


SKIP_DIRS = {"captures", "logs", "agentio", "saves", "release", "__pycache__", ".git", "graft"}


def project_mtimes(project):
    """{relative path: mtime_ns} of the project's files (outputs, caches and the graft index left out)."""
    found = {}
    for root, dirs, files in os.walk(project):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in files:
            path = os.path.join(root, name)
            try:
                found[os.path.relpath(path, project)] = os.stat(path).st_mtime_ns
            except OSError:
                pass
    return found


def write_result(path, report):
    temp = path.with_name(path.name + ".tmp")
    temp.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    os.replace(temp, path)


def render(args):
    project = Path(args.project).resolve()
    width, height = (int(v) for v in args.resolution.lower().split("x"))
    audio = args.format != "gif" and not args.no_audio   # a GIF is silent; an .mp4 always has the game's sound
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_dir = Path(args.out).resolve() if args.out else project / "captures" / args.shot / stamp
    if args.out:
        out_dir.mkdir(parents=True, exist_ok=True)
    else:   # takes of one shot started in the same second (parallel runs) each claim their own folder
        out_dir.parent.mkdir(parents=True, exist_ok=True)
        base, n = out_dir, 2
        while True:
            try:
                out_dir.mkdir()
                break
            except FileExistsError:
                out_dir, n = base.with_name(f"{base.name}-{n}"), n + 1
    report = {"ok": False, "shot": args.shot, "format": args.format, "output": {}, "errors": [],
              "logs": {"engine": str(out_dir / "engine.log")}, "dir": str(out_dir)}
    result_path = out_dir / "result.json"

    def finish(code=1):
        write_result(result_path, report)
        print(json.dumps(report, indent=2, ensure_ascii=False) if args.json else result_path)
        return code

    ffmpeg = find_ffmpeg(args.ffmpeg)
    error = check_ffmpeg(ffmpeg)
    encoder, encoder_error = pick_encoder(ffmpeg, args.encoder) if not error else (None, None)
    report["encoder"] = encoder
    for error in (error, encoder_error, validate_shot(project, args.shot)):
        if error:
            report["errors"].append(error)
    if not ENGINE_EXE.exists():
        report["errors"].append(f"engine not built: {ENGINE_EXE} (run build.bat)")
    if report["errors"]:
        return finish()

    config = {"shot": args.shot, "format": args.format, "resolution": [width, height], "fps": args.fps,
              "audio": audio, "gif_fps": args.gif_fps, "gif_width": args.gif_width, "output_dir": str(out_dir),
              "ffmpeg": ffmpeg, "timeout": args.timeout, "video_args": ENCODERS[encoder](CRF, PRESET)}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
        json.dump(config, f)
    user_data = Path(tempfile.mkdtemp(prefix="iyitrailer-"))   # the take's own settings, audio.json and saves
    command = [str(ENGINE_EXE), "--project", str(project), "--agent-run", "--user-data", str(user_data),
               "--trailer-shot", args.shot, "--trailer-config", f.name]
    command += [] if args.hot_reload else ["--no-hot-reload"]
    command += [] if args.visible else ["--hidden"]
    before = project_mtimes(project)
    try:
        with open(out_dir / "engine.log", "wb") as log:
            # The engine enforces the shot timeout; this one only catches a hung engine (the final encode runs after the shot).
            # IYI_SKIP_INTRO: the old way to skip a game's intro; --agent-run replaces it (kept for games not yet moved).
            code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=args.timeout + 600,
                                  env={**os.environ, "IYI_SKIP_INTRO": "1"}).returncode
    except subprocess.TimeoutExpired:
        code = None
        report["errors"].append(f"engine did not exit within {args.timeout + 600} s and was killed")
    finally:
        os.unlink(f.name)
    report["engine_exit_code"] = code
    after = project_mtimes(project)
    report["changed_project_files"] = sorted(k for k in before.keys() | after.keys() if before.get(k) != after.get(k))
    expectations = user_data / "expectations.json"   # written by the shot's expect() (HR: marketing/hr_bridge.py)
    report["failed_expectations"] = json.loads(expectations.read_text(encoding="utf-8")) if expectations.exists() else []
    report["errors"] += [f"shot expectation failed: {what}" for what in report["failed_expectations"]]
    shutil.rmtree(user_data, ignore_errors=True)

    capture_path = out_dir / "capture.json"
    capture = json.loads(capture_path.read_text(encoding="utf-8")) if capture_path.exists() else None
    if capture is None:
        report["errors"].append("the shot finished without a recording (no capture.json; see engine.log)")
        return finish()
    for key in ("video", "audio", "capture_stats", "ui"):
        report[key] = capture.get(key)
    report["errors"] += capture.get("errors", [])
    if code != 0 and not report["errors"]:
        report["errors"].append(f"engine exited with code {code} (see engine.log)")
    mp4 = Path(capture["output"]["mp4"]) if capture.get("ok") else None
    report["partial_capture_kept"] = mp4 is None and any(out_dir.glob("video.mp4"))
    if mp4 is None or report["errors"]:
        return finish()

    report["errors"] += check_mp4(ffmpeg, mp4, width, height, audio)
    if audio and not report["errors"]:
        report["audio_levels"] = audio_levels(ffmpeg, mp4)
    for kind, wanted, make in (("gif", args.format in ("gif", "both"), lambda p: make_gif(ffmpeg, mp4, p, args.gif_fps, args.gif_width)),
                               ("mpg", args.mpg, lambda p: make_mpg(ffmpeg, mp4, p))):
        if wanted and not report["errors"]:
            path = mp4.with_suffix("." + kind)
            error = make(path)
            if error:
                report["errors"].append(error)
            else:
                report["output"][kind] = str(path)
    if args.format != "gif":
        report["output"]["mp4"] = str(mp4)
    elif not report["errors"]:
        mp4.unlink()   # GIF only: the .mp4 was the intermediate
    report["ok"] = not report["errors"]
    if report["ok"]:
        (out_dir / "engine.log").unlink(missing_ok=True)
        report["logs"] = {}
        if not args.out:
            prune_takes(out_dir.parent)
    return finish(0 if report["ok"] else 1)


def prune_takes(shot_dir):
    """Deletes all but the newest KEEP_TAKES take folders (<timestamp>[-n]) of a shot; other folders stay."""
    takes = sorted((p for p in shot_dir.iterdir() if p.is_dir() and re.fullmatch(r"\d{8}-\d{6}(-\d+)?", p.name)),
                   key=lambda p: (p.name[:15], int(p.name[16:] or 1)))
    for old in takes[:-KEEP_TAKES]:
        shutil.rmtree(old, ignore_errors=True)


def validate_shot(project, shot):
    """Error text when the shot cannot run, else None."""
    path = project / "marketing" / "shots" / f"{shot}.py"
    if not path.exists():
        return f"shot not found: {path} (shots: {', '.join(shots(project)) or 'none'})"
    try:
        compile(path.read_text(encoding="utf-8"), str(path), "exec")
    except SyntaxError as e:
        return f"shot does not compile: {e}"
    return None


def main():
    sys.stdout.reconfigure(encoding="utf-8")   # result paths may hold non-ASCII folder names
    parser = argparse.ArgumentParser(description="IYITrailer: record a game's scripted marketing shot")
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("render", "list", "validate"):
        p = sub.add_parser(name)
        p.add_argument("--project", required=True)
        if name != "list":
            p.add_argument("--shot", required=True)
            p.add_argument("--ffmpeg", help="FFmpeg executable (default: IYI_FFMPEG, PATH, winget install)")
            p.add_argument("--encoder", default=os.environ.get("IYI_ENCODER", "auto"), choices=("auto", *ENCODERS),
                           help="final H.264 encoder (default: IYI_ENCODER, else auto = a working GPU encoder, else cpu)")
    render_parser = sub.choices["render"]
    render_parser.add_argument("--format", choices=("mp4", "gif", "both"), default="mp4")
    render_parser.add_argument("--fps", type=int, default=25)
    render_parser.add_argument("--resolution", default="1920x1080")
    render_parser.add_argument("--no-audio", action="store_true", help="diagnostics only: an .mp4 without sound")
    render_parser.add_argument("--mpg", action="store_true", help="also export <shot>.mpg (MPEG-1 + MP2, BT.601) for engine.video")
    render_parser.add_argument("--visible", action="store_true", help="show the game window (default: --hidden)")
    render_parser.add_argument("--hot-reload", action="store_true", help="keep script hot reload on (default: --no-hot-reload)")
    render_parser.add_argument("--gif-fps", type=int, default=12)
    render_parser.add_argument("--gif-width", type=int, default=720)
    render_parser.add_argument("--timeout", type=float, default=120.0, help="seconds the shot may run")
    render_parser.add_argument("--out", help="output folder (default <project>/captures/<shot>/<timestamp>)")
    render_parser.add_argument("--json", action="store_true", help="print the report instead of its path")
    args = parser.parse_args()

    project = Path(args.project).resolve()
    if args.command == "list":
        print("\n".join(shots(project)))
        return 0
    if args.command == "validate":
        ffmpeg = find_ffmpeg(args.ffmpeg)
        errors = [e for e in (validate_shot(project, args.shot), check_ffmpeg(ffmpeg)) if e]
        if not errors and (encoder_error := pick_encoder(ffmpeg, args.encoder)[1]):
            errors.append(encoder_error)
        errors += [] if ENGINE_EXE.exists() else [f"engine not built: {ENGINE_EXE}"]
        print("\n".join(errors) or "ok")
        return 1 if errors else 0
    return render(args)


if __name__ == "__main__":
    sys.exit(main())
