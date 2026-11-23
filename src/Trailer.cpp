// Implements IYITrailer (Trailer.h, docs/IYITRAILER.md): development-only capture of the finished
// frame and the mixed game audio into an .mp4.
//
// Video: each presented frame is read back (the same readback as the screenshot; in a shot run from
// the offscreen canvas at the recording size, TrailerCanvasSize) and handed to a
// writer thread, which pipes raw RGBA into one long-lived FFmpeg (lossless libx264rgb, fast enough
// for real time). The pipe is read at a
// constant nominal frame rate, so frames are placed by real time since the start: a frame whose
// slot is already written is surplus (skipped, nothing lost), a late one repeats over the slots it
// missed; a frame is dropped (its content lost) only when the writer queue is full. Encoded video
// therefore lasts as long as the real recording, and stays in step with the audio.
// Audio: raylib's mixed processor hands the post-mix stereo float buffer to a callback on the audio
// thread, which only copies it into a preallocated ring (no locks, files or allocation there); the
// writer thread drains the ring to a raw f32 file. On stop FFmpeg encodes the final .mp4 from both:
// H.264 4:2:0 BT.709 with the configured encoder (CPU or GPU) (one RGB to YUV conversion, saturation x1.25),
// AAC sound. The device
// rate is measured (frames per real second) and snapped to a standard rate, since raylib follows
// the device's own rate and does not expose it.

#include "IYI/Trailer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>
#include "raylib.h"
#include "rlgl.h"
#include "IYI/AgentChannel.h"
#include "IYI/Application.h"
#include "IYI/Log.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace IYI {

