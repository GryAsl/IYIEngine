// Implements sprite-stack packing and the batched stack draw (SpriteStack.h).
//
// Why one call for the whole list: drawn one stack at a time from Python, every layer crossed the
// binding as a texture list and a lit stack needed its own shader uniforms, so each object flushed
// raylib's batch several times. Here every layer of every stack is a quad emitted exactly the way
// DrawTexturePro emits one (rlBegin/rlEnd per quad, so rlgl's own batch limit checks apply), and
// anything per object a shader needs travels in the vertex data instead of uniforms.

#include "IYI/SpriteStack.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include "IYI/FramePacket.h"
#include "IYI/Profiler.h"
#include "raymath.h"
#include "rlgl.h"

namespace IYI {

namespace {

constexpr int kMaxAtlasSide = 8192;   // texels; 1 texel of padding between layers on both axes

bool s_culling = true;
bool s_instancing = true;

// Where an item's layers go in a pass: its footprint corners round the pivot (rotated and scaled,
// DrawTexturePro's math) and the world direction its layers climb in. The one place that geometry
// is worked out, for the draw and for the cull.
struct Placement
{
    Vector2 Corner[4];   // top-left, bottom-left, bottom-right, top-right
    float UpX, UpY;
    float TiltX, TiltY;  // ZUp: world units a layer moves sideways per unit of height above the ground
};

// The wind on an item: (gust 0.1..1, flutter -1..1). Gusts roll across the world at ~100 units/s.
Vector2 SwayWave(const SpriteStackItem& item, const SpriteStackPass& pass)
{
    const float along = item.X * pass.SwayDir.x + item.Y * pass.SwayDir.y;
    return Vector2{ 0.55f + 0.45f * std::sin(pass.SwayTime * 0.8f - along * 0.008f),
                    std::sin(pass.SwayTime * 2.1f + item.X * 0.37f + item.Y * 0.61f) };
}

// ZUp: the stack really tilts. Its lean leans it toward screen-left (as the 2D lean turns its climb
// there); the wind's sway leans it downwind, fluttering across.
Vector2 Tilt(const SpriteStackItem& item, const SpriteStackPass& pass)
{
    const float r = pass.ViewRotation * DEG2RAD;
    const float lean = std::tan(item.Lean * DEG2RAD);
    Vector2 t{ -std::cos(r) * lean, std::sin(r) * lean };    // screen-right on the ground is (cos r, -sin r)
    if (item.Sway != 0.0f && pass.SwayStrength > 0.0f) {
        const Vector2 wave = SwayWave(item, pass);
        const float k = item.Sway * pass.SwayStrength * wave.x * DEG2RAD;   // small angles: tan = angle
        const float down = 0.8f * k, across = 0.35f * wave.y * k;
        t.x += pass.SwayDir.x * down - pass.SwayDir.y * across;
        t.y += pass.SwayDir.y * down + pass.SwayDir.x * across;
    }
    return t;
}

Placement Place(const SpriteStackItem& item, const SpriteStackPass& pass)
{
    const float lean = pass.ZUp ? 0.0f : pass.ViewRotation + item.Lean + SwayLean(item, pass);
    const Vector2 tilt = pass.ZUp ? Tilt(item, pass) : Vector2{ 0.0f, 0.0f };
    const SpriteStack* stack = item.Stack;
    const float s = item.Scale;
    const float cosR = std::cos(item.Rotation * DEG2RAD);
    const float sinR = std::sin(item.Rotation * DEG2RAD);
    const float x0 = -stack->Origin.x * s, y0 = -stack->Origin.y * s;
    const float x1 = x0 + stack->Width * s, y1 = y0 + stack->Height * s;
    return Placement{
        { { x0 * cosR - y0 * sinR, x0 * sinR + y0 * cosR },
          { x0 * cosR - y1 * sinR, x0 * sinR + y1 * cosR },
          { x1 * cosR - y1 * sinR, x1 * sinR + y1 * cosR },
          { x1 * cosR - y0 * sinR, x1 * sinR + y0 * cosR } },
        -std::sin(lean * DEG2RAD),   // world direction shown as screen-up
        -std::cos(lean * DEG2RAD),
        tilt.x,
        tilt.y,
    };
}

// Height of layer `layer` above the ground (plus `drop` world units), in world units.
float LayerHeight(const SpriteStackItem& item, const SpriteStackPass& pass, int layer, float drop = 0.0f)
{
    return item.Ground + item.Z + (static_cast<float>(layer) - item.Stack->Origin.z) * pass.LayerHeight * item.Stack->LayerStep * item.Scale + drop;
}

// Where the point `corner` of a layer `h` world units up is drawn: lifted along the climb
// direction on the ground plane, or (ZUp) at height h, moved by the tilt for its height above
// the ground.
Vector3 LayerPoint(const SpriteStackItem& item, const SpriteStackPass& pass, const Placement& p, float h, Vector2 corner)
{
    const float baseX = item.X + pass.Offset.x + corner.x, baseY = item.Y + pass.Offset.y + corner.y;
    if (!pass.ZUp) {
        const float lift = h * pass.HeightScale;
        return Vector3{ baseX + p.UpX * lift, baseY + p.UpY * lift, 0.0f };
    }
    const float above = h - item.Ground;
    return Vector3{ baseX + p.TiltX * above, baseY + p.TiltY * above, h };
}

// Every how many layers an item's quads are drawn (LodPixels): its layer spacing on the target, in
// pixels, measured at the pivot and two opposite corners (a perspective view spreads them unevenly).
int LodStep(const SpriteStackItem& item, const SpriteStackPass& pass, const Placement& p, const Matrix& mvp)
{
    if (pass.LodPixels <= 0.0f || item.Stack->Layers < 3) return 1;
    const float halfW = 0.5f * static_cast<float>(rlGetFramebufferWidth());
    const float halfH = 0.5f * static_cast<float>(rlGetFramebufferHeight());
    auto pixel = [&](Vector3 v) {
        const float w = mvp.m3 * v.x + mvp.m7 * v.y + mvp.m11 * v.z + mvp.m15;
        return Vector2{ (mvp.m0 * v.x + mvp.m4 * v.y + mvp.m8 * v.z + mvp.m12) / w * halfW,
                        (mvp.m1 * v.x + mvp.m5 * v.y + mvp.m9 * v.z + mvp.m13) / w * halfH };
    };
    const float h0 = LayerHeight(item, pass, 0), h1 = LayerHeight(item, pass, 1);
    float spacing = 0.0f;
    for (const Vector2 c : { Vector2{ 0.0f, 0.0f }, p.Corner[0], p.Corner[2] }) {
        spacing = std::max(spacing, Vector2Distance(pixel(LayerPoint(item, pass, p, h0, c)), pixel(LayerPoint(item, pass, p, h1, c))));
    }
    return std::clamp(static_cast<int>(pass.LodPixels / std::max(spacing, 1e-4f)), 1, kMaxLodStep);
}

unsigned char Scale255(unsigned char c, float k)
{
    return static_cast<unsigned char>(std::lround(std::clamp(c * k, 0.0f, 255.0f)));
}

} // namespace

BuildState BuildLayer(float build, int layer, int layers)
{
    if (build >= 1.0f) return {};
    // The build front sweeps from below the first layer to kBuildFlight layers past the last, so
    // build 0 shows nothing and 1 the whole stack, and every layer takes the same time to land.
    const float front = std::max(build, 0.0f) * (static_cast<float>(layers) + kBuildFlight);
    const float t = std::clamp((front - static_cast<float>(layer)) / kBuildFlight, 0.0f, 1.0f);
    return BuildState{ std::min(1.0f, t * 2.5f), (1.0f - t) * (1.0f - t) * kBuildDrop };   // drops fast, then settles
}

float SwayLean(const SpriteStackItem& item, const SpriteStackPass& pass)
{
    if (item.Sway == 0.0f || pass.SwayStrength <= 0.0f) return 0.0f;
    const Vector2 wave = SwayWave(item, pass);
    const float gust = wave.x, flutter = wave.y;
    const float r = pass.ViewRotation * DEG2RAD;
    const float side = pass.SwayDir.x * std::cos(r) - pass.SwayDir.y * std::sin(r);         // wind along screen-right
    // A positive lean turns the climb toward screen-left, so downwind is -side.
    return item.Sway * pass.SwayStrength * gust * (0.35f * flutter - 0.8f * side);
}

namespace {

bool OffTarget(const SpriteStackItem& item, const SpriteStackPass& pass, const Placement& p, const Matrix& mvp)
{
    // Off when every corner is outside one side of the view: x < -w, x > w, y < -w, y > w or w < 0
    // (behind a perspective camera). An orthographic view has w = 1 everywhere.
    bool left = true, right = true, below = true, above = true, behind = true;
    for (const int layer : { 0, item.Stack->Layers - 1 }) {
        const float h = LayerHeight(item, pass, layer);
        for (const Vector2& c : p.Corner) {
            const Vector3 v = LayerPoint(item, pass, p, h, c);
            const float x = mvp.m0 * v.x + mvp.m4 * v.y + mvp.m8 * v.z + mvp.m12;
            const float y = mvp.m1 * v.x + mvp.m5 * v.y + mvp.m9 * v.z + mvp.m13;
            const float w = mvp.m3 * v.x + mvp.m7 * v.y + mvp.m11 * v.z + mvp.m15;
            left = left && x < -w; right = right && x > w;
            below = below && y < -w; above = above && y > w;
            behind = behind && w < 0.0f;
        }
    }
    return left || right || below || above || behind;
}

} // namespace

bool SpriteStackOffTarget(const SpriteStackItem& item, const SpriteStackPass& pass, const Matrix& mvp)
{
    return OffTarget(item, pass, Place(item, pass), mvp);
}

Rectangle SpriteStack::LayerRect(int layer) const
{
    const int column = layer / PerColumn;
    const int row = layer % PerColumn;
    return Rectangle{ static_cast<float>(AtlasX + column * (Width + 1)), static_cast<float>(AtlasY + row * (Height + 1)),
                      static_cast<float>(Width), static_cast<float>(Height) };
}

bool BuildSpriteStack(const std::vector<Image>& layers, SpriteStack& out, Image& atlas)
{
    if (layers.empty()) return false;
    const int w = layers[0].width;
    const int h = layers[0].height;
    for (const Image& layer : layers) {
        if (layer.width != w || layer.height != h || layer.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) return false;
    }
    const int count = static_cast<int>(layers.size());
    // Near square, so blocks pack well into the shared pages: columns ~ sqrt(count * h / w).
    const int square = static_cast<int>(std::ceil(std::sqrt(static_cast<float>(count) * (h + 1) / (w + 1))));
    const int perColumn = std::clamp((count + square - 1) / square, 1, std::max(1, (kMaxAtlasSide + 1) / (h + 1)));
    const int columns = (count + perColumn - 1) / perColumn;

    atlas = GenImageColor(columns * (w + 1) - 1, perColumn * (h + 1) - 1, BLANK);
    auto* dst = static_cast<unsigned char*>(atlas.data);
    for (int i = 0; i < count; ++i) {
        const int x0 = (i / perColumn) * (w + 1);
        const int y0 = (i % perColumn) * (h + 1);
        const auto* src = static_cast<const unsigned char*>(layers[i].data);
        for (int y = 0; y < h; ++y) {
            std::memcpy(dst + (static_cast<size_t>(y0 + y) * atlas.width + x0) * 4, src + static_cast<size_t>(y) * w * 4,
                        static_cast<size_t>(w) * 4);
        }
    }
    out.Texture = Texture2D{ 0, atlas.width, atlas.height, 1, atlas.format };
    out.Layers = count;
    out.Width = w;
    out.Height = h;
    out.PerColumn = perColumn;
    out.Origin = Vector3{ w * 0.5f, h * 0.5f, 0.0f };
    return true;
}

void DrawSpriteStacks(std::vector<SpriteStackItem>& items, const SpriteStackPass& pass)
{
    // Cull first, so only what can show is sorted and drawn. rlgl's matrices are what the quads below
    // go through: the camera in the world pass, a buffer's own space in texture mode. (Nothing pushes
    // an rlgl transform around a stack draw, so model-view and projection are the whole story.)
    if (pass.ZUp && pass.Ground) {
        for (SpriteStackItem& item : items) item.Ground = CellTerrainHeight(*pass.Ground, item.X, item.Y);
    }
    const Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    ProfileCount("stacks_in", static_cast<double>(items.size()));
    {
        ProfileScope scope("cull");
        items.erase(std::remove_if(items.begin(), items.end(), [&](const SpriteStackItem& item) {
            return !item.Stack || item.Stack->Layers == 0 || item.Z > pass.MaxZ ||
                   (s_culling && SpriteStackOffTarget(item, pass, mvp));
        }), items.end());
    }

    // ZUp: far to near by the camera's own depth (clip z grows away from it, in either projection),
    // so the depth test leaves the blended edges of nearer stacks over what lies behind them.
    if (pass.Sort && pass.ZUp) {
        ProfileScope scope("sort");
        auto depth = [&](const SpriteStackItem& item) {
            return mvp.m2 * item.X + mvp.m6 * item.Y + mvp.m10 * (item.Ground + item.Z) + mvp.m14;
        };
        std::stable_sort(items.begin(), items.end(),
                         [&](const SpriteStackItem& a, const SpriteStackItem& b) { return depth(a) > depth(b); });
    }
    // Depth along the view for an orthographic camera pitched down by atan(1 / HeightScale):
    // screen-down ground distance * cos(pitch) + height * sin(pitch). The small ground term keeps a
    // stable order when looking straight down (HeightScale 0).
    else if (pass.Sort) {
        ProfileScope scope("sort");
        const float r = pass.ViewRotation * DEG2RAD;
        const float norm = 1.0f / std::sqrt(1.0f + pass.HeightScale * pass.HeightScale);
        const float groundWeight = pass.HeightScale * norm + 1e-3f;
        const float heightWeight = norm;
        const float sr = std::sin(r), cr = std::cos(r);
        auto depth = [&](const SpriteStackItem& item) {
            return (item.X * sr + item.Y * cr) * groundWeight + (item.Ground + item.Z) * heightWeight;
        };
        std::stable_sort(items.begin(), items.end(),
                         [&](const SpriteStackItem& a, const SpriteStackItem& b) { return depth(a) < depth(b); });
    }

    ProfileScope scope("emit");
    double quads = 0.0;   // drawn, after the LOD
    ProfileCount("stacks", static_cast<double>(items.size()));
    // Sub-sections, timed per stack and added once (ProfileAdd): place (its corners), flush (rlgl
    // uploading and drawing its batch: here, never mid-stack), layers (building its quads). Only
    // with ProfileDetail (F3 open): three clock reads per stack are not free.
    const bool detail = ProfileDetail();
    double placeMs = 0.0, flushMs = 0.0, layersMs = 0.0, flushes = 0.0, textures = 0.0;
    unsigned int texture = 0;
    double t = detail ? ProfileNowMs() : 0.0;
    for (const SpriteStackItem& item : items) {
        const SpriteStack* stack = item.Stack;
        const Placement place = Place(item, pass);
        const Vector2* corner = place.Corner;
        const int step = LodStep(item, pass, place, mvp);
        const bool grounded = pass.GroundInNormal && item.Z <= pass.GroundMaxZ;
        const Color tint = pass.OverrideTint ? pass.Tint : item.Tint;

        const float texW = static_cast<float>(stack->Texture.width);
        const float texH = static_cast<float>(stack->Texture.height);
        const double placed = detail ? ProfileNowMs() : 0.0;
        textures += stack->Texture.id != texture;   // a new draw call; every 256 of them flush the batch
        texture = stack->Texture.id;
        rlSetTexture(stack->Texture.id);
        flushes += rlCheckRenderBatchLimit(4 * stack->Layers);   // room for the whole stack
        const double flushed = detail ? ProfileNowMs() : 0.0;
        // ZUp layers use explicit depth and the same packed texture, so they share one quad
        // stream. In the 2D path rlEnd advances rlgl's layer depth, so keep its old cadence.
        if (pass.ZUp) rlBegin(RL_QUADS);
        for (int i = 0; i < stack->Layers; ++i) {
            // LOD: skipped layers lie within a pixel of the drawn ones; the top `step` always draw, so a
            // roof slab under a one-layer parapet is never skipped.
            if (i % step != 0 && i < stack->Layers - step) continue;
            const BuildState build = BuildLayer(item.Build, i, stack->Layers);
            if (build.Alpha <= 0.0f) continue;
            quads += 1.0;
            const float h = LayerHeight(item, pass, i, build.Drop * pass.LayerHeight * stack->LayerStep * item.Scale);
            Color c = tint;
            c.a = Scale255(c.a, build.Alpha);
            if (pass.BaseShade > 0.0f) {
                const float below = std::max(0.0f, 1.0f - (h - item.Ground) / pass.ShadeReach);
                const float k = 1.0f - pass.BaseShade * below * below;
                c.r = Scale255(c.r, k);
                c.g = Scale255(c.g, k);
                c.b = Scale255(c.b, k);
            }
            const Rectangle src = stack->LayerRect(i);
            const float u0 = src.x / texW, v0 = src.y / texH;
            const float u1 = (src.x + src.width) / texW, v1 = (src.y + src.height) / texH;
            const Vector2 uv[4] = { { u0, v0 }, { u0, v1 }, { u1, v1 }, { u1, v0 } };

            if (!pass.ZUp) rlBegin(RL_QUADS);
            rlColor4ub(c.r, c.g, c.b, c.a);
            for (int k = 0; k < 4; ++k) {
                if (grounded) {
                    rlNormal3f(item.X + corner[k].x, item.Y + corner[k].y, kGroundNormalScale);
                } else if (pass.GroundInNormal) {
                    rlNormal3f(0.0f, 0.0f, -1.0f);
                } else {
                    rlNormal3f(0.0f, 0.0f, 1.0f);
                }
                rlTexCoord2f(uv[k].x, uv[k].y);
                const Vector3 v = LayerPoint(item, pass, place, h, corner[k]);
                if (pass.ZUp) rlVertex3f(v.x, v.y, v.z);
                else rlVertex2f(v.x, v.y);
            }
            if (!pass.ZUp) rlEnd();
        }
        if (pass.ZUp) rlEnd();
        rlSetTexture(0);
        const double done = detail ? ProfileNowMs() : 0.0;
        placeMs += placed - t;
        flushMs += flushed - placed;
        layersMs += done - flushed;
        t = done;
    }
    if (detail) {
        const int n = static_cast<int>(items.size());
        ProfileAdd("place", placeMs, n);
        ProfileAdd("flush", flushMs, n);
        ProfileAdd("layers", layersMs, n);
    }
    ProfileCount("quads", quads);
    ProfileCount("textures", textures);
    ProfileCount("flushes", flushes);
}

// A stack's layers in its own space (DrawSpriteStacksInstanced): corners in texels round the pivot, z
// the layer index; an instance record rotates, scales, raises and tilts them (lit_sprite.vs).
struct SpriteStackMesh
{
    unsigned int Vao = 0, Vbo[2] = { 0, 0 }, Ebo = 0;
    int Indices = 0;
    int LodFirst[4] = {}, LodCount[4] = {};   // indices of the layers LOD step 1, 2, 4, 8 draws (LodLevel)

