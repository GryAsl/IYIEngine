// Implements actor storage, unique name resolution, lifecycle dispatch, and deferred destruction.
// Manages the collection of GameActor instances using index-based loops over pre-pass size snapshots,
// guaranteeing that actors spawned during an update are not processed until the subsequent frame.
// Deferred destruction ensures that actor memory remains valid throughout all lifecycle passes within
// a frame, with cleanup occurring only during RemoveDestroyed() at the frame boundary.
//
// Connected to:
// - Owned and updated by Scene and Application.
// - Dispatches actor events to PythonRuntime::CallActorHook.
// - Manages GameActor memory and transfers persistent actors during scene transitions.
//
// Future evolution (per ROADMAP.md):
// Removed actors Python still references are parked in s_retired, not freed (no generation counter needed).
// ECS archetypes, multi-threaded dispatch, and spatial indexing are deliberately rejected.

#include "IYI/ActorRegistry.h"
#include "IYI/PythonRuntime.h"
#include "IYI/Log.h"
#include <algorithm>

namespace IYI {

static uint64_t s_nextId = 1;

std::string ActorRegistry::UniqueName(const std::string& requestedName)
{
    std::string base = requestedName.empty() ? "Actor" : requestedName;
    if (m_liveNames.find(base) == m_liveNames.end()) {
        m_liveNames.insert(base);
        return base;
    }

    int suffix = 2;
    while (true) {
        std::string candidate = base + "_" + std::to_string(suffix);
        if (m_liveNames.find(candidate) == m_liveNames.end()) {
            m_liveNames.insert(candidate);
            return candidate;
        }
        ++suffix;
    }
}

GameActor* ActorRegistry::Spawn(std::unique_ptr<GameActor> actor,
                                const std::string& requestedName,
                                const std::string& script,
                                Vector3 pos)
{
    if (!actor) return nullptr;

    actor->Id = s_nextId++;
    actor->Name = UniqueName(requestedName);
    actor->Script = script;
    actor->WorldPos = pos;
    actor->PendingDestroy = false;

    m_pendingStartIds.push_back(actor->Id);
    GameActor* rawPtr = actor.get();
    m_actors.push_back(std::move(actor));
    return rawPtr;
}

GameActor* ActorRegistry::SpawnScripted(const std::string& requestedName,
                                        const std::string& script,
                                        Vector3 pos)
{
    return Spawn(std::make_unique<GameActor>(), requestedName, script, pos);
}

void ActorRegistry::Destroy(uint64_t id)
{
    GameActor* actor = FindById(id);
    if (actor) {
        actor->Destroy();
    }
}

GameActor* ActorRegistry::FindById(uint64_t id)
{
    for (auto& actor : m_actors) {
        if (actor && actor->Id == id && !actor->PendingDestroy) {
            return actor.get();
        }
    }
    return nullptr;
}

GameActor* ActorRegistry::FindByName(const std::string& name)
{
    for (auto& actor : m_actors) {
        if (actor && actor->Name == name && !actor->PendingDestroy) {
            return actor.get();
        }
    }
    return nullptr;
}

// The actor's Python script hook, after its C++ virtual (a native actor has no script).
void ActorRegistry::PythonHook(GameActor& actor, ActorHook hook, float dt)
{
#ifdef IYI_WITH_PYTHON
    if (m_pythonRuntime && !actor.Script.empty()) m_pythonRuntime->CallActorHook(actor, hook, dt);
#else
    (void)actor; (void)hook; (void)dt;
#endif
}

void ActorRegistry::StartPending()
{
    if (m_pendingStartIds.empty()) return;

    std::vector<uint64_t> toStart;
    toStart.swap(m_pendingStartIds);

    for (uint64_t id : toStart) {
        GameActor* actor = FindById(id);
        if (actor && !actor->PendingDestroy) {
            actor->Start();
            PythonHook(*actor, ActorHook::Start, 0.0f);
        }
    }
}

// Iteration uses index-based loops against a pre-pass size snapshot taken before the loop.
// Actors spawned during a pass are appended to m_actors and will not be visited until the next pass.
// Destruction only sets the PendingDestroy flag; actual removal is deferred to RemoveDestroyed().
// Using a range-based for loop (for (auto& a : m_actors)) here would cause undefined behavior or
// crashes if a lifecycle hook spawns or destroys actors during iteration.
void ActorRegistry::Update(float dt)
{
    const size_t count = m_actors.size();
    for (size_t i = 0; i < count; ++i) {
        GameActor* actor = m_actors[i].get();
        if (!actor || actor->PendingDestroy || !actor->Active) continue;

        actor->Update(dt);
        PythonHook(*actor, ActorHook::Update, dt);
    }
}

void ActorRegistry::FixedUpdate30(float dt)
{
    const size_t count = m_actors.size();
    for (size_t i = 0; i < count; ++i) {
        GameActor* actor = m_actors[i].get();
        if (!actor || actor->PendingDestroy || !actor->Active) continue;

        actor->FixedUpdate30(dt);
        PythonHook(*actor, ActorHook::FixedUpdate30, dt);
    }
}

void ActorRegistry::FixedUpdate60(float dt)
{
    const size_t count = m_actors.size();
    for (size_t i = 0; i < count; ++i) {
        GameActor* actor = m_actors[i].get();
        if (!actor || actor->PendingDestroy || !actor->Active) continue;

        actor->FixedUpdate60(dt);
        PythonHook(*actor, ActorHook::FixedUpdate60, dt);
    }
}

void ActorRegistry::Drawing(bool screenSpacePass)
{
    const size_t count = m_actors.size();
    for (size_t i = 0; i < count; ++i) {
        GameActor* actor = m_actors[i].get();
        if (!actor || actor->PendingDestroy || !actor->Visible || actor->ScreenSpace != screenSpacePass) continue;

        actor->Drawing();
        PythonHook(*actor, ActorHook::Drawing, 0.0f);
    }
}

void ActorRegistry::DispatchReload(const std::vector<std::string>& reloadedScripts)
{
    if (reloadedScripts.empty() || !m_pythonRuntime) return;

    // Snapshot count before iteration pass per Hard Rule 3.
    const size_t count = m_actors.size();

    // 1. Determine whether any reloaded path matches no actor Script (unowned).
    // Ownership and eligibility are two different questions and must stay separate:
    // the ownership test checks every actor with a script (including PendingDestroy)
    // so an actor script is never mistaken for a helper module or entry script.
    //
    // WHY the broadcast exists: An entry in reloadedScripts that matches no actor Script is
    // a helper module or the entry script. No actor owns it directly, but any actor may have
    // imported it, so every live actor is notified to allow UI trees and cached module state to rebuild.
    // ACCEPTED COST: A helper reload rebuilds retained UI and therefore resets transient UI state
    // such as focus and scroll position. That cost was explicitly accepted by the project owner;
    // do not try to avoid it by adding state serialisation, and do not build a module dependency map.
    bool unowned = false;
    for (const auto& reloaded : reloadedScripts) {
        bool matched = false;
        for (size_t i = 0; i < count; ++i) {
            GameActor* actor = m_actors[i].get();
            if (!actor || actor->Script.empty()) continue;

            std::string actorScript = actor->Script;
            for (char& c : actorScript) if (c == '\\') c = '/';

            if (actorScript == reloaded) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            unowned = true;
            break;
        }
    }

    // 2. For each live actor, fire Reload if unowned is true OR that actor Script is in reloadedScripts.
    // Each actor receives the Reload hook AT MOST ONCE per dispatch.
    // Actors that are PendingDestroy or have an empty Script are skipped.
    std::vector<std::string> notifiedActors;
    for (size_t i = 0; i < count; ++i) {
        GameActor* actor = m_actors[i].get();
        if (!actor || actor->PendingDestroy || actor->Script.empty()) continue;

        bool shouldReload = unowned;
        if (!shouldReload) {
            std::string actorScript = actor->Script;
            for (char& c : actorScript) if (c == '\\') c = '/';

            for (const auto& reloaded : reloadedScripts) {
                if (actorScript == reloaded) {
                    shouldReload = true;
                    break;
                }
            }
        }

        if (shouldReload) {
            PythonHook(*actor, ActorHook::Reload, 0.0f);
            notifiedActors.push_back(actor->Name);
        }
    }

    std::string pathsStr;
    for (size_t i = 0; i < reloadedScripts.size(); ++i) {
        if (i > 0) pathsStr += ", ";
        pathsStr += reloadedScripts[i];
    }

    // The reload effect is logged separately from the reload event (emitted in NoteReloaded)
    // because announcing that a script file was reloaded on disk only confirms the compilation step.
    // Logging the actual dispatch effect records whether running gameplay/UI state was updated,
    // making silent failures (e.g. no actors notified) immediately visible to reading agents.
    if (notifiedActors.empty()) {
        LogWrite(LogLevel::Warn, "reload", "0 live actors notified for reload of " + pathsStr +
                 ": reload announced but no actor was notified so the edit has no effect on the running game");
    } else {
        std::string namesStr;
        for (size_t i = 0; i < notifiedActors.size(); ++i) {
            if (i > 0) namesStr += ", ";
            namesStr += "'" + notifiedActors[i] + "'";
        }
        LogWrite(LogLevel::Info, "reload", std::to_string(notifiedActors.size()) +
                 " live actor(s) received Reload hook (" + namesStr + ") for: " + pathsStr);
    }
}

// Removed actors Python still references, shared by every scene's registry so a scene change cannot free
// them. Freeing one while a wrapper exists would turn that retained reference into a use-after-free.
static std::vector<std::unique_ptr<GameActor>> s_retired;
static ActorReferencedFn s_referencedHook = nullptr;
void SetActorReferencedHook(ActorReferencedFn fn) { s_referencedHook = fn; }
static bool Referenced(const std::unique_ptr<GameActor>& actor) { return s_referencedHook && s_referencedHook(actor.get()); }

void ActorRegistry::FreeRetired() { s_retired.clear(); }

void ActorRegistry::RemoveDestroyed()
{
    std::erase_if(s_retired, [](const std::unique_ptr<GameActor>& actor) { return !Referenced(actor); });
    for (auto& actor : m_actors) {
        if (actor && actor->PendingDestroy) {
            m_liveNames.erase(actor->Name);
#ifdef IYI_WITH_PYTHON
            if (m_pythonRuntime) {
                m_pythonRuntime->OnActorDestroyed(actor->Id);
            }
#endif
            actor->Alive = false;
            if (Referenced(actor)) s_retired.push_back(std::move(actor)); // the null left behind is erased below
        }
    }

    std::erase_if(m_actors, [](const std::unique_ptr<GameActor>& actor) {
        return !actor || actor->PendingDestroy;
    });

    std::erase_if(m_pendingStartIds, [this](uint64_t id) {
        return FindById(id) == nullptr;
    });
}

void ActorRegistry::DestroyAll(bool keepPersistent)
{
    for (auto& actor : m_actors) {
        if (actor) {
            if (!keepPersistent || !actor->Persistent) {
                actor->PendingDestroy = true;
            }
        }
    }
    RemoveDestroyed();
}

std::vector<std::unique_ptr<GameActor>> ActorRegistry::TakePersistent()
{
    std::vector<std::unique_ptr<GameActor>> persistent;
    std::vector<std::unique_ptr<GameActor>> remaining;

    for (auto& actor : m_actors) {
        if (actor && actor->Persistent && !actor->PendingDestroy) {
            m_liveNames.erase(actor->Name);
            persistent.push_back(std::move(actor));
        } else if (actor) {
            remaining.push_back(std::move(actor));
        }
    }

    m_actors = std::move(remaining);
    return persistent;
}

void ActorRegistry::RegisterActor(std::unique_ptr<GameActor> actor)
{
    if (!actor) return;
    actor->Name = UniqueName(actor->Name);
    m_actors.push_back(std::move(actor));
}

// A persistent actor spawned during the frame that requests a scene change has not had its
// Start yet: its id sits in the old registry's pending list, which dies with the old scene.
// Carrying the id across is what keeps Start from being silently skipped for it.
void ActorRegistry::AdoptPersistent(ActorRegistry& from)
{
    for (auto& actor : from.TakePersistent()) {
        const auto& pending = from.m_pendingStartIds;
        if (std::find(pending.begin(), pending.end(), actor->Id) != pending.end()) {
            m_pendingStartIds.push_back(actor->Id);
        }
        RegisterActor(std::move(actor));
    }
}

} // namespace IYI