namespace {

constexpr size_t kQueueFrames = 3;            // ~8 MB each at 1080p
constexpr size_t kRingFloats = size_t(1) << 21;  // ~21 s of 48 kHz stereo; a power of two

std::mutex g_statusMutex;
TrailerStatus g_status;
std::vector<std::string> g_trackNames;                     // game side: engine.trailer.start(track_ui=...)
std::map<std::string, std::vector<TrailerUiRange>> g_ui;  // the ranges so far, under g_statusMutex
TrailerSettings g_defaults;

// The shot a --trailer-shot run spawns (game side).
std::string g_shot;
double g_shotTimeout = 120.0;
double g_shotClock = 0.0;
bool g_shotSpawned = false;
int g_errorsAtSpawn = 0;
std::string g_shotError;   // guarded by g_statusMutex: read by the render side's report

// One recording (render side, plus the writer thread where noted).
struct Frame { unsigned char* Pixels; int Repeats; };
struct Recording
{
    bool Active = false;
    TrailerSettings Settings;
    fs::path Dir;
    int InWidth = 0, InHeight = 0;
    Clock::time_point Start;
    long long NextSlot = 0;
    long long Encoded = 0, Repeated = 0, Dropped = 0, Surplus = 0;
    std::vector<std::string> Errors;
    TrailerProcess Encoder;
    std::thread Writer;
    std::mutex QueueMutex;
    std::condition_variable QueueCv;
    std::deque<Frame> Queue;                 // QueueMutex
    bool Stopping = false;                   // QueueMutex
    std::atomic<bool> WriteFailed{ false };  // writer thread
    std::ofstream AudioOut;                  // writer thread while it runs
};
Recording g_rec;

// The audio ring: the audio thread writes Head, the writer thread advances Tail.
std::vector<float> g_ring;
std::atomic<size_t> g_ringHead{ 0 }, g_ringTail{ 0 };
std::atomic<long long> g_audioFrames{ 0 }, g_audioLost{ 0 };

// Paths are UTF-8 std::string with forward slashes everywhere else (C++20 made u8string a different type).
std::string Utf8(const fs::path& p)
{
    const std::u8string s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

fs::path Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void OnMixedAudio(void* buffer, unsigned int frames)
{
    const float* in = static_cast<const float*>(buffer);
    const size_t n = size_t(frames) * 2;
    const size_t head = g_ringHead.load(std::memory_order_relaxed);
    if (kRingFloats - (head - g_ringTail.load(std::memory_order_acquire)) < n) {
        g_audioLost += frames;   // reported as a failure, never hidden
        return;
    }
    for (size_t i = 0; i < n; ++i) g_ring[(head + i) & (kRingFloats - 1)] = in[i];
    g_ringHead.store(head + n, std::memory_order_release);
    g_audioFrames += frames;
}

void DrainAudio()
{
    const size_t head = g_ringHead.load(std::memory_order_acquire);
    size_t tail = g_ringTail.load(std::memory_order_relaxed);
    while (tail != head) {
        const size_t at = tail & (kRingFloats - 1);
        const size_t n = std::min(head - tail, kRingFloats - at);
        g_rec.AudioOut.write(reinterpret_cast<const char*>(&g_ring[at]), std::streamsize(n * sizeof(float)));
        tail += n;
    }
    g_ringTail.store(tail, std::memory_order_release);
}

void WriterLoop()
{
    const size_t bytes = size_t(g_rec.InWidth) * size_t(g_rec.InHeight) * 4;
    for (;;) {
        if (g_rec.AudioOut.is_open()) DrainAudio();
        Frame frame{ nullptr, 0 };
        bool done = false;
        {
            std::unique_lock<std::mutex> lock(g_rec.QueueMutex);
            g_rec.QueueCv.wait_for(lock, std::chrono::milliseconds(5),
                                   [] { return !g_rec.Queue.empty() || g_rec.Stopping; });
            if (!g_rec.Queue.empty()) {
                frame = g_rec.Queue.front();
                g_rec.Queue.pop_front();
            } else {
                done = g_rec.Stopping;
            }
        }
        if (frame.Pixels) {
            for (int i = 0; i < frame.Repeats && !g_rec.WriteFailed; ++i) {
                if (!TrailerWrite(g_rec.Encoder, frame.Pixels, bytes)) g_rec.WriteFailed = true;
            }
            MemFree(frame.Pixels);
        }
        if (done) break;
    }
    if (g_rec.AudioOut.is_open()) DrainAudio();
}

void SetStatus(const std::string& state, const std::string& error = "")
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_status.State = state;
    g_status.Error = error;
}

std::string Timestamp()
{
    const std::time_t now = std::time(nullptr);
    char text[32];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", std::localtime(&now));
    return text;
}

int RunFfmpeg(const std::vector<std::string>& args, const fs::path& log)
{
    TrailerProcess process;
    if (!TrailerSpawn(args, Utf8(log), false, process)) return -1;
    return TrailerWait(process);
}

void WriteReport(const nlohmann::json& report, const fs::path& path)
{
    fs::path temp = path;
    temp += ".tmp";
    { std::ofstream(temp, std::ios::binary) << report.dump(2); }
    std::error_code ec;
    fs::rename(temp, path, ec);
}

} // namespace

int TrailerFrameRepeats(double seconds, int fps, long long& nextSlot)
{
    const long long slot = static_cast<long long>(std::floor(seconds * fps));
    if (slot < nextSlot) return 0;
    const int repeats = static_cast<int>(slot - nextSlot + 1);
    nextSlot = slot + 1;
    return repeats;
}

int TrailerSnapSampleRate(double measured)
{
    for (int rate : { 22050, 32000, 44100, 48000, 88200, 96000 }) {
        if (std::fabs(measured - rate) <= rate * 0.03) return rate;
    }
    return 0;
}

bool TrailerConfigure(const std::string& shot, const std::string& configPath)
{
    g_shot = shot;
    g_defaults.Name = shot.empty() ? g_defaults.Name : shot;
    if (configPath.empty()) return true;
    try {
        std::ifstream in(Path(configPath), std::ios::binary);
        const nlohmann::json c = nlohmann::json::parse(in);
        TrailerSettings& d = g_defaults;
        d.Name = c.value("shot", d.Name);
        d.OutputDir = c.value("output_dir", d.OutputDir);
        d.Ffmpeg = c.value("ffmpeg", d.Ffmpeg);
        d.Fps = c.value("fps", d.Fps);
        if (c.contains("resolution")) {
            d.Width = c["resolution"].at(0).get<int>();
            d.Height = c["resolution"].at(1).get<int>();
        }
        d.Audio = c.value("audio", d.Audio);
        d.VideoArgs = c.value("video_args", d.VideoArgs);
        g_shotTimeout = c.value("timeout", g_shotTimeout);
    } catch (const std::exception& e) {
        LogWrite(LogLevel::Error, "trailer", "Cannot read --trailer-config " + configPath + ": " + e.what());
        return false;
    }
    return true;
}