    ~SpriteStackMesh()
    {
        if (!IsWindowReady()) return;          // after CloseWindow the GL context went with them
        rlUnloadVertexArray(Vao);
        rlUnloadVertexBuffer(Vbo[0]);
        rlUnloadVertexBuffer(Vbo[1]);
        rlUnloadVertexBuffer(Ebo);
    }
};

namespace {

constexpr int kMaxMeshLayers = 16384;            // 4 vertices a layer, 16-bit indices
constexpr unsigned int kInstanceAttrib = 10;     // lit_sprite.vs: instance inputs at locations 10..14
constexpr int kLodLevels = 4;                    // LOD steps 1, 2, 4, 8 (kMaxLodStep)

// LodStep's step rounded down to a power of two: which of a mesh's index ranges draws an instance.
int LodLevel(int step)
{
    int level = 0;
    while (level + 1 < kLodLevels && (2 << level) <= step) ++level;
    return level;
}

// One item as lit_sprite.vs reads it.
struct StackInstance
{
    float Place[4];          // x, y, ground height, z above the ground
    float Turn[4];           // cos(rotation) * scale, sin(rotation) * scale, world units per layer, layers
    float Motion[4];         // lean (degrees), sway (degrees), build 0..1, pivot layer (Origin.z)
    unsigned char Tint[4];
    float Damage;
};

std::shared_ptr<SpriteStackMesh> BuildStackMesh(const SpriteStack& s)
{
    const int n = s.Layers;
    std::vector<float> pos(12 * static_cast<size_t>(n)), uv(8 * static_cast<size_t>(n));
    std::vector<unsigned short> idx;
    const float x0 = -s.Origin.x, y0 = -s.Origin.y, x1 = x0 + s.Width, y1 = y0 + s.Height;
    const float texW = static_cast<float>(s.Texture.width), texH = static_cast<float>(s.Texture.height);
    for (int i = 0; i < n; ++i) {
        const Rectangle r = s.LayerRect(i);
        const float u0 = r.x / texW, v0 = r.y / texH, u1 = (r.x + r.width) / texW, v1 = (r.y + r.height) / texH;
        const float z = static_cast<float>(i);
        const float p[12] = { x0, y0, z, x0, y1, z, x1, y1, z, x1, y0, z };   // DrawSpriteStacks' corner order
        const float c[8] = { u0, v0, u0, v1, u1, v1, u1, v0 };
        std::copy(p, p + 12, pos.begin() + 12 * i);
        std::copy(c, c + 8, uv.begin() + 8 * i);
    }
    auto m = RenderOwned(new SpriteStackMesh);
    for (int level = 0; level < kLodLevels; ++level) {
        const int step = 1 << level;
        m->LodFirst[level] = static_cast<int>(idx.size());
        for (int i = 0; i < n; ++i) {
            if (i % step != 0 && i < n - step) continue;   // the top `step` always draw (DrawSpriteStacks)
            const auto b = static_cast<unsigned short>(4 * i);
            idx.insert(idx.end(), { b, static_cast<unsigned short>(b + 1), static_cast<unsigned short>(b + 2),
                                    b, static_cast<unsigned short>(b + 2), static_cast<unsigned short>(b + 3) });
        }
        m->LodCount[level] = static_cast<int>(idx.size()) - m->LodFirst[level];
    }
    m->Vao = rlLoadVertexArray();
    rlEnableVertexArray(m->Vao);
    m->Vbo[0] = rlLoadVertexBuffer(pos.data(), static_cast<int>(pos.size() * sizeof(float)), false);
    rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION, 3, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION);
    m->Vbo[1] = rlLoadVertexBuffer(uv.data(), static_cast<int>(uv.size() * sizeof(float)), false);
    rlSetVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD, 2, RL_FLOAT, false, 0, 0);
    rlEnableVertexAttribute(RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD);
    m->Ebo = rlLoadVertexBufferElement(idx.data(), static_cast<int>(idx.size() * sizeof(unsigned short)), false);
    rlDisableVertexArray();
    m->Indices = m->LodCount[0];
    return m;
}

