// Implements the main engine application loop, subsystem orchestration, and frame update pipeline.
// A frame has three parts: the platform frame (main thread: window, input copy), the game step (game
// thread: hot-reload polling, pending starts, fixed-step passes at 60 Hz and 30 Hz, variable updates,
// the agent channel, recording the world and HUD drawing) and the render (main thread: replaying the
// recorded packet, audio, present). Every stage is a frame profiler section with the same name (SPEC §12.3), so
// engine.get_profile() / --profile / the agent channel show where a frame's time went.
// Maintains subsystem lifetimes and guarantees strict initialization and teardown sequences so that
// embedded Python and raylib graphical/audio contexts are destroyed in safe dependency order.
//
// Connected to:
// - Entry point instantiated and invoked by main.cpp.
// - Provides the global runtime context via CurrentApp() for EngineBindings.cpp.
// - Coordinates AssetManager, PythonRuntime, ProjectConfig, and Scene.
//
// Future evolution (per ROADMAP.md):
// May expose window configuration bindings. Engine-rendered debug overlays and job systems are
// intentionally excluded to keep the architecture readable and lean.

#include "IYI/Application.h"
#include "IYI/Ime.h"
#include "IYI/FrameInput.h"
#include "IYI/FramePacket.h"
#include "IYI/Particles.h"

#include <cstdio>
#include <cstdlib>
#include "IYI/LocalizationManager.h"
#include "IYI/AudioManager.h"
#include "IYI/SteamBridge.h"
#include "IYI/Log.h"
#include "IYI/CrashReport.h"
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
#include "IYI/Trailer.h"
#endif
#include "IYI/Video.h"
#include "IYI/FixedStep.h"
#include "IYI/Profiler.h"
#ifdef IYI_NET
#include "net/src/Net.h"   // libraries/net, only in builds with IYI_NET (SPEC §25.2)
#endif
#include "raymath.h"
#include "rlgl.h"
#ifdef IYI_PLATFORM_ANDROID
#include "MobilePaths.h"
#include <android_native_app_glue.h>
#include <android/api-level.h>
extern "C" struct android_app* GetAndroidApp(void);
#endif
#ifndef IYI_MOBILE
#include <GLFW/glfw3.h>   // glfwSwapInterval only: see Application::SetVsync
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include "ShaderSource.h"

#if defined(IYI_MOBILE)
#elif defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* hModule, wchar_t* lpFilename, unsigned long nSize);
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace IYI {

// Multiplayer stops before Steam: its Steam link and lobby still need the Steam API (SPEC §25).
static void StopNet()
{
#ifdef IYI_NET
    NetShutdown();
#endif
}

static Application* s_currentApp = nullptr;

Matrix CameraMatrix2D(const Camera2D& camera, float verticalScale)
{
    if (verticalScale == 1.0f) return GetCameraMatrix2D(camera);
    // Same composition as raylib's GetCameraMatrix2D, with the squash inserted after rotation and
    // zoom but before the offset, so it acts along screen-vertical around the camera offset.
    const Matrix origin = MatrixTranslate(-camera.target.x, -camera.target.y, 0.0f);
    const Matrix rotation = MatrixRotate(Vector3{ 0.0f, 0.0f, 1.0f }, camera.rotation * DEG2RAD);
    const Matrix scale = MatrixScale(camera.zoom, camera.zoom, 1.0f);
    const Matrix squash = MatrixScale(1.0f, verticalScale, 1.0f);
    const Matrix offset = MatrixTranslate(camera.offset.x, camera.offset.y, 0.0f);
    return MatrixMultiply(MatrixMultiply(origin, MatrixMultiply(scale, rotation)), MatrixMultiply(squash, offset));
}

Camera3D WorldCameraToGl(const Camera3D& camera)
{
    Camera3D gl = camera;
    gl.position.z = -camera.position.z;
    gl.target.z = -camera.target.z;
    gl.up.z = -camera.up.z;
    return gl;
}

bool WorldToScreen3D(const Camera3D& camera, Vector3 world, Vector2& screen)
{
    const Camera3D gl = WorldCameraToGl(camera);
    const Vector3 p{ world.x, world.y, -world.z };
    if (Vector3DotProduct(Vector3Subtract(p, gl.position), Vector3Subtract(gl.target, gl.position)) <= 0.0f) return false;
    screen = GetWorldToScreenEx(p, gl, GameInput().ScreenWidth, GameInput().ScreenHeight);   // game side
    return true;
}

namespace {

#ifdef IYI_PLATFORM_ANDROID
// Whether to run a frame now. Paused or unfocused, frames stop, but the first one after focus is lost
// still runs (input Focused = false, window still there) so the game can save before Android may kill it.
bool AndroidShouldRunFrame()
{
    static bool lastSent = false;
    android_app* app = GetAndroidApp();
    if (AndroidResumed() && IsWindowFocused()) {
        lastSent = false;
        return true;
    }
    if (lastSent || !app || !app->window) return false;
    lastSent = true;
    return true;
}
#endif

// raylib's own messages go to the engine log: warnings and errors (a shader that does not compile,
// a file it cannot load) are counted and reach the agent channel's replies; info lines (textures,
// fonts, shaders, audio loaded) land in latest.log too, where a game may show them (HR's boot screen).
void RaylibTrace(int level, const char* text, va_list args)
{
    char buffer[2048];
    std::vsnprintf(buffer, sizeof(buffer), text, args);
    if (level >= LOG_ERROR) LogWrite(LogLevel::Error, "raylib", buffer);
    else if (level >= LOG_WARNING) LogWrite(LogLevel::Warn, "raylib", buffer);
    else if (level >= LOG_INFO) LogWrite(LogLevel::Info, "raylib", buffer);
    else std::printf("DEBUG: %s\n", buffer);
}

// The `in` (or `out`) variables a shader stage declares, as "type name".
std::set<std::string> StageVaryings(const std::string& code, const std::string& qualifier)
{
    std::set<std::string> found;
    std::istringstream lines(code);
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(line);
        std::string q, type, name;
        if (words >> q >> type >> name && q == qualifier) found.insert(type + " " + name.substr(0, name.find(';')));
    }
    return found;
}

// --check-shaders: compiles every shader of the project and its libraries (each <name>.fs under
// assets/, its #include lines expanded) with the vertex shaders it is used with: <name>.vs beside it,
// else raylib's default when that writes all the fragment shader's inputs, else every .vs beside it
// that does (water.fs: wet_ground.vs). A pair fails when loading it logged a warning or an error
// (raylib links a failed stage with its default, so the shader itself still looks valid). Returns how
// many failed.
int CheckShaders(const ProjectConfig& config)
{
    namespace fs = std::filesystem;
    auto read = [](const std::string& path, std::string& out) {
        char* text = LoadFileText(path.c_str());
        if (text) out = text;
        UnloadFileText(text);
        return text != nullptr;
    };
    auto load = [&read](const std::string& path, std::string& code) {
        return read(path, code) && ExpandShaderIncludes(code, path, read).empty();
    };
    const std::set<std::string> defaultOut{ "vec2 fragTexCoord", "vec4 fragColor" };   // raylib's default vertex shader
    std::vector<fs::path> roots{ fs::path(config.ProjectDir) / "assets" };
    for (const std::string& library : config.Libraries) roots.push_back(fs::path(library) / "assets");
    int checked = 0, failed = 0;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root, ec)) {
            if (entry.path().extension() != ".fs") continue;
            std::string fsCode;
            std::vector<fs::path> pairs;   // empty: raylib's default vertex shader
            if (load(entry.path().string(), fsCode)) {
                const std::set<std::string> ins = StageVaryings(fsCode, "in");
                fs::path same = entry.path();
                same.replace_extension(".vs");
                if (fs::exists(same, ec)) pairs.push_back(same);
                else if (std::includes(defaultOut.begin(), defaultOut.end(), ins.begin(), ins.end())) pairs.emplace_back();
                else for (const fs::directory_entry& vs : fs::directory_iterator(entry.path().parent_path(), ec)) {
                    std::string vsCode;
                    if (vs.path().extension() != ".vs" || !load(vs.path().string(), vsCode)) continue;
                    const std::set<std::string> outs = StageVaryings(vsCode, "out");
                    if (std::includes(outs.begin(), outs.end(), ins.begin(), ins.end())) pairs.push_back(vs.path());
                }
            }
            if (pairs.empty()) {
                ++checked;
                ++failed;
                LogWrite(LogLevel::Error, "shaders", "failed (unreadable, or no vertex shader writes its inputs): " +
                                                      entry.path().generic_string());
            }
            for (const fs::path& vs : pairs) {
                const int before = LogWarnCount() + LogErrorCount();
                std::string vsCode;
                const bool vsRead = vs.empty() || load(vs.string(), vsCode);
                Shader shader = LoadShaderFromMemory(vs.empty() ? nullptr : vsCode.c_str(), fsCode.c_str());
                const bool ok = vsRead && IsShaderValid(shader) && LogWarnCount() + LogErrorCount() == before;
                if (shader.id != 0 && shader.id != rlGetShaderIdDefault()) UnloadShader(shader);
                ++checked;
                if (!ok) {
                    ++failed;
                    LogWrite(LogLevel::Error, "shaders", "failed: " + entry.path().generic_string() +
                                                          (vs.empty() ? "" : " with " + vs.filename().string()));
                }
            }
        }
    }
    LogWrite(failed ? LogLevel::Error : LogLevel::Info, "shaders",
             std::to_string(checked) + " shaders checked, " + std::to_string(failed) + " failed");
    return failed;
}

