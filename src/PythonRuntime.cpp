// Implements embedded CPython management, script execution, lifecycle hook dispatch, and hot reload.
// Initializes the embedded Python interpreter via PyConfig with explicit Python home resolution to ensure
// the standard library is correctly located. Manages script modules in isolated namespace objects rather than
// standard sys.modules, allowing atomic hot reloading when files change on disk. Dispatches lifecycle hooks
// to actors by dynamically querying module attributes on every call, and suppresses repeated runtime errors.
// project.json "libraries" (SPEC §2) are extra roots: their scripts/ follow the project's on sys.path and
// hot reload watches them (ScriptFiles). project.json "native" (SPEC §2b) names folders the game built
// itself; they go on sys.path after everything else, so a Python module always wins over a compiled one.
// Every actor hook that runs is a profiler section named by its
// script (CallActorHook). The entry script's optional AgentRequest hook serves the agent channel.
//
// Connected to:
// - Implements PythonRuntime declared in include/IYI/PythonRuntime.h.
// - Registers the embedded 'engine' module containing bindings from EngineBindings.cpp and RaylibBindings.cpp.
// - Invoked by Application (startup/shutdown/reload) and ActorRegistry (hook dispatch).
//
// Future evolution (per ROADMAP.md):
// May incorporate bundled standalone Python binaries (Tier 3) and precompiled bytecode for release builds.
// Multi-threaded Python dispatch, async runtimes, and complex dependency DAG trackers are rejected.

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "IYI/PythonRuntime.h"
#include "IYI/ResourceStore.h"
#include "IYI/GameActor.h"
#include "IYI/Log.h"
#include "IYI/Profiler.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <pybind11/embed.h>
#include <pybind11/eval.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#ifdef IYI_MOBILE
#include "MobilePaths.h"
extern "C" PyObject* PyInit_engine();
#endif
#ifdef IYI_PLATFORM_ANDROID
#include <android/asset_manager.h>
#endif

#ifndef IYI_PYTHON_HOME
#define IYI_PYTHON_HOME ""
#endif

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* hModule, wchar_t* lpFilename, unsigned long nSize);
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace py = pybind11;
namespace fs = std::filesystem;

// Runtime file names of the CPython this build links (CMake IYI_PYTHON_VERSION: 3.14, or legacy 3.11),
// taken from Python.h so they can never disagree with the linked library.
#define IYI_PY_STR2(x) #x
#define IYI_PY_STR(x) IYI_PY_STR2(x)
static const char* const kPythonZip = "python" IYI_PY_STR(PY_MAJOR_VERSION) IYI_PY_STR(PY_MINOR_VERSION) ".zip";
static const char* const kPythonLibDir = "python" IYI_PY_STR(PY_MAJOR_VERSION) "." IYI_PY_STR(PY_MINOR_VERSION);
static const char* const kPythonVersion = IYI_PY_STR(PY_MAJOR_VERSION) "." IYI_PY_STR(PY_MINOR_VERSION);

namespace IYI {

void RegisterRaylibBindings(py::module_& m);
void RegisterEngineBindings(py::module_& m);
void RegisterIYIUIBindings(py::module_& m);
void RegisterLocalizationBindings(py::module_& m);
void RegisterAudioBindings(py::module_& m);
void RegisterSteamBindings(py::module_& m);
void RegisterPlayBindings(py::module_& m);
void RegisterNavBindings(py::module_& m);
void RegisterNavSimBindings(py::module_& m);
void RegisterSightBindings(py::module_& m);
void RegisterLibraryBindings(py::module_& m);
void RegisterGeoWorldBindings(py::module_& m);
void RegisterVideoBindings(py::module_& m);
#ifdef IYI_NET
void RegisterNetBindings(py::module_& m);   // libraries/net, only with IYI_NET
#endif
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
void RegisterTrailerBindings(py::module_& m);
#endif

static fs::path GetExecutablePath()
{
#ifdef _WIN32
    wchar_t buffer[260];
    unsigned long len = GetModuleFileNameW(nullptr, buffer, 260);
    if (len > 0 && len < 260) {
        return fs::path(buffer);
    }
#elif defined(__linux__)
    char buffer[1024];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return fs::path(buffer);
    }
#elif defined(__APPLE__)
    char buffer[1024];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) {
        return fs::path(buffer);
    }
#endif
    return fs::current_path();
}

static fs::path GetExecutableDir()
{
    return GetExecutablePath().parent_path();
}

static std::string ResolvePythonHome()
{
#ifdef IYI_PLATFORM_ANDROID
    return (MobileDataRoot() / "python").string();
#elif defined(IYI_PLATFORM_IOS)
    return (GetDataDir() / "python").string();
#elif defined(IYI_SHIPPING_BUILD)
    // Strictly resolve from the <OutputName>_Data directory in shipping builds.
    // Both PYTHONHOME and compiled IYI_PYTHON_HOME are ignored.
    fs::path dataDir = GetDataDir();
    std::error_code ec;

    // Check fixed subfolder names if present
    fs::path subRuntime = dataDir / "runtime";
    // Linux/macOS: python-build-standalone prefix, stdlib in <runtime>/lib/python3.X.
    if (fs::exists(subRuntime / "lib" / kPythonLibDir, ec)) {
        return subRuntime.lexically_normal().string();
    }
    if (fs::exists(subRuntime / kPythonZip, ec)) {
        return subRuntime.lexically_normal().string();
    }
    fs::path subPython = dataDir / "python";
    if (fs::exists(subPython / kPythonZip, ec)) {
        return subPython.lexically_normal().string();
    }

    // Default: embedded runtime files live directly in <OutputName>_Data
    if (fs::exists(dataDir / kPythonZip, ec)) {
        return dataDir.lexically_normal().string();
    }
    if (fs::exists(dataDir, ec) && fs::is_directory(dataDir, ec)) {
        return dataDir.lexically_normal().string();
    }
    return "";
#else
    // 1. Explicit PYTHONHOME environment variable
    const char* envHome = std::getenv("PYTHONHOME");
    if (envHome != nullptr && *envHome != '\0') {
        fs::path p(envHome);
        std::error_code ec;
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            return p.lexically_normal().string();
        }
    }

    // 2. <executable directory>\python
    fs::path exePython = GetExecutableDir() / "python";
    std::error_code ec;
    if (fs::exists(exePython, ec) && fs::is_directory(exePython, ec)) {
        return exePython.lexically_normal().string();
    }

    // 3. Compiled-in IYI_PYTHON_HOME
    std::string compiledHome = IYI_PYTHON_HOME;
    if (!compiledHome.empty()) {
        fs::path p(compiledHome);
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            return p.lexically_normal().string();
        }
    }

    // 4. Fall back to letting Python configure itself
    return "";
#endif
}