// What the instanced path can draw (the rest goes to DrawSpriteStacks).
bool Instanceable(const SpriteStackItem& item, const SpriteStackPass& pass)
{
    const SpriteStack* stack = item.Stack;
    const Color tint = pass.OverrideTint ? pass.Tint : item.Tint;
    return stack && stack->Layers > 0 && stack->Layers <= kMaxMeshLayers && stack->Texture.id != 0 && item.Z <= pass.MaxZ &&
           tint.a == 255;
}

StackInstance Record(const SpriteStackItem& item, const SpriteStackPass& pass)
{
    const SpriteStack& stack = *item.Stack;
    const Color tint = pass.OverrideTint ? pass.Tint : item.Tint;
    const float r = item.Rotation * DEG2RAD;
    return StackInstance{
        { item.X, item.Y, item.Ground, item.Z },
        { std::cos(r) * item.Scale, std::sin(r) * item.Scale, pass.LayerHeight * stack.LayerStep * item.Scale,
          static_cast<float>(stack.Layers) },
        { item.Lean, item.Sway, item.Build, stack.Origin.z },
        { tint.r, tint.g, tint.b, tint.a },
        item.Damage,
    };
}

// A pass the static chunks serve: the plain camera pass (a shadow pass tints or cuts items itself).
bool Chunkable(const SpriteStackPass& pass)
{
    return s_instancing && pass.ZUp && pass.Offset.x == 0.0f && pass.Offset.y == 0.0f && !pass.OverrideTint &&
           std::isinf(pass.MaxZ);
}

