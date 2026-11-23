// Implements the frame input (FrameInput.h): one copy of raylib's input and window state per frame.

#include "IYI/FrameInput.h"

#include <algorithm>
#include <numeric>
#include <cmath>
#ifdef IYI_PLATFORM_ANDROID
#include "MobilePaths.h"
#endif
#ifndef IYI_MOBILE
#include <GLFW/glfw3.h>   // the monitor's video modes: raylib has no call for them
#endif
#include "IYI/AudioManager.h"
#include "IYI/FramePacket.h"

namespace IYI {

namespace {

FrameInput s_none;
FrameInput* s_game = &s_none;

std::vector<std::pair<int, int>> MonitorResolutions(int index)
{
    std::vector<std::pair<int, int>> sizes;
#ifdef IYI_MOBILE
    if (index == 0) {
        const int w = GetScreenWidth(), h = GetScreenHeight(), g = std::gcd(w, h);
        if (g > 0) {
            sizes.emplace_back(w, h);
            for (int percent = 95; percent >= 50; percent -= 7) {
                int k = g * percent / 100;
                if (((w / g) % 2 || (h / g) % 2) && k % 2) --k;
                if (k > 0 && sizes.back() != std::make_pair(k * (w / g), k * (h / g)))
                    sizes.emplace_back(k * (w / g), k * (h / g));
            }
        }
    }
#else
    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    if (monitors == nullptr || index < 0 || index >= count) return sizes;
    int modeCount = 0;
    const GLFWvidmode* modes = glfwGetVideoModes(monitors[index], &modeCount);
    for (int i = 0; i < modeCount; ++i) {
        const std::pair<int, int> size{ modes[i].width, modes[i].height };
        if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) sizes.push_back(size);
    }
#endif
    return sizes;
}

} // namespace

void TakeFrameInput(FrameInput& in)
{
    in.FrameTime = GetFrameTime();
    in.Fps = GetFPS();
    in.ScreenWidth = GetScreenWidth();
    in.ScreenHeight = GetScreenHeight();
    in.RenderWidth = GetRenderWidth();
    in.RenderHeight = GetRenderHeight();
    in.Focused = IsWindowFocused();
#ifdef IYI_PLATFORM_ANDROID
    in.Focused = in.Focused && AndroidResumed();   // the frame run after a pause must read as unfocused
#endif
    in.Minimized = IsWindowMinimized();
    in.Fullscreen = IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
#if defined(IYI_MOBILE)
    in.Monitor = 0;
    in.MonitorResolutions = MonitorResolutions(0);
#ifdef IYI_PLATFORM_ANDROID
    in.MonitorRefreshRate = AndroidRefreshRate();
#else
    in.MonitorRefreshRate = 0;
#endif
#elif !defined(__EMSCRIPTEN__)   // a browser has no monitor list or refresh rate (raylib warns every call)
    const int monitor = GetCurrentMonitor();
    if (monitor != in.Monitor || in.MonitorResolutions.empty()) in.MonitorResolutions = MonitorResolutions(monitor);
    in.Monitor = monitor;
    in.MonitorRefreshRate = GetMonitorRefreshRate(monitor);
#endif

    for (int k = 0; k < static_cast<int>(in.KeyDown.size()); ++k) {
        in.KeyDown[k] = IsKeyDown(k);
        in.KeyPressed[k] = IsKeyPressed(k);
        in.KeyReleased[k] = IsKeyReleased(k);
        in.KeyRepeat[k] = IsKeyPressedRepeat(k);
    }
#ifdef IYI_PLATFORM_ANDROID
    in.KeyDown[KEY_ESCAPE] = in.KeyDown[KEY_ESCAPE] || in.KeyDown[KEY_BACK];
    in.KeyPressed[KEY_ESCAPE] = in.KeyPressed[KEY_ESCAPE] || in.KeyPressed[KEY_BACK];
    in.KeyReleased[KEY_ESCAPE] = in.KeyReleased[KEY_ESCAPE] || in.KeyReleased[KEY_BACK];
    in.KeyRepeat[KEY_ESCAPE] = in.KeyRepeat[KEY_ESCAPE] || in.KeyRepeat[KEY_BACK];
    in.KeyDown[KEY_BACK] = in.KeyPressed[KEY_BACK] = in.KeyReleased[KEY_BACK] = in.KeyRepeat[KEY_BACK] = false;
    // The volume keys belong to the system (raylib still records them): never a game key press.
    for (const int key : { KEY_VOLUME_UP, KEY_VOLUME_DOWN })
        in.KeyDown[key] = in.KeyPressed[key] = in.KeyReleased[key] = in.KeyRepeat[key] = false;
#endif
    in.Keys.clear();
    for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed()) {
#ifdef IYI_PLATFORM_ANDROID
        if (key == KEY_BACK) key = KEY_ESCAPE;
        if (key == KEY_VOLUME_UP || key == KEY_VOLUME_DOWN) continue;
#endif
        in.Keys.push_back(key);
    }
    in.Chars.clear();
    for (int c = GetCharPressed(); c != 0; c = GetCharPressed()) in.Chars.push_back(c);