#ifdef IYI_PLATFORM_ANDROID
static bool ExtractPython()
{
    try {
        AAssetManager* assets = MobileAssets();
        if (!assets) throw std::runtime_error("asset manager unavailable");
        auto readAsset = [assets](const std::string& name) {
            AAsset* asset = AAssetManager_open(assets, name.c_str(), AASSET_MODE_STREAMING);
            if (!asset) throw std::runtime_error("missing asset " + name);
            std::string data;
            char buffer[16384];
            int count;
            while ((count = AAsset_read(asset, buffer, sizeof(buffer))) > 0) data.append(buffer, count);
            AAsset_close(asset);
            if (count < 0) throw std::runtime_error("failed reading asset " + name);
            return data;
        };
        std::istringstream manifest(readAsset("python/manifest.txt"));
        std::string hash, line, stamp;
        if (!std::getline(manifest, hash) || hash.empty()) throw std::runtime_error("empty Python manifest hash");
        if (hash.back() == '\r') hash.pop_back();
        if (hash.empty()) throw std::runtime_error("empty Python manifest hash");
        fs::path root = MobileDataRoot();
        if (root.empty()) throw std::runtime_error("mobile data root unavailable");
        fs::path dest = root / "python", staging = root / "python.staging";
        std::ifstream oldStamp(dest / ".stamp", std::ios::binary);
        std::getline(oldStamp, stamp);
        if (stamp == hash) return true;
        fs::remove_all(staging);
        fs::create_directories(staging);
        size_t files = 0;
        while (std::getline(manifest, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            fs::path rel(line);
            if (line.empty() || line == ".stamp" || rel.is_absolute() || rel.has_root_name()) throw std::runtime_error("invalid Python manifest path: " + line);
            for (const auto& part : rel) if (part == ".." || part == ".") throw std::runtime_error("invalid Python manifest path: " + line);
            fs::path out = staging / rel;
            fs::create_directories(out.parent_path());
            std::string data = readAsset("python/" + line);
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            file.write(data.data(), static_cast<std::streamsize>(data.size()));
            file.close();
            if (!file) throw std::runtime_error("failed writing " + out.string());
            ++files;
        }
        if (!files) throw std::runtime_error("Python manifest contains no files");
        std::ofstream file(staging / ".stamp", std::ios::binary | std::ios::trunc);
        file << hash << '\n';
        file.close();
        if (!file) throw std::runtime_error("failed writing Python stamp");
        fs::remove_all(dest);
        fs::rename(staging, dest);
        return true;
    } catch (const std::exception& e) {
        LogWrite(LogLevel::Error, "python", std::string("Python extraction failed: ") + e.what());
        return false;
    }
}
#endif

struct PythonRuntimeState
{
    std::unique_ptr<py::scoped_interpreter> m_guard;
    std::string m_projectDir;
    std::vector<std::string> m_libraryDirs;   // project.json "libraries", laid under the project
    std::vector<std::string> m_nativeDirs;    // project.json "native", the game's own built modules
    std::string m_entryScript;
    bool m_hotReloadEnabled = true;
    float m_pollTimer = 0.0f;
    ResourceStore* m_resources = nullptr;
    std::unique_ptr<ResourceStore> m_ownedResources;

    std::unordered_map<std::string, py::object> m_modules;
    std::unordered_map<uint64_t, py::dict> m_actorState;
    std::unordered_set<std::string> m_suppressed;
    std::unordered_map<std::string, int> m_suppressedCounts;
    std::unordered_set<std::string> m_warnedNoHooks;
    std::unordered_map<std::string, fs::file_time_type> m_watchedFiles;
    std::vector<std::string> m_reloadedScripts;
    py::object m_origStdout;
    py::object m_origStderr;
    py::object m_captureStdout;
    py::object m_captureStderr;
    bool m_stdioCaptureInstalled = false;
    py::object m_hangArm;     // faulthandler.dump_traceback_later bound to the hang file (EnableFaultStacks)
    double m_hangArmedAt = -1.0;

    // NoteReloaded: Single chokepoint for announcing and recording a reloaded script/module.
    // WHY it exists: A reload that is announced to the console but not recorded in m_reloadedScripts
    // silently starves live actors of their Reload hook (actors are never notified that a dependency
    // or module was refreshed).
    // WHAT BREAKS: If a future contributor prints "[IYI] reloaded ..." directly instead of calling
    // NoteReloaded, the console will announce the reload but ActorRegistry::DispatchReload will never
    // receive the path, leaving UI trees and gameplay state stale while misleading developers.
    void NoteReloaded(const std::string& relPath)
    {
        m_reloadedScripts.push_back(relPath);
        LogWrite(LogLevel::Info, "reload", "reloaded " + relPath);
    }

#ifdef IYI_SHIPPING_BUILD
    void InstallPackagedBootstrap()
    {
        const char* bootstrapCode = R"(
import sys
import marshal
from importlib.machinery import ModuleSpec

class _PackagedLoader:
    def __init__(self, fullname, data, is_bytecode, filename, is_package):
        self.fullname = fullname
        self.data = data
        self.is_bytecode = is_bytecode
        self.filename = filename
        self.is_package = is_package

    def create_module(self, spec):
        return None

    def exec_module(self, module):
        module.__file__ = self.filename
        if self.is_package:
            pkg_dir = self.filename.rsplit('/', 1)[0]
            module.__path__ = [pkg_dir]

        if self.is_bytecode:
            try:
                code = marshal.loads(self.data)
            except Exception:
                code = marshal.loads(self.data[16:])
            exec(code, module.__dict__)
        else:
            code = compile(self.data.decode('utf-8'), self.filename, 'exec')
            exec(code, module.__dict__)

class _PackagedFinder:
    def find_spec(self, fullname, path, target=None):
        import engine
        rel = fullname[8:] if fullname.startswith('scripts.') else fullname
        rel = rel.replace('.', '/')
        base = f"scripts/{rel}"

        # Search order:
        # 1. Package bytecode: .../__init__.pyc
        # 2. Package source:   .../__init__.py
        # 3. Module bytecode:  ...pyc
        # 4. Module source:    ...py
        candidates = [
            (f"{base}/__init__.pyc", True, True),
            (f"{base}/__init__.py", False, True),
            (f"{base}.pyc", True, False),
            (f"{base}.py", False, False),
        ]

        for cand_path, is_bytecode, is_package in candidates:
            data = engine._read_resource(cand_path)
            if data is not None:
                loader = _PackagedLoader(fullname, data, is_bytecode, cand_path, is_package)
                return ModuleSpec(fullname, loader, origin=cand_path, is_package=is_package)

        return None

sys.meta_path.insert(0, _PackagedFinder())
)";
        py::exec(bootstrapCode);
    }
#endif

    std::string MakeUniqueModuleName(const std::string& relPath)
    {
        std::string name = "iyi_script__";
        for (char c : relPath) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                name += c;
            } else {
                name += '_';
            }
        }
        return name;
    }

    py::object LoadModuleFromSource(const std::string& relPath, const std::string& absPath)
    {
        std::string source;
        std::vector<unsigned char> bytecode;
        bool isBytecode = false;
        bool loaded = false;

        if (m_resources) {
#ifdef IYI_SHIPPING_BUILD
            std::string pycPath = relPath;
            if (pycPath.size() >= 3 && pycPath.substr(pycPath.size() - 3) == ".py") {
                pycPath += "c";
            }
            if (m_resources->Exists(pycPath) && m_resources->ReadBytes(pycPath, bytecode)) {
                isBytecode = true;
                loaded = true;
            } else if (m_resources->Exists(relPath) && m_resources->ReadText(relPath, source)) {
                loaded = true;
            }
#else
            loaded = m_resources->ReadText(relPath, source);
#endif
        }

#ifndef IYI_SHIPPING_BUILD
        if (!loaded && !absPath.empty()) {
            std::ifstream file(absPath, std::ios::binary);
            if (file.is_open()) {
                source.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                loaded = true;
            }
        }
#endif

        if (!loaded) {
            LogWrite(LogLevel::Error, "python", "Could not open script: " + (absPath.empty() ? relPath : absPath));
            return py::none();
        }

        std::string filePath = absPath;
        if (filePath.empty()) {
            if (m_resources) {
                filePath = m_resources->GetPhysicalPath(relPath);
            }
            if (filePath.empty()) {
                fs::path p = fs::path(m_projectDir) / relPath;
                filePath = p.lexically_normal().generic_string();
            }
        }

        py::module_ builtins = py::module_::import("builtins");
        py::object execFunc = builtins.attr("exec");
        py::module_ typesMod = py::module_::import("types");
        py::object moduleType = typesMod.attr("ModuleType");

        std::string uniqueName = MakeUniqueModuleName(relPath);
        py::object mod = moduleType(uniqueName);
        mod.attr("__file__") = filePath;
        mod.attr("__builtins__") = builtins;

        if (isBytecode) {
            py::module_ marshalMod = py::module_::import("marshal");
            py::bytes pyBytes(reinterpret_cast<const char*>(bytecode.data()), bytecode.size());
            py::object codeObj;
            try {
                codeObj = marshalMod.attr("loads")(pyBytes);
            } catch (...) {
                if (bytecode.size() > 16) {
                    py::bytes strippedBytes(reinterpret_cast<const char*>(bytecode.data() + 16), bytecode.size() - 16);
                    codeObj = marshalMod.attr("loads")(strippedBytes);
                } else {
                    throw;
                }
            }
            execFunc(codeObj, mod.attr("__dict__"));
        } else {
            py::object compileFunc = builtins.attr("compile");
            py::object codeObj = compileFunc(source, filePath, "exec");
            execFunc(codeObj, mod.attr("__dict__"));
        }

        return mod;
    }

    py::object GetOrLoadScript(const std::string& relPath)
    {
        auto it = m_modules.find(relPath);
        if (it != m_modules.end()) {
            return it->second;
        }

        bool exists = false;
        std::string physPath;
        if (m_resources) {
            exists = m_resources->Exists(relPath);
#ifdef IYI_SHIPPING_BUILD
            if (!exists && relPath.size() >= 3 && relPath.substr(relPath.size() - 3) == ".py") {
                exists = m_resources->Exists(relPath + "c");
            }
#endif
            physPath = m_resources->GetPhysicalPath(relPath);
        }

#ifndef IYI_SHIPPING_BUILD
        if (!exists) {
            fs::path absPath = fs::path(m_projectDir) / relPath;
            std::error_code ec;
            if (fs::exists(absPath, ec)) {
                exists = true;
                physPath = absPath.lexically_normal().generic_string();
            }
        }
#endif

        if (!exists) {
            LogWrite(LogLevel::Error, "python", "Script not found: " + relPath);
            return py::none();
        }

        try {
            py::object mod = LoadModuleFromSource(relPath, physPath);
            if (mod && !mod.is_none()) {
                m_modules[relPath] = mod;
                if (m_warnedNoHooks.find(relPath) == m_warnedNoHooks.end()) {
                    if (!py::hasattr(mod, "Start") && !py::hasattr(mod, "Update") &&
                        !py::hasattr(mod, "FixedUpdate30") && !py::hasattr(mod, "FixedUpdate60") &&
                        !py::hasattr(mod, "Drawing") && !py::hasattr(mod, "Reload") &&
                        !py::hasattr(mod, "GameStart") && !py::hasattr(mod, "SceneStart")) {
                        m_warnedNoHooks.insert(relPath);
                        LogWrite(LogLevel::Warn, "python",
                            "script '" + relPath + "' defines none of the lifecycle hooks (Start, Update, FixedUpdate30, FixedUpdate60, Drawing, Reload) -- a misspelled hook name is the usual cause");
                    }
                }
                return mod;
            }
        } catch (py::error_already_set& e) {
            HandlePythonError(relPath, "<load>", e);
        } catch (const std::exception& e) {
            LogWrite(LogLevel::Error, "python", std::string("Error loading script ") + relPath + ": " + e.what());
        }

        return py::none();
    }

    void HandlePythonError(const std::string& scriptRelPath, const std::string& hookName, py::error_already_set& e)
    {
        std::string key = scriptRelPath + "|" + hookName;
        if (m_suppressed.find(key) != m_suppressed.end()) {
            m_suppressedCounts[key]++;
            return;
        }

        LogWrite(LogLevel::Error, "python", "Python error in " + scriptRelPath + "." + hookName + ":");
        try {
            py::module_ tb = py::module_::import("traceback");
            py::object format_exc = tb.attr("format_exception");
            py::list lines = format_exc(e.type(), e.value(), e.trace());
            for (auto line : lines) {
                LogWrite(LogLevel::Error, "python", line.cast<std::string>());
            }
        } catch (...) {
            LogWrite(LogLevel::Error, "python", e.what());
        }

        m_suppressed.insert(key);
    }

    void ClearSuppressedErrors(const std::string& scriptRelPath)
    {
        std::string prefix = scriptRelPath + "|";
        for (auto it = m_suppressed.begin(); it != m_suppressed.end(); ) {
            if (it->rfind(prefix, 0) == 0) {
                it = m_suppressed.erase(it);
            } else {
                ++it;
            }
        }
    }

    py::dict GetActorState(uint64_t actorId)
    {
        auto it = m_actorState.find(actorId);
        if (it != m_actorState.end()) {
            return it->second;
        }
        py::dict d;
        m_actorState[actorId] = d;
        return d;
    }

    // true when the hook ran (or is absent) without a script load/exception failure.
    bool CallGameStart()
    {
        if (m_entryScript.empty()) return true;
        std::string relPath = m_entryScript;
        for (char& c : relPath) if (c == '\\') c = '/';

        py::object mod = GetOrLoadScript(relPath);
        if (!mod || mod.is_none()) return false;

        try {
            if (py::hasattr(mod, "GameStart")) {
                py::object func = mod.attr("GameStart");
                if (func && !func.is_none() && py::isinstance<py::function>(func)) {
                    func();
                }
            }
        } catch (py::error_already_set& e) {
            HandlePythonError(relPath, "GameStart", e);
            return false;
        }
        return true;
    }

    // The entry script's AgentRequest(request) for the agent channel. JSON crosses in both directions
    // so the public header stays pybind-free; the reply is the hook's return value as JSON, or an
    // {"error": ...} object. A missing hook replies null.
    std::string CallAgentRequest(const std::string& requestJson)
    {
        if (m_entryScript.empty()) return "null";
        std::string relPath = m_entryScript;
        for (char& c : relPath) if (c == '\\') c = '/';
        py::object mod = GetOrLoadScript(relPath);
        if (!mod || mod.is_none() || !py::hasattr(mod, "AgentRequest")) return "null";
        try {
            py::module_ json = py::module_::import("json");
            py::object reply = mod.attr("AgentRequest")(json.attr("loads")(requestJson));
            // default=repr: a value JSON cannot hold (a Vector2, a Color) still reaches the agent as text.
            return json.attr("dumps")(reply, py::arg("default") = py::module_::import("builtins").attr("repr")).cast<std::string>();
        } catch (py::error_already_set& e) {
            HandlePythonError(relPath, "AgentRequest", e);
            py::module_ json = py::module_::import("json");
            return json.attr("dumps")(py::dict(py::arg("error") = std::string(e.what()))).cast<std::string>();
        }
    }

    // true when the hook ran (or is absent) without a script load/exception failure.
    bool CallSceneStart(const std::string& scriptRelPath)
    {
        if (scriptRelPath.empty()) return true;
        std::string relPath = scriptRelPath;
        for (char& c : relPath) if (c == '\\') c = '/';

        py::object mod = GetOrLoadScript(relPath);
        if (!mod || mod.is_none()) return false;

        try {
            if (py::hasattr(mod, "SceneStart")) {
                py::object func = mod.attr("SceneStart");
                if (func && !func.is_none() && py::isinstance<py::function>(func)) {
                    func();
                }
            }
        } catch (py::error_already_set& e) {
            HandlePythonError(relPath, "SceneStart", e);
            return false;
        }
        return true;
    }

    void CallActorHook(GameActor& actor, ActorHook hook, float dt)
    {
        if (actor.Script.empty()) return;

        std::string relPath = actor.Script;
        for (char& c : relPath) if (c == '\\') c = '/';

        // Hooks run several times per actor per frame, so the disk is only consulted for a
        // script that is not loaded. A loaded module leaves m_modules only when the hot-reload
        // scan sees its file gone, which routes a deleted script back through this check.
        bool scriptExists = m_modules.find(relPath) != m_modules.end();
        if (!scriptExists && m_resources) {
            scriptExists = m_resources->Exists(relPath);
#ifdef IYI_SHIPPING_BUILD
            if (!scriptExists && relPath.size() >= 3 && relPath.substr(relPath.size() - 3) == ".py") {
                scriptExists = m_resources->Exists(relPath + "c");
            }
#endif
        }
#ifndef IYI_SHIPPING_BUILD
        if (!scriptExists) {
            fs::path absScript = fs::path(m_projectDir) / relPath;
            std::error_code ec;
            scriptExists = fs::exists(absScript, ec);
        }
#endif
        if (!scriptExists) {
            std::string warnKey = std::to_string(actor.Id) + "|missing|" + relPath;
            if (m_suppressed.find(warnKey) == m_suppressed.end()) {
                m_suppressed.insert(warnKey);
                LogWrite(LogLevel::Warn, "python",
                    "actor '" + actor.Name + "' (id=" + std::to_string(actor.Id) + ") script file no longer exists: " + relPath);
            }
            return;
        }

        py::object mod = GetOrLoadScript(relPath);
        if (!mod || mod.is_none()) return;

        const char* hookName = nullptr;
        bool isUpdateHook = false;
        switch (hook) {
            case ActorHook::Start: hookName = "Start"; break;
            case ActorHook::Update: hookName = "Update"; isUpdateHook = true; break;
            case ActorHook::FixedUpdate30: hookName = "FixedUpdate30"; isUpdateHook = true; break;
            case ActorHook::FixedUpdate60: hookName = "FixedUpdate60"; isUpdateHook = true; break;
            case ActorHook::Drawing: hookName = "Drawing"; break;
            case ActorHook::Reload: hookName = "Reload"; break;
        }

        try {
            // Lifecycle hook functions are looked up from the module dict on EVERY call, never cached.
            // Caching hook function objects across frames silently breaks hot reload, causing script edits
            // to be ignored while the game continues running stale code.
            if (!py::hasattr(mod, hookName)) {
                return;
            }
            py::object func = mod.attr(hookName);
            if (!func || func.is_none() || !py::isinstance<py::function>(func)) {
                return;
            }

            py::object pyActor = py::cast(&actor, py::return_value_policy::reference);
            // One profiler section per hook that runs, named by the script, inside the caller's frame
            // stage. Closing back to the entry depth also ends a profile_begin the script left open.
            const size_t depth = ProfileDepth();
            ProfileBegin(relPath);
            try {
                if (isUpdateHook) {
                    func(pyActor, dt);
                } else {
                    func(pyActor);
                }
            } catch (...) {
                ProfileEndTo(depth);
                throw;
            }
            ProfileEndTo(depth);
        } catch (py::error_already_set& e) {
            HandlePythonError(relPath, hookName, e);
        }
    }

    // The .py files hot reload watches, as (project-relative path, file): the project's scripts/ first,
    // then each library's. A library file the project also has is shadowed, exactly as the resource
    // store and sys.path resolve it, so only the file actually imported is watched. The write time comes
    // from the directory listing and the path is made relative by text: a per-file fs::relative and
    // last_write_time (several syscalls each) made every 0.25 s poll a 25 ms hitch.
    struct ScriptFile { std::string Rel; fs::path Path; fs::file_time_type WriteTime; };
    std::vector<ScriptFile> ScriptFiles() const
    {
        std::vector<ScriptFile> files;
        std::unordered_set<std::string> seen;
        std::vector<std::string> roots{ m_projectDir };
        roots.insert(roots.end(), m_libraryDirs.begin(), m_libraryDirs.end());
        for (const std::string& root : roots) {
            const fs::path scriptsDir = fs::path(root) / "scripts";
            if (!fs::exists(scriptsDir)) continue;
            for (const auto& entry : fs::recursive_directory_iterator(scriptsDir)) {
                if (!entry.is_regular_file() || entry.path().extension() != ".py") continue;
                std::string rel = entry.path().lexically_relative(root).generic_string();
                if (seen.insert(rel).second) files.push_back({ std::move(rel), entry.path(), entry.last_write_time() });
            }
        }
        return files;
    }

    void PollHotReload(float dt)
    {
        if (!m_hotReloadEnabled) return;

        m_pollTimer += dt;
        if (m_pollTimer < 0.25f) return;
        m_pollTimer = 0.0f;

        std::unordered_set<std::string> currentFiles;

        {
            for (const auto& [relPath, filePath, currentWriteTime] : ScriptFiles()) {
                currentFiles.insert(relPath);

                auto it = m_watchedFiles.find(relPath);
                if (it == m_watchedFiles.end()) {
                    m_watchedFiles[relPath] = currentWriteTime;
                } else if (it->second != currentWriteTime) {
                    // Reload atomicity: a new module is compiled and executed in isolation before replacing the active one.
                    // If compilation or execution fails, the previous working module remains active in m_modules.
                    // Crucially, it->second is updated to currentWriteTime on both success and failure so broken scripts
                    // do not retry every 0.25s poll and flood the console with repeated tracebacks.
                    it->second = currentWriteTime;

                    if (m_modules.find(relPath) != m_modules.end()) {
                        try {
                            py::object newMod = LoadModuleFromSource(relPath, filePath.string());
                            if (newMod && !newMod.is_none()) {
                                m_modules[relPath] = newMod;
                                ClearSuppressedErrors(relPath);
                                NoteReloaded(relPath);
                            }
                        } catch (py::error_already_set& e) {
                            LogWrite(LogLevel::Error, "python", "Python error in " + relPath + ":");
                            try {
                                py::module_ tb = py::module_::import("traceback");
                                py::object format_exc = tb.attr("format_exception");
                                py::list lines = format_exc(e.type(), e.value(), e.trace());
                                for (auto line : lines) {
                                    LogWrite(LogLevel::Error, "python", line.cast<std::string>());
                                }
                            } catch (...) {
                                LogWrite(LogLevel::Error, "python", e.what());
                            }
                        }
                    } else {
                        // Check if module is in sys.modules
                        std::string modName;
                        std::string prefix = "scripts/";
                        if (relPath.rfind(prefix, 0) == 0) {
                            std::string sub = relPath.substr(prefix.length());
                            if (sub.length() > 3 && sub.substr(sub.length() - 3) == ".py") {
                                sub = sub.substr(0, sub.length() - 3);
                                for (char& c : sub) {
                                    if (c == '/' || c == '\\') c = '.';
                                }
                                modName = sub;
                            }
                        }
                        bool sysReloaded = false;
                        if (!modName.empty()) {
                            try {
                                py::module_ sysMod = py::module_::import("sys");
                                py::dict sysModules = sysMod.attr("modules");
                                if (sysModules.contains(modName)) {
                                    py::module_ importlib = py::module_::import("importlib");
                                    py::object helper = sysModules[modName.c_str()];
                                    // Helper reload atomicity: importlib.reload re-executes the file inside the live
                                    // module's own __dict__ (the module object must stay the same one, see SPEC), so an
                                    // exception halfway through would leave it half old, half new. The dict is
                                    // snapshotted and put back on failure: the previous module keeps working exactly as
                                    // it was, and the catch below still reports it with the new mtime already recorded.
                                    py::dict ns = helper.attr("__dict__");
                                    py::dict before = ns.attr("copy")();
                                    try {
                                        importlib.attr("reload")(helper);
                                    } catch (py::error_already_set&) {
                                        ns.attr("clear")();
                                        ns.attr("update")(before);
                                        throw;
                                    }
                                    ClearSuppressedErrors(relPath);
                                    NoteReloaded(relPath);
                                    sysReloaded = true;
                                }
                            } catch (py::error_already_set& e) {
                                LogWrite(LogLevel::Error, "python", "Python error reloading " + modName + " (" + relPath + "):");
                                try {
                                    py::module_ tb = py::module_::import("traceback");
                                    py::object format_exc = tb.attr("format_exception");
                                    py::list lines = format_exc(e.type(), e.value(), e.trace());
                                    for (auto line : lines) {
                                        LogWrite(LogLevel::Error, "python", line.cast<std::string>());
                                    }
                                } catch (...) {
                                    LogWrite(LogLevel::Error, "python", e.what());
                                }
                                sysReloaded = true; // Error during reload, but it was tracked
                            }
                        }
                        if (!sysReloaded) {
                            LogWrite(LogLevel::Warn, "reload",
                                "file changed but will not reload: " + relPath +
                                " -- module name looked for: '" + modName + "'" +
                                " -- the edit will not take effect until something imports this file");
                        }
                    }
                }
            }
        }

        if (!m_entryScript.empty()) {
            std::string entryRel = m_entryScript;
            for (char& c : entryRel) if (c == '\\') c = '/';
            fs::path entryAbs = fs::path(m_projectDir) / entryRel;
            if (fs::exists(entryAbs)) {
                currentFiles.insert(entryRel);
                auto entryMtime = fs::last_write_time(entryAbs);
                auto it = m_watchedFiles.find(entryRel);
                if (it == m_watchedFiles.end()) {
                    m_watchedFiles[entryRel] = entryMtime;
                } else if (it->second != entryMtime) {
                    it->second = entryMtime;
                    if (m_modules.find(entryRel) != m_modules.end()) {
                        try {
                            py::object newMod = LoadModuleFromSource(entryRel, entryAbs.string());
                            if (newMod && !newMod.is_none()) {
                                m_modules[entryRel] = newMod;
                                ClearSuppressedErrors(entryRel);
                                NoteReloaded(entryRel);
                            }
                        } catch (py::error_already_set& e) {
                            LogWrite(LogLevel::Error, "python", "Python error in " + entryRel + ":");
                            try {
                                py::module_ tb = py::module_::import("traceback");
                                py::object format_exc = tb.attr("format_exception");
                                py::list lines = format_exc(e.type(), e.value(), e.trace());
                                for (auto line : lines) {
                                    LogWrite(LogLevel::Error, "python", line.cast<std::string>());
                                }
                            } catch (...) {
                                LogWrite(LogLevel::Error, "python", e.what());
                            }
                        }
                    }
                }
            }
        }

        for (auto it = m_watchedFiles.begin(); it != m_watchedFiles.end(); ) {
            if (currentFiles.find(it->first) == currentFiles.end()) {
                m_modules.erase(it->first);
                it = m_watchedFiles.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<std::string> TakeReloadedScripts()
    {
        std::vector<std::string> result = std::move(m_reloadedScripts);
        m_reloadedScripts.clear();
        return result;
    }

    void InstallStdioCapture()
    {
        try {
            py::module_ sys = py::module_::import("sys");
            m_origStdout = sys.attr("stdout");
            m_origStderr = sys.attr("stderr");

            py::dict scope;
            scope["sys"] = sys;
            scope["engine"] = py::module_::import("engine");

            py::exec(R"(
class _StreamCapture:
    _reentrant = False

    def __init__(self, orig, log_func):
        self._orig = orig
        self._log_func = log_func
        self._buf = ""

    def write(self, s):
        if not s:
            return 0
        if _StreamCapture._reentrant:
            try:
                return self._orig.write(s)
            except Exception:
                return len(s)
        _StreamCapture._reentrant = True
        try:
            self._buf += str(s)
            while "\n" in self._buf:
                line, self._buf = self._buf.split("\n", 1)
                if line.endswith("\r"):
                    line = line[:-1]
                self._log_func(line)
            return len(s)
        except Exception:
            try:
                return self._orig.write(s)
            except Exception:
                return len(s)
        finally:
            _StreamCapture._reentrant = False

    def flush(self):
        if _StreamCapture._reentrant:
            return
        _StreamCapture._reentrant = True
        try:
            if self._buf:
                line = self._buf
                if line.endswith("\r"):
                    line = line[:-1]
                self._buf = ""
                self._log_func(line)
            try:
                self._orig.flush()
            except Exception:
                pass
        except Exception:
            pass
        finally:
            _StreamCapture._reentrant = False

    def isatty(self):
        try:
            return self._orig.isatty()
        except Exception:
            return False

    def close(self):
        self.flush()
        try:
            self._orig.close()
        except Exception:
            pass

    def __getattr__(self, name):
        return getattr(self._orig, name)

sys.stdout = _StreamCapture(sys.stdout, lambda msg: engine.log_info(msg, "python"))
sys.stderr = _StreamCapture(sys.stderr, lambda msg: engine.log_error(msg, "python"))
)", scope);

            m_captureStdout = sys.attr("stdout");
            m_captureStderr = sys.attr("stderr");
            m_stdioCaptureInstalled = true;
        } catch (const std::exception& e) {
            try {
                py::module_ sys = py::module_::import("sys");
                if (m_origStdout && !m_origStdout.is_none()) sys.attr("stdout") = m_origStdout;
                if (m_origStderr && !m_origStderr.is_none()) sys.attr("stderr") = m_origStderr;
            } catch (...) {}
            m_stdioCaptureInstalled = false;
            m_captureStdout = py::object();
            m_captureStderr = py::object();
            m_origStdout = py::object();
            m_origStderr = py::object();
            LogWrite(LogLevel::Warn, "python", std::string("Failed to install stdio capture: ") + e.what());
        } catch (...) {
            try {
                py::module_ sys = py::module_::import("sys");
                if (m_origStdout && !m_origStdout.is_none()) sys.attr("stdout") = m_origStdout;
                if (m_origStderr && !m_origStderr.is_none()) sys.attr("stderr") = m_origStderr;
            } catch (...) {}
            m_stdioCaptureInstalled = false;
            m_captureStdout = py::object();
            m_captureStderr = py::object();
            m_origStdout = py::object();
            m_origStderr = py::object();
            LogWrite(LogLevel::Warn, "python", "Failed to install stdio capture: unknown error");
        }
    }

    void RestoreStdioCapture()
    {
        if (!m_stdioCaptureInstalled) return;
        m_stdioCaptureInstalled = false;

        try {
            if (m_captureStdout && !m_captureStdout.is_none()) {
                m_captureStdout.attr("flush")();
            }
            if (m_captureStderr && !m_captureStderr.is_none()) {
                m_captureStderr.attr("flush")();
            }

            py::module_ sys = py::module_::import("sys");
            if (m_origStdout && !m_origStdout.is_none()) {
                sys.attr("stdout") = m_origStdout;
            }
            if (m_origStderr && !m_origStderr.is_none()) {
                sys.attr("stderr") = m_origStderr;
            }
        } catch (const std::exception& e) {
            LogWrite(LogLevel::Warn, "python", std::string("Failed to restore stdio capture: ") + e.what());
        } catch (...) {
            LogWrite(LogLevel::Warn, "python", "Failed to restore stdio capture: unknown error");
        }

        m_captureStdout = py::object();
        m_captureStderr = py::object();
        m_origStdout = py::object();
        m_origStderr = py::object();
    }

    void Shutdown()
    {
        for (const auto& [key, count] : m_suppressedCounts) {
            if (count > 0) {
                auto sep = key.find('|');
                std::string script = (sep != std::string::npos) ? key.substr(0, sep) : key;
                std::string hook = (sep != std::string::npos) ? key.substr(sep + 1) : "";
                LogWrite(LogLevel::Warn, "python",
                    "suppressed error summary: " + script + "." + hook +
                    " failed " + std::to_string(count) + " additional time(s) after the first");
            }
        }
        RestoreStdioCapture();
        if (m_hangArm) {
            py::module_::import("faulthandler").attr("cancel_dump_traceback_later")();
            m_hangArm = py::object();
        }
        m_modules.clear();
        m_actorState.clear();
        m_suppressed.clear();
        m_suppressedCounts.clear();
        m_warnedNoHooks.clear();
        m_watchedFiles.clear();
        m_reloadedScripts.clear();
        m_guard.reset();
        m_resources = nullptr;
        m_ownedResources.reset();
    }
};

struct PythonRuntime::Impl : public PythonRuntimeState
{
};

static PythonRuntimeState* s_activeRuntimeState = nullptr;

py::dict GetActorStateDict(uint64_t actorId)
{
    if (s_activeRuntimeState) {
        return s_activeRuntimeState->GetActorState(actorId);
    }
    return py::dict();
}

ResourceStore* GetActiveResourceStore()
{
    if (s_activeRuntimeState) {
        return s_activeRuntimeState->m_resources;
    }
    return nullptr;
}

static void EnsurePybindInternals() {
    pybind11::detail::get_internals_pp_manager().unref();
    auto* pp = pybind11::detail::get_internals_pp_manager().get_pp();
    if (pp && !pp->get()) {
        pp->reset(new pybind11::detail::internals());
        if (!(*pp)->instance_base) {
            (*pp)->instance_base = pybind11::detail::make_object_base_type((*pp)->default_metaclass);
        }
    }
}

static void EnsurePybindLocalInternals() {
    auto& l_ppmgr = pybind11::detail::get_local_internals_pp_manager();
    auto* l_pp = l_ppmgr.get_pp();
    if (l_pp && !l_pp->get()) {
        l_pp->reset(new pybind11::detail::local_internals());
    }
}

PythonRuntime::PythonRuntime() = default;

PythonRuntime::~PythonRuntime()
{
    Shutdown();
}

bool PythonRuntime::Initialize(const std::string& projectDir,
                               const std::string& entryScript,
                               bool hotReloadEnabled,
                               ResourceStore* resources,
                               const std::vector<std::string>& libraryDirs,
                               const std::vector<std::string>& nativeDirs)
{
    LogWrite(LogLevel::Info, "python", "PythonRuntime::Initialize start");
    m_impl = std::make_unique<Impl>();
    s_activeRuntimeState = m_impl.get();

    m_impl->m_projectDir = projectDir;
    for (char& c : m_impl->m_projectDir) if (c == '\\') c = '/';
    m_impl->m_libraryDirs = libraryDirs;
    m_impl->m_nativeDirs = nativeDirs;
    m_impl->m_entryScript = entryScript;
#if defined(IYI_SHIPPING_BUILD) || defined(__EMSCRIPTEN__)   // the browser's virtual FS never changes underneath
    m_impl->m_hotReloadEnabled = false;
#else
    m_impl->m_hotReloadEnabled = hotReloadEnabled;
#endif

    m_impl->m_resources = resources;
    if (!m_impl->m_resources) {
#ifdef IYI_SHIPPING_BUILD
        m_impl->m_ownedResources = std::make_unique<ResourceStore>(
            std::make_unique<PackagedResourceBackend>());
        m_impl->m_resources = m_impl->m_ownedResources.get();
#else
        if (!m_impl->m_projectDir.empty()) {
            m_impl->m_ownedResources = std::make_unique<ResourceStore>(
                std::make_unique<DevResourceBackend>(m_impl->m_projectDir));
            m_impl->m_resources = m_impl->m_ownedResources.get();
        }
#endif
    }

    if (!Py_IsInitialized()) {
#ifdef IYI_PLATFORM_ANDROID
        if (!ExtractPython()) return false;
#endif
#ifdef IYI_MOBILE
        if (PyImport_AppendInittab("engine", PyInit_engine) != 0) {
            LogWrite(LogLevel::Error, "python", "Failed to register engine module");
            return false;
        }
#endif
#ifdef IYI_MOBILE
        PyPreConfig preconfig;
        PyPreConfig_InitIsolatedConfig(&preconfig);
        preconfig.utf8_mode = 1;
#ifdef IYI_PLATFORM_ANDROID
        // pymalloc is not aware of arm64 memory tagging (MTE, cpython bpo-43593); suspected cause of the
        // intermittent segfault in PyUnicode_New on the first import (docs/mobile/ISSUES.md).
        preconfig.allocator = PYMEM_ALLOCATOR_MALLOC;
#endif
        PyStatus preStatus = Py_PreInitialize(&preconfig);
        if (PyStatus_Exception(preStatus)) {
            LogWrite(LogLevel::Error, "python", std::string("Python preinitialization failed: ") + (preStatus.err_msg ? preStatus.err_msg : "unknown error"));
            return false;
        }
#endif
        std::string resolvedHome = ResolvePythonHome();
        LogWrite(LogLevel::Info, "python", "Resolved Python home: " + (resolvedHome.empty() ? "<empty>" : resolvedHome));

        PyConfig config;
#ifdef IYI_MOBILE
        PyConfig_InitIsolatedConfig(&config);
        config.write_bytecode = 0;
        config.user_site_directory = 0;
#ifdef IYI_PLATFORM_IOS
        config.install_signal_handlers = 1;
#endif
#else
        PyConfig_InitPythonConfig(&config);
#endif
        config.buffered_stdio = 0;
#ifdef IYI_SHIPPING_BUILD
        // Shipping runs only the bundled runtime: ignore PYTHONPATH/PYTHONHOME/etc and the user site.
        config.use_environment = 0;
        config.user_site_directory = 0;
#endif

        if (!resolvedHome.empty()) {
#ifdef IYI_MOBILE
            PyStatus status = PyConfig_SetBytesString(&config, &config.home, resolvedHome.c_str());
#else
            std::wstring wHome = fs::path(resolvedHome).wstring();
            PyStatus status = PyConfig_SetString(&config, &config.home, wHome.c_str());
#endif
            if (PyStatus_Exception(status)) {
                std::string err = status.err_msg ? status.err_msg : "Failed to set config.home";
                PyConfig_Clear(&config);
                LogWrite(LogLevel::Error, "python", "Failed to set Python home path.");
                LogWrite(LogLevel::Error, "python", "Resolved Python home: " + resolvedHome);
                LogWrite(LogLevel::Error, "python", "Reason: " + err);
                LogWrite(LogLevel::Error, "python", "Hint: Check PYTHONHOME or IYI_PYTHON_HOME configuration.");
                return false;
            }

#ifdef IYI_MOBILE
            config.module_search_paths_set = 1;
            for (const fs::path& path : {fs::path(resolvedHome) / "lib" / kPythonLibDir,
                                         fs::path(resolvedHome) / "lib" / kPythonLibDir / "lib-dynload"}) {
                wchar_t* wide = Py_DecodeLocale(path.string().c_str(), nullptr);
                if (!wide) {
                    PyConfig_Clear(&config);
                    LogWrite(LogLevel::Error, "python", "Failed to decode Python search path: " + path.string());
                    return false;
                }
                PyStatus pathStatus = PyWideStringList_Append(&config.module_search_paths, wide);
                PyMem_RawFree(wide);
                if (PyStatus_Exception(pathStatus)) {
                    PyConfig_Clear(&config);
                    LogWrite(LogLevel::Error, "python", "Failed to add Python search path: " + path.string());
                    return false;
                }
            }
#endif

#ifdef _WIN32
            fs::path zipPath = fs::path(resolvedHome) / kPythonZip;
            if (fs::exists(zipPath)) {
                config.module_search_paths_set = 1;
                std::wstring wZip = zipPath.wstring();
                PyWideStringList_Append(&config.module_search_paths, wZip.c_str());
                PyWideStringList_Append(&config.module_search_paths, wHome.c_str());
                LogWrite(LogLevel::Info, "python", "Set module_search_paths with zip and home");
            } else {
                LogWrite(LogLevel::Warn, "python", std::string(kPythonZip) + " does not exist at: " + zipPath.string());
            }
#endif
        }

        LogWrite(LogLevel::Info, "python", "Instantiating scoped_interpreter...");
        try {
            m_impl->m_guard = std::make_unique<py::scoped_interpreter>(&config);
            LogWrite(LogLevel::Info, "python", "scoped_interpreter created successfully.");
        } catch (const std::exception& e) {
            LogWrite(LogLevel::Error, "python", "Failed to initialize Python interpreter.");
            LogWrite(LogLevel::Error, "python", "Resolved Python home: " + (resolvedHome.empty() ? "<auto/none>" : resolvedHome));
            LogWrite(LogLevel::Error, "python", std::string("Reason: ") + e.what());
            LogWrite(LogLevel::Error, "python", std::string("Hint: Ensure the Python ") + kPythonVersion + " standard library (Lib/encodings) exists at the Python home path.");
            return false;
        }

        LogWrite(LogLevel::Info, "python", "Interpreter initialization complete.");
    }

    try {
        LogWrite(LogLevel::Info, "python", "Importing sys...");
        py::module_ sys = py::module_::import("sys");
#ifdef IYI_PLATFORM_ANDROID
        LogWrite(LogLevel::Info, "startup", "Python " + py::str(sys.attr("version")).cast<std::string>());
#endif
        py::list path = sys.attr("path");
#ifndef IYI_SHIPPING_BUILD
        std::string scriptsDir = (fs::path(m_impl->m_projectDir) / "scripts").lexically_normal().string();
        for (char& c : scriptsDir) if (c == '\\') c = '/';

        path.attr("insert")(0, scriptsDir);
        path.attr("insert")(0, m_impl->m_projectDir);
        // Library scripts/ folders come right after the project's: a module the project has wins.
        for (size_t i = 0; i < m_impl->m_libraryDirs.size(); ++i) {
            path.attr("insert")(2 + i, (fs::path(m_impl->m_libraryDirs[i]) / "scripts").lexically_normal().generic_string());
        }
        // Declared native module folders go last of all, appended: a Python module of the same name
        // always wins over a compiled one, in development and in a packaged build alike.
        for (const std::string& nativeDir : m_impl->m_nativeDirs) {
            std::error_code ec;
            if (!fs::is_directory(nativeDir, ec)) {
                LogWrite(LogLevel::Error, "python", "Native module folder not found: " + nativeDir);
                continue;
            }
            path.attr("append")((fs::path(nativeDir)).lexically_normal().generic_string());
        }
#else
        // A packaged build flattens every declared native folder into one <OutputName>_Data/native
        // (release.py; the file names are unique per interpreter), which the packaged finder does not
        // read. Appended, not prepended: a packaged Python module still wins over a native one.
        path.attr("append")((GetDataDir() / "native").lexically_normal().generic_string());
#endif

        EnsurePybindInternals();
        EnsurePybindLocalInternals();

        LogWrite(LogLevel::Info, "python", "Importing engine...");
        py::module_::import("engine");
        LogWrite(LogLevel::Info, "python", "Installing stdio capture...");
        m_impl->InstallStdioCapture();
#ifdef IYI_SHIPPING_BUILD
        LogWrite(LogLevel::Info, "python", "Installing packaged bootstrap...");
        m_impl->InstallPackagedBootstrap();
        LogWrite(LogLevel::Info, "python", "Packaged bootstrap installed.");
#endif
    } catch (py::error_already_set& e) {
        std::string resolvedHome = ResolvePythonHome();
        LogWrite(LogLevel::Error, "python", std::string("Failed to initialize Python environment: ") + e.what());
        LogWrite(LogLevel::Error, "python", "Resolved Python home: " + (resolvedHome.empty() ? "<auto/none>" : resolvedHome));
        LogWrite(LogLevel::Error, "python", std::string("Reason: ") + e.what());
        LogWrite(LogLevel::Error, "python", "Hint: Check Python environment configuration and script paths.");
        m_impl->Shutdown();
        return false;
    } catch (const std::exception& e) {
        std::string resolvedHome = ResolvePythonHome();
        LogWrite(LogLevel::Error, "python", std::string("Failed to initialize Python environment: ") + e.what());
        LogWrite(LogLevel::Error, "python", "Resolved Python home: " + (resolvedHome.empty() ? "<auto/none>" : resolvedHome));
        LogWrite(LogLevel::Error, "python", std::string("Reason: ") + e.what());
        LogWrite(LogLevel::Error, "python", "Hint: Check Python environment configuration and script paths.");
        m_impl->Shutdown();
        return false;
    }

    // Populate watched files initial timestamps
    if (m_impl->m_hotReloadEnabled) {
        for (const auto& file : m_impl->ScriptFiles()) {
            m_impl->m_watchedFiles[file.Rel] = file.WriteTime;
        }

        if (!m_impl->m_entryScript.empty()) {
            std::string entryRel = m_impl->m_entryScript;
            for (char& c : entryRel) if (c == '\\') c = '/';
            fs::path entryAbs = fs::path(m_impl->m_projectDir) / entryRel;
            if (fs::exists(entryAbs)) {
                m_impl->m_watchedFiles[entryRel] = fs::last_write_time(entryAbs);
            }
        }
    }

    return true;
}

void PythonRuntime::Shutdown()
{
    if (m_impl) {
        m_impl->Shutdown();
        if (s_activeRuntimeState == m_impl.get()) {
            s_activeRuntimeState = nullptr;
        }
        m_impl.reset();
    }
}

bool PythonRuntime::CallGameStart()
{
    return m_impl && m_impl->CallGameStart();
}

std::string PythonRuntime::CallAgentRequest(const std::string& requestJson)
{
    return m_impl ? m_impl->CallAgentRequest(requestJson) : "null";
}

bool PythonRuntime::CallSceneStart(const std::string& scriptRelPath)
{
    return m_impl && m_impl->CallSceneStart(scriptRelPath);
}

void PythonRuntime::CallActorHook(GameActor& actor, ActorHook hook, float dt)
{
    if (m_impl) {
        m_impl->CallActorHook(actor, hook, dt);
    }
}

void PythonRuntime::EnableFaultStacks(const std::string& crashFile, const std::string& hangFile)
{
    if (!m_impl) return;
    try {
        py::module_ faulthandler = py::module_::import("faulthandler");
        py::object open = py::module_::import("builtins").attr("open");
        // faulthandler keeps a reference to each file, so they stay open for the whole run.
        if (!crashFile.empty()) faulthandler.attr("enable")(open(crashFile, "a"), py::arg("all_threads") = true);
        if (!hangFile.empty()) {
            m_impl->m_hangArm = py::cpp_function([later = faulthandler.attr("dump_traceback_later"),
                                                  file = open(hangFile, "w")] {
                later(2.5, py::arg("file") = file);   // re-arming cancels the previous timer
            });
        }
    } catch (const std::exception& e) {
        LogWrite(LogLevel::Warn, "python", std::string("faulthandler not set up: ") + e.what());
    }
}

void PythonRuntime::ArmHangDump(double nowSeconds)
{
    // Twice a second is enough: the dump fires 2.5 s after the last arm, so a stall of 2 to 2.5 s.
    if (!m_impl || !m_impl->m_hangArm || nowSeconds - m_impl->m_hangArmedAt < 0.5) return;
    m_impl->m_hangArmedAt = nowSeconds;
    m_impl->m_hangArm();
}

void PythonRuntime::PollHotReload(float dt)
{
    if (!m_impl) return;
    // A .py renamed or deleted mid-scan (editor atomic saves, git checkout) makes the
    // std::filesystem walk throw, and nothing above this catches it: the game would close.
    // Skipping one poll is harmless; the next one, 0.25 s later, sees the settled tree.
    try {
        m_impl->PollHotReload(dt);
    } catch (const std::exception& e) {
        LogWrite(LogLevel::Warn, "reload", std::string("hot reload scan skipped: ") + e.what());
    }
}

std::vector<std::string> PythonRuntime::TakeReloadedScripts()
{
    if (m_impl) {
        return m_impl->TakeReloadedScripts();
    }
    return {};
}

void PythonRuntime::OnActorDestroyed(uint64_t actorId)
{
    if (m_impl) {
        m_impl->m_actorState.erase(actorId);
    }
}

} // namespace IYI

static int pybind11_exec_engine(PyObject *);

extern "C" PYBIND11_EXPORT PyObject *PyInit_engine() {
    IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> PyInit_engine entered");
    PYBIND11_CHECK_PYTHON_VERSION
    IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> PyInit_engine version OK");
    try {
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> calling unref...");
        pybind11::detail::get_internals_pp_manager().unref();
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> calling get_pp...");
        auto* pp = pybind11::detail::get_internals_pp_manager().get_pp();
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> get_pp returned: " + std::to_string((uintptr_t)pp));
        if (pp) {
            IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> *pp is: " + std::to_string((uintptr_t)pp->get()));
        }
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 3: acquiring GIL...");
        PyGILState_STATE gstate = PyGILState_Ensure();
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 4: calling make_static_property_type...");
        auto* spt = pybind11::detail::make_static_property_type();
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 5: spt=" + std::to_string((uintptr_t)spt));
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 6: calling make_default_metaclass...");
        auto* dmc = pybind11::detail::make_default_metaclass();
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 7: dmc=" + std::to_string((uintptr_t)dmc));
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 7.5: calling new pybind11::detail::internals()...");
        auto* test_internals = new pybind11::detail::internals();
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 7.6: new internals succeeded: " + std::to_string((uintptr_t)test_internals));
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 7.7: testing pp->reset(test_internals)...");
        pp->reset(test_internals);
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 7.8: pp->reset succeeded!");
        if (!(*pp)->instance_base) {
            IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 7.9: calling make_object_base_type...");
            (*pp)->instance_base = pybind11::detail::make_object_base_type((*pp)->default_metaclass);
            IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> Step 7.10: instance_base made: " + std::to_string((uintptr_t)(*pp)->instance_base));
        }
        PyGILState_Release(gstate);
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> calling get_internals...");
        pybind11::detail::get_internals();
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> PyInit_engine ensure_internals OK");
        static ::pybind11::detail::slots_array mod_def_slots
            = ::pybind11::detail::init_slots(&pybind11_exec_engine);
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> PyInit_engine init_slots OK");
        static PyModuleDef def{
            PyModuleDef_HEAD_INIT,
            "engine",
            nullptr,
            0,
            nullptr,
            mod_def_slots.data(),
            nullptr,
            nullptr,
            nullptr
        };
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> PyInit_engine calling PyModuleDef_Init");
        PyObject* ret = PyModuleDef_Init(&def);
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> PyInit_engine PyModuleDef_Init finished");
        return ret;
    }
    PYBIND11_CATCH_INIT_EXCEPTIONS
    IYI::LogWrite(IYI::LogLevel::Error, "python", ">>> PyInit_engine caught exception!");
    return nullptr;
}

struct EngineModuleRegisterer {
    EngineModuleRegisterer() {
        (void)PyImport_AppendInittab("engine", PyInit_engine);
    }
};
#ifndef IYI_MOBILE
static EngineModuleRegisterer s_engine_module_registerer;
#endif

static void pybind11_init_engine(::pybind11::module_ &);
int pybind11_exec_engine(PyObject * pm) {
    IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> pybind11_exec_engine entered");
    try {
        pybind11::detail::ensure_internals();
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> pybind11_exec_engine ensure_internals OK");
        auto m = pybind11::reinterpret_borrow<::pybind11::module_>(pm);
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> pybind11_exec_engine checking cached module");
        if (!pybind11::detail::get_cached_module(m.attr("__spec__").attr("name"))) {
            IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> pybind11_exec_engine calling pybind11_init_engine");
            pybind11_init_engine(m);
            IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> pybind11_exec_engine cache_completed_module");
            pybind11::detail::cache_completed_module(m);
        }
        IYI::LogWrite(IYI::LogLevel::Info, "python", ">>> pybind11_exec_engine finished successfully");
        return 0;
    }
    PYBIND11_CATCH_INIT_EXCEPTIONS
    IYI::LogWrite(IYI::LogLevel::Error, "python", ">>> pybind11_exec_engine caught exception!");
    return -1;
}

void pybind11_init_engine(::pybind11::module_ & m)
{
    IYI::LogWrite(IYI::LogLevel::Info, "python", "Registering Raylib bindings...");
    IYI::RegisterRaylibBindings(m);
    IYI::LogWrite(IYI::LogLevel::Info, "python", "Registering Engine bindings...");
    IYI::RegisterEngineBindings(m);
    IYI::LogWrite(IYI::LogLevel::Info, "python", "Registering IYIUI bindings...");
    IYI::RegisterIYIUIBindings(m);
    IYI::LogWrite(IYI::LogLevel::Info, "python", "Registering Localization bindings...");
    IYI::RegisterLocalizationBindings(m);
    IYI::LogWrite(IYI::LogLevel::Info, "python", "Registering Audio bindings...");
    IYI::RegisterAudioBindings(m);
    IYI::LogWrite(IYI::LogLevel::Info, "python", "Registering Steam bindings...");
    IYI::RegisterSteamBindings(m);
    IYI::RegisterPlayBindings(m);
    IYI::RegisterNavBindings(m);
    IYI::RegisterNavSimBindings(m);
    IYI::RegisterSightBindings(m);
    IYI::RegisterLibraryBindings(m);
    IYI::RegisterGeoWorldBindings(m);
    IYI::RegisterVideoBindings(m);
#ifdef IYI_NET
    IYI::RegisterNetBindings(m);
#endif
#if !defined(IYI_SHIPPING_BUILD) && !defined(IYI_MOBILE)
    IYI::RegisterTrailerBindings(m);   // development only: engine.trailer does not exist in a shipping build
#endif
    IYI::LogWrite(IYI::LogLevel::Info, "python", "Embedded module 'engine' initialized successfully.");
}