// Near and far depth bounds that suit a 3D camera's distance (raylib's fixed 0.01..1000 does not).
Vector2 WorldDepthRange(const Camera3D& camera)
{
    const float distance = Vector3Distance(camera.position, camera.target);
    return Vector2{ std::max(1.0f, distance * 0.05f), distance * 4.0f + 10000.0f };
}

// A render texture like LoadRenderTexture's, but its depth is a texture the postprocess shader can
// read (uDepth, depthTexture true); a plain one if the driver refuses. Without a 3D camera nothing is
// depth tested (only 3D passes enable it), so the target gets no depth at all.
RenderTexture2D LoadPostTarget(int width, int height, bool with3D, bool& depthTexture)
{
    depthTexture = false;
    RenderTexture2D target{};
    target.id = rlLoadFramebuffer();
    if (target.id == 0) return LoadRenderTexture(width, height);
    rlEnableFramebuffer(target.id);
    target.texture = Texture2D{ rlLoadTexture(nullptr, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1),
                                width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    rlFramebufferAttach(target.id, target.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    if (with3D) {
        target.depth = Texture2D{ rlLoadTextureDepth(width, height, false), width, height, 1, 19 };
        rlFramebufferAttach(target.id, target.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    }
    const bool complete = rlFramebufferComplete(target.id);
    rlDisableFramebuffer();
    depthTexture = complete && with3D;
    if (complete) return target;
    UnloadRenderTexture(target);
    return LoadRenderTexture(width, height);
}

// BeginMode3D with depth bounds that suit the camera's distance (raylib's fixed 0.01..1000 does not),
// the z flip of WorldCameraToGl, and no face culling (terrain walls and stack layers face any way).
// aspect: the frame's (the window's, or a shot run's canvas).
void BeginWorld3D(const Camera3D& world, double aspect)
{
    const Camera3D cam = WorldCameraToGl(world);
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    const Vector2 range = WorldDepthRange(cam);
    const double nearZ = range.x, farZ = range.y;
    if (cam.projection == CAMERA_PERSPECTIVE) {
        const double top = nearZ * std::tan(cam.fovy * 0.5 * DEG2RAD);
        rlFrustum(-top * aspect, top * aspect, -top, top, nearZ, farZ);
    } else {
        const double top = cam.fovy * 0.5;
        rlOrtho(-top * aspect, top * aspect, -top, top, nearZ, farZ);
    }
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(MatrixLookAt(cam.position, cam.target, cam.up)));
    rlScalef(1.0f, 1.0f, -1.0f);
    rlEnableDepthTest();
    rlDisableBackfaceCulling();
}

// Where a shot run's canvas shows in a window of w x h: fitted, never stretched, centred.
Rectangle CanvasFit(const RenderTexture2D& canvas, float w, float h)
{
    const float scale = std::min(w / canvas.texture.width, h / canvas.texture.height);
    const float cw = canvas.texture.width * scale, ch = canvas.texture.height * scale;
    return Rectangle{ (w - cw) * 0.5f, (h - ch) * 0.5f, cw, ch };
}

} // namespace

Application& CurrentApp()
{
    return *s_currentApp;
}

Application::Application()
{
    s_currentApp = this;
}

Application::~Application()
{
    Shutdown();
    LogShutdown();
    if (s_currentApp == this) {
        s_currentApp = nullptr;
    }
}

static std::filesystem::path GetExecutableDir(const char* argv0 = nullptr)
{
#if defined(IYI_MOBILE)
    (void)argv0;
#elif defined(_WIN32)
    wchar_t buffer[260];
    unsigned long len = GetModuleFileNameW(nullptr, buffer, 260);
    if (len > 0 && len < 260) {
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__linux__)
    char buffer[1024];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::filesystem::path(buffer).parent_path();
    }
#elif defined(__APPLE__)
    char buffer[1024];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        return std::filesystem::path(buffer).parent_path();
    }
#endif
    if (argv0 != nullptr && *argv0 != '\0') {
        std::error_code ec;
        auto p = std::filesystem::absolute(argv0, ec);
        if (!ec) {
            return p.parent_path();
        }
    }
    return std::filesystem::current_path();
}

static void PrintUsage()
{
    std::cout << "Usage: IYIEngine.exe [--project <path-to-game-project>] [--no-hot-reload] [--log-level <debug|info|warn|error>]\n"
                 "                     [--frames <n>] [--screenshot <file.png>] [--profile] [--check-shaders] [--single-thread]\n"
                 "  --frames <n>         quit after n frames\n"
                 "  --screenshot <file>  with --frames: save the last frame as an image\n"
                 "  --profile            log the frame profiler's last one-second window at exit\n"
                 "  --check-shaders      compile every shader of the project and its libraries, then exit (1: one failed)\n"
                 "  --single-thread      run the game step and the render on the main thread, one after the other\n"
#ifndef IYI_SHIPPING_BUILD
                 "  --agent-run          an agent or scripted run: engine.is_agent_run() is True (games skip intros, tutorials)\n"
                 "  --user-data <dir>    use <dir> as the user-data root (saves, settings, audio.json) for this run\n"
                 "  --hidden             no visible window; real keyboard and mouse input ignored, no background FPS cap\n"
                 "  --fixed-dt <s>       every frame advances exactly <s> seconds of game time\n"
                 "  --seed <n>           engine.run_seed() returns n, for the game's RNG\n"
                 "  --unthrottled        no FPS limit and no VSync: run as fast as the machine can\n"
                 "  --trailer-shot <name>    run marketing/shots/<name>.py and record it (tools/trailer.py)\n"
                 "  --trailer-config <file>  recording settings for engine.trailer (JSON, written by tools/trailer.py)\n"
#endif
                 ;
}

bool Application::Initialize(int argc, char* argv[], std::unique_ptr<Game> game)
{
    m_game = std::move(game);
    std::string projectPath;
    bool cliNoHotReload = false;
#ifndef IYI_SHIPPING_BUILD
    std::string trailerShot, trailerConfig;
#endif
    std::string userData;   // --user-data

#ifdef IYI_MOBILE
#ifdef IYI_PLATFORM_IOS
    m_singleThread = true;
#endif
#else
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--project") {
            if (i + 1 < argc) {
                projectPath = argv[++i];
            } else {
                LogWrite(LogLevel::Error, "startup", "Missing value for --project");
                PrintUsage();
                return false;
            }
        } else if (arg == "--no-hot-reload") {
            cliNoHotReload = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            m_quitAfterFrames = std::max(1, std::atoi(argv[++i]));
        } else if (arg == "--screenshot" && i + 1 < argc) {
            std::error_code ec;
            m_finalScreenshot = std::filesystem::absolute(argv[++i], ec).generic_string();
        } else if (arg == "--profile") {
            m_logProfile = true;
        } else if (arg == "--check-shaders") {
            m_checkShaders = true;
        } else if (arg == "--single-thread") {
            m_singleThread = true;
#ifndef IYI_SHIPPING_BUILD
        } else if (arg == "--agent-run") {
            m_agentRun = true;
        } else if (arg == "--user-data" && i + 1 < argc) {
            std::error_code ec;
            userData = std::filesystem::absolute(argv[++i], ec).generic_string();
        } else if (arg == "--hidden") {
            m_hidden = true;
        } else if (arg == "--fixed-dt" && i + 1 < argc) {
            m_fixedDt = std::max(0.0f, static_cast<float>(std::atof(argv[++i])));
        } else if (arg == "--seed" && i + 1 < argc) {
            m_seed = std::atoll(argv[++i]);
            m_hasSeed = true;
        } else if (arg == "--unthrottled") {
            m_unthrottled = true;
        } else if (arg == "--window-size" && i + 1 < argc) {
            m_pinnedWindow = std::sscanf(argv[++i], "%dx%d", &m_pinnedWidth, &m_pinnedHeight) == 2
                             && m_pinnedWidth > 0 && m_pinnedHeight > 0;
        } else if (arg == "--trailer-shot" && i + 1 < argc) {
            trailerShot = argv[++i];
        } else if (arg == "--trailer-config" && i + 1 < argc) {
            trailerConfig = argv[++i];
#endif
        } else if (arg == "--log-level") {
            if (i + 1 < argc) {
                std::string levelStr = argv[++i];
                bool ok = false;
                LogLevel level = LogParseLevel(levelStr, ok);
                if (ok) {
                    LogSetMinLevel(level);
                } else {
                    LogWrite(LogLevel::Warn, "startup", "Unrecognized log level '" + levelStr + "', keeping default 'info'");
                }
            } else {
                LogWrite(LogLevel::Warn, "startup", "Missing value for --log-level, keeping default 'info'");
            }
        } else {
            LogWrite(LogLevel::Error, "startup", "Unknown argument: " + arg);
            PrintUsage();
            return false;
        }
    }

#endif
    std::string autoDiscoveredProject;
