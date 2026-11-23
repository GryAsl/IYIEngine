// Declares the top-level engine application that drives the main loop and coordinates all subsystems.
// Acts as the root owner of ProjectConfig, AssetManager, PythonRuntime, and the active Scene.
// Instead of using singletons with lazy initialization or dependency injection frameworks, the engine
// creates a single Application instance on the stack and exposes it via CurrentApp() for bindings.
// It manages the fixed-timestep accumulators (30 Hz and 60 Hz), camera states, and deferred scene transitions.
// Each frame stage is a profiler section (Profiler.h); the dev-only agent channel (AgentChannel.h) and the
// --frames / --screenshot / --profile flags (scripted runs, benchmarks) live here too.
// Threads: the main thread owns the window, input polling and every GL call (the render side); a game
// thread runs Python and every hook (the game side), one game step per frame, pipelined one frame ahead
// of the render (FramePacket.h, FrameInput.h). --single-thread runs both on the main thread.
//
// Connected to:
// - Instantiated and run by main.cpp.
// - Called through Api.cpp (Python bindings and native games) for scenes, camera and window properties.
//
// Future evolution (per ROADMAP.md):
// May expand with window control bindings; an audio thread may take over the frame packet's Audio pass.
// Job systems, engine-drawn debug overlays (the profiler overlay is an opt-in Python library) and
// generic scheduling frameworks are explicitly rejected.

#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "raylib.h"
#include "IYI/AgentChannel.h"
#include "IYI/ProjectConfig.h"
#include "IYI/AssetManager.h"
#include "IYI/FrameInput.h"
#include "IYI/FramePacket.h"
#include "IYI/Game.h"
#include "IYI/PythonRuntime.h"
#include "IYI/Scene.h"

namespace IYI {

class Application;
Application& CurrentApp();

// raylib's 2D camera matrix with an extra screen-space vertical scale applied after rotation
// (a tilted, foreshortened view). verticalScale == 1 returns GetCameraMatrix2D unchanged.
Matrix CameraMatrix2D(const Camera2D& camera, float verticalScale);

// A world camera for set_camera_3d is given in world space with z = height up (x, y the ground, y
// down the screen as in the 2D view). That space is mirrored against raylib's, so the world pass
// draws through raylib's Camera3D of the same camera with z negated, and so do ray queries.
Camera3D WorldCameraToGl(const Camera3D& camera);
// A world point (z up) on the game's screen through such a camera; false behind the camera.
bool WorldToScreen3D(const Camera3D& camera, Vector3 world, Vector2& screen);

class Application
{
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // game: a native game (SPEC §2c); nullptr runs the project's Python entry script.
    bool Initialize(int argc, char* argv[], std::unique_ptr<Game> game = nullptr);
    void Run();
    bool Frame();
    void Shutdown();

    // A scene script path (Python) or a registered scene name (native game), applied at the end of the step.
    void RequestSceneChange(const std::string& scene);
    void RegisterScene(const std::string& name, std::function<void()> sceneStart) { m_nativeScenes[name] = std::move(sceneStart); }
    // The game's agent keys (AgentChannel): the native game's AgentRequest or the entry script's.
    std::string CallAgentRequest(const std::string& requestJson);
    void SetCamera(const Camera2D& camera, float verticalScale = 1.0f);
    // The world pass through a 3D camera (WorldCameraToGl) with depth testing; replaces a 2D camera.
    void SetCamera3D(const Camera3D& camera);
    // Where a world point lies on screen (window pixels) through the world camera: the 3D camera
    // (z up), else the 2D one (z ignored); false when it is behind a 3D camera.
    bool WorldToScreen(Vector3 world, Vector2& screen) const;
    void ClearCamera();
    void SetBackgroundColor(Color color);
    // worldOnly: the shader gets the world pass alone and the HUD is drawn after it, unchanged.
    // textMask: the HUD's text lowers texture0's alpha by its coverage (FramePacket.h SetTextMask).
    void SetPostprocessShader(Shader* shader, float strength, float radialWarp = 0.0f, bool worldOnly = false,
                              bool textMask = false);
    // The warp UI hit tests must undo (rule 20): none when the HUD is not drawn through the shader.
    float GetPostprocessRadialWarp() const
    {
        return m_postprocessShader && !m_postprocessWorldOnly ? m_postprocessRadialWarp : 0.0f;
    }
    std::string ResolveProjectPath(const std::string& relPath) const;
    // Window calls: main (render) thread only; the game side requests them (FramePacket.h RecordSetup).
    void ToggleFullscreen();
    void SetVsync(int interval);   // 0 off, 1 every refresh, 2 every second refresh
    void SetTargetFps(int fps) { m_config.TargetFPS = fps; }   // 0 unlimited; the frame loop applies it (VSync and background rate first)
#ifdef IYI_MOBILE
    void SetMobileSize(int width, int height);
    void SetDisplayCutout(bool use) { m_useCutout = use; }
    void UpdateMobileCanvas();
#endif
    void RequestQuit() { m_quitRequested = true; }
    // While on, the window's close button / Alt+F4 only raise a request the game takes with
    // TakeCloseRequest() and answers with RequestQuit(). A second close while one is still
    // untaken quits anyway, so a stuck script can never trap the window open.
    void SetCloseIntercept(bool enabled) { m_closeIntercept = enabled; }
    bool TakeCloseRequest() { bool requested = m_closeRequested; m_closeRequested = false; return requested; }
    // Saves the next finished frame (world + HUD, after postprocess) as an image at path.
    void RequestScreenshot(const std::string& path) { m_pendingScreenshot = path; }
    // Game-side offscreen drawing. Refused during the draw pass, where it would drop the camera
    // transform and the postprocess target; a mode left open is closed before the frame draws.
    bool BeginUserTextureMode(const RenderTexture2D& target);
    void EndUserTextureMode();

