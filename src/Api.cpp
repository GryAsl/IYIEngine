// Implements IYI::Api's draw, input, window, camera, asset and scene calls (Api.h): the one
// implementation behind both a native game's C++ and the Python bindings (RaylibBindings.cpp,
// EngineBindings.cpp). Every raylib call inside is written ::Name so it can never resolve to the
// Api function of the same name (which would record itself again instead of drawing).
//
// Connected to:
// - FramePacket.h (Record, RecordSetup) and FrameInput.h (GameInput): the threading boundary (SPEC §8a).
// - Application (camera, texture mode, window requests, scenes) and the AssetManager (Load*).

#include "IYI/Api.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include "raymath.h"
#include "rlgl.h"
#include "IYI/Application.h"
#include "IYI/FrameInput.h"
#include "IYI/FramePacket.h"
#include "IYI/Log.h"
// raylib exposes only the monitor's current mode, never its work area (SetWindowSize recentres in it):
// the GLFW raylib already bundles and links on desktop. No new dependency.
#ifndef IYI_MOBILE
#include <GLFW/glfw3.h>
#endif

namespace IYI::Api {

// Draw

void ClearBackground(Color color) { Record([=]() { ::ClearBackground(color); }); }
void BeginShaderMode(const Shader& shader) { Record([s = &shader]() { ::BeginShaderMode(*s); }); }
void EndShaderMode() { Record([]() { ::EndShaderMode(); }); }

// raylib applies a uniform immediately but draws in batches, so already-queued vertices would
// render with the new value; both setters flush first. A flush also drops raylib's extra
// sampler slots, so set textures last: inside BeginShaderMode, after every SetShaderValue.
void SetShaderValue(const Shader& shader, const std::string& name, const std::vector<float>& value)
{
    static const int kTypes[] = { SHADER_UNIFORM_FLOAT, SHADER_UNIFORM_VEC2, SHADER_UNIFORM_VEC3, SHADER_UNIFORM_VEC4 };
    if (value.empty() || value.size() > 4) return;
    Record([s = &shader, name, value]() {        // the location is looked up where the GL context is
        const int loc = ::GetShaderLocation(*s, name.c_str());
        if (loc < 0) return;
        rlDrawRenderBatchActive();
        ::SetShaderValue(*s, loc, value.data(), kTypes[value.size() - 1]);
    });
}

void SetShaderTexture(const Shader& shader, const std::string& name, const Texture2D& texture)
{
    Record([s = &shader, name, t = &texture]() {
        const int loc = ::GetShaderLocation(*s, name.c_str());
        if (loc < 0) return;
        rlDrawRenderBatchActive();
        ::SetShaderValueTexture(*s, loc, *t);
    });
}

void DrawTexture(const Texture2D& texture, int x, int y, Color tint) { Record([=, t = &texture]() { ::DrawTexture(*t, x, y, tint); }); }
void DrawTextureV(const Texture2D& texture, Vector2 position, Color tint) { Record([=, t = &texture]() { ::DrawTextureV(*t, position, tint); }); }
void DrawTextureRec(const Texture2D& texture, Rectangle source, Vector2 position, Color tint)
{
    Record([=, t = &texture]() { ::DrawTextureRec(*t, source, position, tint); });
}
void DrawTexturePro(const Texture2D& texture, Rectangle source, Rectangle dest, Vector2 origin, float rotation, Color tint)
{
    Record([=, t = &texture]() { ::DrawTexturePro(*t, source, dest, origin, rotation, tint); });
}
void DrawRectangle(int x, int y, int width, int height, Color color) { Record([=]() { ::DrawRectangle(x, y, width, height, color); }); }
void DrawRectangleRec(Rectangle rec, Color color) { Record([=]() { ::DrawRectangleRec(rec, color); }); }
void DrawRectangleLines(int x, int y, int width, int height, Color color) { Record([=]() { ::DrawRectangleLines(x, y, width, height, color); }); }
void DrawRectangleLinesEx(Rectangle rec, float thickness, Color color) { Record([=]() { ::DrawRectangleLinesEx(rec, thickness, color); }); }
void DrawRectanglePro(Rectangle rec, Vector2 origin, float rotation, Color color) { Record([=]() { ::DrawRectanglePro(rec, origin, rotation, color); }); }
// raylib 6.x computes the segment count only for segments < 1 (5.5: < 4).
void DrawRectangleRounded(Rectangle rec, float roundness, int segments, Color color)
{
    Record([=]() { ::DrawRectangleRounded(rec, roundness, segments, color); });
}
void DrawRectangleGradientV(Rectangle rec, Color top, Color bottom)
{
    Record([=]() { ::DrawRectangleGradientV((int)rec.x, (int)rec.y, (int)rec.width, (int)rec.height, top, bottom); });
}
void DrawRectangleGradientH(Rectangle rec, Color left, Color right)
{
    Record([=]() { ::DrawRectangleGradientH((int)rec.x, (int)rec.y, (int)rec.width, (int)rec.height, left, right); });
}
void DrawRectangleGradientEx(Rectangle rec, Color topLeft, Color bottomLeft, Color bottomRight, Color topRight)
{
    // raylib 5.5 names these topLeft, bottomLeft, topRight, bottomRight but draws them counter-clockwise too.
    Record([=]() { ::DrawRectangleGradientEx(rec, topLeft, bottomLeft, bottomRight, topRight); });
}
void DrawCircleGradient(Vector2 center, float radius, Color inner, Color outer)
{
#if RAYLIB_VERSION_MAJOR >= 6
    Record([=]() { ::DrawCircleGradient(center, radius, inner, outer); });
#else
    Record([=]() { ::DrawCircleGradient((int)center.x, (int)center.y, radius, inner, outer); });
#endif
}
void DrawCircle(int centerX, int centerY, float radius, Color color) { Record([=]() { ::DrawCircle(centerX, centerY, radius, color); }); }
void DrawCircleV(Vector2 center, float radius, Color color) { Record([=]() { ::DrawCircleV(center, radius, color); }); }
void DrawCircleLines(int centerX, int centerY, float radius, Color color) { Record([=]() { ::DrawCircleLines(centerX, centerY, radius, color); }); }
void DrawCircleSector(Vector2 center, float radius, float startAngle, float endAngle, int segments, Color color)
{
    Record([=]() { ::DrawCircleSector(center, radius, startAngle, endAngle, segments, color); });
}
void DrawRing(Vector2 center, float innerRadius, float outerRadius, float startAngle, float endAngle, int segments, Color color)
{
    Record([=]() { ::DrawRing(center, innerRadius, outerRadius, startAngle, endAngle, segments, color); });
}
void DrawSphere(Vector3 center, float radius, Color color) { Record([=]() { ::DrawSphere(center, radius, color); }); }

void DrawBillboards(const Texture2D& texture, const Camera3D& camera, std::vector<Vector3> centers, Vector2 size, Color tint)
{
    if (centers.empty()) return;
    // The world camera uses z up. Derive its screen axes once for all icons, so a dense
    // set of markers costs one textured quad stream instead of one raylib call per marker.
    const Matrix view = MatrixLookAt(camera.position, camera.target, camera.up);
    const Vector3 right{ view.m0 * size.x * 0.5f, view.m4 * size.x * 0.5f, view.m8 * size.x * 0.5f };
    const Vector3 up{ view.m1 * size.y * 0.5f, view.m5 * size.y * 0.5f, view.m9 * size.y * 0.5f };
    Record([t = &texture, centers = std::move(centers), tint, right, up]() {
        if (t->id == 0) return;
        rlSetTexture(t->id);
        rlBegin(RL_QUADS);
        rlColor4ub(tint.r, tint.g, tint.b, tint.a);
        for (const Vector3& p : centers) {
            rlTexCoord2f(0.0f, 0.0f); rlVertex3f(p.x - right.x + up.x, p.y - right.y + up.y, p.z - right.z + up.z);
            rlTexCoord2f(0.0f, 1.0f); rlVertex3f(p.x - right.x - up.x, p.y - right.y - up.y, p.z - right.z - up.z);
            rlTexCoord2f(1.0f, 1.0f); rlVertex3f(p.x + right.x - up.x, p.y + right.y - up.y, p.z + right.z - up.z);
            rlTexCoord2f(1.0f, 0.0f); rlVertex3f(p.x + right.x + up.x, p.y + right.y + up.y, p.z + right.z + up.z);
        }
        rlEnd();
        rlSetTexture(0);
    });
}

void DrawLine(int startX, int startY, int endX, int endY, Color color) { Record([=]() { ::DrawLine(startX, startY, endX, endY, color); }); }
void DrawLineEx(Vector2 start, Vector2 end, float thickness, Color color) { Record([=]() { ::DrawLineEx(start, end, thickness, color); }); }
void DrawLineStrip(std::vector<Vector2> points, Color color)
{
    Record([p = std::move(points), color]() { ::DrawLineStrip(p.data(), static_cast<int>(p.size()), color); });
}
void DrawSplineLinear(std::vector<Vector2> points, float thick, Color color)
{
    Record([p = std::move(points), thick, color]() { ::DrawSplineLinear(p.data(), static_cast<int>(p.size()), thick, color); });
}
void DrawTriangle(Vector2 v1, Vector2 v2, Vector2 v3, Color color) { Record([=]() { ::DrawTriangle(v1, v2, v3, color); }); }
void DrawTriangleFan(std::vector<Vector2> points, Color color)
{
    Record([p = std::move(points), color]() { ::DrawTriangleFan(p.data(), static_cast<int>(p.size()), color); });
}
void DrawTriangleStrip(std::vector<Vector2> points, Color color)
{
    Record([p = std::move(points), color]() { ::DrawTriangleStrip(p.data(), static_cast<int>(p.size()), color); });
}
void DrawPoly(Vector2 center, int sides, float radius, float rotation, Color color) { Record([=]() { ::DrawPoly(center, sides, radius, rotation, color); }); }

void DrawText(const std::string& text, int x, int y, int fontSize, Color color)
{
    Record([=]() {
        BeginTextDraw();
        ::DrawText(text.c_str(), x, y, fontSize, color);
        EndTextDraw();
    });
}

void DrawTextEx(const Font& font, const std::string& text, Vector2 position, float fontSize, float spacing, Color color)
{
    const Font* drawn = CurrentApp().GetAssets().FontForText(font, text);   // may add glyphs: game side
    Record([drawn, text, position, fontSize, spacing, color]() {
        BeginTextDraw();
        ::DrawTextEx(*drawn, text.c_str(), position, fontSize, spacing, color);
        EndTextDraw();
    });
}

// The same font view DrawTextEx and UI text use, so the size matches what they draw.
Vector2 MeasureTextEx(const Font& font, const std::string& text, float fontSize, float spacing)
{
    return ::MeasureTextEx(*CurrentApp().GetAssets().FontForText(font, text), text.c_str(), fontSize, spacing);
}

void BeginMode2D(Camera2D camera) { Record([camera]() { ::BeginMode2D(camera); }); }
void EndMode2D() { Record([]() { ::EndMode2D(); }); }
bool BeginTextureMode(const RenderTexture2D& target) { return CurrentApp().BeginUserTextureMode(target); }
void EndTextureMode() { CurrentApp().EndUserTextureMode(); }
void SetBackgroundColor(Color color) { CurrentApp().SetBackgroundColor(color); }
void SetPostprocessShader(Shader* shader, float strength, float radialWarp, bool worldOnly, bool textMask)
{
    CurrentApp().SetPostprocessShader(shader, strength, radialWarp, worldOnly, textMask);
}
void SetSpriteStackCulling(bool enabled) { Record([enabled]() { IYI::SetSpriteStackCulling(enabled); }); }
void SetSpriteStackInstancing(bool enabled) { Record([enabled]() { IYI::SetSpriteStackInstancing(enabled); }); }

// Input, answered from this frame's copy (FrameInput.h), never raylib's live state. IME
// conversion/commit/cancel keys belong to the IME, including its completed-poll frame.

bool IsKeyDown(int key) { return !GameInput().Ime.KeyboardCaptured && InputBit(GameInput().KeyDown, key); }
bool IsKeyUp(int key) { return !GameInput().Ime.KeyboardCaptured && !InputBit(GameInput().KeyDown, key); }
bool IsKeyPressed(int key) { return !GameInput().Ime.KeyboardCaptured && InputBit(GameInput().KeyPressed, key); }
bool IsKeyReleased(int key) { return !GameInput().Ime.KeyboardCaptured && InputBit(GameInput().KeyReleased, key); }
int GetKeyPressed()
{
    if (GameInput().Ime.KeyboardCaptured) GameInput().Keys.clear();
    return PopInput(GameInput().Keys);
}
bool IsImeComposing() { return GameInput().Ime.Composing; }
bool IsMouseButtonDown(int button) { return InputBit(GameInput().MouseDown, button); }
bool IsMouseButtonPressed(int button) { return InputBit(GameInput().MousePressed, button); }
bool IsMouseButtonReleased(int button) {
#ifdef IYI_MOBILE
    if (button == MOUSE_BUTTON_LEFT && GameInput().LongTouchRelease) return false;
#endif
    return InputBit(GameInput().MouseReleased, button);
}
Vector2 GetMousePosition() { return GameInput().Mouse; }
Vector2 GetMouseDelta() { return GameInput().MouseDelta; }
float GetMouseWheelMove()
{
    const Vector2 w = GameInput().Wheel;
    return std::fabs(w.x) > std::fabs(w.y) ? w.x : w.y;
}
Vector2 GetMouseWheelMoveV() { return GameInput().Wheel; }
float GetPinchZoom() { return GameInput().Pinch; }

static const GamepadInput* Pad(int gamepad)
{
    return gamepad >= 0 && gamepad < 4 && GameInput().Gamepads[gamepad].Available ? &GameInput().Gamepads[gamepad] : nullptr;
}
bool IsGamepadAvailable(int gamepad) { return Pad(gamepad) != nullptr; }
bool IsGamepadButtonDown(int gamepad, int button) { return Pad(gamepad) && InputBit(Pad(gamepad)->Down, button); }
bool IsGamepadButtonPressed(int gamepad, int button) { return Pad(gamepad) && InputBit(Pad(gamepad)->Pressed, button); }
float GetGamepadAxisMovement(int gamepad, int axis) { return Pad(gamepad) && axis >= 0 && axis < 8 ? Pad(gamepad)->Axes[axis] : 0.0f; }

// Window: the window's state as of this frame (FrameInput.h); changes are platform requests the render
// side runs before it draws this frame, so they show in the next frame's values.

float GetFrameTime() { return GameInput().FrameTime; }
int GetFPS() { return GameInput().Fps; }
int GetScreenWidth() { return GameInput().ScreenWidth; }
int GetScreenHeight() { return GameInput().ScreenHeight; }
void SetClipboardText(const std::string& text) { RecordSetup([text]() { ::SetClipboardText(text.c_str()); }); }
std::string GetClipboardText() { return GameInput().Clipboard; }

// Recentres the window in its monitor's work area (the desktop minus the taskbar), so a
// window shrunk to fit never keeps its title bar off-screen.
void SetWindowSize(int width, int height)
{
    if (CurrentApp().WindowSizePinned()) return;   // --window-size: a visual test keeps its size
    RecordSetup([width, height]() {
#ifdef IYI_MOBILE
        CurrentApp().SetMobileSize(width, height);
#else
        ::SetWindowSize(width, height);
        int monitorCount = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
        int index = ::GetCurrentMonitor();
        if (::IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) || monitors == nullptr || index < 0 || index >= monitorCount) {
            return;
        }
        int areaX = 0, areaY = 0, areaW = 0, areaH = 0;
        glfwGetMonitorWorkarea(monitors[index], &areaX, &areaY, &areaW, &areaH);
        ::SetWindowPosition(areaX + std::max(0, (areaW - width) / 2), areaY + std::max(0, (areaH - height) / 2));
#endif
    });
}

