// Implements the video player (Video.h): pl_mpeg decodes on a worker thread (inline in a browser build)
// into bounded queues; the render side shows the frame that is due and feeds the audio stream.

#include "IYI/Video.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include "IYI/Application.h"
#include "IYI/AudioManager.h"
#include "IYI/FramePacket.h"
#include "IYI/Log.h"

#define PL_MPEG_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)   // third-party code: its own warnings are not ours
#endif
#include "pl_mpeg/pl_mpeg.h"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
// CreateFileW, declared here so windows.h (which clashes with raylib) stays out.
extern "C" __declspec(dllimport) void* __stdcall CreateFileW(const wchar_t* name, unsigned long access, unsigned long share,
    void* security, unsigned long disposition, unsigned long flags, void* templateFile);
extern "C" __declspec(dllimport) int __stdcall CloseHandle(void* handle);
#endif
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace IYI {

namespace {
constexpr size_t kFrames = 3;            // decoded frames queued ahead
constexpr double kAudioLead = 0.25;      // seconds of audio queued ahead
constexpr int kChunk = 1536;             // audio stream sub-buffer, frames (~32 ms at 48 kHz)
constexpr double kTail = 0.1;            // the stream's buffered audio still plays after the last sample
constexpr int kMaxPixels = 3840 * 2160;  // larger videos are refused
}

struct Video::Impl
{
    struct Frame { double pts; std::vector<unsigned char> rgba; };

    std::vector<unsigned char> bytes;    // packaged builds only: the whole compressed file, read in place
    plm_t* plm = nullptr;
    int width = 0, height = 0, sampleRate = 0;
    double origin = -1.0;                // the first frame's time: clocks count from it

    std::mutex mutex;                    // guards everything below shared with the worker
    std::deque<Frame> frames;
    std::vector<std::vector<unsigned char>> pool;
    std::deque<float> audio;             // interleaved stereo, not yet in the stream
    bool videoDone = false, audioDone = false;

    std::thread worker;
    std::atomic<bool> quit{ false };

    // Render side only.
    Texture2D texture{};
    AudioStream stream{};
    bool hasFrame = false, running = false, started = false, released = false;
    double clock = 0.0;
    float volume = 1.0f;

    // Decodes one frame or one audio chunk a queue has room for; false when there is nothing to do.
    bool DecodeOne()
    {
        bool wantVideo, wantAudio;
        std::vector<unsigned char> buffer;
        {
            std::lock_guard<std::mutex> lock(mutex);
            wantVideo = !videoDone && frames.size() < kFrames;
            wantAudio = sampleRate > 0 && !audioDone && audio.size() < static_cast<size_t>(kAudioLead * sampleRate * 2);
            if (wantVideo && !pool.empty()) { buffer = std::move(pool.back()); pool.pop_back(); }
        }
        if (wantVideo) {
            plm_frame_t* f = plm_decode_video(plm);
            if (f) {
                if (buffer.empty()) buffer.assign(static_cast<size_t>(width) * height * 4, 255);   // alpha stays 255
                plm_frame_to_rgba(f, buffer.data(), width * 4);
                if (origin < 0.0) origin = f->time;
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (f) frames.push_back({ f->time - origin, std::move(buffer) });
            else videoDone = true;
            return true;
        }
        if (wantAudio) {
            plm_samples_t* s = plm_decode_audio(plm);
            std::lock_guard<std::mutex> lock(mutex);
            if (s) audio.insert(audio.end(), s->interleaved, s->interleaved + s->count * 2);
            else audioDone = true;
            return true;
        }
        return false;
    }

    void StartWorker()
    {
#ifndef __EMSCRIPTEN__   // no threads in the browser: Tick decodes inline
        quit = false;
        worker = std::thread([this]() {
            while (!quit) {
                if (!DecodeOne()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
#endif
    }

    void StopWorker()
    {
        quit = true;
        if (worker.joinable()) worker.join();
    }

    // Back to the start with empty queues; the worker must be stopped.
    void Rewind()
    {
        plm_rewind(plm);
        for (Frame& f : frames) pool.push_back(std::move(f.rgba));
        frames.clear();
        audio.clear();
        videoDone = audioDone = false;
        origin = -1.0;
        clock = 0.0;
        hasFrame = false;
    }

    void Release()
    {
        if (released) return;
        released = true;
        running = false;
        StopWorker();
        if (texture.id != 0) UnloadTexture(texture);
        if (stream.buffer) UnloadAudioStream(stream);
        if (plm) plm_destroy(plm);
        plm = nullptr;
        frames.clear(); pool.clear(); audio.clear();
        std::vector<unsigned char>().swap(bytes);
    }
};

namespace {
std::vector<std::weak_ptr<Video>> s_live;   // render side: videos that have started, ticked each frame
}

Video::Video(const std::string& relPath) : Video(CurrentApp().GetAssets().GetResourceStore(), relPath) {}

Video::Video(const ResourceStore& store, const std::string& relPath) : m_impl(std::make_unique<Impl>())
{
    Impl& d = *m_impl;
    const auto fail = [&](const std::string& why) {   // an Error video holds no file handle or buffer
        if (d.plm) plm_destroy(d.plm);
        d.plm = nullptr;
        std::vector<unsigned char>().swap(d.bytes);
        m_error = why;
        LogWrite(LogLevel::Warn, "assets", "engine.video.open('" + relPath + "'): " + why);
    };
    // A loose file streams from disk through pl_mpeg's own small buffer (as music does). A packed entry
    // is held whole: iyipack encrypts and authenticates each file as one chunk (XChaCha20-Poly1305), so
    // no byte of it can be used before all of it is read. shortcut: streaming a packed video needs
    // chunked encryption in iyipack; worth it only once a shipped .mpg is large (tens of MB).
    const std::string physical = store.GetPhysicalPath(relPath);
    if (!physical.empty()) {
#ifdef _WIN32
        // Wide path (fopen takes the ANSI code page) and every share mode, delete included: an open
        // video never blocks re-exporting, replacing or deleting the file (playback keeps the old one).
        constexpr unsigned long kGenericRead = 0x80000000UL, kShareAll = 0x7, kOpenExisting = 3, kNormal = 0x80;
        FILE* file = nullptr;
        void* handle = CreateFileW(std::filesystem::path(physical).c_str(), kGenericRead, kShareAll, nullptr,
                                   kOpenExisting, kNormal, nullptr);
        if (handle != reinterpret_cast<void*>(static_cast<intptr_t>(-1))) {
            const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
            if (fd < 0) CloseHandle(handle);
            else if (!(file = _fdopen(fd, "rb"))) _close(fd);   // fclose / _close also close the handle
        }
#else
        FILE* file = std::fopen(physical.c_str(), "rb");
#endif
        if (file) d.plm = plm_create_with_file(file, 1);
    } else if (store.ReadBytes(relPath, d.bytes) && !d.bytes.empty()) {
        d.plm = plm_create_with_memory(d.bytes.data(), d.bytes.size(), 0);
    }
    if (!d.plm) { fail("file not found"); return; }
    if (!plm_probe(d.plm, 5 * 1024 * 1024) || !plm_has_headers(d.plm) || plm_get_num_video_streams(d.plm) == 0) {
        { fail("not an MPEG-1 video in an MPEG-PS (.mpg) file"); return; }
    }
    d.width = plm_get_width(d.plm);
    d.height = plm_get_height(d.plm);
    if (d.width <= 0 || d.height <= 0 || d.width * d.height > kMaxPixels) {
        { fail("unsupported size " + std::to_string(d.width) + "x" + std::to_string(d.height)); return; }
    }
    m_duration = static_cast<float>(plm_get_duration(d.plm));
    if (!(m_duration > 0.0f)) { fail("zero duration"); return; }
    m_hasAudio = plm_get_num_audio_streams(d.plm) > 0;
    if (m_hasAudio) d.sampleRate = plm_get_samplerate(d.plm);
    else plm_set_audio_enabled(d.plm, 0);
    m_width = d.width;
    m_height = d.height;
    m_state = State::Ready;
}

Video::~Video()
{
    m_impl->Release();
}

const char* Video::StateName() const
{
    static const char* const names[] = { "ready", "playing", "paused", "ended", "error", "closed" };
    return names[static_cast<int>(m_state.load())];
}

void Video::Play()
{
    const State s = m_state;
    if (s == State::Paused) return Resume();
    if (s != State::Ready && s != State::Ended) return;
    m_state = State::Playing;
    RecordSetup([self = shared_from_this()]() {
        Impl& d = *self->m_impl;
        if (d.released) return;
        if (!d.started) {
            d.started = true;
            const Image black = GenImageColor(d.width, d.height, BLACK);
            d.texture = LoadTextureFromImage(black);
            UnloadImage(black);
            SetTextureFilter(d.texture, TEXTURE_FILTER_BILINEAR);
            if (d.sampleRate > 0 && IsAudioDeviceReady()) {
                SetAudioStreamBufferSizeDefault(kChunk);
                d.stream = LoadAudioStream(static_cast<unsigned int>(d.sampleRate), 32, 2);
                SetAudioStreamBufferSizeDefault(0);
            } else if (d.sampleRate > 0) {
                plm_set_audio_enabled(d.plm, 0);   // no device: never queue samples nobody plays
                d.sampleRate = 0;
            }
            s_live.push_back(self);
        }
        d.StopWorker();
        if (d.clock > 0.0 || d.videoDone) d.Rewind();
        d.StartWorker();
        d.running = true;
        if (d.stream.buffer) PlayAudioStream(d.stream);
        State ended = State::Ended;   // a tick of the previous run may have ended it meanwhile
        self->m_state.compare_exchange_strong(ended, State::Playing);
    });
}

void Video::Pause()
{
    if (m_state != State::Playing) return;
    m_state = State::Paused;
    RecordSetup([self = shared_from_this()]() {
        Impl& d = *self->m_impl;
        if (d.released || !d.running) return;
        d.running = false;
        if (d.stream.buffer) PauseAudioStream(d.stream);
    });
}

void Video::Resume()
{
    if (m_state != State::Paused) return;
    m_state = State::Playing;
    RecordSetup([self = shared_from_this()]() {
        Impl& d = *self->m_impl;
        if (d.released || !d.started) return;
        d.running = true;
        if (d.stream.buffer) ResumeAudioStream(d.stream);
    });
}

void Video::Stop()
{
    const State s = m_state;
    if (s != State::Playing && s != State::Paused && s != State::Ended) return;
    m_state = State::Ready;
    m_position = 0.0f;
    RecordSetup([self = shared_from_this()]() {
        Impl& d = *self->m_impl;
        if (d.released || !d.started) return;
        d.running = false;
        d.StopWorker();
        if (d.stream.buffer) StopAudioStream(d.stream);
        d.Rewind();
        self->m_position = 0.0f;
    });
}

void Video::Close()
{
    if (m_state == State::Closed) return;
    m_state = State::Closed;
    RecordSetup([self = shared_from_this()]() { self->m_impl->Release(); });
}

void Video::SetVolume(float volume)
{
    if (!(volume >= 0.0f)) volume = 0.0f;   // NaN too
    RecordSetup([self = shared_from_this(), v = std::min(volume, 1.0f)]() { self->m_impl->volume = v; });
}

void Video::DrawFit(Rectangle area)
{
    const State s = m_state;
    if (s == State::Closed || s == State::Error) return;
    Record([self = shared_from_this(), area]() {
        const Impl& d = *self->m_impl;
        DrawRectangleRec(area, BLACK);
        if (d.released || !d.hasFrame || area.width <= 0.0f || area.height <= 0.0f) return;
        const float scale = std::min(area.width / d.width, area.height / d.height);
        const float w = d.width * scale, h = d.height * scale;
        DrawTexturePro(d.texture, { 0.0f, 0.0f, static_cast<float>(d.width), static_cast<float>(d.height) },
                       { area.x + (area.width - w) * 0.5f, area.y + (area.height - h) * 0.5f, w, h },
                       { 0.0f, 0.0f }, 0.0f, WHITE);
    });
}

void Video::DrawRegion(Rectangle src, Rectangle dst, Color tint)
{
    const State s = m_state;
    if (s == State::Closed || s == State::Error) return;
    Record([self = shared_from_this(), src, dst, tint]() {
        const Impl& d = *self->m_impl;
        if (!d.released && d.hasFrame) DrawTexturePro(d.texture, src, dst, { 0.0f, 0.0f }, 0.0f, tint);
    });
}

void Video::Tick(float dt)
{
    Impl& d = *m_impl;
    if (d.released || !d.running) return;
#ifdef __EMSCRIPTEN__
    while (d.DecodeOne()) {}
#endif
    d.clock += dt;
    m_position = static_cast<float>(std::min(d.clock, static_cast<double>(m_duration)));

    std::vector<unsigned char> due;
    bool ended;
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        while (!d.frames.empty() && d.frames.front().pts <= d.clock) {   // late frames are dropped
            if (!due.empty()) d.pool.push_back(std::move(due));
            due = std::move(d.frames.front().rgba);
            d.frames.pop_front();
        }
        ended = d.videoDone && d.frames.empty() && d.audio.empty() && (d.audioDone || d.sampleRate == 0);
    }
    if (!due.empty()) {
        UpdateTexture(d.texture, due.data());
        d.hasFrame = true;
        std::lock_guard<std::mutex> lock(d.mutex);
        d.pool.push_back(std::move(due));
    }

    if (d.stream.buffer) {
        SetAudioStreamVolume(d.stream, AudioManager::Get().GetBusVolume("master") * AudioManager::Get().GetBusVolume("music") * d.volume);
        float chunk[kChunk * 2];
        while (IsAudioStreamProcessed(d.stream)) {
            std::lock_guard<std::mutex> lock(d.mutex);
            const size_t n = std::min(d.audio.size(), static_cast<size_t>(kChunk * 2));
            std::copy_n(d.audio.begin(), n, chunk);
            d.audio.erase(d.audio.begin(), d.audio.begin() + static_cast<std::ptrdiff_t>(n));
            std::fill(chunk + n, chunk + kChunk * 2, 0.0f);   // starved or finished: silence
            UpdateAudioStream(d.stream, chunk, kChunk);
        }
    }

    if (ended && d.clock >= m_duration + kTail) {
        d.running = false;
        d.StopWorker();
        if (d.stream.buffer) StopAudioStream(d.stream);
        State playing = State::Playing;   // a Stop or Close the game side asked for meanwhile wins
        m_state.compare_exchange_strong(playing, State::Ended);
        m_position = m_duration;
    }
}

std::shared_ptr<Video> OpenVideo(const std::string& relPath)
{
    return RenderOwned(new Video(relPath));
}

void UpdateVideos(float dt)
{
    s_live.erase(std::remove_if(s_live.begin(), s_live.end(), [](const std::weak_ptr<Video>& w) { return w.expired(); }),
                 s_live.end());
    for (const std::weak_ptr<Video>& w : s_live) {
        if (const std::shared_ptr<Video> v = w.lock()) v->Tick(dt);
    }
}

} // namespace IYI
