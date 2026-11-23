// Implements particles (Particles.h): a flat pool, CPU update, one quad per particle in raylib's batch.

#include "IYI/Particles.h"

#include <algorithm>
#include <cmath>
#include "IYI/FramePacket.h"
#include "IYI/Profiler.h"
#include "rlgl.h"

namespace IYI {

void EmitParticle(ParticlePool& pool, const Particle& p)
{
    if (pool.Capacity == 0 || !(p.Life > 0.0f)) return;
    if (pool.Live.size() < pool.Capacity) {
        pool.Live.push_back(p);
        return;
    }
    // Full: replace in a ring, so the ones replaced are the oldest, near enough.
    pool.Live[pool.Next] = p;
    pool.Next = (pool.Next + 1) % pool.Live.size();
}

void UpdateParticles(ParticlePool& pool, float dt)
{
    if (dt <= 0.0f) return;
    for (Particle& p : pool.Live) {
        p.Age += dt;
        p.Velocity.z += p.Lift * dt;
        const float keep = std::exp(-p.Drag * dt);
        p.Velocity.x *= keep;
        p.Velocity.y *= keep;
        p.Velocity.z *= keep;
        p.Position.x += p.Velocity.x * dt;
        p.Position.y += p.Velocity.y * dt;
        p.Position.z = std::max(0.0f, p.Position.z + p.Velocity.z * dt);
    }
    const size_t before = pool.Live.size();
    pool.Live.erase(std::remove_if(pool.Live.begin(), pool.Live.end(), [](const Particle& p) { return p.Age >= p.Life; }),
                    pool.Live.end());
    if (pool.Live.size() != before) pool.Next = 0;   // order is kept; the oldest are first again
}

Vector2 ParticleLook(const Particle& p)
{
    const float t = std::clamp(p.Age / p.Life, 0.0f, 1.0f);
    const float in = std::min(1.0f, t / 0.1f);
    const float out = 1.0f - std::max(0.0f, (t - 0.1f) / 0.9f);
    return Vector2{ p.Size0 + (p.Size1 - p.Size0) * t, in * out * out };
}

namespace {
Texture2D s_puff{};   // render side: the disc every pool draws with
}

void DrawParticles(const std::vector<Particle>& live, float viewRotation, float heightScale, bool zUp, const CellTerrain* ground)
{
    if (live.empty()) return;
    ProfileScope scope("particles");
    ProfileCount("particles", static_cast<double>(live.size()));
    if (s_puff.id == 0) {
        // White to transparent white: fading to BLANK (transparent black) greys the edges into rings.
        Image disc = GenImageGradientRadial(64, 64, 0.0f, WHITE, Color{ 255, 255, 255, 0 });
        s_puff = LoadTextureFromImage(disc);
        UnloadImage(disc);
        SetTextureFilter(s_puff, TEXTURE_FILTER_BILINEAR);
    }
    const float r = viewRotation * DEG2RAD;
    Vector2 right{ std::cos(r), -std::sin(r) }, up{ -std::sin(r), -std::cos(r) };
    const float stretch = std::sqrt(1.0f + heightScale * heightScale);   // 1 / sin(pitch): undo the squash
    // zUp: the quad faces the camera: its right and up are the camera's own, the model-view's first
    // two rows (it maps world points, so those rows are world directions; right is level, no roll).
    Vector3 camUp{};
    if (zUp) {
        const Matrix mv = rlGetMatrixModelview();
        right = Vector2{ mv.m0, mv.m4 };
        camUp = Vector3{ mv.m1, mv.m5, mv.m9 };
        rlDrawRenderBatchActive();
        rlDisableDepthMask();
    }
    rlSetTexture(s_puff.id);
    for (const Particle& p : live) {
        const Vector2 look = ParticleLook(p);
        const unsigned char a = static_cast<unsigned char>(std::lround(p.Tint.a * std::clamp(look.y, 0.0f, 1.0f)));
        if (a == 0) continue;
        if (zUp) {
            const float hw = look.x * 0.5f;
            const Vector3 c{ p.Position.x, p.Position.y, p.Position.z + (ground ? CellTerrainHeight(*ground, p.Position.x, p.Position.y) : 0.0f) };
            const Vector3 corner[4] = {
                { c.x - right.x * hw + camUp.x * hw, c.y - right.y * hw + camUp.y * hw, c.z + camUp.z * hw },
                { c.x - right.x * hw - camUp.x * hw, c.y - right.y * hw - camUp.y * hw, c.z - camUp.z * hw },
                { c.x + right.x * hw - camUp.x * hw, c.y + right.y * hw - camUp.y * hw, c.z - camUp.z * hw },
                { c.x + right.x * hw + camUp.x * hw, c.y + right.y * hw + camUp.y * hw, c.z + camUp.z * hw },
            };
            const Vector2 uv[4] = { { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f } };
            rlBegin(RL_QUADS);
            rlColor4ub(p.Tint.r, p.Tint.g, p.Tint.b, a);
            rlNormal3f(0.0f, 0.0f, 1.0f);
            for (int k = 0; k < 4; ++k) {
                rlTexCoord2f(uv[k].x, uv[k].y);
                rlVertex3f(corner[k].x, corner[k].y, corner[k].z);
            }
            rlEnd();
            continue;
        }
        const float lift = p.Position.z * heightScale;
        const float cx = p.Position.x + up.x * lift, cy = p.Position.y + up.y * lift;
        const float hw = look.x * 0.5f, hh = look.x * 0.5f * stretch;
        const Vector2 corner[4] = {
            { cx - right.x * hw + up.x * hh, cy - right.y * hw + up.y * hh },   // top-left
            { cx - right.x * hw - up.x * hh, cy - right.y * hw - up.y * hh },   // bottom-left
            { cx + right.x * hw - up.x * hh, cy + right.y * hw - up.y * hh },   // bottom-right
            { cx + right.x * hw + up.x * hh, cy + right.y * hw + up.y * hh },   // top-right
        };
        const Vector2 uv[4] = { { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 1.0f, 0.0f } };
        rlBegin(RL_QUADS);
        rlColor4ub(p.Tint.r, p.Tint.g, p.Tint.b, a);
        rlNormal3f(0.0f, 0.0f, 1.0f);
        for (int k = 0; k < 4; ++k) {
            rlTexCoord2f(uv[k].x, uv[k].y);
            rlVertex2f(corner[k].x, corner[k].y);
        }
        rlEnd();
    }
    rlSetTexture(0);
    if (zUp) {
        rlDrawRenderBatchActive();
        rlEnableDepthMask();
    }
}

void UnloadParticleTexture()
{
    if (s_puff.id != 0 && IsWindowReady()) UnloadTexture(s_puff);   // no GL context left: nothing to free
    s_puff = Texture2D{};
}

namespace Api {

void DrawParticles(const ParticlePool& pool, float viewRotation, float heightScale, bool zUp, std::shared_ptr<CellTerrain> ground)
{
    Record([live = pool.Live, viewRotation, heightScale, zUp, ground]() {
        IYI::DrawParticles(live, viewRotation, heightScale, zUp, ground.get());
    });
}

} // namespace Api

} // namespace IYI
