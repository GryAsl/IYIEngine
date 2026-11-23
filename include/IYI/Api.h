// Declares IYI::Api, the engine.* functions in C++: what a native game (SPEC §2c) calls, and what the
// Python bindings call too, so each has one implementation. Names and arguments are raylib's where
// raylib has the call (Api::DrawRectangle, Api::IsKeyPressed); the rest follow the Python name
// (engine.load_texture -> Api::LoadTexture).
//
// Why a separate namespace: on the game thread a draw, window or audio call must be recorded for the
// render side and input must come from the frame's copy (SPEC §8a, rules 29 and 30). Api::DrawRectangle
// records raylib's DrawRectangle; calling raylib's own from game code would touch GL on the wrong thread.
// Always qualify (Api::X): an unqualified X may resolve to raylib's. Pure raylib helpers that touch no
// window, GL, input or audio (raymath, collision, MeasureText, GetTime, GetRandomValue, file and text
// helpers) are called directly, as are the engine's plain C++ calls (ProfileBegin/ProfileScope/
// ProfileCount, LogWrite, CurrentApp().GetAssets()).
//
// Textures, fonts and shaders are passed by reference to the AssetManager's cached objects (what the
// Load* functions return): the recording keeps their address until the render side draws, so never
// pass a local copy.
//
// Connected to:
// - Api.cpp (this header's draw, input, window, camera, asset and scene calls).
// - RaylibBindings.cpp, EngineBindings.cpp: their m.def lambdas call these.
// - Subsystem headers add their own Api functions (SteamBridge.h, AudioManager.h, ...); this header
//   includes them, so a game includes only IYI/Api.h and IYI/Game.h.

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "raylib.h"
#include "IYI/Particles.h"
#include "IYI/SpriteStack.h"
#include "IYI/StackLighting.h"
#include "IYI/SteamBridge.h"
#include "IYI/PlayBridge.h"

