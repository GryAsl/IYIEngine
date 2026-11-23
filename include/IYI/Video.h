// Declares the video player: plays an MPEG-1 video with optional MP2 audio (.mpg, MPEG-PS) through
// pl_mpeg (third_party/pl_mpeg, MIT), drawn into one engine texture with its sound on a raylib audio
// stream on the "music" bus. Python sees it as engine.video (VideoBindings.cpp); a native game uses
// this class directly.
//
// Sides (SPEC §8a): the constructor runs on the game side (reads the file through the ResourceStore,
// parses its headers, so sizes and duration are known at once). Play/Pause/... record their work for
// the render side and show their state at once. The render side ticks every playing video once per
// frame (UpdateVideos, after the AudioManager): it advances the clock, uploads the frame that is due
// and feeds the audio. A worker thread decodes ahead into small bounded queues (3 frames, ~0.25 s of
// audio); a browser build has no threads, so the tick decodes inline there.
//
// Clock: a wall clock from play, frozen while paused; audio samples are queued in step with it, so
// over a clip of a few minutes it cannot drift noticeably. A late frame is dropped, never queued up.
//
// Connected to:
// - VideoBindings.cpp (engine.video), Application::RenderPacket (UpdateVideos).
// - AudioManager::CalculateEffectiveVolume ("music" bus x master x the video's own volume).

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include "raylib.h"

namespace IYI {

class Video : public std::enable_shared_from_this<Video>
{
public:
    enum class State { Ready, Playing, Paused, Ended, Error, Closed };

    // Game side. A missing, unreadable or non-MPEG-PS file gives State::Error with Error(); never throws.
    explicit Video(const std::string& relPath);              // through the running game's ResourceStore
    Video(const class ResourceStore& store, const std::string& relPath);
    ~Video();   // render side (RenderOwned): joins the decoder, frees the texture and the stream

    // Game side; each records its render-side work. Play restarts an ended or stopped video from the
    // start and resumes a paused one; Stop rewinds to the start (Ready); Close frees everything at once
    // and is safe to repeat. Every call after Close, or on an Error video, does nothing.
    void Play();
    void Pause();
    void Resume();
    void Stop();
    void Close();
    void SetVolume(float volume);
    // Records a draw: black over area, the current frame aspect-fit in its middle.
    void DrawFit(Rectangle area);
    // Records a draw of the current frame's src part into dst (nothing before the first frame);
    // a UI image showing this video (UIImage::VideoSource) draws through it.
    void DrawRegion(Rectangle src, Rectangle dst, Color tint);

    State GetState() const { return m_state.load(); }
    const char* StateName() const;
    float Duration() const { return m_duration; }   // the container's estimate: may be short by one GOP (~0.5 s); State::Ended is exact
    float Position() const { return m_position.load(); }
    int Width() const { return m_width; }
    int Height() const { return m_height; }
    bool HasAudio() const { return m_hasAudio; }
    const std::string& Error() const { return m_error; }

    void Tick(float dt);   // render side, once per frame (UpdateVideos)

private:
    struct Impl;   // decoder, queues, texture, stream: render side and the worker only
    std::unique_ptr<Impl> m_impl;
    std::atomic<State> m_state{ State::Error };
    std::atomic<float> m_position{ 0.0f };
    float m_duration = 0.0f;
    int m_width = 0;
    int m_height = 0;
    bool m_hasAudio = false;
    std::string m_error;
};

// Game side: opens a video shared with the commands that draw it, deleted on the render side.
std::shared_ptr<Video> OpenVideo(const std::string& relPath);
// Render side, once per frame: ticks every video that is playing.
void UpdateVideos(float dt);

} // namespace IYI
