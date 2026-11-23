// Declares the agent channel: a development-only file interface that lets an agent (or a script)
// look at a running game without game code. Every game gets it for free: write
// <project>/agentio/request.json, read <project>/agentio/<id>.json (and <id>.png). The entry script
// may define AgentRequest(request) to apply game-specific parts of a request (camera, debug state);
// its return value comes back in the reply. Shipping builds compile it out (SPEC §3.1).
//
// Every reply carries the log's new warnings and errors. For checking a picture by numbers instead
// of looking at it, a request may ask for "regions" (mean colour per screen cell), "compare" (the
// difference from an earlier request's image) and "pixels" (the colour at world or screen points);
// those take the frame's image as a screenshot does and measure the saved file. "track_ui" (element
// names) records each waited frame's on-screen rect and opacity of those UI elements, for motion.
//
// Connected to:
// - Application::Run calls UpdateAgentChannel once per frame, in the update stage.
// - Uses Application (screenshots, quit), PythonRuntime::CallAgentRequest and the profiler.

#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace IYI {

// One frame of agent input ("click", "type", "key", "scroll", "drag"), laid over the game's frame input.
struct AgentInputStep
{
    int Key = 0;             // a key pressed (KeyDown) this frame, or released when KeyUp
    bool KeyUp = false;
    bool KeyHold = false;    // held down without a new press ("hold_frames")
    std::vector<int> Mods;   // modifier keys held down this frame
    int Mouse = 0;           // 1: left button pressed at X, Y; 2: released there; 3: moved there, held; 4: moved there
    float X = -1.0f, Y = -1.0f;
    float Wheel = 0.0f;      // vertical wheel move this frame
    std::vector<int> Chars;  // typed characters
};

struct AgentChannel
{
    std::string Dir;           // <project>/agentio; "" turns the channel off
    float PollTimer = 0.0f;
    bool Lockstep = false;     // --agent-run --fixed-dt: no frame runs while no request is waiting
    std::function<bool()> CloseAsked;   // the window was closed: a lockstep wait ends at once
    std::function<bool()> LauncherGone; // --agent-run: the process that started this one has ended (WatchLauncher)
    int IdleWaits = 0;         // lockstep waits in a row that timed out (60 s each) with no request

    // The request being served (one at a time; a new file waits until this one is answered).
    bool Pending = false;
    std::string Id;
    std::string GameReply;     // AgentRequest's reply, JSON
    bool Screenshot = false;
    bool Profile = false;
    bool Health = false;
    bool Quit = false;
    bool Image = false;        // regions / compare / pixels: the frame's image is needed
    std::string Request;       // the request as given, JSON (its image keys are read at the reply)
    std::string Points;        // "pixels" with their screen positions, JSON, worked out at the shot
    std::vector<std::string> TrackNames;    // "track_ui": UI element names to record every frame
    std::vector<std::string> TrackFrames;   // one JSON object per recorded frame
    int LogMark = 0;           // warnings + errors logged before the request
    int EventMark = 0;         // structured events logged before the request
    int FramesLeft = 0;        // frames still to run before the outputs
    float SecondsLeft = 0.0f;  // and time still to wait
    bool ShotRequested = false;
    int ShotStep = 0;            // the game step whose frame the image shows
    std::vector<AgentInputStep> Input;   // "click" / "type" / "key", one step per frame
    size_t InputNext = 0;
    std::string InputReply;      // JSON: where each action landed, or why it could not
    std::string WaitScene, WaitVisible;  // "wait_until"
    int WaitTimeout = 0, WaitFrames = 0;
    bool Waiting = false, WaitMet = false;
    bool UiDump = false;         // "ui": every named element on screen
    bool UiProblems = false;     // "ui_problems": the UI rules' findings (UICanvas::Problems)

    // Kept every frame for the health report, whether or not a request is waiting.
    std::array<float, 240> FrameMs{};   // the last 240 frame times, a ring
    int Frames = 0;                     // frames since start
    double Seconds = 0.0;               // time since start
    bool Focused = false;
    int FocusChanges = 0;               // since start: a window that keeps losing focus flickers
    double LastMemSeconds = -1.0;       // the previous health report: memory then, for the growth rate
    double LastPrivateMb = 0.0;
};

// The named UI elements as this frame will draw them (game side, after every hook), as a JSON object
// name -> {rect: [x, y, w, h] on-screen canvas units, opacity, visible}; a name not found is left out.
std::string TrackUi(const std::vector<std::string>& names);
// Polls for a request (4 times a second) and advances the one being served.
void UpdateAgentChannel(AgentChannel& channel, float dt);
// The process that started this one, watched: true once it has ended. An agent run then quits, so a
// launcher that was killed (a tool timeout, a closed terminal) never leaves a hidden game running.
std::function<bool()> WatchLauncher();

// Game side, before the frame's hooks: lays the next agent input step over GameInput().
void ApplyAgentInput(AgentChannel& channel);

// The profiler's last window as JSON (also used by the --profile shutdown log).
std::string ProfileJson();

} // namespace IYI