const Vector3 kCorners[8] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 } };

Vector3 Corner(const BoundingBox& b, int i)
{
    return Vector3{ kCorners[i].x ? b.max.x : b.min.x, kCorners[i].y ? b.max.y : b.min.y, kCorners[i].z ? b.max.z : b.min.z };
}

// OffTarget for a box: every corner outside one side of clip space. inside: every corner in it.
bool BoxOffTarget(const BoundingBox& b, const Matrix& mvp, bool* inside = nullptr)
{
    bool left = true, right = true, below = true, above = true, behind = true, in = true;
    for (int i = 0; i < 8; ++i) {
        const Vector3 v = Corner(b, i);
        const float x = mvp.m0 * v.x + mvp.m4 * v.y + mvp.m8 * v.z + mvp.m12;
        const float y = mvp.m1 * v.x + mvp.m5 * v.y + mvp.m9 * v.z + mvp.m13;
        const float w = mvp.m3 * v.x + mvp.m7 * v.y + mvp.m11 * v.z + mvp.m15;
        left = left && x < -w; right = right && x > w;
        below = below && y < -w; above = above && y > w;
        behind = behind && w < 0.0f;
        in = in && w > 0.0f && std::abs(x) <= w && std::abs(y) <= w;
    }
    if (inside) *inside = in;
    return left || right || below || above || behind;
}