#ifdef IYI_PLATFORM_ANDROID
    AndroidTakeText(in.Chars);
    // The soft keyboard's Backspace / Enter and the back gesture arrive in the text queue as
    // '\b' / '\n' / ESC (IYIActivity).
    // shortcut: two of the same in one frame count once; fine at human typing speed.
    in.Chars.erase(std::remove_if(in.Chars.begin(), in.Chars.end(), [&](int c) {
        const int key = c == '\b' ? KEY_BACKSPACE : c == '\n' ? KEY_ENTER : c == 27 ? KEY_ESCAPE : 0;
        if (key) { in.KeyPressed[key] = true; in.Keys.push_back(key); }
        return key != 0;
    }), in.Chars.end());
#endif

    in.Mouse = GetMousePosition();
    in.MouseDelta = GetMouseDelta();
    in.Wheel = GetMouseWheelMoveV();
    for (int b = 0; b <= MOUSE_BUTTON_BACK; ++b) {
        in.MouseDown[b] = IsMouseButtonDown(b);
        in.MousePressed[b] = IsMouseButtonPressed(b);
        in.MouseReleased[b] = IsMouseButtonReleased(b);
    }
#ifdef IYI_PLATFORM_ANDROID
    static bool tapRelease = false;
    static Vector2 tapPosition{};
    float tapX = 0, tapY = 0;
    const bool tap = AndroidTakeTap(tapX, tapY, !tapRelease && !in.MousePressed[MOUSE_BUTTON_LEFT] &&
                                    !in.MouseDown[MOUSE_BUTTON_LEFT]);
    if (tapRelease) {
        in.Mouse = tapPosition;
        in.MouseDown[MOUSE_BUTTON_LEFT] = false;
        in.MouseReleased[MOUSE_BUTTON_LEFT] = true;
        tapRelease = false;
    } else if (tap && !in.MousePressed[MOUSE_BUTTON_LEFT] && !in.MouseDown[MOUSE_BUTTON_LEFT]) {
        tapPosition = { tapX, tapY };
        in.Mouse = tapPosition;
        in.MouseDelta = {};
        in.MouseDown[MOUSE_BUTTON_LEFT] = in.MousePressed[MOUSE_BUTTON_LEFT] = true;
        tapRelease = true;
    }