    Scene& GetScene() { return *m_currentScene; }
    int SceneChanges() const { return m_sceneChanges; }
    bool IsAgentRun() const { return m_agentRun; }
    bool WindowSizePinned() const { return m_pinnedWindow; }   // --window-size: the game cannot resize
    void PinWindowSize(int width, int height);   // what --window-size does, mid-run (agent "window_size")
    const std::string& RunDir() const { return m_runDir; }   // logs/ and agentio/ (Api::RunPath)
    bool HasRunSeed() const { return m_hasSeed; }
    long long RunSeed() const { return m_seed; }
    int FrameCount() const { return m_frameCount; }   // game steps run so far
    AssetManager& GetAssets() { return m_assets; }
    const ProjectConfig& GetConfig() const { return m_config; }

private:
    void RunFixedUpdates(float dt);
    bool ApplyPendingSceneChange();
    void RecordDrawPasses(FramePacket& packet);
    // The frame's three parts (Application.cpp Run) and the game side's start and end.
    bool StartGame();
    void StopGame();
    bool StartGameSide();
    void GameThread();
    void PlatformFrame(FrameInput& in);
    void TakeInput(FrameInput& in);
    void GameStep(FramePacket& packet);
    void RenderPacket(FramePacket& packet);
    void SingleThreadFrame();
    void EndRun();
    void NoteFrameForReadyMarker(bool sceneApplied);
    void HandleFullscreenToggle();
    void CountFrame();
    void LogProfile();

    ProjectConfig           m_config;
    AssetManager            m_assets;
    std::unique_ptr<Game>   m_game;                        // a native game; null runs Python
#ifdef IYI_WITH_PYTHON
    std::unique_ptr<PythonRuntime> m_python;               // null for a native game
#endif
    std::unordered_map<std::string, std::function<void()>> m_nativeScenes;   // RegisterScene
    std::unique_ptr<Scene>  m_currentScene;

