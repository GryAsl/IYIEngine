// Declares the frame packet: everything the game side asks the render side to draw for one frame.
// Game-side code (Python hooks and the engine C++ they call: the raylib forwards, IYIUI, sprite stacks,
// lighting, particles, terrain) never calls OpenGL itself. It records each GL call, in order, as a
// small closure with Record(); the render side replays the packet on the thread that holds the GL
// context (Application::Run). This is what lets Python run on a game thread while raylib stays on the
// main thread, and it keeps the game's Python API unchanged: engine.draw_texture still "draws".
//
// A closure captures values, never references to game-side state that may change before the replay:
// a texture, font or shader is captured as a pointer into the AssetManager cache (stable, and filled
// in by the render side before use), a list of items is copied or moved in.
//
// Connected to:
// - Application::GameStep records a game step into a packet; Application::RenderPacket replays it.
// - The bindings (RaylibBindings.cpp, EngineBindings.cpp) and IYIUI record through Record().

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "raylib.h"

namespace IYI {

using RenderCommand = std::function<void()>;

// Which list Record() appends to. GL work from Start, Update and the other non-drawing hooks is
// offscreen work (texture-mode passes) and runs before the frame's draw; Drawing hooks fill the world
// pass (inside the camera) and the screen pass (the HUD).
// Setup holds what must happen before the frame draws: window requests (GLFW is main-thread only)
// and GPU uploads of resources the game side just loaded. It runs first. Audio holds engine.audio
// calls for the audio side (AudioManager), run by whichever thread owns it.
enum class RecordPass { Offscreen = 0, World = 1, Screen = 2, Setup = 3, Audio = 4 };

struct FramePacket
{
    std::vector<RenderCommand> Passes[5];        // indexed by RecordPass

    // The frame state the replay draws with, taken from the game side when the draw pass is recorded.
    Color Background = RAYWHITE;
    Camera2D View2D = { { 0.0f, 0.0f }, { 0.0f, 0.0f }, 0.0f, 1.0f };
    float CameraVerticalScale = 1.0f;
    bool CameraActive = false;
    Camera3D View3D{};
    bool Camera3DActive = false;
    Shader* Postprocess = nullptr;                // AssetManager owns it
    float PostprocessStrength = 1.0f;
    double Time = 0.0;                           // the frame input's clock: the postprocess uTime
    float PostprocessRadialWarp = 0.0f;
    bool PostprocessWorldOnly = false;
    bool PostprocessTextMask = false;            // text marks texture0's alpha (BeginTextDraw)
    std::string Screenshot;                      // "" = none requested
    bool Quit = false;                           // the game side ends after this frame

    void Clear();
};

// Appends a GL call to the recording packet's current pass. Game side only.
void Record(RenderCommand command);
// Appends a text draw that calls BeginTextDraw itself: consecutive text draws share one text blend
// (the mask's blend switch flushes the batch), and the next Record() appends the EndTextDraw.
void RecordText(RenderCommand command);
// Appends a call that draws nothing (matrix, scissor), so a run of text draws stays one run.
void RecordState(RenderCommand command);
// Appends to a given pass, whatever pass is current (Screen: after every draw of the packet).
void RecordTo(RecordPass pass, RenderCommand command);
// Appends a window call or a GPU upload to the packet's Setup pass, whatever pass is current.
void RecordSetup(RenderCommand command);
// Appends an AudioManager call to the packet's Audio pass.
void RecordAudio(RenderCommand command);
// The packet Record() fills and the pass it appends to (Application switches them).
void SetRecordingPacket(FramePacket* packet);
void SetRecordPass(RecordPass pass);
RecordPass CurrentRecordPass();

// Render side: while on, draws keep the target's alpha at 1 and text drawn between BeginTextDraw and
// EndTextDraw lowers it by its coverage, so a text_mask postprocess shader can tell text apart.
// BeginTextDraw and EndTextDraw only switch the blend when it is not already the one asked for.
void SetTextMask(bool on);
void BeginTextDraw();
void EndTextDraw();

// Runs one pass of a packet, in recorded order. Render side only.
void ReplayPass(FramePacket& packet, RecordPass pass);
// Render side: replaying the world pass through a 3D camera (depth tested) right now.
bool ReplayingWorld3D();

// True on the thread while it acts as the render side (Application sets it around the replay).
void SetRenderSide(bool on);
bool OnRenderSide();

// Shared ownership for an engine object that holds GL resources and is referenced from both sides:
// Python's game-side handle and the recorded commands that draw with it. Whichever lets go last, the
// object is deleted on the render side, since its destructor frees GL objects: on the game side the
// deletion is recorded instead of run.
template <class T>
std::shared_ptr<T> RenderOwned(T* object)
{
    return std::shared_ptr<T>(object, [](T* p) {
        if (OnRenderSide()) delete p;
        else Record([p]() { delete p; });
    });
}

} // namespace IYI
