// Declares the frame profiler: named, nested CPU sections timed every frame and averaged over
// one-second windows. The engine times its own frame stages (hot reload, fixed updates, update, the
// world and screen draw passes, present) and every actor hook under its script path; C++ code times
// any work worth seeing with a ProfileScope (draw_sprite_stacks: read, cull, sort, emit), and Python
// adds its own sections with engine.profile_begin/profile_end. Every section nests under whatever is
// open when it starts, so engine work shows up under the hook or script section that asked for it.
// Results are data only (rule 9): a game or a library draws them, an agent reads them through the
// agent channel or the --profile log. A parent with active child scopes also reports a synthetic
// "self" child: CPU time spent outside those scopes, measured separately in each frame.
//
// GPU timing (ProfileSetGpu, off by default): every section also drops an OpenGL timestamp where it
// opens and closes, and the results gain a second tree, "gpu/<section>", with the GPU time between
// the marks. Read a few frames late, so it costs no stall. raylib batches draws, so they count in
// the section where it submits them (a shader, blend or texture mode ending, a full batch), not
// always where they were made; the marks never flush the batch, as that would change the picture.
// It is the GPU's work only while the GPU is the slower side (behind the CPU, as when present
// waits); while it waits for the CPU, the wait counts too.
//
// Threads: each thread role keeps its own tree (ProfileSetThread), so the game thread and the render
// thread never share section state and need no lock to time themselves. The engine opens one root
// section per role ("game", "render"), so results read "game/update/..." and "render/draw/...".
// Their times overlap when the roles run on two threads: never add them up as one frame. GPU marks go
// only into the render tree (the GL thread). Published results are read from any thread.
//
// Connected to:
// - Application::Run opens the frame stages and closes the frame with ProfileEndFrame.
// - PythonRuntime::CallActorHook opens one section per hook call, named by the actor's script.
// - EngineBindings exposes profile_begin, profile_end and get_profile.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace IYI {

struct ProfileSection
{
    std::string Name;     // nested sections join with '/': "update/scripts/actors/world.py/sim"
    float AvgMs = 0.0f;   // mean time per frame over the last window (frames without the section count as 0)
    float MaxMs = 0.0f;   // worst single frame in the window
    float Calls = 0.0f;   // mean calls per frame
    std::vector<std::pair<std::string, float>> Counts;   // ProfileCount totals, mean per frame
};

// The tree the calling thread's sections go to from now on. A thread starts on the game tree. With
// both roles on one thread (--single-thread) the engine switches between them; their open sections
// are kept apart, so a switch never closes or mixes anything.
enum class ProfileThread { Game = 0, Render = 1 };
void ProfileSetThread(ProfileThread thread);

// Opens a section inside the innermost open one. A section seen before is found among its parent's
// children without building its path, so a begin/end pair costs two clock reads and a short scan:
// fine in any engine function called up to a few thousand times a frame, not per item of a loop.
// `script` marks a section opened from Python.
void ProfileBegin(std::string_view name, bool script = false);
// Closes the innermost open section; ignored when none is open. A script may only close its own
// sections, so a stray engine.profile_end() cannot end the engine's stage or hook section.
void ProfileEnd(bool script = false);
// Open-section depth, so a caller can close whatever a callee left open (a Python exception
// between profile_begin and profile_end) with ProfileEndTo.
size_t ProfileDepth();
void ProfileEndTo(size_t depth);
// The path of `thread`'s innermost open section ("" when none), read from any thread (hang watchdog,
// crash report). wait = false (a crash handler) answers "?" instead of blocking on a held lock.
std::string ProfileOpenSection(ProfileThread thread, bool wait = true);
// Called once per frame for each tree by the thread using it. Closes anything still open in it.
void ProfileEndFrame();

// Times a C++ scope as a section: opened here, closed on every way out (return, exception), together
// with anything a callee left open inside it. The way C++ code profiles itself:
//     ProfileScope scope("cull");
class ProfileScope
{
public:
    explicit ProfileScope(std::string_view name) : m_depth(ProfileDepth()) { ProfileBegin(name); }
    ~ProfileScope() { ProfileEndTo(m_depth); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    size_t m_depth;
};

// Adds n to a named counter of the innermost open section (ignored when none is open): what a pass
// handled, e.g. ProfileCount("quads", 4) for every quad it emits. Published per section as a mean
// per frame next to its time, so an agent sees the work behind a cost ("profile" reply, F3).
void ProfileCount(std::string_view counter, double n);

// Adds `ms` (summed by the caller over `calls` pieces, ProfileNowMs) to the child `name` of the
// innermost open section: a sub-section for work split too finely for a ProfileScope per piece,
// e.g. one part of every stack in a loop over thousands. No GPU time; ignored when calls is 0.
// Callers time those pieces only while ProfileDetail() is on (the F3 overlay is open), so the clock
// reads cost nothing otherwise.
void ProfileAdd(std::string_view name, double ms, int calls);
double ProfileNowMs();
void ProfileSetDetail(bool enabled);   // from any thread
bool ProfileDetail();

// The last completed one-second window of each tree in tree order (each section followed by its
// children): the game tree, then the render tree with its gpu/ rows. A copy; any thread.
std::vector<ProfileSection> ProfileResults();
// Render frames in the render tree's last window per second of it (the measured frame rate); the
// game tree's when nothing renders.
float ProfileFps();
// A run shorter than one window (--frames 30) has no result yet: publish what it has. Only while no
// thread is using a tree (at exit, after the game thread has stopped).
void ProfilePublishPartial();
// Looks up the GL timestamp query functions. Once, on the render thread, after the window exists.
void ProfileInitGpu();
// GPU timing on or off (see above), from any thread; the render tree follows at its next frame end.
// False when the driver has no timestamp queries.
bool ProfileSetGpu(bool enabled);

} // namespace IYI