void SetDisplayCutout(bool use)
{
#ifdef IYI_MOBILE
    RecordSetup([use]() { CurrentApp().SetDisplayCutout(use); });
#else
    (void)use;
#endif
}

std::vector<std::pair<int, int>> GetMonitorResolutions() { return GameInput().MonitorResolutions; }
int GetMonitorRefreshRate() { return GameInput().MonitorRefreshRate; }
void ToggleFullscreen() { RecordSetup([]() { CurrentApp().ToggleFullscreen(); }); }
bool IsWindowFullscreen() { return GameInput().Fullscreen; }
bool IsWindowFocused() { return GameInput().Focused; }
void SetVsync(int interval) { RecordSetup([interval]() { CurrentApp().SetVsync(interval); }); }
void SetTargetFPS(int fps) { RecordSetup([fps]() { CurrentApp().SetTargetFps(fps); }); }

// Source images are cached RGBA per path (tiny, freed at process exit); call again with the next
// frame to animate.
void SetWindowTitle(const std::string& title)
{
    RecordSetup([title]() { ::SetWindowTitle(title.c_str()); });
}

bool SetWindowIcon(const std::string& relPath, int frame, int frameCount)
{
#ifdef IYI_MOBILE
    return false;   // no window icon on phones (the game animates it every few frames: no log spam)
#endif
    static std::unordered_map<std::string, Image> s_iconImages;
    auto it = s_iconImages.find(relPath);
    if (it == s_iconImages.end()) {
        ResourceStore& store = CurrentApp().GetAssets().GetResourceStore();
        std::vector<unsigned char> bytes;
        if (!store.Exists(relPath) || !store.ReadBytes(relPath, bytes) || bytes.empty()) {
            LogWrite(LogLevel::Warn, "asset", "Window icon not found: " + relPath);
            return false;
        }
        std::string ext = std::filesystem::path(relPath).extension().string();
        Image img = ::LoadImageFromMemory(ext.c_str(), bytes.data(), static_cast<int>(bytes.size()));
        if (!::IsImageValid(img)) {
            LogWrite(LogLevel::Warn, "asset", "Failed to load window icon: " + relPath);
            return false;
        }
        ::ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        it = s_iconImages.emplace(relPath, img).first;
    }
    const Image& strip = it->second;
    int count = frameCount > 0 ? frameCount : 1;
    int frameWidth = strip.width / count;
    if (frameWidth <= 0) {
        return false;
    }
    int index = ((frame % count) + count) % count;
    Image icon = ::ImageFromImage(strip, Rectangle{ static_cast<float>(index * frameWidth), 0.0f,
                                                    static_cast<float>(frameWidth), static_cast<float>(strip.height) });
    RecordSetup([icon]() {
#ifndef __EMSCRIPTEN__   // a browser tab has no window icon (raylib warns on every call)
        ::SetWindowIcon(icon);  // GLFW copies the pixels
#endif
        ::UnloadImage(icon);
    });
    return true;
}