// Pixels on the target one world unit of height covers at v: d(clip / w) / dz, exactly. At or behind
// the eye: infinite (no LOD).
float PixelsPerUnitUp(Vector3 v, const Matrix& mvp, Vector2 half)
{
    const float x = mvp.m0 * v.x + mvp.m4 * v.y + mvp.m8 * v.z + mvp.m12;
    const float y = mvp.m1 * v.x + mvp.m5 * v.y + mvp.m9 * v.z + mvp.m13;
    const float w = mvp.m3 * v.x + mvp.m7 * v.y + mvp.m11 * v.z + mvp.m15;
    if (w <= 1e-4f) return INFINITY;
    const float dx = (mvp.m8 * w - x * mvp.m11) / (w * w) * half.x;
    const float dy = (mvp.m9 * w - y * mvp.m11) / (w * w) * half.y;
    return std::sqrt(dx * dx + dy * dy);
}

constexpr int kChunksAcross = 64;   // a static list's chunk grid: at most this many on its longer side

// Instances with one stack and LOD level, drawn by one instanced call.
struct Run
{
    const SpriteStack* Stack;
    int Lod;
    size_t First, Count;
};

} // namespace

struct StaticStacks
{
    struct Entry
    {
        int Group = -1;            // index in Groups, -1: in Rest
        unsigned int Slot = 0;     // place in its group's Members
        int Chunk = 0;
        BoundingBox Box{};         // what the item can cover, lean and sway included
        StackInstance Record{};
    };
    struct Group
    {
        const SpriteStack* Stack = nullptr;
        std::vector<unsigned int> Members;
    };
    std::vector<SpriteStackItem> Items;
    std::vector<Entry> Entries;
    std::vector<Group> Groups;
    std::vector<int> Order;                       // Groups by texture, then stack: few texture switches
    std::unordered_map<const SpriteStack*, int> GroupOf;
    std::vector<unsigned int> Rest;               // items the instanced path cannot take
    std::vector<BoundingBox> Chunks;              // their items' bounds; an edit grows them, nothing shrinks them
    Vector2 Origin{};
    float ChunkSize = 1.0f;
    int Nx = 1, Ny = 1;
    const CellTerrain* Ground = nullptr;          // what the records were made for
    float LayerHeight = -1.0f;                    // -1: not made (or stale)