#ifndef IYI_MOBILE
    if (projectPath.empty()) {
        namespace fs = std::filesystem;
        fs::path exeDir = GetExecutableDir(argc > 0 ? argv[0] : nullptr);
        fs::path cwd = fs::current_path();

        std::error_code ec;
#ifdef IYI_SHIPPING_BUILD
        // In shipping builds, skip loose file probing and resolve strictly to the executable directory backed by the archive
        if (m_assets.GetResourceStore().Exists("project.json") || fs::exists(exeDir / "game.rres", ec)) {
            projectPath = exeDir.string();
        }
#else
        // 1. the directory containing the executable
        if (fs::exists(exeDir / "project.json", ec)) {
            projectPath = exeDir.string();
        }
        // 2. the current working directory
        else if (fs::exists(cwd / "project.json", ec)) {
            projectPath = cwd.string();
        }
        // 3. <executable directory>/game/
        else if (fs::exists((exeDir / "game") / "project.json", ec)) {
            projectPath = (exeDir / "game").string();
        }
        // 4. a native game's exe sits in <project>/bin/ (SPEC §2c)
        else if (m_game && fs::exists(exeDir.parent_path() / "project.json", ec)) {
            projectPath = exeDir.parent_path().string();
        }
#endif

        if (!projectPath.empty()) {
            std::string absProjectPath = fs::canonical(projectPath, ec).string();
            if (ec) {
                absProjectPath = fs::absolute(projectPath, ec).string();
            }
            for (char& c : absProjectPath) {
                if (c == '\\') c = '/';
            }
            autoDiscoveredProject = absProjectPath;
        }
    }

#endif
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    if (!TrailerConfigure(trailerShot, trailerConfig)) return false;
#endif

    if (!m_finalScreenshot.empty() && m_quitAfterFrames == 0) {
        LogWrite(LogLevel::Warn, "startup", "--screenshot needs --frames <n>; ignored");
        m_finalScreenshot.clear();
    }

#ifndef IYI_MOBILE
    if (projectPath.empty()) {
        PrintUsage();
        if (argc == 1) {
            std::cin.get();
        }
        return false;
    }
#endif

#ifndef IYI_SHIPPING_BUILD
    // An isolated run (--user-data) keeps its logs/ and agentio/ there: it never rotates or truncates
    // the logs of another run of the same project (the player's own game, another agent).
    LogInit(userData.empty() ? projectPath : userData);
#else
    // Shipping opens its log only after project.json is parsed, under the
    // per-user root; never create a logs/ directory inside the install dir.
#endif

    if (!autoDiscoveredProject.empty()) {
        LogWrite(LogLevel::Info, "startup", "using project: " + autoDiscoveredProject);
    }

#ifndef IYI_SHIPPING_BUILD
    m_assets.SetProjectDir(projectPath);
#endif

    if (!m_config.Load(projectPath, m_assets.GetResourceStore())) {
        LogShutdown();
        return false;
    }
    m_config.UserDataOverride = userData;
    if (!userData.empty()) LogWrite(LogLevel::Info, "startup", "user data root, logs and agentio (--user-data): " + userData);

#ifdef IYI_SHIPPING_BUILD
    // A shipped game must never write into its install directory, which may be
    // read-only (Program Files, a Steam library). Now that project.json is parsed
    // we know the publisher/name, so re-open the log under the per-user root that
    // saves already use. The few startup lines before this point stay on stdout.
    LogInit(m_config.UserDataRoot());
    LogWrite(LogLevel::Info, "startup", "log reopened under user data root: " + m_config.UserDataRoot());
    m_runDir = m_config.UserDataRoot();
#endif

    // An agent run never talks to Steam: it would unlock achievements, write stats and show
    // "playing" on Gry's account, and several runs at once would share one Steam session.
    if (m_config.SteamAppId != 0 && m_agentRun) {
        LogWrite(LogLevel::Info, "startup", "Steam skipped (--agent-run)");
    } else if (m_config.SteamAppId != 0) {
        SteamInitResult steamRes = SteamStartup(m_config.SteamAppId);
        if (steamRes == SteamInitResult::RestartAppQuit) {
            LogWrite(LogLevel::Info, "startup", "Steam requested application restart via Steam client; exiting");
            LogShutdown();
            // Nothing beyond the log is up yet (no window, audio or Python), so exiting here
            // skips no teardown. Exit 0: Steam relaunching the game is not a failure.
            std::exit(0);
        } else if (steamRes == SteamInitResult::Failed) {
            LogWrite(LogLevel::Warn, "startup", "Steam initialization failed; continuing without Steam");
        }
    }
#ifdef IYI_NET
    {   // engine.net exists for a project that lists libraries/net (SPEC §25.2)
        bool listed = false;
        for (const std::string& library : m_config.Libraries) listed = listed || std::filesystem::path(library).filename() == "net";
        NetStartup(listed);
    }
#endif

#ifndef IYI_SHIPPING_BUILD
    m_runDir = userData.empty() ? m_config.ProjectDir : userData;   // where LogInit opened logs/
    m_agent.Dir = (std::filesystem::path(m_runDir) / "agentio").generic_string();
    m_agent.Lockstep = m_agentRun && m_fixedDt > 0.0f;
    if (m_agentRun) m_agent.LauncherGone = WatchLauncher();
    // A lockstep wait ends on a window close: one seen during a wait (m_closeSeen stays set, so no later frame
    // waits either; the next PlatformFrame reports it once) or one already in this frame's input.
    m_agent.CloseAsked = [this] {
        if (m_quitRequested || m_closeRequested) return true;
        if (m_singleThread) {   // this is the main thread; threaded, the main thread polls while it waits (Run)
            PollInputEvents();
            if (WindowShouldClose()) m_closeSeen = m_closeUnread = true;
        }
        return m_closeSeen.load();
    };
    // A lockstep run waits for its agent on purpose: no stall to report there.
    if (!m_agent.Lockstep) WatchdogStart(m_agent.Dir);
#endif
    CrashReportInstall((std::filesystem::path(m_runDir) / "logs").generic_string(), m_config.Name,
                       !m_agentRun && !m_hidden);

    if (cliNoHotReload) {
        m_hotReloadEnabled = false;
    } else {
#ifdef IYI_SHIPPING_BUILD
        m_hotReloadEnabled = false;
#else
        m_hotReloadEnabled = m_config.HotReload;
#endif
    }
    LogWrite(LogLevel::Info, "startup", std::string("hot reload: ") + (m_hotReloadEnabled ? "enabled" : "disabled"));

    if (m_config.WindowResizable) {
        SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    }
    if (m_checkShaders || m_hidden) SetConfigFlags(FLAG_WINDOW_HIDDEN);
    SetTraceLogCallback(RaylibTrace);

    if (m_pinnedWindow) {
        m_config.WindowWidth = m_pinnedWidth;
        m_config.WindowHeight = m_pinnedHeight;
    }
#ifdef IYI_MOBILE
    // The screen is the window: 0x0 makes raylib use the native display size. A fixed 1920x1080
    // would be upscaled with border bars and the UI root would not match the framebuffer.
    m_config.WindowWidth = 0;
    m_config.WindowHeight = 0;
#endif
    InitWindow(m_config.WindowWidth, m_config.WindowHeight, m_config.Name.c_str());
    if (!IsWindowReady()) {
        LogWrite(LogLevel::Error, "startup", "InitWindow failed: no usable OpenGL context");
        WatchdogStop();   // the thread must not outlive main, and Initialize's failure skips Shutdown
        return false;
    }
#ifdef IYI_PLATFORM_ANDROID
#if defined(__aarch64__)
    const char* abi = "arm64-v8a";
#elif defined(__arm__)
    const char* abi = "armeabi-v7a";
#elif defined(__x86_64__)
    const char* abi = "x86_64";
#else
    const char* abi = "x86";
#endif
    AndroidInstallCallbacks();
    // GL vendor/renderer/version: raylib's own "GL:" init lines in the same log.
    LogWrite(LogLevel::Info, "startup", std::string("Android ABI=") + abi + " API=" + std::to_string(android_get_device_api_level()) + " IYI Engine V5");
#endif
    ProfileInitGpu();
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    int canvasW = 0, canvasH = 0;
    if (TrailerCanvasSize(canvasW, canvasH)) {
        // A shot run draws at the recording size, not the window's: Windows clamps a window to the
        // monitor's work area (1920x1080 asked came out 1920x1061), and a 1440p take needs no 1440p screen.
        if (canvasW > 0 && canvasH > 0) m_canvas = LoadRenderTexture(canvasW, canvasH);
        const std::string size = std::to_string(canvasW) + "x" + std::to_string(canvasH);
        if (m_canvas.id == 0) {
            LogWrite(LogLevel::Error, "trailer", "cannot create the " + size + " canvas for the shot");
            CloseWindow();
            return false;
        }
        SetTextureFilter(m_canvas.texture, TEXTURE_FILTER_BILINEAR);   // the window's preview only; the capture reads texels
        LogWrite(LogLevel::Info, "trailer", "canvas " + size + " (window " + std::to_string(GetRenderWidth()) + "x" +
                 std::to_string(GetRenderHeight()) + ")");
    }