void Quit() { CurrentApp().RequestQuit(); }
void SetCloseIntercept(bool enabled) { CurrentApp().SetCloseIntercept(enabled); }
bool TakeCloseRequest() { return CurrentApp().TakeCloseRequest(); }
void TakeScreenshot(const std::string& path) { CurrentApp().RequestScreenshot(path); }

// Camera

void SetCamera(const Camera2D& camera, float verticalScale) { CurrentApp().SetCamera(camera, verticalScale); }
void SetCamera3D(const Camera3D& camera) { CurrentApp().SetCamera3D(camera); }
void ClearCamera() { CurrentApp().ClearCamera(); }
// verticalScale must match the one given to SetCamera, or points land off a tilted view.
Vector2 GetWorldToScreen2D(Vector2 position, Camera2D camera, float verticalScale)
{
    const Vector3 p = Vector3Transform(Vector3{ position.x, position.y, 0.0f }, CameraMatrix2D(camera, verticalScale));
    return Vector2{ p.x, p.y };
}
Vector2 GetScreenToWorld2D(Vector2 position, Camera2D camera, float verticalScale)
{
    const Vector3 p = Vector3Transform(Vector3{ position.x, position.y, 0.0f }, MatrixInvert(CameraMatrix2D(camera, verticalScale)));
    return Vector2{ p.x, p.y };
}
Ray GetScreenToWorldRay(Vector2 position, const Camera3D& camera)
{
    const Ray ray = ::GetScreenToWorldRayEx(position, WorldCameraToGl(camera), GameInput().ScreenWidth, GameInput().ScreenHeight);
    return Ray{ Vector3{ ray.position.x, ray.position.y, -ray.position.z }, Vector3{ ray.direction.x, ray.direction.y, -ray.direction.z } };
}
std::optional<Vector2> GetWorldToScreen(Vector3 position, const Camera3D& camera)
{
    Vector2 screen{};
    if (!WorldToScreen3D(camera, position, screen)) return std::nullopt;
    return screen;
}