    void Add(unsigned int i, const SpriteStackPass& pass)
    {
        Entry& e = Entries[i];
        SpriteStackItem& item = Items[i];
        item.Ground = pass.Ground ? CellTerrainHeight(*pass.Ground, item.X, item.Y) : 0.0f;
        if (!Instanceable(item, pass)) {
            e.Group = -1;
            Rest.push_back(i);
            return;
        }
        e.Record = Record(item, pass);
        const SpriteStack& st = *item.Stack;
        const float rx = std::max(std::abs(st.Origin.x), std::abs(st.Width - st.Origin.x));
        const float ry = std::max(std::abs(st.Origin.y), std::abs(st.Height - st.Origin.y));
        const float layer = e.Record.Turn[2];
        const float z0 = item.Ground + item.Z - st.Origin.z * layer, z1 = z0 + static_cast<float>(st.Layers) * layer;
        // Room for the lean and the wind's sway (at most about the item's Sway in degrees).
        const float tilt = std::tan(std::min(std::abs(item.Lean) + 2.0f * std::abs(item.Sway), 60.0f) * DEG2RAD);
        const float reach = std::sqrt(rx * rx + ry * ry) * item.Scale + std::abs(z1 - z0) * tilt;
        const int cx = std::clamp(static_cast<int>((item.X - Origin.x) / ChunkSize), 0, Nx - 1);
        const int cy = std::clamp(static_cast<int>((item.Y - Origin.y) / ChunkSize), 0, Ny - 1);
        e.Chunk = cy * Nx + cx;
        e.Box = BoundingBox{ { item.X - reach, item.Y - reach, std::min(z0, z1) }, { item.X + reach, item.Y + reach, std::max(z0, z1) } };
        BoundingBox& b = Chunks[e.Chunk];
        b.min = Vector3Min(b.min, e.Box.min);
        b.max = Vector3Max(b.max, e.Box.max);
        const auto [at, fresh] = GroupOf.try_emplace(item.Stack, static_cast<int>(Groups.size()));
        if (fresh) {
            Groups.push_back(Group{ item.Stack, {} });
            Order.push_back(at->second);
            std::sort(Order.begin(), Order.end(), [this](int a, int b) {
                const SpriteStack *sa = Groups[a].Stack, *sb = Groups[b].Stack;
                return sa->Texture.id != sb->Texture.id ? sa->Texture.id < sb->Texture.id : sa < sb;
            });
        }
        e.Group = at->second;
        e.Slot = static_cast<unsigned int>(Groups[e.Group].Members.size());
        Groups[e.Group].Members.push_back(i);
    }

    void Remove(unsigned int i)
    {
        Entry& e = Entries[i];
        if (e.Group < 0) {
            Rest.erase(std::find(Rest.begin(), Rest.end(), i));
            return;
        }
        std::vector<unsigned int>& members = Groups[e.Group].Members;
        const unsigned int last = members.back();
        members[e.Slot] = last;
        Entries[last].Slot = e.Slot;
        members.pop_back();
        e.Group = -1;
    }

    void Build(const SpriteStackPass& pass)
    {
        Ground = pass.Ground;
        LayerHeight = pass.LayerHeight;
        Entries.assign(Items.size(), Entry{});
        Groups.clear();
        Order.clear();
        GroupOf.clear();
        Rest.clear();
        Vector2 lo{ INFINITY, INFINITY }, hi{ -INFINITY, -INFINITY };
        for (const SpriteStackItem& item : Items) {
            lo = Vector2{ std::min(lo.x, item.X), std::min(lo.y, item.Y) };
            hi = Vector2{ std::max(hi.x, item.X), std::max(hi.y, item.Y) };
        }
        if (Items.empty()) lo = hi = Vector2{ 0.0f, 0.0f };
        Origin = lo;
        ChunkSize = std::max(std::max(hi.x - lo.x, hi.y - lo.y) / kChunksAcross, 1e-3f);
        Nx = std::min(static_cast<int>((hi.x - lo.x) / ChunkSize) + 1, kChunksAcross);
        Ny = std::min(static_cast<int>((hi.y - lo.y) / ChunkSize) + 1, kChunksAcross);
        Chunks.assign(static_cast<size_t>(Nx) * Ny, BoundingBox{ { INFINITY, INFINITY, INFINITY }, { -INFINITY, -INFINITY, -INFINITY } });
        for (unsigned int i = 0; i < Items.size(); ++i) Add(i, pass);
    }
};

std::shared_ptr<StaticStacks> MakeStaticStacks()
{
    return std::make_shared<StaticStacks>();
}