#endif
#ifdef IYI_MOBILE
    constexpr double kLongPressSeconds = 0.5;
    static double touchStarted = -1.0;
    if (in.MousePressed[MOUSE_BUTTON_LEFT]) touchStarted = GetTime();
    in.LongTouchRelease = in.MouseReleased[MOUSE_BUTTON_LEFT] && touchStarted >= 0.0 &&
                          GetTime() - touchStarted > kLongPressSeconds;
    if (in.MouseReleased[MOUSE_BUTTON_LEFT]) touchStarted = -1.0;
    // Two fingers: the mouse sits at their midpoint (a two-finger drag pans) and Pinch is the zoom.
    // Until every finger lifts, the delta stays 0 (the remaining finger would jump the mouse) and
    // the release lands nowhere, so a pinch never clicks.
    static float pinchDistance = 0.0f;
    static Vector2 pinchMid{};
    static bool pinched = false;
    in.Pinch = 1.0f;
    if (GetTouchPointCount() >= 2) {
        const Vector2 a = GetTouchPosition(0), b = GetTouchPosition(1);
        const Vector2 mid = { (a.x + b.x) / 2.0f, (a.y + b.y) / 2.0f };
        const float distance = std::hypot(a.x - b.x, a.y - b.y);
        if (pinched && pinchDistance > 0.0f && distance > 0.0f) {
            in.Pinch = distance / pinchDistance;
            in.MouseDelta = { mid.x - pinchMid.x, mid.y - pinchMid.y };
        } else in.MouseDelta = {};
        pinchDistance = distance; pinchMid = mid; pinched = true;
        in.Mouse = mid;
    } else if (pinched) {
        pinchDistance = 0.0f;
        in.MouseDelta = {};
        if (in.MouseReleased.any()) in.Mouse = { -1.0f, -1.0f };
        if (GetTouchPointCount() == 0) pinched = false;
    }
    // A released touch retains its last mouse position in raylib; keep it only for the release frame.
    if (GetTouchPointCount() == 0 && in.MouseReleased.none() && in.MousePressed.none()) in.Mouse = { -1.0f, -1.0f };
#endif
    for (int g = 0; g < 4; ++g) {
        GamepadInput& pad = in.Gamepads[g];
        pad.Available = IsGamepadAvailable(g);
        if (!pad.Available) continue;
        for (int b = 0; b < static_cast<int>(pad.Down.size()); ++b) {
            pad.Down[b] = IsGamepadButtonDown(g, b);
            pad.Pressed[b] = IsGamepadButtonPressed(g, b);
        }
        for (int a = 0; a < 8; ++a) pad.Axes[a] = GetGamepadAxisMovement(g, a);
    }

#ifdef __APPLE__
    const bool pasteMod = in.KeyDown[KEY_LEFT_SUPER] || in.KeyDown[KEY_RIGHT_SUPER];
#else
    const bool pasteMod = in.KeyDown[KEY_LEFT_CONTROL] || in.KeyDown[KEY_RIGHT_CONTROL];
#endif
    const char* clip = pasteMod && in.KeyPressed[KEY_V] ? GetClipboardText() : nullptr;
    in.Clipboard = clip ? clip : "";

    in.Ime = ImeTakeFrame();

    const AudioManager& audio = AudioManager::Get();
    in.AudioPlaying = audio.PlayingHandles();
    in.Music = audio.CurrentMusic();
    in.BusVolumes = audio.BusVolumes();
}

void SetGameInput(FrameInput* input)
{
    s_game = input ? input : &s_none;
}

FrameInput& GameInput()
{
    return *s_game;
}

void ImeRequest(const void* token, float x, float y, float height, float width, float screenHeight)
{
    RecordSetup([=]() { ImeRequestNow(token, x, y, height, width, screenHeight); });
}

void ImeCancelOwner(const void* token)
{
    if (!token) return;
    // This frame's text stops reaching the field at once (game side), the window follows (main thread).
    ImeFrame& ime = GameInput().Ime;
    if (ime.CommitOwner == token || (ime.Owner == token && ime.KeyboardCaptured)) ime.Discard = true;
    RecordSetup([token]() { ImeCancelOwnerNow(token); });
}

bool ImeAcceptText(const void* token)
{
    const ImeFrame& ime = GameInput().Ime;
    return !ime.Discard && (!ime.CommitOwner || ime.CommitOwner == token);
}

int PopInput(std::vector<int>& queue)
{
    if (queue.empty()) return 0;
    const int value = queue.front();
    queue.erase(queue.begin());
    return value;
}

} // namespace IYI
