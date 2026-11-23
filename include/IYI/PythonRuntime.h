// Declares the embedded CPython runtime interface for script loading, hook dispatch, and hot reload.
// Encapsulates the Python interpreter lifecycle, actor script execution, and filesystem change polling.
// Employs the Pimpl (Pointer to Implementation) idiom to strictly prevent Python.h and pybind11 headers
// from leaking into the engine's public header files, keeping compilation times low and headers clean.
//
// Connected to:
// - Owned by Application and initialized during engine startup.
// - Called by ActorRegistry to dispatch Start, Update, FixedUpdate, and Drawing hooks.
// - Coordinates with EngineBindings.cpp and RaylibBindings.cpp to expose the built-in 'engine' module.
//
// Multithreaded Python execution and asynchronous script dispatch are explicitly rejected.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace IYI {

class GameActor;
class ResourceStore;

enum class ActorHook { Start, Update, FixedUpdate30, FixedUpdate60, Drawing, Reload };

class PythonRuntime
{
public:
    PythonRuntime();
    ~PythonRuntime();
    PythonRuntime(const PythonRuntime&) = delete;
    PythonRuntime& operator=(const PythonRuntime&) = delete;

    bool Initialize(const std::string& projectDir,
                    const std::string& entryScript,
                    bool hotReloadEnabled,
                    ResourceStore* resources = nullptr,
                    const std::vector<std::string>& libraryDirs = {},
                    const std::vector<std::string>& nativeDirs = {});
    void Shutdown();

    // Both return false when the script failed to load or the hook raised; a missing hook is success.
    bool CallGameStart();
    bool CallSceneStart(const std::string& scriptRelPath);
    void CallActorHook(GameActor& actor, ActorHook hook, float dt);
    // The entry script's AgentRequest(request) hook (agent channel). JSON in, JSON reply out:
    // the hook's return value, "null" when the hook is missing, {"error": ...} when it raised.
    std::string CallAgentRequest(const std::string& requestJson);

    // Python stacks for the crash and hang reports (faulthandler): a fatal error appends every thread's
    // stack to crashFile; when ArmHangDump stops being called for 2.5 s, they are written to hangFile.
    // Either path may be empty.
    void EnableFaultStacks(const std::string& crashFile, const std::string& hangFile);
    void ArmHangDump(double nowSeconds);   // game thread, every frame (re-arms at most twice a second)

    void PollHotReload(float dt);
    std::vector<std::string> TakeReloadedScripts();
    void OnActorDestroyed(uint64_t actorId);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace IYI