#endif
    SetRecordingPacket(&m_packets[0]);   // GameStart's loads record their uploads into the first frame (FramePacket.h)
    SetTargetFPS(m_config.TargetFPS);
    InitAudioDevice();
    SetExitKey(KEY_NULL);
    ImeInitialize(GetWindowHandle());

    m_assets.SetProjectDir(m_config.ProjectDir, m_config.Libraries);
    for (const std::string& library : m_config.Libraries) {
        LogWrite(LogLevel::Info, "startup", "library: " + library);
    }
    if (m_checkShaders) {                               // nothing else is up yet besides the window and audio
        const int failed = CheckShaders(m_config);
        CloseAudioDevice();
        ImeShutdown();
        CloseWindow();
        StopNet();
        SteamShutdown();
        LogShutdown();
        WatchdogStop();   // std::exit destroys the global thread; a joinable one would terminate
        std::exit(failed ? 1 : 0);
    }

    LocalizationManager::Get().Initialize(&m_assets, m_config.DefaultLanguage);
    AudioManager::Get().Initialize(&m_assets, m_config);
    TakeInput(m_inputs[0]);   // GameStart already reads the window and the audio side (FrameInput.h)
    SetGameInput(&m_inputs[0]);

    m_initialized = true;
    if (!StartGameSide()) {
        WatchdogStop();   // this path returns without Shutdown, so the global thread must be joined here
        AudioManager::Get().Shutdown();
        m_assets.UnloadAll();
        CloseAudioDevice();
        ImeShutdown();
        CloseWindow();
        StopNet();
        SteamShutdown();
        LogShutdown();
        m_initialized = false;
        return false;
    }
    return true;
}

// Game side, once: the interpreter, the entry script's GameStart and the first scene request (or a
// native game's Game::Start). On the game thread when threaded, so Python's whole life (start to
// finalize) is on that one thread.
// A native game's start or scene start throwing is logged like a Python traceback, never std::terminate.
static bool RunNative(const std::string& what, const std::function<void()>& fn)
{
    try { fn(); return true; }
    catch (const std::exception& e) { LogWrite(LogLevel::Error, "game", what + " threw: " + e.what()); }
    return false;
}

bool Application::StartGame()
{
    if (m_game) {
        m_currentScene = std::make_unique<Scene>();
        LogWrite(LogLevel::Info, "startup", "native game: calling Game::Start()...");
        m_startupOk = RunNative("Game::Start", [this] { m_game->Start(); });
        m_firstSceneExpected = m_hasPendingScene;
        return m_startupOk;
    }
#ifdef IYI_WITH_PYTHON
    m_python = std::make_unique<PythonRuntime>();
    LogWrite(LogLevel::Info, "startup", "Calling m_python.Initialize...");
    if (!m_python->Initialize(m_config.ProjectDir, m_config.EntryScript, m_hotReloadEnabled, &m_assets.GetResourceStore(),
                              m_config.Libraries, m_config.NativeDirs)) {
        LogWrite(LogLevel::Error, "startup", "m_python.Initialize returned false!");
        m_python->Shutdown();
        return false;
    }
    LogWrite(LogLevel::Info, "startup", "m_python.Initialize returned true.");
    {
        const std::filesystem::path run(m_runDir);
        std::string hangFile;
#ifndef IYI_SHIPPING_BUILD
        if (!m_agent.Lockstep) hangFile = (std::filesystem::path(m_agent.Dir) / "hang_stack.txt").generic_string();
#endif
        m_python->EnableFaultStacks((run / "logs" / "crash_python.txt").generic_string(), hangFile);
    }

    m_currentScene = std::make_unique<Scene>();
    m_currentScene->Actors.SetPythonRuntime(m_python.get());

    LogWrite(LogLevel::Info, "startup", "Calling m_python.CallGameStart()...");
    m_startupOk = m_python->CallGameStart();
    LogWrite(m_startupOk ? LogLevel::Info : LogLevel::Error, "startup",
             m_startupOk ? "m_python.CallGameStart() completed." : "m_python.CallGameStart() failed.");
    m_firstSceneExpected = m_hasPendingScene;
    return true;
#else
    LogWrite(LogLevel::Error, "startup", "built without Python (IYI_WITH_PYTHON OFF) and no native game given");
    return false;
#endif
}

// Game side, once at the end: actors, then the interpreter or the native Game object (shutdown order,
// rule 4). The GL objects they let go of are recorded into m_finalPacket, which the render side runs after this.
void Application::StopGame()
{
    if (m_gameStopped) return;
    m_gameStopped = true;
    SetRecordingPacket(&m_finalPacket);
    SetRecordPass(RecordPass::Offscreen);
    if (m_currentScene) {
        m_currentScene->Actors.DestroyAll(false);
        m_currentScene.reset();
    }
    m_game.reset();
#ifdef IYI_WITH_PYTHON
    if (m_python) m_python->Shutdown();
#endif
    ActorRegistry::FreeRetired(); // no wrapper is left once Python is finalized; GL deletions still record here
    SetRecordingPacket(nullptr);
}

// The game thread: Python from start to finalize, one game step per frame input the main thread
// hands over (pipelined 1:1: step k runs while the main thread draws step k-1).
void Application::GameThread()
{
    ProfileSetThread(ProfileThread::Game);
    const bool ok = StartGame();
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_gameReady = true;
        m_gameOk = ok;
    }
    m_frameCv.notify_all();
    for (int k = 0; ok && !m_quitRequested; ++k) {
        ProfileBegin("game");
        {
            ProfileScope wait("wait");                     // for the main thread's input of this frame
            std::unique_lock<std::mutex> lock(m_frameMutex);
            m_frameCv.wait(lock, [&] { return m_inputFrame >= k || m_stopGame; });
            if (m_inputFrame < k) break;   // stopped; an input already sent still runs (Android's save-on-pause frame)
        }
        SetGameInput(&m_inputs[k % 2]);
        GameStep(m_packets[k % 2]);
        ProfileEnd();
        ProfileEndFrame();
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_packetFrame = k;
        }
        m_frameCv.notify_all();
    }
    ProfileEndTo(0);
    if (ok) StopGame();
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_gameExited = true;
    }
    m_frameCv.notify_all();
}

// Starts the game side: on its own thread (it reports back once GameStart has run) or, with
// --single-thread, right here.
bool Application::StartGameSide()
{
    if (m_singleThread) return StartGame();
    m_gameThread = std::thread([this] { GameThread(); });
    std::unique_lock<std::mutex> lock(m_frameMutex);
    m_frameCv.wait(lock, [this] { return m_gameReady; });
    if (!m_gameOk) {
        lock.unlock();
        m_gameThread.join();
    }
    return m_gameOk;
}

// raylib's input plus the run flags: the one place a hidden run drops real input (an agent's own
// input is applied on the game side, AgentChannel) and a fixed-step run replaces the real frame time.
void Application::TakeInput(FrameInput& in)
{
#ifdef IYI_MOBILE
    UpdateMobileCanvas();
#endif
    TakeFrameInput(in);
    if (m_canvas.id != 0) {   // a shot run (Trailer.h): the game sees the canvas, and the pointer in its pixels
#ifdef IYI_MOBILE
        const Rectangle fit = m_mobileArea;
        const float sx = fit.width / m_canvas.texture.width, sy = fit.height / m_canvas.texture.height;
        if (sx > 0.0f && sy > 0.0f && in.Mouse.x >= 0.0f && in.Mouse.y >= 0.0f) {
            in.Mouse = { (in.Mouse.x - fit.x) / sx, (in.Mouse.y - fit.y) / sy };
            in.MouseDelta = { in.MouseDelta.x / sx, in.MouseDelta.y / sy };
        }
#else
        const Rectangle fit = CanvasFit(m_canvas, static_cast<float>(in.ScreenWidth), static_cast<float>(in.ScreenHeight));
        const float scale = fit.width / m_canvas.texture.width;
        if (scale > 0.0f) {
            in.Mouse = { (in.Mouse.x - fit.x) / scale, (in.Mouse.y - fit.y) / scale };
            in.MouseDelta = { in.MouseDelta.x / scale, in.MouseDelta.y / scale };
        }
#endif
        in.ScreenWidth = in.RenderWidth = m_canvas.texture.width;
        in.ScreenHeight = in.RenderHeight = m_canvas.texture.height;
    }
    if (m_fixedDt > 0.0f) in.FrameTime = m_fixedDt;
    in.Time = m_fixedDt > 0.0f ? (m_fixedClock += m_fixedDt) : GetTime();   // a fixed-step run repeats, shaders included
    if (!m_hidden) return;
    in.Focused = true;     // a hidden run behaves like a watched one (no pause, no background rate)
    in.Minimized = false;
    in.KeyDown.reset(); in.KeyPressed.reset(); in.KeyReleased.reset(); in.KeyRepeat.reset();
    in.Keys.clear(); in.Chars.clear();
    in.Mouse = { -1.0f, -1.0f }; in.MouseDelta = {}; in.Wheel = {};
    in.MouseDown.reset(); in.MousePressed.reset(); in.MouseReleased.reset();
    for (GamepadInput& pad : in.Gamepads) pad = GamepadInput{};
}

