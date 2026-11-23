// Manages the lifecycle, storage, and lookup of active GameActor instances in a Scene.
// Actors are owned via std::unique_ptr in a flat std::vector rather than an ECS or complex hierarchy,
// keeping iteration simple and cache-friendly. Actor names are guaranteed unique within the registry,
// and IDs are monotonically assigned across the entire process lifetime. Destruction is deferred via
// a pending flag so mutating the actor list mid-frame never invalidates ongoing iteration.
//
// Connected to:
// - Owned directly by Scene (and indirectly managed by Application).
// - Calls into PythonRuntime to dispatch actor lifecycle hooks (Start, Update, FixedUpdate, Drawing).
// - Operates on GameActor instances.
//
// Future evolution (per ROADMAP.md):
// May eventually support multi-script composition per actor. Python handle safety is the retired list (SetActorReferencedHook).
// ECS architectures, component schedulers, and spatial partitioning trees are deliberately excluded.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>
#include "raylib.h"
#include "IYI/GameActor.h"

namespace IYI {

class PythonRuntime;
enum class ActorHook;   // PythonRuntime.h

// True while Python still holds a wrapper for this actor (set from the bindings). A removed actor that is
// still referenced is parked instead of freed, so a retained Python reference never points at freed memory.
using ActorReferencedFn = bool (*)(const GameActor* actor);
void SetActorReferencedHook(ActorReferencedFn fn);

class ActorRegistry
{
public:
    ActorRegistry() = default;
    ActorRegistry(const ActorRegistry&) = delete;
    ActorRegistry& operator=(const ActorRegistry&) = delete;

    void SetPythonRuntime(PythonRuntime* runtime) { m_pythonRuntime = runtime; }

    GameActor* Spawn(std::unique_ptr<GameActor> actor,
                     const std::string& requestedName,
                     const std::string& script,
                     Vector3 pos);

    GameActor* SpawnScripted(const std::string& requestedName,
                             const std::string& script,
                             Vector3 pos);

    void Destroy(uint64_t id);
    GameActor* FindById(uint64_t id);
    GameActor* FindByName(const std::string& name);

    void StartPending();
    void Update(float dt);
    void FixedUpdate30(float dt);
    void FixedUpdate60(float dt);
    void Drawing(bool screenSpacePass);
    void DispatchReload(const std::vector<std::string>& reloadedScripts);
    void RemoveDestroyed();
    void DestroyAll(bool keepPersistent);
    // Frees every parked actor regardless of references: only after Python is finalized (shutdown).
    static void FreeRetired();
    std::vector<std::unique_ptr<GameActor>> TakePersistent();

    void RegisterActor(std::unique_ptr<GameActor> actor);
    // Scene change: moves every persistent actor out of `from`, keeping a still-pending Start.
    void AdoptPersistent(ActorRegistry& from);

    const std::vector<std::unique_ptr<GameActor>>& GetActors() const { return m_actors; }

private:
    std::string UniqueName(const std::string& requestedName);
    void PythonHook(GameActor& actor, ActorHook hook, float dt);

    std::vector<std::unique_ptr<GameActor>> m_actors;
    std::unordered_set<std::string>        m_liveNames;
    std::vector<uint64_t>                  m_pendingStartIds;
    PythonRuntime*                         m_pythonRuntime = nullptr;
};

} // namespace IYI