namespace {

// Brings a list's copy up to date with what its draw brought: the whole list, or its edits. chunked:
// the records and chunks too (for the pass's ground and layer height); else only the items, and an
// edit leaves the records stale for the next chunked draw.
void Update(StaticStacksDraw& sd, const SpriteStackPass& pass, bool chunked)
{
    StaticStacks& s = *sd.Index;
    if (!sd.All.empty()) {
        s.Items = std::move(sd.All);
        sd.All.clear();
        s.LayerHeight = -1.0f;
    }
    const bool rebuild = s.LayerHeight != pass.LayerHeight || s.Ground != pass.Ground;
    for (const auto& [i, item] : sd.Edits) {
        if (i >= s.Items.size()) continue;
        if (!chunked || rebuild) {
            s.Items[i] = item;
            if (!chunked) s.LayerHeight = -1.0f;
            continue;
        }
        s.Remove(i);
        s.Items[i] = item;
        s.Add(i, pass);
    }
    sd.Edits.clear();
    if (chunked && rebuild) s.Build(pass);
}

// (item, LOD level) of what one frame draws of s: the visible chunks' items (one by one in a chunk
// only partly in view), by group (Order) and then by level, so equal neighbours form one Run.
// visible counts the chunks in view.
void Pick(const StaticStacks& s, const SpriteStackPass& pass, const Matrix& mvp, Vector2 half,
          std::vector<std::pair<unsigned int, int>>& out, int& visible)
{
    static std::vector<signed char> s_view;   // per chunk: -1 culled or empty, 0 partly in view, 1 all in view
    s_view.assign(s.Chunks.size(), -1);
    for (size_t c = 0; c < s.Chunks.size(); ++c) {
        const BoundingBox& b = s.Chunks[c];
        bool inside = !s_culling;
        if (b.min.x > b.max.x || (s_culling && BoxOffTarget(b, mvp, &inside))) continue;
        s_view[c] = inside;
        ++visible;
    }
    static std::vector<unsigned int> s_level[kLodLevels];
    for (const int g : s.Order) {
        for (auto& level : s_level) level.clear();
        for (const unsigned int m : s.Groups[g].Members) {
            const StaticStacks::Entry& e = s.Entries[m];
            const signed char view = s_view[e.Chunk];
            if (view < 0 || (view == 0 && BoxOffTarget(e.Box, mvp))) continue;
            int level = 0;
            if (pass.LodPixels > 0.0f && e.Record.Turn[3] >= 3.0f) {   // LodStep's rule, at the item's pivot
                const Vector3 pivot{ (e.Box.min.x + e.Box.max.x) * 0.5f, (e.Box.min.y + e.Box.max.y) * 0.5f, e.Box.min.z };
                const float spacing = e.Record.Turn[2] * PixelsPerUnitUp(pivot, mvp, half);
                level = LodLevel(std::clamp(static_cast<int>(pass.LodPixels / std::max(spacing, 1e-4f)), 1, kMaxLodStep));
            }
            s_level[level].push_back(m);
        }
        for (int level = 0; level < kLodLevels; ++level) {
            for (const unsigned int m : s_level[level]) out.emplace_back(m, level);
        }
    }
}

} // namespace

void ExpandStaticStacks(std::vector<StaticStacksDraw>& statics, const SpriteStackPass& pass, std::vector<SpriteStackItem>& items)
{
    for (StaticStacksDraw& sd : statics) {
        Update(sd, pass, false);
        items.insert(items.end(), sd.Index->Items.begin(), sd.Index->Items.end());
    }
}

std::vector<std::pair<unsigned int, int>> SelectStaticStacks(StaticStacksDraw& statics, const SpriteStackPass& pass,
                                                             const Matrix& mvp, Vector2 half_size)
{
    Update(statics, pass, true);
    std::vector<std::pair<unsigned int, int>> out;
    int visible = 0;
    Pick(*statics.Index, pass, mvp, half_size, out, visible);
    return out;
}

