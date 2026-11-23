// Implements project.json loading, JSON parsing, validation, and project directory canonicalization.
// Uses nlohmann/json entirely within this translation unit to keep public engine headers free of third-party
// JSON dependencies. Validates configuration keys (name, window dimensions, target FPS, entry script) and applies
// sensible defaults for any missing fields. Canonicalizes the project root directory and normalizes path separators.
//
// Connected to:
// - Implements ProjectConfig::Load declared in include/IYI/ProjectConfig.h.
// - Called by Application::Initialize during startup to configure window settings and script paths.
//
// "libraries" resolves to absolute folders here (development only; a shipping build has them packed).
// "native" resolves the same way, but a folder that is not there yet is not a load error: the engine
// reports it when Python goes to import from it, so a game may build its module after the first run.
//
// Future evolution (per ROADMAP.md):
// Configuration remains intentionally simple: a flat set of optional keys. Dynamic configuration schemas,
// cascading settings files, and runtime settings editors are explicitly excluded.

#include "IYI/ProjectConfig.h"
#include <cstdlib>
#include "IYI/ResourceStore.h"
#include "IYI/Log.h"
#include "MobilePaths.h"
#ifdef IYI_PLATFORM_ANDROID
#include <android_native_app_glue.h>
extern "C" struct android_app* GetAndroidApp(void);
#endif
#include <filesystem>
#include <nlohmann/json.hpp>

namespace IYI {

#ifdef IYI_MOBILE
std::filesystem::path MobileDataRoot()
{
#ifdef IYI_PLATFORM_ANDROID
    auto* app = GetAndroidApp();
    return app && app->activity && app->activity->internalDataPath ? app->activity->internalDataPath : "";
#else
    // On iOS HOME is the app container.
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) / "Library" / "Application Support" : std::filesystem::path();
#endif
}
#ifdef IYI_PLATFORM_ANDROID
AAssetManager* MobileAssets()
{
    auto* app = GetAndroidApp();
    return app && app->activity ? app->activity->assetManager : nullptr;
}
#endif
#endif

