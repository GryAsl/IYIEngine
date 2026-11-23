// Declares the frame input: raylib's input and window state as the render (main) thread saw it right
// after polling events, copied once per frame for the game step. raylib's input is plain globals that
// every poll rewrites, and GLFW's window functions belong to the main thread, so game-side code (the
// bindings, IYIUI) reads this copy, never raylib's input or window functions. One copy per step, taken
// right after the poll: a press and release between two steps are both seen, each once.
// The other way (resize, fullscreen, clipboard, IME placement) goes as platform requests in the frame
// packet (FramePacket.h), run by the render side.

#pragma once

#include <bitset>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "raylib.h"
#include "IYI/Ime.h"

namespace IYI {

struct GamepadInput
{
    bool Available = false;
    std::bitset<32> Down, Pressed;
    float Axes[8] = {};
};

struct FrameInput
{
    float FrameTime = 0.0f;
    double Time = 0.0;                              // seconds since start; with --fixed-dt the sum of the steps
    int Fps = 0;
    int ScreenWidth = 0, ScreenHeight = 0;
    int RenderWidth = 0, RenderHeight = 0;         // the framebuffer (DPI scaled)
    int Presented = 0;                              // game steps the render side had drawn when this was taken
    bool Focused = true, Minimized = false, Fullscreen = false;
    int Monitor = -1, MonitorRefreshRate = 0;
    std::vector<std::pair<int, int>> MonitorResolutions;   // the current monitor's modes, unique sizes

    std::bitset<512> KeyDown, KeyPressed, KeyReleased, KeyRepeat;   // raylib's MAX_KEYBOARD_KEYS
    std::vector<int> Keys, Chars;   // GetKeyPressed's and GetCharPressed's queues, popped in order
    Vector2 Mouse{}, MouseDelta{}, Wheel{};
    float Pinch = 1.0f;   // two-finger zoom this frame: finger distance now / last frame (1 = none)
    std::bitset<8> MouseDown, MousePressed, MouseReleased;
#ifdef IYI_MOBILE
    // Keep the real release for UI capture cleanup, but do not turn a long touch into a click.
    bool LongTouchRelease = false;
#endif
    GamepadInput Gamepads[4];       // raylib's MAX_GAMEPADS
    std::string Clipboard;          // read only on the paste shortcut (Ctrl/Cmd+V): reading it every frame is slow
    bool CloseRequested = false;    // close button or Alt+F4 this frame

    ImeFrame Ime;                   // as the window's message hook left it for this frame (Ime.h)

    // The audio side (AudioManager) as of the last Audio pass it ran. What the game side asked since
    // is held by the bindings until AudioDone reaches it (AudioBindings.cpp).
    std::vector<int> AudioPlaying;          // handles playing
    std::string Music;                      // current music path
    std::unordered_map<std::string, float> BusVolumes;
    long long AudioDone = 0;                // engine.audio calls the audio side has run so far
};

// Render side, right after the poll. `in` still holds an older frame's copy, whose monitor modes are
// kept when the monitor has not changed (asking GLFW for them every frame is slow).
void TakeFrameInput(FrameInput& in);

// Game side: the copy this step reads (Application sets it before the step).
void SetGameInput(FrameInput* input);
FrameInput& GameInput();

// A bit of one of the bitsets above; false for an index out of range (a key code from Python).
template <size_t N>
bool InputBit(const std::bitset<N>& bits, int index)
{
    return index >= 0 && index < static_cast<int>(N) && bits[static_cast<size_t>(index)];
}

// Pops a queue as raylib's GetKeyPressed / GetCharPressed do: 0 when empty.
int PopInput(std::vector<int>& queue);

} // namespace IYI