TrailerSettings TrailerDefaults() { return g_defaults; }

bool TrailerCanvasSize(int& width, int& height)
{
    if (g_shot.empty()) return false;
    width = g_defaults.Width;
    height = g_defaults.Height;
    return true;
}

// What a recording reads: the canvas in a shot run, else the window's framebuffer (not the DPI-scaled screen size).
static void SourceSize(int& width, int& height)
{
    if (TrailerCanvasSize(width, height)) return;
    width = GetRenderWidth();
    height = GetRenderHeight();
}

TrailerStatus TrailerStatusNow()
{
    std::lock_guard<std::mutex> lock(g_statusMutex);
    return g_status;
}

void TrailerMarkState(const std::string& state) { SetStatus(state); }

void TrailerAddUiSample(std::vector<TrailerUiRange>& ranges, double t, TrailerUiRange sample)
{
    if (!ranges.empty()) {
        TrailerUiRange& last = ranges.back();
        if (last.X == sample.X && last.Y == sample.Y && last.W == sample.W && last.H == sample.H &&
            last.Opacity == sample.Opacity && last.Visible == sample.Visible) {
            last.To = t;
            return;
        }
    }
    sample.From = sample.To = t;
    ranges.push_back(sample);
}

void TrailerTrackUi(const std::vector<std::string>& names)
{
    g_trackNames = names;
    std::lock_guard<std::mutex> lock(g_statusMutex);
    g_ui.clear();
}

// Game side, every step of a recording: where the tracked elements are, at the video's clock, so the
// edit can aim at "CapturePanel" instead of pixel numbers measured by eye.
static void TrackUiStep()
{
    if (g_trackNames.empty()) return;
    double t = 0.0;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        if (g_status.State != "recording") return;
        t = g_status.Elapsed;
    }
    const nlohmann::json frame = nlohmann::json::parse(TrackUi(g_trackNames));
    std::lock_guard<std::mutex> lock(g_statusMutex);
    for (const std::string& name : g_trackNames) {   // gone this frame (no canvas has it): not visible
        if (!frame.contains(name) && !g_ui[name].empty()) {
            TrailerUiRange gone = g_ui[name].back();
            gone.Visible = false;
            TrailerAddUiSample(g_ui[name], t, gone);
        }
    }
    for (const auto& [name, e] : frame.items()) {
        const auto& r = e["rect"];
        TrailerUiRange sample;
        sample.X = static_cast<int>(std::lround(r[0].get<double>()));
        sample.Y = static_cast<int>(std::lround(r[1].get<double>()));
        sample.W = static_cast<int>(std::lround(r[2].get<double>()));
        sample.H = static_cast<int>(std::lround(r[3].get<double>()));
        sample.Opacity = std::round(e["opacity"].get<float>() * 100.0f) / 100.0f;
        sample.Visible = e["visible"].get<bool>();
        TrailerAddUiSample(g_ui[name], t, sample);
    }
}

void TrailerGameStep(float dt, bool runtimeReady)
{
    TrackUiStep();
    if (g_shot.empty()) return;
    g_shotClock += dt;
    std::string failure;
    if (!g_shotSpawned && runtimeReady) {
        g_shotSpawned = true;
        g_errorsAtSpawn = LogErrorCount();
        const std::string script = "marketing/shots/" + g_shot + ".py";
        std::error_code ec;
        if (!fs::exists(Path(CurrentApp().ResolveProjectPath(script)), ec)) {
            failure = "shot script not found: " + script;
        } else {
            GameActor* actor = CurrentApp().GetScene().Actors.SpawnScripted("trailer_shot", script, Vector3{ 0, 0, 0 });
            if (actor) actor->Persistent = true;   // a shot may change scenes
            LogWrite(LogLevel::Info, "trailer", "shot spawned: " + script);
        }
    } else if (g_shotSpawned && LogErrorCount() > g_errorsAtSpawn) {
        failure = "an error was logged during the shot (see the engine log)";
    }
    if (failure.empty() && g_shotClock > g_shotTimeout) {
        failure = "shot timed out after " + std::to_string(static_cast<int>(g_shotTimeout)) + " s";
    }
    if (failure.empty()) return;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        if (!g_shotError.empty()) return;
        g_shotError = failure;
    }
    LogWrite(LogLevel::Error, "trailer", failure);
    CurrentApp().RequestQuit();   // the recording, if any, is finalized at the end of Run
}

