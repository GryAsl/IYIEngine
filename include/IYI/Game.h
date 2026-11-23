// Declares the entry point of a native game: a game written fully in C++ against the engine, with no
// Python in its runtime (SPEC §2c). The game's own exe has a main() that calls RunGame with its Game
// object; the engine runs exactly as IYIEngine.exe does, with Game::Start in place of the entry
// script's GameStart, registered scene functions in place of scene scripts and GameActor subclasses
// in place of actor scripts. Everything runs on the game thread (SPEC §8a), like Python.
//
// Connected to:
// - Application (Initialize takes the Game; StartGame, scene changes and the agent channel call it).
// - IYI/Api.h: the C++ form of the engine.* functions a game calls (one implementation with Python).

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include "raylib.h"
#include "IYI/ActorRegistry.h"
#include "IYI/GameActor.h"

namespace IYI {

struct Game
{
    virtual ~Game() = default;
    virtual void Start() = 0;   // GameStart: settings, RegisterScene, the first Api::ChangeScene
    // Development builds (agent channel, SPEC §3.1): the request as JSON text in, the reply's "game"
    // as JSON text out. "null" (the default) is what a Python game without AgentRequest answers.
    virtual std::string AgentRequest(const std::string& requestJson) { (void)requestJson; return "null"; }
};

// main() of a native game: Initialize (with the same command line as IYIEngine.exe), Run, Shutdown.
// IYIEngine.exe calls it with no game (Python). Returns the process exit code.
int RunGame(int argc, char* argv[], std::unique_ptr<Game> game);

// A scene of a native game: Api::ChangeScene(name) runs `sceneStart` (the C++ SceneStart) at the end
// of the step, after the non-persistent actors are cleared.
void RegisterScene(const std::string& name, std::function<void()> sceneStart);

// The current scene's actors (Spawn, FindActor, and the plain ActorRegistry calls).
ActorRegistry& SceneActors();

// A native actor: a GameActor subclass whose virtuals are its hooks and whose members are its state.
template <class T, class... Args>
T* Spawn(const std::string& name, Vector3 pos = { 0.0f, 0.0f, 0.0f }, Args&&... args)
{
    return static_cast<T*>(SceneActors().Spawn(std::make_unique<T>(std::forward<Args>(args)...), name, "", pos));
}

// The live actor of that name if it is a T (nullptr otherwise).
template <class T>
T* FindActor(const std::string& name)
{
    return dynamic_cast<T*>(SceneActors().FindByName(name));
}

} // namespace IYI
