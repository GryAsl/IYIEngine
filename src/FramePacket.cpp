// Implements the frame packet (FramePacket.h): recording GL calls on the game side and replaying them,
// in order, on the render side.

#include "IYI/FramePacket.h"

#include "IYI/Log.h"
#include "IYI/Profiler.h"
#include "rlgl.h"

namespace IYI {

namespace {

// Game side only (one thread at a time: set before the game thread starts, then used only by it).
FramePacket* s_recording = nullptr;   // the packet being filled
RecordPass s_pass = RecordPass::Offscreen;
thread_local bool t_renderSide = false;
thread_local bool t_world3D = false;      // ReplayPass is running the world pass of a 3D-camera packet
thread_local bool t_textMask = false;     // SetTextMask: the screen pass marks text in the target's alpha
thread_local bool t_textBlend = false;    // the text blend is on (BeginTextDraw until EndTextDraw)
bool s_textRun[5] = {};                   // game side, per pass: the last command recorded was RecordText

// Colour blends as BLEND_ALPHA; alpha either adds up (clamped at 1) or, for text, drops by coverage.
void SetMaskBlend(bool text)
{
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, text ? RL_ZERO : RL_ONE,
                              text ? RL_ONE_MINUS_SRC_ALPHA : RL_ONE, RL_FUNC_ADD, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);   // flushes what was queued under the previous blend
}

} // namespace

void FramePacket::Clear()
{
    for (std::vector<RenderCommand>& pass : Passes) pass.clear();   // keeps capacity: no reallocation per frame
    Screenshot.clear();
}

void Record(RenderCommand command)
{
    // Render-side code that reaches a recording call (a replayed pass using a game-side helper) would
    // append to the packet being replayed: run it now instead, and say so once, as it is a bug there.
    if (t_renderSide) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LogWrite(LogLevel::Warn, "render", "Record() called on the render side; ran it at once");
        }
        command();
        return;
    }
    if (s_recording && s_textRun[static_cast<int>(s_pass)]) {
        s_textRun[static_cast<int>(s_pass)] = false;
        s_recording->Passes[static_cast<int>(s_pass)].push_back([]() { EndTextDraw(); });
    }
    RecordState(std::move(command));
}

void RecordState(RenderCommand command)
{
    if (t_renderSide) return Record(std::move(command));   // the warning and the immediate run
    if (s_recording) s_recording->Passes[static_cast<int>(s_pass)].push_back(std::move(command));
    else LogWrite(LogLevel::Warn, "render", "Record() with no frame packet: dropped");
}

void RecordText(RenderCommand command)
{
    if (t_renderSide) return Record(std::move(command));
    RecordState(std::move(command));
    if (s_recording) s_textRun[static_cast<int>(s_pass)] = true;
}

void RecordTo(RecordPass target, RenderCommand command)
{
    if (t_renderSide) return Record(std::move(command));   // runs at once; the game side's pass is not touched
    const RecordPass pass = s_pass;
    s_pass = target;
    Record(std::move(command));
    s_pass = pass;
}

void RecordSetup(RenderCommand command)
{
    RecordTo(RecordPass::Setup, std::move(command));
}

void RecordAudio(RenderCommand command)
{
    RecordTo(RecordPass::Audio, std::move(command));
}

void SetRecordingPacket(FramePacket* packet)
{
    s_recording = packet;
    for (bool& run : s_textRun) run = false;   // a new packet starts with no text run open
}

void SetRecordPass(RecordPass pass)
{
    s_pass = pass;
}

RecordPass CurrentRecordPass()
{
    return s_pass;
}

void SetRenderSide(bool on)
{
    t_renderSide = on;
}

bool OnRenderSide()
{
    return t_renderSide;
}

void ReplayPass(FramePacket& packet, RecordPass pass)
{
    std::vector<RenderCommand>& commands = packet.Passes[static_cast<int>(pass)];
    ProfileCount("commands", static_cast<double>(commands.size()));
    t_world3D = pass == RecordPass::World && packet.Camera3DActive;
    for (RenderCommand& command : commands) command();
    EndTextDraw();   // a pass that ended on a text run (RecordText) closes it
    t_world3D = false;
}

void SetTextMask(bool on)
{
    t_textMask = on;
    t_textBlend = false;
    if (on) SetMaskBlend(false);
    else rlSetBlendMode(RL_BLEND_ALPHA);
}

void BeginTextDraw()
{
    if (t_textMask && !t_textBlend) {
        SetMaskBlend(true);
        t_textBlend = true;
    }
}

void EndTextDraw()
{
    if (t_textMask && t_textBlend) {
        SetMaskBlend(false);
        t_textBlend = false;
    }
}

bool ReplayingWorld3D()
{
    return t_world3D;
}

} // namespace IYI