int TrailerExitCode()
{
    if (g_shot.empty()) return 0;
    const TrailerStatus status = TrailerStatusNow();
    std::lock_guard<std::mutex> lock(g_statusMutex);
    if (!g_shotError.empty()) return 2;
    return status.State == "done" ? 0 : 3;   // 3: the shot ended without a finished recording
}

void TrailerStart(TrailerSettings s)
{
    if (g_rec.Active) {
        LogWrite(LogLevel::Warn, "trailer", "start ignored: already recording");
        return;
    }
    auto fail = [](const std::string& error) {
        LogWrite(LogLevel::Error, "trailer", error);
        SetStatus("error", error);
    };
    if (s.Width <= 0 || s.Height <= 0 || s.Width % 2 || s.Height % 2 || s.Fps <= 0) {
        return fail("invalid recording size or fps (H.264 4:2:0 needs even width and height)");
    }
    if (s.Audio && !IsAudioDeviceReady()) {
        return fail("no audio device: the recording needs the game's audio (audio=False only for diagnostics)");
    }

    Recording& r = g_rec;
    r.Settings = s;
    r.Dir = s.OutputDir.empty()
        ? Path(CurrentApp().ResolveProjectPath("captures")) / Path(s.Name) / Timestamp()
        : Path(s.OutputDir);
    std::error_code ec;
    fs::create_directories(r.Dir.parent_path(), ec);
    if (s.OutputDir.empty()) {   // takes of one shot in the same second (parallel runs): each claims its own folder
        const fs::path base = r.Dir;
        int n = 2;
        for (; !fs::create_directory(r.Dir, ec) && !ec && n < 100; ++n)
            r.Dir = fs::path(base.native() + fs::path("-" + std::to_string(n)).native());
        if (n == 100) return fail("no free folder next to " + Utf8(base));
    } else {
        fs::create_directories(r.Dir, ec);
    }
    if (ec) return fail("cannot create " + Utf8(r.Dir) + ": " + ec.message());

    SourceSize(r.InWidth, r.InHeight);
    std::vector<std::string> args = {
        s.Ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostats", "-y",
        "-f", "rawvideo", "-pixel_format", "rgba",
        "-video_size", std::to_string(r.InWidth) + "x" + std::to_string(r.InHeight),
        "-framerate", std::to_string(s.Fps), "-i", "pipe:0", "-an",
    };
    if (r.InWidth != s.Width || r.InHeight != s.Height) {
        // A source of another size or aspect is fitted, never stretched.
        const std::string w = std::to_string(s.Width), h = std::to_string(s.Height);
        args.insert(args.end(), { "-vf", "scale=" + w + ":" + h + ":force_original_aspect_ratio=decrease:flags=lanczos,"
                                         "pad=" + w + ":" + h + ":(ow-iw)/2:(oh-ih)/2,setsar=1" });
    }
    args.insert(args.end(), { "-c:v", "libx264rgb", "-preset", "ultrafast", "-qp", "0", "-pix_fmt", "bgr0",
                              Utf8(r.Dir / "video.mp4") });
    r.Encoder = TrailerProcess{};
    if (!TrailerSpawn(args, Utf8(r.Dir / "video.log"), true, r.Encoder)) {
        return fail("cannot start FFmpeg '" + s.Ffmpeg + "': install it, or pass its path (tools/trailer.py --ffmpeg)");
    }

    r.Errors.clear();
    r.NextSlot = r.Encoded = r.Repeated = r.Dropped = r.Surplus = 0;
    r.Stopping = false;
    r.WriteFailed = false;
    g_audioFrames = g_audioLost = 0;
    g_ringHead = g_ringTail = 0;
    if (s.Audio) {
        g_ring.assign(kRingFloats, 0.0f);
        r.AudioOut.open(r.Dir / "audio.f32", std::ios::binary);
    }
    r.Writer = std::thread(WriterLoop);
    r.Start = Clock::now();
    if (s.Audio) AttachAudioMixedProcessor(OnMixedAudio);
    r.Active = true;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status = TrailerStatus{};
        g_status.State = "recording";
    }
    LogWrite(LogLevel::Info, "trailer", "recording " + s.Name + " (" + std::to_string(r.InWidth) + "x" +
             std::to_string(r.InHeight) + " -> " + std::to_string(s.Width) + "x" + std::to_string(s.Height) +
             " @ " + std::to_string(s.Fps) + ") into " + Utf8(r.Dir));
}