// Main thread, right after the poll: the window side of a frame and the frame input for the game step.
void Application::PlatformFrame(FrameInput& in)
{
    ProfileScope scope("input");
    ImeBeginFrame();
    // Unfocused or minimized, nobody watches the frames: draw at a background rate. Not below 30,
    // because music streams refill once per frame from raylib's ~66 ms buffer and would stutter.
    // --frames runs keep the configured rate so a benchmark measures the real thing.
    constexpr int kBackgroundFps = 30;
    const bool background = m_quitAfterFrames == 0 && (!IsWindowFocused() || IsWindowMinimized());
    // With VSync on the swap paces the frames; the limiter only guards against a driver that
    // forces VSync off, so it sits at twice the paced rate instead of fighting the refresh.
#ifdef __EMSCRIPTEN__
    const int wantedFps = 0;   // requestAnimationFrame paces the browser; raylib's limiter would block its main thread
    (void)background; (void)kBackgroundFps;
#else
    int wantedFps = m_config.TargetFPS;
    if (m_vsyncInterval > 0) {
        const int refresh = GetMonitorRefreshRate(GetCurrentMonitor());
        wantedFps = 2 * (refresh > 0 ? refresh : 60) / m_vsyncInterval;
    }
    if (background && !m_hidden && (wantedFps <= 0 || wantedFps > kBackgroundFps)) wantedFps = kBackgroundFps;
    if (m_unthrottled) wantedFps = 0;
#endif
    if (wantedFps != m_appliedFps) {
        SetTargetFPS(wantedFps);
#ifdef IYI_PLATFORM_ANDROID
        AndroidSetFrameRate(wantedFps);
#endif
        m_appliedFps = wantedFps;
    }
    // Alt+F4 relies on the OS posting WM_CLOSE for the window's system menu; some
    // window configurations (e.g. after ToggleFullscreen()) don't reliably surface
    // that, so handle the shortcut directly instead of trusting WindowShouldClose().
    // raylib raises WindowShouldClose() only for the one frame after a close; both
    // arriving together count as one close.
    const bool altHeldForClose = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
#ifdef __EMSCRIPTEN__
    // A browser tab cannot be closed from inside; raylib's web WindowShouldClose() also calls
    // emscripten_sleep, which needs Asyncify and kills the main loop without it.
    const bool closeAsked = false;
    (void)altHeldForClose;
    // The page stretches the canvas over the tab (shell.html), so its pixel buffer follows the
    // tab's CSS size times devicePixelRatio: sharp on any screen, and a resolution a game sets
    // is undone here. GLFW reports the mouse in CSS pixels, so it is scaled to buffer pixels.
    double cssW = 0.0, cssH = 0.0;
    emscripten_get_element_css_size("#canvas", &cssW, &cssH);
    if (cssW > 0.0 && cssH > 0.0) {
        const double dpr = emscripten_get_device_pixel_ratio();
        const int bufferW = static_cast<int>(cssW * dpr + 0.5);
        const int bufferH = static_cast<int>(cssH * dpr + 0.5);
        if (bufferW != GetScreenWidth() || bufferH != GetScreenHeight()) SetWindowSize(bufferW, bufferH);
        SetMouseScale(static_cast<float>(GetScreenWidth() / cssW), static_cast<float>(GetScreenHeight() / cssH));
    }
#else
    // m_closeUnread: a close polled during a lockstep wait, which raylib's flag no longer shows.
    const bool closeAsked = WindowShouldClose() || (altHeldForClose && IsKeyPressed(KEY_F4)) || m_closeUnread.exchange(false);
#endif
    HandleFullscreenToggle();
    TakeInput(in);
    in.CloseRequested = closeAsked;
    in.Presented = m_presented;
    in.AudioDone = m_audioDone;
}

// Game side: one frame of hooks, recording its GL, window and audio work into `packet`.
void Application::GameStep(FramePacket& packet)
{
    SetRecordingPacket(&packet);
    SetRecordPass(RecordPass::Offscreen);
    const FrameInput& in = GameInput();
    if (in.CloseRequested) {
        if (!m_closeIntercept || m_closeRequested) m_quitRequested = true;
        m_closeRequested = true;
    }
    const float dt = in.FrameTime;
    ApplyAgentInput(m_agent);   // before every hook reads this frame's input

    SteamRunFrame();
#ifdef IYI_NET
    ProfileBegin("net_receive");
    NetFrameBegin();   // after Steam callbacks, before any hook reads the replicated state
    ProfileEnd();
#endif

    // Frame stages are profiler sections (Profiler.h); actor hooks nest under them by script.
    ProfileBegin("hot_reload");
#ifndef IYI_SHIPPING_BUILD
    if (m_hotReloadEnabled) {
        LocalizationManager::Get().PollForChanges(dt);
    }
#endif
#ifdef IYI_WITH_PYTHON
    if (m_python) {
        m_python->PollHotReload(dt);
        auto reloadedScripts = m_python->TakeReloadedScripts();
        if (!reloadedScripts.empty() && m_currentScene) {
            // Before the Reload hooks rebuild UI: an asset added since it was found missing loads now.
            m_assets.ForgetFailedPaths();
            m_currentScene->Actors.DispatchReload(reloadedScripts);
        }
    }
#endif
    ProfileEnd();
    ProfileBegin("start");
    m_currentScene->Actors.StartPending();
    ProfileEnd();

    ProfileBegin("fixed_update");
    RunFixedUpdates(dt);
    ProfileEnd();
    ProfileBegin("update");
    m_currentScene->Actors.Update(dt);
    ProfileEnd();
    ProfileBegin("agent");
    UpdateAgentChannel(m_agent, dt);
    ProfileEnd();
#ifdef IYI_NET
    ProfileBegin("net_send");
    NetFrameEnd();   // after Update: what the hooks wrote this step goes out (SPEC §25.3)
    ProfileEnd();
#endif
    RecordDrawPasses(packet);

    ProfileBegin("scene");
    m_currentScene->Actors.RemoveDestroyed();
    const bool sceneApplied = ApplyPendingSceneChange();   // SceneStart records into this packet too
    ProfileEnd();
    NoteFrameForReadyMarker(sceneApplied);
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    TrailerGameStep(dt, m_readyLogged);
#endif
    CountFrame();
#ifndef IYI_SHIPPING_BUILD
    WatchdogFrame();
#ifdef IYI_WITH_PYTHON
    if (m_python) m_python->ArmHangDump(GetTime());
#endif
#endif
    packet.Quit = m_quitRequested;
}