void DrawSpriteStacksInstanced(std::vector<SpriteStackItem>& items, const SpriteStackPass& pass, const Shader& shader,
                               std::vector<StaticStacksDraw>* statics)
{
    const int instancedLoc = s_instancing && pass.ZUp ? rlGetLocationUniform(shader.id, "uInstanced") : -1;
    if (statics && (!Chunkable(pass) || instancedLoc < 0)) {
        ExpandStaticStacks(*statics, pass, items);
        statics = nullptr;
    }
    if (statics) {
        for (StaticStacksDraw& sd : *statics) {
            Update(sd, pass, true);
            for (const unsigned int i : sd.Index->Rest) items.push_back(sd.Index->Items[i]);
        }
    }
    if (!s_instancing || !pass.ZUp || pass.Offset.x != 0.0f || pass.Offset.y != 0.0f || instancedLoc < 0 ||
        (items.empty() && !statics)) {
        return;
    }
    ProfileScope scope("instanced");
    if (pass.Ground) {
        for (SpriteStackItem& item : items) item.Ground = CellTerrainHeight(*pass.Ground, item.X, item.Y);
    }
    const Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    const Vector2 half{ 0.5f * static_cast<float>(rlGetFramebufferWidth()), 0.5f * static_cast<float>(rlGetFramebufferHeight()) };

    static std::vector<StackInstance> s_instances;
    static std::vector<Run> s_runs;
    s_instances.clear();
    s_runs.clear();
    auto add = [](const SpriteStack* stack, int lod, const StackInstance& record) {
        if (s_runs.empty() || s_runs.back().Stack != stack || s_runs.back().Lod != lod) s_runs.push_back(Run{ stack, lod, s_instances.size(), 0 });
        ++s_runs.back().Count;
        s_instances.push_back(record);
    };
    if (statics) {
        ProfileScope chunks("chunks");
        static std::vector<std::pair<unsigned int, int>> s_picks;
        int visible = 0;
        for (const StaticStacksDraw& sd : *statics) {
            s_picks.clear();
            Pick(*sd.Index, pass, mvp, half, s_picks, visible);
            for (const auto& [m, lod] : s_picks) add(sd.Index->Items[m].Stack, lod, sd.Index->Entries[m].Record);
        }
        ProfileCount("chunks", visible);
    }

    // Split the moving items: what goes as instances (culled here, with their LOD level) and what stays for DrawSpriteStacks.
    struct Drawn
    {
        SpriteStackItem Item;
        int Lod;
    };
    static std::vector<Drawn> s_drawn;
    s_drawn.clear();
    size_t keep = 0;
    {
        ProfileScope split("split");
        for (const SpriteStackItem& item : items) {
            if (!Instanceable(item, pass)) {
                items[keep++] = item;
            } else {
                const Placement p = Place(item, pass);   // once for the cull and the LOD
                if (!s_culling || !OffTarget(item, pass, p, mvp)) s_drawn.push_back(Drawn{ item, LodLevel(LodStep(item, pass, p, mvp)) });
            }
        }
    }
    items.resize(keep);
    std::sort(s_drawn.begin(), s_drawn.end(), [](const Drawn& a, const Drawn& b) {
        const SpriteStack *sa = a.Item.Stack, *sb = b.Item.Stack;
        if (sa->Texture.id != sb->Texture.id) return sa->Texture.id < sb->Texture.id;
        return sa != sb ? sa < sb : a.Lod < b.Lod;
    });
    for (const Drawn& d : s_drawn) add(d.Item.Stack, d.Lod, Record(d.Item, pass));
    if (s_instances.empty()) return;

    // One buffer for every pass's instances, grown as needed (render side only).
    static unsigned int s_buffer = 0;
    static int s_capacity = 0;
    const int bytes = static_cast<int>(s_instances.size() * sizeof(StackInstance));
    if (bytes > s_capacity) {
        if (s_buffer) rlUnloadVertexBuffer(s_buffer);
        s_capacity = std::max(bytes, 2 * s_capacity);
        s_buffer = rlLoadVertexBuffer(nullptr, s_capacity, true);
    }
    rlUpdateVertexBuffer(s_buffer, s_instances.data(), bytes, 0);

    rlDrawRenderBatchActive();                         // what the batch holds goes first: these draw at once
    rlEnableShader(shader.id);
    const float on = 1.0f, off = 0.0f, viewRotation = pass.ViewRotation;
    const float sway[4] = { pass.SwayTime, pass.SwayStrength, pass.SwayDir.x, pass.SwayDir.y };
    const float shade[2] = { pass.BaseShade, std::max(pass.ShadeReach, 1e-3f) };
    const float groundMaxZ = pass.GroundInNormal ? pass.GroundMaxZ : -INFINITY;
    rlSetUniform(instancedLoc, &on, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(rlGetLocationUniform(shader.id, "uViewRotation"), &viewRotation, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniform(rlGetLocationUniform(shader.id, "uSway"), sway, RL_SHADER_UNIFORM_VEC4, 1);
    rlSetUniform(rlGetLocationUniform(shader.id, "uShade"), shade, RL_SHADER_UNIFORM_VEC2, 1);
    rlSetUniform(rlGetLocationUniform(shader.id, "uGroundMaxZ"), &groundMaxZ, RL_SHADER_UNIFORM_FLOAT, 1);
    rlSetUniformMatrix(shader.locs[SHADER_LOC_MATRIX_MVP], mvp);
    const int unit = 0;
    rlSetUniform(shader.locs[SHADER_LOC_MAP_DIFFUSE], &unit, RL_SHADER_UNIFORM_INT, 1);
    rlActiveTextureSlot(0);

    const size_t offsets[5] = { offsetof(StackInstance, Place), offsetof(StackInstance, Turn),
                                offsetof(StackInstance, Motion), offsetof(StackInstance, Tint),
                                offsetof(StackInstance, Damage) };
    const int sizes[5] = { 4, 4, 4, 4, 1 };
    unsigned int texture = 0;
    double quads = 0.0;
    for (const Run& run : s_runs) {
        const SpriteStack* stack = run.Stack;
        if (stack->Texture.id != texture) {
            texture = stack->Texture.id;
            rlEnableTexture(texture);
        }
        if (!stack->Mesh) stack->Mesh = BuildStackMesh(*stack);
        rlEnableVertexArray(stack->Mesh->Vao);
        rlEnableVertexBuffer(s_buffer);
        for (unsigned int a = 0; a < 5; ++a) {
            rlSetVertexAttribute(kInstanceAttrib + a, sizes[a], a == 3 ? RL_UNSIGNED_BYTE : RL_FLOAT, a == 3,
                                 sizeof(StackInstance), static_cast<int>(run.First * sizeof(StackInstance) + offsets[a]));
            rlEnableVertexAttribute(kInstanceAttrib + a);
            rlSetVertexAttributeDivisor(kInstanceAttrib + a, 1);
        }
        rlDrawVertexArrayElementsInstanced(stack->Mesh->LodFirst[run.Lod], stack->Mesh->LodCount[run.Lod], nullptr,
                                           static_cast<int>(run.Count));
        quads += static_cast<double>(stack->Mesh->LodCount[run.Lod] / 6) * static_cast<double>(run.Count);
    }
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableTexture();
    rlSetUniform(instancedLoc, &off, RL_SHADER_UNIFORM_FLOAT, 1);   // the batch path draws through the same shader
    ProfileCount("stacks", static_cast<double>(s_instances.size()));
    ProfileCount("draws", static_cast<double>(s_runs.size()));
    ProfileCount("quads", quads);
}

void SetSpriteStackInstancing(bool enabled)
{
    s_instancing = enabled;
}

void SetSpriteStackCulling(bool enabled)
{
    s_culling = enabled;
}

void SpriteStackList::Set(size_t index, const SpriteStackItem& item)
{
    Items[index] = item;
    if (!Sent) return;                                 // the next lit draw takes it; too many: send it whole
    Edits.emplace_back(static_cast<unsigned int>(index), item);
    if (Edits.size() > Items.size() / 4 + 64) {
        Sent = false;
        Edits.clear();
    }
}

namespace Api {

void DrawSpriteStacks(std::vector<SpriteStackItem> items, const SpriteStackPass& pass)
{
    Record([items = std::move(items), pass]() mutable {
        ProfileScope scope("sprite_stacks");
        IYI::DrawSpriteStacks(items, pass);
    });
}

} // namespace Api

} // namespace IYI
