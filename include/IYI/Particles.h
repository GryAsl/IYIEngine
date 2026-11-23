// Declares particles: soft round puffs (smoke, exhaust, dust) that live in the world, rise or fall,
// grow and fade, drawn as camera-facing quads in raylib's batch. Python emits them one at a time
// (engine.Particles.emit) and calls update and draw once a frame; the pool is a flat vector with a
// fixed capacity, an old particle (the oldest, near enough) giving way when it is full.
//
// They are simulated on the CPU, which is plenty for a few thousand puffs. A GPU version (positions
// computed in a vertex shader from emit time and initial velocity, so update costs nothing) is noted
// in docs/ROADMAP.md for when a game needs far more of them.
//
// Particles are drawn over whatever was drawn before them in the frame: a game draws them after its
// sprite stacks, so a puff behind a building shows in front of it. Short-lived small puffs hide this.
//
// Connected to:
// - RaylibBindings exposes engine.Particles.
// - SpriteStack.h: the same tilted view (view rotation and height scale) places a particle's height.

#pragma once

#include <memory>
#include <vector>
#include "IYI/CellTerrain.h"
#include "raylib.h"

namespace IYI {

struct Particle
{
    Vector3 Position{};          // world x, y; z = height above the ground, world units
    Vector3 Velocity{};          // world units per second
    float Age = 0.0f, Life = 1.0f;   // seconds
    float Size0 = 1.0f, Size1 = 1.0f;   // diameter at birth and at death, world units
    Color Tint = WHITE;          // alpha = the most opaque it gets
    float Drag = 0.0f;           // velocity *= exp(-Drag * dt)
    float Lift = 0.0f;           // added to the vertical velocity per second (negative: falls)
};

// Game side, CPU only: the draw is recorded with a copy of Live (FramePacket.h), and the one puff
// texture every pool draws with belongs to the render side.
struct ParticlePool
{
    std::vector<Particle> Live;
    size_t Capacity = 4096;
    size_t Next = 0;             // which particle a full pool replaces next

    ParticlePool() = default;
    explicit ParticlePool(size_t capacity) : Capacity(capacity) { Live.reserve(capacity); }
    void Clear() { Live.clear(); Next = 0; }
};

void EmitParticle(ParticlePool& pool, const Particle& p);
// Ages and moves every particle and drops the dead ones. A particle never sinks below the ground.
void UpdateParticles(ParticlePool& pool, float dt);
// How a particle looks at its age: (diameter, alpha 0..1). It fades in over the first tenth of its
// life and out over the rest.
Vector2 ParticleLook(const Particle& p);
// World pass: every particle as a soft disc at its ground point lifted toward screen-up by
// z * heightScale, made round on screen for a camera squashed by a pitch with cot = heightScale.
// zUp (a 3D camera): at its real height instead, plus the ground under it when given, facing the
// camera (its right and up from the model-view; viewRotation and heightScale are not used); drawn
// without writing depth, so overlapping puffs never cut each other.
// Render side. The puff texture (a soft white disc) is made by the first draw.
void DrawParticles(const std::vector<Particle>& live, float viewRotation, float heightScale, bool zUp = false,
                   const CellTerrain* ground = nullptr);
// Render side, at shutdown before the window closes: frees the puff texture.
void UnloadParticleTexture();

namespace Api {
// Game side: records DrawParticles with a copy of the pool's particles (world pass).
void DrawParticles(const ParticlePool& pool, float viewRotation = 0.0f, float heightScale = 0.0f, bool zUp = false,
                   std::shared_ptr<CellTerrain> ground = nullptr);
} // namespace Api

} // namespace IYI