void TrailerCaptureFrame()
{
    Recording& r = g_rec;
    if (!r.Active) return;
    if (r.WriteFailed) {
        r.Errors.push_back("FFmpeg stopped accepting frames (see video.log)");
        TrailerStop();
        return;
    }
    int width = 0, height = 0;
    SourceSize(width, height);
    if (width != r.InWidth || height != r.InHeight) {
        r.Errors.push_back("the window was resized during the recording");
        TrailerStop();
        return;
    }
    const double seconds = std::chrono::duration<double>(Clock::now() - r.Start).count();
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status.Elapsed = seconds;
    }
    const int repeats = TrailerFrameRepeats(seconds, r.Settings.Fps, r.NextSlot);
    if (repeats == 0) {   // rendered faster than the target rate: this slot already has a frame
        ++r.Surplus;
        return;
    }
    r.Encoded += repeats;
    r.Repeated += repeats - 1;
    {
        std::lock_guard<std::mutex> lock(r.QueueMutex);
        if (r.Queue.size() >= kQueueFrames) {
            // The encoder is behind: this frame's content is dropped, its slots repeat the last one.
            r.Queue.back().Repeats += repeats;
            ++r.Dropped;
            r.Repeated += repeats;
            return;
        }
    }
    rlDrawRenderBatchActive();   // whatever the last pass queued is not in the framebuffer until drawn
    unsigned char* pixels = rlReadScreenPixels(r.InWidth, r.InHeight);   // RGBA, top row first
    {
        std::lock_guard<std::mutex> lock(r.QueueMutex);
        r.Queue.push_back(Frame{ pixels, repeats });
    }
    r.QueueCv.notify_one();
}