// Assets

Texture2D* LoadTexture(const std::string& relPath) { return CurrentApp().GetAssets().GetTexture(relPath); }
SpriteStack* LoadSpriteStack(const std::vector<std::string>& layerPaths) { return CurrentApp().GetAssets().GetSpriteStack(layerPaths); }
RenderTexture2D* LoadRenderTexture(const std::string& name, int width, int height) { return CurrentApp().GetAssets().GetRenderTexture(name, width, height); }
Font* LoadFont(const std::string& relPath, int fontSize, const std::string& glyphText, bool pixel) { return CurrentApp().GetAssets().GetFont(relPath, fontSize, glyphText, pixel); }
bool AddPixelFontSize(const Font& font, const std::string& relPath, int fontSize) { return CurrentApp().GetAssets().AddPixelFontSize(font, relPath, fontSize); }
Shader* LoadShader(const std::string& vsRelPath, const std::string& fsRelPath) { return CurrentApp().GetAssets().GetShader(vsRelPath, fsRelPath); }
std::string ProjectPath(const std::string& relPath) { return CurrentApp().ResolveProjectPath(relPath); }
std::string RunPath(const std::string& relPath)
{
    return (std::filesystem::path(CurrentApp().RunDir()) / relPath).lexically_normal().generic_string();
}

std::string UserDataPath(const std::string& subdir)
{
    namespace fs = std::filesystem;
    // Single source of truth for the per-user writable root (saves, config, logs).
    fs::path fullPath = fs::path(CurrentApp().GetConfig().UserDataRoot());
    if (!subdir.empty()) {
        fullPath /= subdir;
    }
    fullPath = fullPath.lexically_normal();
    std::error_code ec;
    fs::create_directories(fullPath, ec);
    if (ec) {
        LogWrite(LogLevel::Warn, "engine", "Failed to create user data directory '" + fullPath.generic_string() + "': " + ec.message());
    }
    return fullPath.generic_string();
}

bool IsAgentRun() { return CurrentApp().IsAgentRun(); }
bool RunSeed(long long& seed)
{
    seed = CurrentApp().RunSeed();
    return CurrentApp().HasRunSeed();
}

// Scenes

void ChangeScene(const std::string& scene) { CurrentApp().RequestSceneChange(scene); }

} // namespace IYI::Api
