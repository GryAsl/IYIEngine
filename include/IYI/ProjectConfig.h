// Declares the game's project.json settings (window, frame rate, entry script, libraries, native
// folders, ...). JSON parsing stays in ProjectConfig.cpp so this header is free of nlohmann/json.
// Every field is optional and falls back to the default below; a field of the wrong type fails Load.
//
// Connected to:
// - Owned by Application and loaded in Application::Initialize.
// - Gives PythonRuntime the entry script and folders, raylib the window settings.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace IYI {

class ResourceStore;

struct ProjectConfig
{
    std::string Name = "IYI Game";
    std::string Publisher = "IYI";
    int WindowWidth  = 1280;
    int WindowHeight = 720;
    int TargetFPS    = 60;
    std::string EntryScript = "scripts/main.py";
    std::string ProjectDir; // resolved absolute path
    bool WindowResizable = false;
    bool HotReload = true;
    std::string DefaultLanguage = "en";
    std::unordered_map<std::string, float> AudioBusDefaults;
    int SteamAppId = 0;
    // "libraries": folders laid under the project in development (absolute, in priority order). Each has
    // the project layout (scripts/, assets/) and fills in any project-relative path the project lacks;
    // release packaging copies them into the game. Empty in shipping builds (already packed).
    std::vector<std::string> Libraries;
    // "native": folders holding modules the game itself built (e.g. a .pyd), relative to the project.
    // Absolute in development, where they are appended to sys.path after everything else so a Python
    // module of the same name always wins. Empty in shipping builds: release packaging flattens them
    // into one <OutputName>_Data/native, which PythonRuntime puts on sys.path (SPEC §2b).
    std::vector<std::string> NativeDirs;

    bool Load(const std::string& projectPath, ResourceStore& resources);
    bool Load(ResourceStore& resources);
    bool Load(const std::string& projectPath);

    // Per-user writable root for saves, config and logs, e.g.
    // %LOCALAPPDATA%/<Publisher>/<Name>. Never the installed game directory.
    std::string UserDataRoot() const;
    std::string UserDataOverride;   // --user-data: replaces the root above for this run
};

} // namespace IYI
