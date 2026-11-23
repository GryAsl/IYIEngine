// Declares IYITrailer: development-only recording of the finished game frame (HUD included) and the
// game's mixed audio into an H.264 + AAC .mp4 through an FFmpeg subprocess, plus the scripted shot a
// CLI run spawns (docs/IYITRAILER.md, SPEC §3.2). Never compiled into a shipping build: CMake leaves
// Trailer*.cpp out and this header refuses to compile there, so nothing of it can reach a release.
//
// Threads: TrailerStart / TrailerCaptureFrame / TrailerStop run on the render side (they read the
// framebuffer and own the audio processor); TrailerConfigure runs at startup, TrailerGameStep on
// the game side; TrailerStatusNow and the setters are safe from any thread.
//
// Connected to:
// - Application.cpp parses --trailer-shot / --trailer-config, draws a shot run into the canvas
//   (TrailerCanvasSize), captures each frame before EndDrawing, ticks the shot on the game side and
//   finalizes at the end of Run.
// - TrailerBindings.cpp exposes engine.trailer; TrailerProcess.cpp runs FFmpeg.

#pragma once

#ifdef IYI_SHIPPING_BUILD
#error "IYITrailer is development-only and must never be part of a shipping build"
#endif

#include <string>
#include <vector>

namespace IYI {

struct TrailerSettings
{
    std::string Name = "capture";
    std::string OutputDir;          // "" = <project>/captures/<name>/<timestamp>
    std::string Ffmpeg = "ffmpeg";  // path or a name found on PATH
    int Width = 1920;
    int Height = 1080;
    int Fps = 25;
    bool Audio = true;              // off only as an explicit diagnostic option
    // The final encode's FFmpeg video codec arguments (tools/trailer.py picks a GPU encoder); the live capture is lossless.
    std::vector<std::string> VideoArgs = { "-c:v", "libx264", "-preset", "slow", "-crf", "18" };
};

struct TrailerStatus
{
    std::string State = "idle";     // idle, starting, recording, stopping, done, error
    double Elapsed = 0.0;           // seconds since recording began
    std::string Mp4;
    std::string Report;             // capture.json of the last finished recording
    std::string Error;
};

// Startup (Application::Initialize): reads the --trailer-config JSON into the defaults engine.trailer.start
// uses and remembers the shot to spawn. false (and a logged error) on a bad config.
bool TrailerConfigure(const std::string& shot, const std::string& configPath);
// Game side, once per step: spawns the shot actor once the runtime is ready, enforces the shot
// timeout and fails the run on a Python error. Requests quit when the shot must end.
void TrailerGameStep(float dt, bool runtimeReady);
// The process exit code a trailer run ends with: 0 unless a shot run failed.
int TrailerExitCode();

TrailerSettings TrailerDefaults();
TrailerStatus TrailerStatusNow();
// Game side: shows a start/stop as requested ("starting" / "stopping") until the render side runs it.
void TrailerMarkState(const std::string& state);

// One stretch of a recording in which a tracked UI element looks the same (capture.json "ui"):
// rect in whole canvas units, opacity to 0.01.
struct TrailerUiRange
{
    double From = 0.0, To = 0.0;   // seconds of the video
    int X = 0, Y = 0, W = 0, H = 0;
    float Opacity = 1.0f;
    bool Visible = true;
};
// Extends the last range when the sample at t looks the same, else starts a new one.
void TrailerAddUiSample(std::vector<TrailerUiRange>& ranges, double t, TrailerUiRange sample);
// Game side, before a start: the UI elements (by name) whose rects the next recording reports.
void TrailerTrackUi(const std::vector<std::string>& names);

// Render side.
// A --trailer-shot run draws every frame into an offscreen canvas of the configured recording size
// (Application owns it), so the game sees that size whatever the window or the monitor allows:
// 2560x1440 records on a 1080p screen, and a hidden window clamped to the work area does not shrink
// it. false outside a shot run, where the recording reads the window's framebuffer.
bool TrailerCanvasSize(int& width, int& height);

void TrailerStart(TrailerSettings settings);
void TrailerCaptureFrame();   // reads the bound target: the canvas in a shot run, else the window
void TrailerStop();

// TrailerProcess.cpp: a child process (no shell) whose stdout/stderr go to logPath, with an optional
// pipe into its stdin. Paths and arguments are UTF-8.
struct TrailerProcess { void* Handle = nullptr; void* Input = nullptr; };
bool TrailerSpawn(const std::vector<std::string>& args, const std::string& logPath, bool pipeInput, TrailerProcess& out);
bool TrailerWrite(TrailerProcess& process, const void* data, size_t size);
// Closes the input pipe, waits for the exit and returns its code (-1 when there was no process).
int TrailerWait(TrailerProcess& process);

// Pure helpers, exposed for the native tests.
// How many encoded frames the frame rendered at `seconds` fills: 0 = its slot is already written
// (stale, dropped), 1 = on time, n > 1 = it also repeats over n - 1 missed slots. Advances nextSlot.
int TrailerFrameRepeats(double seconds, int fps, long long& nextSlot);
// The standard sample rate closest to a measured one, or 0 when none is within 3%.
int TrailerSnapSampleRate(double measured);
// One command line from arguments, quoted the way CommandLineToArgvW splits it back.
std::string TrailerQuoteArgs(const std::vector<std::string>& args);

} // namespace IYI