// Render side: runs a game step's packet (window requests, uploads, audio, offscreen passes, the
// world and screen passes, postprocess), presents it and polls input (EndDrawing).
void Application::RenderPacket(FramePacket& packet)
{
    ReplayPass(packet, RecordPass::Setup);   // window requests and uploads before drawing
    ProfileBegin("audio");                   // the audio side lives here until it has its own thread
    ReplayPass(packet, RecordPass::Audio);
    m_audioDone += static_cast<long long>(packet.Passes[static_cast<int>(RecordPass::Audio)].size());
    AudioManager::Get().Update(GetFrameTime());
    UpdateVideos(GetFrameTime());            // after the Setup pass: this frame's play/pause/stop are in
    ProfileEnd();

    // Texture-mode passes recorded by Start, Update and the other hooks run before the frame's draw.
    ProfileBegin("offscreen");
    ReplayPass(packet, RecordPass::Offscreen);
    ProfileEnd();

    // The frame: the window, or in a shot run the canvas at the recording size (Trailer.h), which the
    // window then shows fitted after the capture has read it.
    const bool canvas = m_canvas.id != 0;
    const int width = canvas ? m_canvas.texture.width : GetScreenWidth();
    const int height = canvas ? m_canvas.texture.height : GetScreenHeight();
    auto beginFrame = [this, canvas]() {
        BeginDrawing();
        if (canvas) BeginTextureMode(m_canvas);
    };

    // The world pass and the screen pass. With a world-only postprocess shader the screen pass (the
    // HUD) is drawn after the shader, straight onto the frame; otherwise right after the world.
    auto drawWorld = [&packet, width, height]() {
        ClearBackground(packet.Background);
        if (packet.Camera3DActive) {
            BeginWorld3D(packet.View3D, static_cast<double>(width) / std::max(1, height));
        } else if (packet.CameraActive && packet.CameraVerticalScale == 1.0f) {
            BeginMode2D(packet.View2D);               // the original path, untouched
        } else if (packet.CameraActive) {
            rlDrawRenderBatchActive();                // what BeginMode2D does, with the tilted matrix
            rlLoadIdentity();
            rlMultMatrixf(MatrixToFloat(CameraMatrix2D(packet.View2D, packet.CameraVerticalScale)));
        }
        ProfileBegin("world");
        ReplayPass(packet, RecordPass::World);
        ProfileEnd();
        if (packet.Camera3DActive) {
            EndMode3D();
            rlEnableBackfaceCulling();
        } else if (packet.CameraActive) {
            EndMode2D();
        }
    };
    auto drawScreen = [&packet]() {
        ProfileBegin("screen");
        ReplayPass(packet, RecordPass::Screen);
        ProfileEnd();
    };

    // Read back the finished frame before EndDrawing swaps it away; after the swap the back
    // buffer holds nothing defined, which is why this is not a plain raylib forward. Sized by
    // the render (framebuffer) size: LoadImageFromScreen multiplies the screen size by the DPI
    // scale, which on a scaled Windows desktop reads past the frame into black.
    // The batch is flushed first: whatever the last pass queued (a screen-space actor's text and
    // lines) is not in the framebuffer until rlgl draws it, and would be missing from the image.
    auto captureScreenshot = [&packet, canvas, width, height]() {
        if (packet.Screenshot.empty()) return;
        rlDrawRenderBatchActive();
        const int w = canvas ? width : GetRenderWidth();
        const int h = canvas ? height : GetRenderHeight();
        Image image = { rlReadScreenPixels(w, h), w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
        const bool ok = ExportImage(image, packet.Screenshot.c_str());
        UnloadImage(image);
        LogWrite(ok ? LogLevel::Info : LogLevel::Warn, "render",
                 (ok ? "screenshot saved: " : "screenshot failed: ") + packet.Screenshot);
    };

    PostprocessState& post = m_post;
    Shader* activePostprocessShader = packet.Postprocess && IsShaderValid(*packet.Postprocess) ? packet.Postprocess : nullptr;
    // The depth texture exists only while a 3D camera is active; the target is remade when that flips.
    if (post.Target.id != 0 && (post.Target.texture.width != width || post.Target.texture.height != height
                                || post.With3D != packet.Camera3DActive)) {
        UnloadRenderTexture(post.Target);
        post.Target = { 0 };
    }
    if (activePostprocessShader && post.Target.id == 0 && width > 0 && height > 0) {
        post.With3D = packet.Camera3DActive;
        post.Target = LoadPostTarget(width, height, post.With3D, post.Depth);
    }

    if (activePostprocessShader && post.Target.id != 0 && post.Target.texture.id != 0) {
        // The world (and, unless world-only, the HUD) goes through this one target, so the final
        // shader affects it. Skipping this path keeps the old direct draw unchanged.
        const bool worldOnly = packet.PostprocessWorldOnly;
        ProfileBegin("draw");
        BeginTextureMode(post.Target);
            drawWorld();
            if (!worldOnly && packet.PostprocessTextMask) {
                // Alpha 1 everywhere (the world may have left less), then the HUD under the mask blend.
                rlDrawRenderBatchActive();
                rlColorMask(false, false, false, true);
                DrawRectangle(0, 0, width, height, BLACK);
                rlDrawRenderBatchActive();
                rlColorMask(true, true, true, true);
                SetTextMask(true);
                drawScreen();
                SetTextMask(false);
            } else if (!worldOnly) {
                drawScreen();
            }
        EndTextureMode();

        if (post.Uniforms != activePostprocessShader) {
            post.Uniforms = activePostprocessShader;
            post.TimeLoc = GetShaderLocation(*post.Uniforms, "uTime");
            post.ResolutionLoc = GetShaderLocation(*post.Uniforms, "uResolution");
            post.StrengthLoc = GetShaderLocation(*post.Uniforms, "uStrength");
            post.RadialWarpLoc = GetShaderLocation(*post.Uniforms, "uRadialWarp");
            post.TextMaskLoc = GetShaderLocation(*post.Uniforms, "uTextMask");
            post.DepthLoc = GetShaderLocation(*post.Uniforms, "uDepth");
            post.ViewLoc = GetShaderLocation(*post.Uniforms, "uView");
        }
        // uView: the 3D camera's near and far depth, 1 when orthographic, and the distance to its
        // target (what is in focus); all 0 without a 3D camera (no depth to read).
        float view[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const bool depth = packet.Camera3DActive && post.DepthLoc >= 0 && post.Depth;
        if (depth) {
            const Vector2 range = WorldDepthRange(packet.View3D);
            view[0] = range.x;
            view[1] = range.y;
            view[2] = packet.View3D.projection == CAMERA_ORTHOGRAPHIC ? 1.0f : 0.0f;
            view[3] = Vector3Distance(packet.View3D.position, packet.View3D.target);
        }
        Shader& shader = *post.Uniforms;
        if (post.ViewLoc >= 0) SetShaderValue(shader, post.ViewLoc, view, SHADER_UNIFORM_VEC4);
        const float time = static_cast<float>(packet.Time);
        const float resolution[2] = { static_cast<float>(width), static_cast<float>(height) };
        if (post.TimeLoc >= 0) SetShaderValue(shader, post.TimeLoc, &time, SHADER_UNIFORM_FLOAT);
        if (post.ResolutionLoc >= 0) SetShaderValue(shader, post.ResolutionLoc, resolution, SHADER_UNIFORM_VEC2);
        if (post.StrengthLoc >= 0) SetShaderValue(shader, post.StrengthLoc, &packet.PostprocessStrength, SHADER_UNIFORM_FLOAT);
        if (post.RadialWarpLoc >= 0) SetShaderValue(shader, post.RadialWarpLoc, &packet.PostprocessRadialWarp, SHADER_UNIFORM_FLOAT);
        const float textMask = packet.PostprocessTextMask && !worldOnly ? 1.0f : 0.0f;
        if (post.TextMaskLoc >= 0) SetShaderValue(shader, post.TextMaskLoc, &textMask, SHADER_UNIFORM_FLOAT);

        beginFrame();
            ClearBackground(BLACK);
            BeginShaderMode(shader);
                if (depth) SetShaderValueTexture(shader, post.DepthLoc, post.Target.depth);   // after the shader switch's flush
                // Render textures are vertically inverted in raylib's framebuffer space.
                DrawTextureRec(post.Target.texture,
                               Rectangle{ 0.0f, 0.0f, static_cast<float>(width), -static_cast<float>(height) },
                               Vector2{ 0.0f, 0.0f }, WHITE);
            EndShaderMode();
            if (worldOnly) drawScreen();
        ProfileEnd();
    } else {
        if (!activePostprocessShader && post.Target.id != 0) {
            UnloadRenderTexture(post.Target);
            post.Target = { 0 };
        }
        ProfileBegin("draw");
        beginFrame();
            drawWorld();
            drawScreen();
        ProfileEnd();
    }
    ProfileBegin("present");   // GPU submit, swap, the frame-rate wait and the input poll
    captureScreenshot();
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    TrailerCaptureFrame();
#endif
    if (canvas) {
        EndTextureMode();
#ifdef IYI_PLATFORM_ANDROID
        // EndTextureMode set the viewport to the window size; a Low buffer is smaller (same projection)
        rlViewport(0, 0, std::lround(GetScreenWidth() * m_bufferScale), std::lround(GetScreenHeight() * m_bufferScale));
#endif
        ClearBackground(BLACK);
        const Rectangle source{ 0.0f, 0.0f, static_cast<float>(width), -static_cast<float>(height) };   // render textures are upside down
        DrawTexturePro(m_canvas.texture, source,
#ifdef IYI_MOBILE
                       m_mobileArea,
#else
                       CanvasFit(m_canvas, static_cast<float>(GetScreenWidth()), static_cast<float>(GetScreenHeight())),
#endif
                       Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
    }
    ImeEndFrame();
    EndDrawing();
    ProfileEnd();

    packet.Clear();      // render side: a command holding an object's last reference deletes it here
    ++m_presented;
}

// The main thread's loop. Threaded: each frame takes the input for game step k, hands it over, then
// draws step k-1's packet while step k runs (two inputs, two packets in turn). --single-thread runs
// the same three parts one after the other on this thread.
void Application::Run()
{
    if (m_singleThread) {
#ifdef __EMSCRIPTEN__
        // The browser owns the loop: one frame per animation frame. main() never resumes (the call
        // unwinds it), so the frame that quits also ends the run and shuts down.
        emscripten_set_main_loop_arg([](void* self) {
            Application& app = *static_cast<Application*>(self);
            app.SingleThreadFrame();
            if (!app.m_quitRequested) return;
            emscripten_cancel_main_loop();
            app.EndRun();
            app.Shutdown();
        }, this, 0, true);
#endif
        while (!m_quitRequested) {
#ifdef IYI_MOBILE
            Frame();
#else
            SingleThreadFrame();
#endif
        }
    } else {
        ProfileSetThread(ProfileThread::Render);
        SetRenderSide(true);   // the main thread is the render side from here on
        for (int k = 0;; ++k) {
#ifdef IYI_PLATFORM_ANDROID
            android_app* app = GetAndroidApp();
            while (app && !app->destroyRequested && !AndroidShouldRunFrame()) PollInputEvents();
            if (!app || app->destroyRequested) break;
#endif
            ProfileBegin("render");
            PlatformFrame(m_inputs[k % 2]);
#ifdef IYI_PLATFORM_ANDROID
            m_inputs[k % 2].FrameTime = ClampFrameDelta(m_inputs[k % 2].FrameTime);
#endif
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_inputFrame = k;
            }
            m_frameCv.notify_all();
            if (k > 0) {
                {
                    ProfileScope wait("wait");               // for game step k-1
                    std::unique_lock<std::mutex> lock(m_frameMutex);
                    const auto ready = [&] { return m_packetFrame >= k - 1 || m_gameExited; };
                    // In lockstep the game side may wait seconds for the agent: keep the window answering
                    // and let a close end that wait (AgentChannel::CloseAsked).
                    while (m_agent.Lockstep && !m_frameCv.wait_for(lock, std::chrono::milliseconds(50), ready)) {
                        lock.unlock();
                        PollInputEvents();
                        if (WindowShouldClose()) m_closeSeen = m_closeUnread = true;
                        lock.lock();
                    }
                    m_frameCv.wait(lock, ready);
                    if (m_packetFrame < k - 1) break;       // the game side ended without this frame
                }
                FramePacket& packet = m_packets[(k - 1) % 2];
                const bool quit = packet.Quit;
                RenderPacket(packet);
                if (quit) break;
            }
            ProfileEnd();
            ProfileEndFrame();
        }
        ProfileEndTo(0);
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            m_stopGame = true;
        }
        m_frameCv.notify_all();
        m_gameThread.join();
    }
    EndRun();
}

// Both sides of one frame on this thread (--single-thread, and always in the browser).
void Application::SingleThreadFrame()
{
    const int k = m_frameIndex++;
    ProfileSetThread(ProfileThread::Render);
    SetRenderSide(true);
    ProfileBegin("render");
    PlatformFrame(m_inputs[k % 2]);
#ifdef IYI_MOBILE
    m_inputs[k % 2].FrameTime = ClampFrameDelta(m_inputs[k % 2].FrameTime);
#endif
    ProfileEnd();
    ProfileSetThread(ProfileThread::Game);
    SetRenderSide(false);
    SetGameInput(&m_inputs[k % 2]);
    ProfileBegin("game");
    GameStep(m_packets[k % 2]);
    ProfileEnd();
    ProfileEndFrame();
    ProfileSetThread(ProfileThread::Render);
    SetRenderSide(true);
    ProfileBegin("render");
    RenderPacket(m_packets[k % 2]);
    ProfileEnd();
    ProfileEndFrame();
}

// After the last frame, before Shutdown.
void Application::EndRun()
{
    if (m_runEnded) return;
    m_runEnded = true;
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    TrailerStop();   // a run that quit mid-recording still gets a finished file and its report
#endif
    SetRenderSide(false);
    if (m_post.Target.id != 0) UnloadRenderTexture(m_post.Target);
    if (m_canvas.id != 0) UnloadRenderTexture(m_canvas);
    if (m_logProfile) LogProfile();
}

bool Application::Frame()
{
    if (!m_initialized || m_runEnded || m_quitRequested || !m_singleThread) return false;
#ifdef IYI_MOBILE
#ifdef IYI_PLATFORM_ANDROID
    android_app* app = GetAndroidApp();
    if (!AndroidShouldRunFrame()) {
        PollInputEvents();
        if (app && app->destroyRequested) m_quitRequested = true;
        return !m_quitRequested;
    }
#else
    if (!IsWindowFocused()) return true;
#endif
#endif
    SingleThreadFrame();
    return !m_quitRequested;
}

// --frames / --screenshot: the screenshot is requested one frame early so the last frame draws it.
void Application::CountFrame()
{
    ++m_frameCount;
    LogSetFrame(m_frameCount);
    if (m_quitAfterFrames == 0) return;
    if (m_frameCount + 1 == m_quitAfterFrames && !m_finalScreenshot.empty()) RequestScreenshot(m_finalScreenshot);
    if (m_frameCount >= m_quitAfterFrames) m_quitRequested = true;
}

// --profile: the last one-second window, one line per section, for agents reading the log.
void Application::LogProfile()
{
    ProfilePublishPartial();
    LogWrite(LogLevel::Info, "profile", "fps " + std::to_string(ProfileFps()) + "  (avg ms / max ms / calls per frame  section)");
    for (const ProfileSection& section : ProfileResults()) {
        char line[96];
        std::snprintf(line, sizeof(line), "%8.3f / %8.3f / %6.1f  ", section.AvgMs, section.MaxMs, section.Calls);
        LogWrite(LogLevel::Info, "profile", line + section.Name);
    }
}

// Alt+Enter toggles borderless windowed "fullscreen": the window covers the monitor while the
// display keeps its own video mode, which is what raylib's examples use ToggleBorderlessWindowed()
// for. Exclusive fullscreen (ToggleFullscreen) changes the monitor's resolution instead, which
// makes alt-tabbing slow and rearranges every other application's windows.
void Application::HandleFullscreenToggle()
{
    if (ImeKeyboardCaptured()) return;
    bool altHeld = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    if (!altHeld || !IsKeyPressed(KEY_ENTER)) {
        return;
    }

    ToggleFullscreen();
}

void Application::PinWindowSize(int width, int height)
{
    m_pinnedWidth = width;
    m_pinnedHeight = height;
    m_pinnedWindow = true;
    RecordSetup([width, height]() {
        if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE)) ToggleBorderlessWindowed();
        SetWindowSize(width, height);
    });
}