void TrailerStop()
{
    Recording& r = g_rec;
    if (!r.Active) return;
    r.Active = false;
    const double seconds = std::chrono::duration<double>(Clock::now() - r.Start).count();
    if (r.Settings.Audio) DetachAudioMixedProcessor(OnMixedAudio);   // raylib locks the mixer: no callback after this
    {
        std::lock_guard<std::mutex> lock(r.QueueMutex);
        r.Stopping = true;
    }
    r.QueueCv.notify_one();
    r.Writer.join();
    r.AudioOut.close();
    g_ring.clear();
    g_ring.shrink_to_fit();
    if (r.WriteFailed) r.Errors.push_back("FFmpeg stopped accepting frames (see video.log)");
    const int videoCode = TrailerWait(r.Encoder);
    if (videoCode != 0) r.Errors.push_back("FFmpeg video encoding failed with exit code " + std::to_string(videoCode) + " (see video.log)");
    if (r.Encoded == 0) r.Errors.push_back("no frame was recorded");

    const TrailerSettings& s = r.Settings;
    const std::string mp4 = Utf8(r.Dir / Path(s.Name + ".mp4"));
    const int sampleRate = seconds > 0.0 ? TrailerSnapSampleRate(double(g_audioFrames.load()) / seconds) : 0;
    int muxCode = 0;
    if (s.Audio) {
        if (g_audioLost > 0) r.Errors.push_back("audio buffer overflow: " + std::to_string(g_audioLost.load()) + " frames lost");
        if (g_audioFrames == 0) r.Errors.push_back("no audio was received from the mixer");
        else if (sampleRate == 0) r.Errors.push_back("unrecognized audio device rate (" +
                                                     std::to_string(double(g_audioFrames.load()) / seconds) + " frames/s)");
    }
    if (r.Errors.empty()) {
        std::vector<std::string> args = { s.Ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostats", "-y",
                                          "-i", Utf8(r.Dir / "video.mp4") };
        if (s.Audio) {
            args.insert(args.end(), { "-f", "f32le", "-ar", std::to_string(sampleRate), "-ac", "2",
                                      "-i", Utf8(r.Dir / "audio.f32"), "-map", "0:v", "-map", "1:a",
                                      "-c:a", "aac", "-b:a", "256k", "-ar", "48000", "-shortest" });
        }
        // saturation 1.25: video players show the game's palette paler than the game itself does (judged by eye).
        args.insert(args.end(), { "-vf", "scale=out_color_matrix=bt709:flags=lanczos+accurate_rnd+full_chroma_int,format=yuv420p,eq=saturation=1.25,"
                                         "setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709" });
        args.insert(args.end(), s.VideoArgs.begin(), s.VideoArgs.end());
        args.insert(args.end(), { "-movflags", "+faststart", mp4 });
        muxCode = RunFfmpeg(args, r.Dir / "mux.log");
        if (muxCode != 0) r.Errors.push_back("FFmpeg mux failed with exit code " + std::to_string(muxCode) + " (see mux.log)");
    }

    std::string shotError;
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        shotError = g_shotError;
    }
    if (!shotError.empty()) r.Errors.push_back(shotError);
    const bool ok = r.Errors.empty();
    std::error_code ec;
    if (ok) {   // temporaries and logs go; a failure keeps them for diagnosis
        for (const char* temp : { "video.mp4", "audio.f32", "video.log", "mux.log" }) fs::remove(r.Dir / temp, ec);
    }

    nlohmann::json report = {
        { "ok", ok },
        { "shot", s.Name },
        { "output", { { "mp4", ok ? mp4 : "" } } },
        { "video", { { "width", s.Width }, { "height", s.Height }, { "source_width", r.InWidth },
                     { "source_height", r.InHeight }, { "fps_nominal", s.Fps }, { "seconds", seconds },
                     { "frames_encoded", r.Encoded } } },
        { "audio", { { "included", s.Audio && ok }, { "channels", 2 }, { "codec", s.Audio ? "aac" : "" },
                     { "device_sample_rate", sampleRate }, { "lost_frames", g_audioLost.load() } } },
        { "capture_stats", { { "dropped_frames", r.Dropped }, { "repeated_frames", r.Repeated }, { "surplus_frames", r.Surplus } } },
        { "errors", r.Errors },
        { "dir", Utf8(r.Dir) },
    };
    nlohmann::json& ui = report["ui"] = nlohmann::json::object();   // engine.trailer.start(track_ui=...)
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        for (const auto& [name, ranges] : g_ui) {
            for (const TrailerUiRange& u : ranges) {
                ui[name].push_back({ { "from", std::round(u.From * 1000.0) / 1000.0 }, { "to", std::round(u.To * 1000.0) / 1000.0 },
                                     { "rect", { u.X, u.Y, u.W, u.H } }, { "opacity", std::round(u.Opacity * 100.0) / 100.0 }, { "visible", u.Visible } });
            }
        }
    }
    const fs::path reportPath = r.Dir / "capture.json";
    WriteReport(report, reportPath);
    {
        std::lock_guard<std::mutex> lock(g_statusMutex);
        g_status.State = ok ? "done" : "error";
        g_status.Elapsed = seconds;
        g_status.Mp4 = ok ? mp4 : "";
        g_status.Report = Utf8(reportPath);
        g_status.Error = ok ? "" : r.Errors.front();
    }
    LogWrite(ok ? LogLevel::Info : LogLevel::Error, "trailer",
             ok ? "recording saved: " + mp4 : "recording failed: " + r.Errors.front() + " (" + Utf8(reportPath) + ")");
}

} // namespace IYI