bool ProjectConfig::Load(const std::string& projectPath, ResourceStore& resources)
{
    namespace fs = std::filesystem;

    if (!projectPath.empty()) {
        fs::path p(projectPath);
        std::error_code ec;
        if (fs::exists(p, ec)) {
            if (fs::is_directory(p, ec)) {
                ProjectDir = fs::canonical(p, ec).string();
                if (ec) ProjectDir = fs::absolute(p, ec).string();
            } else {
                ProjectDir = fs::canonical(p.parent_path(), ec).string();
                if (ec) ProjectDir = fs::absolute(p.parent_path(), ec).string();
            }
            for (char& c : ProjectDir) {
                if (c == '\\') c = '/';
            }
        } else {
#ifndef IYI_SHIPPING_BUILD
            LogWrite(LogLevel::Error, "config", "Project path does not exist: " + projectPath);
            return false;
#endif
        }
    }

    if (!resources.HasBackend()) {
#ifndef IYI_SHIPPING_BUILD
        if (!ProjectDir.empty()) {
            resources.SetBackend(std::make_unique<DevResourceBackend>(ProjectDir));
        } else {
            LogWrite(LogLevel::Error, "config", "No resource backend and no ProjectDir available");
            return false;
        }
#else
        LogWrite(LogLevel::Error, "config", "No packaged resource backend mounted in shipping build");
        return false;
#endif
    }

    std::string jsonText;
    if (!resources.ReadText("project.json", jsonText)) {
        LogWrite(LogLevel::Error, "config", "project.json not found in ResourceStore");
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonText);
    } catch (const nlohmann::json::parse_error& e) {
        LogWrite(LogLevel::Error, "config", std::string("Malformed project.json: ") + e.what());
        return false;
    }

    if (!j.is_object()) {
        LogWrite(LogLevel::Error, "config", "Root of project.json must be a JSON object");
        return false;
    }

    // All fields in project.json are optional and fall back to these defaults if omitted:
    // - "name": "IYI Game"
    // - "publisher": "IYI"
    // - "window_width": 1280
    // - "window_height": 720
    // - "target_fps": 60
    // - "entry_script": "scripts/main.py"
    // - "window_resizable": false
    // - "hot_reload": true
    // Missing or malformed project.json causes loading to fail with exit code 1.
    if (j.contains("name")) {
        if (j["name"].is_string()) {
            Name = j["name"].get<std::string>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'name' in project.json, must be string");
            return false;
        }
    }
    if (j.contains("publisher")) {
        if (j["publisher"].is_string()) {
            Publisher = j["publisher"].get<std::string>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'publisher' in project.json, must be string");
            return false;
        }
    }
    if (j.contains("default_language")) {
        if (j["default_language"].is_string()) {
            DefaultLanguage = j["default_language"].get<std::string>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'default_language' in project.json, must be string");
            return false;
        }
    }
    if (j.contains("audio")) {
        if (j["audio"].is_object()) {
            for (auto& [k, v] : j["audio"].items()) {
                if (v.is_number()) {
                    AudioBusDefaults[k] = v.get<float>();
                } else {
                    LogWrite(LogLevel::Error, "config", "Invalid bus volume in 'audio' in project.json, must be number");
                    return false;
                }
            }
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'audio' in project.json, must be object");
            return false;
        }
    }
    if (j.contains("window_width")) {
        if (j["window_width"].is_number_integer()) {
            WindowWidth = j["window_width"].get<int>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'window_width' in project.json, must be integer");
            return false;
        }
    }
    if (j.contains("window_height")) {
        if (j["window_height"].is_number_integer()) {
            WindowHeight = j["window_height"].get<int>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'window_height' in project.json, must be integer");
            return false;
        }
    }
    if (j.contains("target_fps")) {
        if (j["target_fps"].is_number_integer()) {
            TargetFPS = j["target_fps"].get<int>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'target_fps' in project.json, must be integer");
            return false;
        }
    }
    if (j.contains("entry_script")) {
        if (j["entry_script"].is_string()) {
            EntryScript = j["entry_script"].get<std::string>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'entry_script' in project.json, must be string");
            return false;
        }
    }
    if (j.contains("window_resizable")) {
        if (j["window_resizable"].is_boolean()) {
            WindowResizable = j["window_resizable"].get<bool>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'window_resizable' in project.json, must be boolean");
            return false;
        }
    }
    if (j.contains("hot_reload")) {
        if (j["hot_reload"].is_boolean()) {
            HotReload = j["hot_reload"].get<bool>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'hot_reload' in project.json, must be boolean");
            return false;
        }
    }

    if (j.contains("libraries")) {
        if (!j["libraries"].is_array()) {
            LogWrite(LogLevel::Error, "config", "Invalid 'libraries' in project.json, must be an array of folder paths");
            return false;
        }
#ifndef IYI_SHIPPING_BUILD
        // Resolved against the project folder. A missing library fails here, by name, rather than
        // as an ImportError or a missing asset somewhere later.
        for (const auto& entry : j["libraries"]) {
            if (!entry.is_string()) {
                LogWrite(LogLevel::Error, "config", "Invalid entry in 'libraries' in project.json, must be a string");
                return false;
            }
            std::error_code ec;
            const fs::path dir = fs::path(ProjectDir) / entry.get<std::string>();
            if (!fs::is_directory(dir, ec)) {
                LogWrite(LogLevel::Error, "config", "Library folder not found: " + dir.lexically_normal().generic_string());
                return false;
            }
            fs::path canonical = fs::canonical(dir, ec);
            Libraries.push_back((ec ? dir.lexically_normal() : canonical).generic_string());
        }
#endif
    }

    if (j.contains("native")) {
        if (!j["native"].is_array()) {
            LogWrite(LogLevel::Error, "config", "Invalid 'native' in project.json, must be an array of folder paths");
            return false;
        }
#ifndef IYI_SHIPPING_BUILD
        // Resolved against the project folder, like "libraries". A folder that is not there is kept
        // anyway: the engine logs it when the folder goes on sys.path, so a game that has not built its
        // module yet still starts. A packaged build has no such folders on disk; they are packed into
        // <OutputName>_Data/native and PythonRuntime points sys.path at that one directory.
        for (const auto& entry : j["native"]) {
            if (!entry.is_string()) {
                LogWrite(LogLevel::Error, "config", "Invalid entry in 'native' in project.json, must be a string");
                return false;
            }
            std::error_code ec;
            const fs::path dir = fs::path(ProjectDir) / entry.get<std::string>();
            fs::path canonical = fs::canonical(dir, ec);
            NativeDirs.push_back((ec ? dir.lexically_normal() : canonical).generic_string());
        }
#endif
    }

    if (j.contains("steam_app_id")) {
        if (j["steam_app_id"].is_number_integer()) {
            SteamAppId = j["steam_app_id"].get<int>();
        } else {
            LogWrite(LogLevel::Error, "config", "Invalid 'steam_app_id' in project.json, must be integer");
            return false;
        }
    }

    return true;
}

bool ProjectConfig::Load(ResourceStore& resources)
{
    return Load("", resources);
}

bool ProjectConfig::Load(const std::string& projectPath)
{
    ResourceStore localStore;
    return Load(projectPath, localStore);
}

std::string ProjectConfig::UserDataRoot() const
{
    namespace fs = std::filesystem;
#ifdef IYI_MOBILE
    return MobileDataRoot().lexically_normal().generic_string();
#else
    if (!UserDataOverride.empty()) return fs::path(UserDataOverride).lexically_normal().generic_string();
    fs::path base;
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData && *localAppData != '\0') {
        base = fs::path(localAppData);
    } else {
#ifdef _WIN32
        const char* userProfile = std::getenv("USERPROFILE");
        if (userProfile && *userProfile != '\0') {
            base = fs::path(userProfile) / "AppData" / "Local";
        } else {
            base = fs::temp_directory_path();
        }
#elif defined(__APPLE__)
        const char* home = std::getenv("HOME");
        if (home && *home != '\0') {
            base = fs::path(home) / "Library" / "Application Support";
        } else {
            base = fs::temp_directory_path();
        }
#else
        const char* xdg = std::getenv("XDG_DATA_HOME");
        if (xdg && *xdg != '\0') {
            base = fs::path(xdg);
        } else {
            const char* home = std::getenv("HOME");
            if (home && *home != '\0') {
                base = fs::path(home) / ".local" / "share";
            } else {
                base = fs::temp_directory_path();
            }
        }
#endif
    }
    return (base / Publisher / Name).lexically_normal().generic_string();
#endif
}

} // namespace IYI