// Shared by the Alt+Enter shortcut above and the Python-exposed engine.toggle_fullscreen()
// binding (EngineBindings.cpp), so a Settings-menu button can trigger the same behavior.
void Application::ToggleFullscreen()
{
    if (m_pinnedWindow) return;
    ToggleBorderlessWindowed();
#if RAYLIB_VERSION_MAJOR < 6
    // raylib 5.5 also pins the borderless window topmost, which keeps it drawn over whatever the
    // user alt-tabs to. The borderless size and position are what we want; the pinning is not.
    // raylib 6.x no longer pins it (SPEC §0.1).
    ClearWindowState(FLAG_WINDOW_TOPMOST);
#endif
}

// Two fixed-timestep accumulators (60 Hz and 30 Hz). The frame delta is clamped once (FixedStep.h) and
// fed to both, so a stall is discarded identically on both timelines; per-clock catch-up is also capped
// to avoid the "spiral of death". The constant step is passed to actors, never dt, for determinism.
void Application::RunFixedUpdates(float dt)
{
    dt = ClampFrameDelta(dt);
    for (int n = AdvanceFixedClock(m_acc60, dt, kFixedStep60); n > 0; --n) {
        m_currentScene->Actors.FixedUpdate60(kFixedStep60);
#ifdef IYI_NET
        NetFixedStep60();   // network ticks count on the 60 Hz clock
#endif
    }
    for (int n = AdvanceFixedClock(m_acc30, dt, kFixedStep30); n > 0; --n) {
        m_currentScene->Actors.FixedUpdate30(kFixedStep30);
    }
}

// Emits the one stable "runtime ready" line that release smoke tests wait for. It proves, in order:
// Python initialized, GameStart() succeeded, the first requested scene was applied, its SceneStart()
// succeeded, and a further full update/draw frame then ran with that scene live. Never emit it earlier.
void Application::NoteFrameForReadyMarker(bool sceneApplied)
{
    if (m_readyLogged || !m_startupOk) return;
    if (sceneApplied) {
        m_firstSceneApplied = true; // its actors have not updated or drawn yet; wait one more frame
    } else if (m_firstSceneApplied || !m_firstSceneExpected) {
        m_readyLogged = true;
        LogWrite(LogLevel::Info, "startup", "IYI_RUNTIME_READY");
    }
}

bool Application::ApplyPendingSceneChange()
{
    if (!m_hasPendingScene) return false;

    std::string nextScript = m_pendingScenePath;
    m_hasPendingScene = false;
    m_pendingScenePath.clear();

    auto nextScene = std::make_unique<Scene>();
#ifdef IYI_WITH_PYTHON
    nextScene->Actors.SetPythonRuntime(m_python.get());
#endif
    nextScene->Actors.AdoptPersistent(m_currentScene->Actors);
    m_currentScene->Actors.DestroyAll(false);
    m_currentScene = std::move(nextScene);

    m_currentScene->ScriptPath = nextScript;
    ++m_sceneChanges;
    LogWrite(LogLevel::Info, "scene", nextScript.empty() ? std::string("(empty scene)") : nextScript);
    LogEvent("scene", { { "path", nextScript } });
    if (nextScript.empty()) return true;
    if (m_game) {
        auto scene = m_nativeScenes.find(nextScript);
        if (scene == m_nativeScenes.end()) {
            LogWrite(LogLevel::Error, "scene", "no native scene registered as '" + nextScript + "'");
            m_startupOk = false; // a failed SceneStart must never produce the ready marker
        } else if (!RunNative("scene '" + nextScript + "'", scene->second)) {
            m_startupOk = false;
        }
    }
#ifdef IYI_WITH_PYTHON
    else if (!m_python->CallSceneStart(nextScript)) {
        m_startupOk = false;
    }
#endif
    return true;
}

std::string Application::CallAgentRequest(const std::string& requestJson)
{
    if (m_game) return m_game->AgentRequest(requestJson);
#ifdef IYI_WITH_PYTHON
    if (m_python) return m_python->CallAgentRequest(requestJson);
#endif
    return "null";
}

// Shutdown order is critical and must not be altered:
// 1. Destroy all actors (releases actor resources and clears Python references).
// 2. Finalize PythonRuntime (shuts down interpreter while raylib subsystems are still alive), or
//    destroy a native game's Game object.
//    Then replay the GL deletions Python's finalize recorded (FramePacket.h RenderOwned).
// 3. Shut down AudioManager (stops active music/sound streams and flushes config before asset unload).
// 4. Unload AssetManager resources (textures/sounds freed before audio/window close).
// 5. Close raylib audio device, then close window.
// 6. Shut down multiplayer (StopNet: libraries/net, if built), then Steam (SteamAPI_Shutdown) as the very last step.
// Reordering these causes crashes from Python referencing unloaded assets or accessing destroyed contexts.
void Application::Shutdown()
{
    if (m_initialized) {
        if (m_singleThread) EndRun();
        WatchdogStop();
        LogEvent("quit", { { "frames", std::to_string(m_frameCount) } });
        StopGame();   // threaded: the game thread did this before it ended
        // GL objects Python let go of while finalizing were recorded for deletion (RenderOwned): run
        // those on the render side now, while the window and the assets they may use still exist.
        // Frames never drawn are dropped, which deletes what only they held.
        SetRenderSide(true);
        ReplayPass(m_finalPacket, RecordPass::Offscreen);
        for (FramePacket* packet : { &m_finalPacket, &m_packets[0], &m_packets[1] }) packet->Clear();
        UnloadParticleTexture();
        SetRenderSide(false);
        AudioManager::Get().Shutdown();
        m_assets.UnloadAll();
        CloseAudioDevice();
        ImeShutdown();
        CloseWindow();
        StopNet();
        SteamShutdown();
        m_initialized = false;
        
        int warns = LogWarnCount();
        int errs = LogErrorCount();
        if (warns == 0 && errs == 0) {
            LogWrite(LogLevel::Info, "shutdown", "run complete: 0 warnings, 0 errors");
        } else {
            LogWrite(LogLevel::Warn, "shutdown", 
                "run complete: " + std::to_string(warns) + " warning(s), " + std::to_string(errs) + " error(s)");
        }

        LogShutdown();
    }
}