namespace IYI::Api {

// Draw (recorded into the current pass: offscreen from Update, world/HUD from Drawing)
void ClearBackground(Color color);
void BeginShaderMode(const Shader& shader);
void EndShaderMode();
// value: 1 to 4 floats (float .. vec4); others are ignored. Both setters flush raylib's batch first.
void SetShaderValue(const Shader& shader, const std::string& name, const std::vector<float>& value);
void SetShaderTexture(const Shader& shader, const std::string& name, const Texture2D& texture);
void DrawTexture(const Texture2D& texture, int x, int y, Color tint = WHITE);
void DrawTextureV(const Texture2D& texture, Vector2 position, Color tint = WHITE);
void DrawTextureRec(const Texture2D& texture, Rectangle source, Vector2 position, Color tint = WHITE);
void DrawTexturePro(const Texture2D& texture, Rectangle source, Rectangle dest, Vector2 origin, float rotation, Color tint = WHITE);
void DrawRectangle(int x, int y, int width, int height, Color color);
void DrawRectangleRec(Rectangle rec, Color color);
void DrawRectangleLines(int x, int y, int width, int height, Color color);
void DrawRectangleLinesEx(Rectangle rec, float thickness, Color color);
void DrawRectanglePro(Rectangle rec, Vector2 origin, float rotation, Color color);
void DrawRectangleRounded(Rectangle rec, float roundness, int segments, Color color);
void DrawRectangleGradientV(Rectangle rec, Color top, Color bottom);
void DrawRectangleGradientH(Rectangle rec, Color left, Color right);
// Corner colours counter-clockwise from the top left.
void DrawRectangleGradientEx(Rectangle rec, Color topLeft, Color bottomLeft, Color bottomRight, Color topRight);
void DrawCircleGradient(Vector2 center, float radius, Color inner, Color outer);
void DrawCircle(int centerX, int centerY, float radius, Color color);
void DrawCircleV(Vector2 center, float radius, Color color);
void DrawCircleLines(int centerX, int centerY, float radius, Color color);
void DrawCircleSector(Vector2 center, float radius, float startAngle, float endAngle, int segments, Color color);
void DrawRing(Vector2 center, float innerRadius, float outerRadius, float startAngle, float endAngle, int segments, Color color);
void DrawSphere(Vector3 center, float radius, Color color);
// Camera-facing quads of one texture at world points (z up) through that camera, one quad stream.
void DrawBillboards(const Texture2D& texture, const Camera3D& camera, std::vector<Vector3> centers, Vector2 size, Color tint = WHITE);
void DrawLine(int startX, int startY, int endX, int endY, Color color);
void DrawLineEx(Vector2 start, Vector2 end, float thickness, Color color);
void DrawLineStrip(std::vector<Vector2> points, Color color);
// A thick polyline with mitered joints; split it at turns sharper than ~120 degrees.
void DrawSplineLinear(std::vector<Vector2> points, float thick, Color color);
void DrawTriangle(Vector2 v1, Vector2 v2, Vector2 v3, Color color);   // counter-clockwise on screen
void DrawTriangleFan(std::vector<Vector2> points, Color color);
void DrawTriangleStrip(std::vector<Vector2> points, Color color);
void DrawPoly(Vector2 center, int sides, float radius, float rotation, Color color);
// World-space text, deliberately unclipped (rule 15 covers UI text only).
void DrawText(const std::string& text, int x, int y, int fontSize, Color color);
void DrawTextEx(const Font& font, const std::string& text, Vector2 position, float fontSize, float spacing, Color color);
Vector2 MeasureTextEx(const Font& font, const std::string& text, float fontSize, float spacing);   // DrawTextEx's size
// A 2D camera inside BeginTextureMode (Update), never in Drawing (the world pass has its camera).
void BeginMode2D(Camera2D camera);
void EndMode2D();
// Game-side offscreen drawing (rule 24): false (refused) during the draw pass.
bool BeginTextureMode(const RenderTexture2D& target);
void EndTextureMode();
void SetBackgroundColor(Color color);
void SetPostprocessShader(Shader* shader, float strength = 1.0f, float radialWarp = 0.0f, bool worldOnly = false, bool textMask = false);
void SetSpriteStackCulling(bool enabled);
void SetSpriteStackInstancing(bool enabled);

// Input (this frame's copy, FrameInput.h; keys the IME owns read as up)
bool IsKeyDown(int key);
bool IsKeyUp(int key);
bool IsKeyPressed(int key);
bool IsKeyReleased(int key);
int GetKeyPressed();
bool IsImeComposing();
bool IsMouseButtonDown(int button);
bool IsMouseButtonPressed(int button);
bool IsMouseButtonReleased(int button);
Vector2 GetMousePosition();
Vector2 GetMouseDelta();
float GetMouseWheelMove();   // raylib's rule: the larger axis
Vector2 GetMouseWheelMoveV(); // both axes: x is a touchpad's sideways scroll
float GetPinchZoom();   // two-finger zoom factor this frame, 1 when none; the mouse is the fingers' midpoint
bool IsGamepadAvailable(int gamepad);
bool IsGamepadButtonDown(int gamepad, int button);
bool IsGamepadButtonPressed(int gamepad, int button);
float GetGamepadAxisMovement(int gamepad, int axis);

// Window (state as of this frame; changes are requests that show in the next frame's values)
float GetFrameTime();
int GetFPS();
int GetScreenWidth();
int GetScreenHeight();
void SetClipboardText(const std::string& text);
std::string GetClipboardText();   // set on Ctrl/Cmd+V only
void SetWindowSize(int width, int height);   // and recentres it in its monitor's work area
void SetDisplayCutout(bool use);
void SetWindowTitle(const std::string& title);
std::vector<std::pair<int, int>> GetMonitorResolutions();   // distinct sizes, smallest first
int GetMonitorRefreshRate();
void ToggleFullscreen();
bool IsWindowFullscreen();
bool IsWindowFocused();
void SetVsync(int interval);   // 0 off, 1 every refresh, 2 every second refresh
void SetTargetFPS(int fps);
// Window/taskbar icon from a horizontal frame strip (frame of frameCount); false if it cannot load.
bool SetWindowIcon(const std::string& relPath, int frame = 0, int frameCount = 1);
void Quit();
void SetCloseIntercept(bool enabled);
bool TakeCloseRequest();
void TakeScreenshot(const std::string& path);

// Camera (world space; a 3D camera has z = height up, Application.h WorldCameraToGl)
void SetCamera(const Camera2D& camera, float verticalScale = 1.0f);   // verticalScale finite, > 0
void SetCamera3D(const Camera3D& camera);
void ClearCamera();
Vector2 GetWorldToScreen2D(Vector2 position, Camera2D camera, float verticalScale = 1.0f);
Vector2 GetScreenToWorld2D(Vector2 position, Camera2D camera, float verticalScale = 1.0f);
Ray GetScreenToWorldRay(Vector2 position, const Camera3D& camera);   // world space, z up
std::optional<Vector2> GetWorldToScreen(Vector3 position, const Camera3D& camera);   // nullopt behind it

// Assets (cached by the AssetManager for the whole run; nullptr when they fail to load)
Texture2D* LoadTexture(const std::string& relPath);
SpriteStack* LoadSpriteStack(const std::vector<std::string>& layerPaths);
RenderTexture2D* LoadRenderTexture(const std::string& name, int width, int height);
Font* LoadFont(const std::string& relPath, int fontSize = 32, const std::string& glyphText = "", bool pixel = false);
bool AddPixelFontSize(const Font& font, const std::string& relPath, int fontSize);
Shader* LoadShader(const std::string& vsRelPath, const std::string& fsRelPath);
std::string ProjectPath(const std::string& relPath = "");
std::string UserDataPath(const std::string& subdir = "");   // created if missing
std::string RunPath(const std::string& relPath = "");   // this run's own files (logs/, agentio/): --user-data, else the project; never shared by two isolated runs
bool IsAgentRun();                  // --agent-run: skip intros, tutorials and other human-only flows
bool RunSeed(long long& seed);      // --seed: false without it
constexpr bool IsShipping()
{
#ifdef IYI_SHIPPING_BUILD
    return true;
#else
    return false;
#endif
}

// Scenes: a registered scene name (native game) or a scene script path (Python), applied at the end of the step.
void ChangeScene(const std::string& scene);

} // namespace IYI::Api