    float                   m_acc60 = 0.0f;
    float                   m_acc30 = 0.0f;
    Color                   m_backgroundColor = RAYWHITE;
    Shader*                 m_postprocessShader = nullptr;  // AssetManager owns this shader.
    float                   m_postprocessStrength = 1.0f;
    float                   m_postprocessRadialWarp = 0.0f;
    bool                    m_postprocessWorldOnly = false;   // the HUD is drawn after the shader
    bool                    m_postprocessTextMask = false;    // HUD text marks the target's alpha
    Camera2D                m_camera = { {0.0f, 0.0f}, {0.0f, 0.0f}, 0.0f, 1.0f };
    bool                    m_cameraActive = false;
    float                   m_cameraVerticalScale = 1.0f;  // 1 = plain BeginMode2D
    Camera3D                m_camera3D{};                  // world space, z up (WorldCameraToGl)
    bool                    m_camera3DActive = false;
    std::string             m_pendingScenePath;
    int                     m_sceneChanges = 0;
    std::string             m_pendingScreenshot;           // "" = none requested
    bool                    m_hasPendingScene = false;
    bool                    m_hotReloadEnabled = true;
    bool                    m_initialized = false;
    bool                    m_quitRequested = false;
    bool                    m_closeIntercept = false;
    bool                    m_closeRequested = false;      // intercepted close not yet taken
    bool                    m_inDrawPass = false;          // between the frame's first and last draw call
    bool                    m_userTextureMode = false;     // begin_texture_mode is open
    bool                    m_startupOk = false;           // GameStart and every SceneStart so far succeeded
    bool                    m_firstSceneExpected = false;  // GameStart requested a scene
    bool                    m_firstSceneApplied = false;
    bool                    m_readyLogged = false;
    // Threads (Run): the main thread takes inputs and draws packets; the game step fills them, two
    // of each in turn. The frame mutex and condition guard the indices and flags below them.
    FrameInput              m_inputs[2];                   // input and window state per frame (FrameInput.h)
    FramePacket             m_packets[2];                  // a game step's recorded work (FramePacket.h)
    FramePacket             m_finalPacket;                 // GL deletions of the game side's shutdown
#ifdef __EMSCRIPTEN__
    bool                    m_singleThread = true;         // the browser build has no threads
#else
    bool                    m_singleThread = false;        // --single-thread
#endif
    int                     m_frameIndex = 0;              // single-thread frame counter (picks m_inputs/m_packets)
    bool                    m_gameStopped = false;
    bool                    m_runEnded = false;
    int                     m_appliedFps = 0;
    int                     m_presented = 0;               // packets drawn (main thread)
    long long               m_audioDone = 0;               // engine.audio calls the audio side ran (main thread)
    const RenderTexture2D*  m_userTarget = nullptr;        // the open begin_texture_mode's target
    std::thread             m_gameThread;
    std::mutex              m_frameMutex;
    std::condition_variable m_frameCv;
    int                     m_inputFrame = -1;             // the last frame whose input is handed over
    int                     m_packetFrame = -1;            // the last game step that finished its packet
    bool                    m_gameReady = false, m_gameOk = false, m_gameExited = false, m_stopGame = false;
    struct PostprocessState                                // render side: the postprocess target and uniforms
    {
        RenderTexture2D Target{};
        bool Depth = false;                                // Target's depth is a texture
        bool With3D = false;                               // Target was made for a 3D camera (it has depth)
        Shader* Uniforms = nullptr;                        // the shader the locations below belong to
        int TimeLoc = -1, ResolutionLoc = -1, StrengthLoc = -1, RadialWarpLoc = -1, TextMaskLoc = -1, DepthLoc = -1, ViewLoc = -1;
    };
    PostprocessState        m_post;
    RenderTexture2D         m_canvas{};                    // --trailer-shot: each frame at the recording size (Trailer.h)
#ifdef IYI_MOBILE
    int m_mobileWidth = 0, m_mobileHeight = 0;
    bool m_useCutout = false;
    Rectangle m_mobileArea{};
    float m_bufferScale = 1.0f;   // Android Low: window buffer / window size
#endif
    AgentChannel            m_agent;                       // dev-only file channel (AgentChannel.h)
    int                     m_frameCount = 0;
    int                     m_quitAfterFrames = 0;         // --frames; 0 = run until closed
    std::string             m_finalScreenshot;             // --screenshot
    bool                    m_logProfile = false;          // --profile
    bool                    m_checkShaders = false;        // --check-shaders
    int                     m_vsyncInterval = 0;           // swap interval set by SetVsync
    bool                    m_agentRun = false;            // --agent-run
    std::atomic<bool>       m_closeSeen{ false };          // a close was seen during a lockstep wait: none waits again
    std::atomic<bool>       m_closeUnread{ false };        // ... and the next PlatformFrame has not reported it yet
    std::string             m_runDir;                      // --user-data, else the project; user data when shipped
    bool                    m_hidden = false;              // --hidden
    int                     m_pinnedWidth = 0, m_pinnedHeight = 0;   // --window-size WxH
    bool                    m_pinnedWindow = false;
    bool                    m_unthrottled = false;         // --unthrottled
    float                   m_fixedDt = 0.0f;              // --fixed-dt; 0 = real frame time
    double                  m_fixedClock = 0.0;            // --fixed-dt: the game time so far
    long long               m_seed = 0;                    // --seed
    bool                    m_hasSeed = false;
};

} // namespace IYI