void Application::RequestSceneChange(const std::string& scene)
{
    m_pendingScenePath = scene;
    m_hasPendingScene = true;
}

void Application::SetCamera(const Camera2D& camera, float verticalScale)
{
    m_camera = camera;
    m_cameraVerticalScale = verticalScale;
    m_cameraActive = true;
    m_camera3DActive = false;
}

void Application::SetCamera3D(const Camera3D& camera)
{
    m_camera3D = camera;
    m_camera3DActive = true;
    m_cameraActive = false;
}

void Application::ClearCamera()
{
    m_cameraActive = false;
    m_camera3DActive = false;
}

// The game side of the frame's draw: closes a forgotten texture mode, takes the frame state the
// replay draws with, and records the Drawing hooks into the world and screen passes. Afterwards
// Record() appends offscreen work again (SceneStart, the next frame's hooks).
void Application::RecordDrawPasses(FramePacket& packet)
{
    if (m_userTextureMode) {   // a script forgot end_texture_mode: never draw the frame into its target
        static bool warned = false;
        if (!warned) {
            warned = true;
            LogWrite(LogLevel::Warn, "render", "begin_texture_mode left open; closed before drawing");
        }
        EndUserTextureMode();
    }
    packet.Background = m_backgroundColor;
    packet.View2D = m_camera;
    packet.CameraVerticalScale = m_cameraVerticalScale;
    packet.CameraActive = m_cameraActive;
    packet.View3D = m_camera3D;
    packet.Camera3DActive = m_camera3DActive;
    packet.Postprocess = m_postprocessShader;
    packet.PostprocessStrength = m_postprocessStrength;
    packet.Time = GameInput().Time;
    packet.PostprocessRadialWarp = m_postprocessRadialWarp;
    packet.PostprocessWorldOnly = m_postprocessWorldOnly;
    packet.PostprocessTextMask = m_postprocessTextMask;
    packet.Screenshot = m_pendingScreenshot;
    m_pendingScreenshot.clear();

    m_inDrawPass = true;
    ProfileBegin("draw");
    SetRecordPass(RecordPass::World);
    ProfileBegin("world");
    m_currentScene->Actors.Drawing(false);
    ProfileEnd();
    SetRecordPass(RecordPass::Screen);
    ProfileBegin("screen");
    m_currentScene->Actors.Drawing(true);
    ProfileEnd();
    ProfileEnd();
    SetRecordPass(RecordPass::Offscreen);
    m_inDrawPass = false;
}

// Chokepoint for game-side BeginTextureMode. Inside the draw pass it would reset the projection
// and model-view (the Camera2D transform is gone for every later world actor) and, with a
// postprocess shader active, unbind the frame's own target so the rest of the frame lands on
// screen unshaded. So it is refused there; offscreen passes belong in Update/Start.
bool Application::BeginUserTextureMode(const RenderTexture2D& target)
{
    if (m_inDrawPass || m_userTextureMode) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            LogWrite(LogLevel::Warn, "render", m_inDrawPass ? "begin_texture_mode refused inside Drawing; call it from Update"
                                                          : "begin_texture_mode refused: a texture mode is already open");
        }
        return false;
    }
    m_userTarget = &target;   // an AssetManager cache entry: stable until shutdown
    Record([t = m_userTarget]() { if (t->id != 0) BeginTextureMode(*t); });   // 0: its creation failed
    m_userTextureMode = true;
    return true;
}

void Application::EndUserTextureMode()
{
    if (!m_userTextureMode) return;   // an unmatched end would close the engine's own target
    Record([t = m_userTarget]() { if (t->id != 0) EndTextureMode(); });
    m_userTextureMode = false;
}

void Application::SetBackgroundColor(Color color)
{
    m_backgroundColor = color;
}

// raylib only switches VSync on or off (FLAG_VSYNC_HINT); every second refresh needs the swap
// interval itself, from the GLFW raylib already bundles (same reasoning as RaylibBindings.cpp).
void Application::SetVsync(int interval)
{
#ifdef IYI_MOBILE
    (void)interval;
    static bool logged = false;
    if (!logged) { LogWrite(LogLevel::Debug, "window", "set_vsync has no effect on mobile"); logged = true; }
#else
    m_vsyncInterval = m_unthrottled ? 0 : std::clamp(interval, 0, 2);
#if !defined(IYI_MOBILE) && !defined(__EMSCRIPTEN__)   // the browser presents on requestAnimationFrame; its glfwSwapInterval would
    glfwSwapInterval(m_vsyncInterval);   // set main-loop timing before the loop exists and stall it
#endif
#endif
}

#ifdef IYI_MOBILE
void Application::SetMobileSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    m_mobileWidth = width;
    m_mobileHeight = height;
}

void Application::UpdateMobileCanvas()
{
    const int windowW = GetScreenWidth(), windowH = GetScreenHeight();
    if (windowW <= 0 || windowH <= 0) return;
    // The window changed size (a foldable's cover screen to its main one): the game's render size
    // keeps its share of the window per axis, so High stays native and Low keeps the new aspect.
    static int lastW = windowW, lastH = windowH;
    if (m_mobileWidth && (windowW != lastW || windowH != lastH)) {
        m_mobileWidth = static_cast<int>(std::lround(static_cast<double>(m_mobileWidth) * windowW / lastW));
        m_mobileHeight = static_cast<int>(std::lround(static_cast<double>(m_mobileHeight) * windowH / lastH));
    }
    lastW = windowW; lastH = windowH;
    int left = 0, top = 0, right = 0, bottom = 0;
#ifdef IYI_PLATFORM_ANDROID
    if (!m_useCutout) {
        const MobileInsets insets = AndroidCutoutInsets();
        left = insets.left; top = insets.top; right = insets.right; bottom = insets.bottom;
    }
    const int density = AndroidDensity();
#endif
    const int areaW = std::max(1, windowW - left - right), areaH = std::max(1, windowH - top - bottom);
    int lift = 0;   // soft keyboard open: move the whole canvas up so the focused field sits 10 mm above it
#ifdef IYI_PLATFORM_ANDROID
    const float field = ImeFieldBottom();
    const int ime = AndroidImeHeight();
    if (field >= 0.0f && ime > 0)
        lift = std::max(0, static_cast<int>(std::lround(top + field * areaH + density * 10.0f / 25.4f)) - (windowH - ime));
#endif
    m_mobileArea = { static_cast<float>(left), static_cast<float>(top - lift), static_cast<float>(areaW), static_cast<float>(areaH) };
    const bool native = !m_mobileWidth || (m_mobileWidth == windowW && m_mobileHeight == windowH);
    const int width = m_mobileWidth ? m_mobileWidth : areaW;
    const int height = m_mobileHeight ? m_mobileHeight : areaH;
    const bool needTarget = !native || left || top || right || bottom || lift;
#ifdef IYI_PLATFORM_ANDROID
    // Low: the buffer is the window scaled to the target's width; coordinates stay in window pixels.
    m_bufferScale = native ? 1.0f : static_cast<float>(width) / areaW;
    AndroidSetBufferSize(std::lround(windowW * m_bufferScale), std::lround(windowH * m_bufferScale));
#endif
    if (m_canvas.id && (!needTarget || m_canvas.texture.width != width || m_canvas.texture.height != height)) {
        UnloadRenderTexture(m_canvas);
        m_canvas = {};
    }
    if (needTarget && !m_canvas.id) {
        m_canvas = LoadRenderTexture(width, height);
        if (m_canvas.id) SetTextureFilter(m_canvas.texture, TEXTURE_FILTER_BILINEAR);
        else LogWrite(LogLevel::Error, "window", "cannot create mobile render target");
    }
}
#endif

bool Application::WorldToScreen(Vector3 world, Vector2& screen) const
{
    if (m_camera3DActive) return WorldToScreen3D(m_camera3D, world, screen);
    if (m_cameraActive) {
        const Vector3 s = Vector3Transform(Vector3{ world.x, world.y, 0.0f }, CameraMatrix2D(m_camera, m_cameraVerticalScale));
        screen = Vector2{ s.x, s.y };
        return true;
    }
    screen = Vector2{ world.x, world.y };
    return true;
}

void Application::SetPostprocessShader(Shader* shader, float strength, float radialWarp, bool worldOnly, bool textMask)
{
    m_postprocessWorldOnly = worldOnly;
    m_postprocessTextMask = textMask;
    m_postprocessShader = shader;   // a cache entry; whether it compiled is known on the render side
    m_postprocessStrength = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 1.0f;
    m_postprocessRadialWarp = m_postprocessShader && std::isfinite(radialWarp)
        ? std::clamp(radialWarp, 0.0f, 0.25f) : 0.0f;
}

std::string Application::ResolveProjectPath(const std::string& relPath) const
{
    if (relPath.empty()) {
        return m_config.ProjectDir;
    }
    return m_assets.ResolvePath(relPath);
}

} // namespace IYI
