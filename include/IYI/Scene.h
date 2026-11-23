// Declares the lightweight Scene container representing the active gameplay environment.
// Consists solely of an ActorRegistry and an optional scene script path. The engine deliberately avoids
// scene graph hierarchies, node trees, visual editor metadata, or built-in binary scene serialization;
// scenes are simply Python scripts whose SceneStart() function programmatically populates the registry.
//
// Connected to:
// - Owned by Application as m_currentScene.
// - Created fresh during engine initialization and upon deferred scene transitions in Application.
// - Holds the primary ActorRegistry instance for the active level.
//
// Future evolution (per ROADMAP.md):
// Scene management remains intentionally bare. Any persistent game state or save systems are intended
// to be handled game-side in Python scripts using engine.project_path().

#pragma once

#include <string>
#include "IYI/ActorRegistry.h"

namespace IYI {

class Scene
{
public:
    ActorRegistry Actors;
    std::string   ScriptPath;   // "" for the implicit initial scene
};

} // namespace IYI
