// Defines the core GameActor entity class representing objects in the game world or HUD.
// Uses plain public data members without getter/setter boilerplate or deep inheritance hierarchies.
// In place of an entity-component-system (ECS) or transform tree, actors hold position, lifecycle flags,
// and an optional path to a Python script that provides behavior. The ScreenSpace flag allows an actor
// to opt out of 2D camera transformations to render as HUD elements.
//
// Connected to:
// - Instantiated, stored, and managed by ActorRegistry.
// - Passed by reference to PythonRuntime for script lifecycle dispatch.
// - Exposed to embedded Python scripts via pybind11 in EngineBindings.cpp.
//
// Component models, transform hierarchies and prefab systems are rejected (ROADMAP.md).

#pragma once

#include <cstdint>
#include <string>
#include "raylib.h"

namespace IYI {

class GameActor
{
public:
    virtual ~GameActor() = default;

    uint64_t    Id   = 0;          // engine-assigned, monotonic, never reused
    std::string Name;              // engine-assigned, unique within its registry
    Vector3     WorldPos { 0.0f, 0.0f, 0.0f };

    bool Active         = true;    // false -> no Update/FixedUpdate calls
    bool Visible        = true;    // false -> no Drawing call
    bool PendingDestroy = false;
    bool Persistent     = false;   // true -> survives change_scene
    bool ScreenSpace    = false;   // true -> Drawing runs outside BeginMode2D (HUD)
    bool Alive          = true;    // false once removed from its registry; Python access then raises ReferenceError

    std::string Script;            // project-relative .py path, "" for pure native actors

    virtual void Start() {}
    virtual void Update(float dt) { (void)dt; }
    virtual void FixedUpdate30(float dt) { (void)dt; }
    virtual void FixedUpdate60(float dt) { (void)dt; }
    virtual void Drawing() {}

    void Destroy() { PendingDestroy = true; }
    bool IsVisible() const { return Visible; }
};

} // namespace IYI
