// Implements lighting for sprite stacks (StackLighting.h): the world-fixed shadow, contact and light
// buffers, the static set's cached buffers and their patches, and the world-pass composites.
//
// Every buffer is black (stack) on white (ground): drawing a stack again where it already is changes
// nothing, which is what lets a patch redraw generously round a change. Stacks land in a buffer
// scaled to its texels per unit (position, height and size), so the passes stay one DrawSpriteStacks
// call each and their blurs (BlurBuffer) keep their width in world units.

#include "IYI/StackLighting.h"
#include "IYI/FramePacket.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "IYI/Log.h"
#include "IYI/Profiler.h"
#include "raymath.h"
#include "rlgl.h"
#ifdef IYI_MOBILE
#include <GLES3/gl3.h>
#define IYI_GL_PROC(name) (&name)
#else
#include <GLFW/glfw3.h>
#define IYI_GL_PROC(name) glfwGetProcAddress(#name)
#endif

#if defined(_WIN32)
#define IYI_GLAPI __stdcall
#else
#define IYI_GLAPI
#endif

namespace IYI {

namespace {

constexpr float kShadowSpread = 1.2f;     // world units between blur taps: the sun shadow's penumbra
constexpr float kContactSpread = 3.0f;    // ... how far the contact halo reaches
constexpr float kShadowFalloff = 4.5f;    // blur weights exp(-i*i / falloff), i = -2..2 taps
constexpr float kContactFalloff = 3.0f;
constexpr float kContactMapSpread = 0.6f; // world units between blur taps with a camera-local contact map
constexpr float kContactMapMin = 64.0f;   // the camera-local contact area's smallest side (world units)
constexpr float kSunCasterTop = 160.0f;   // world height the sun map's box reaches (hills + the tallest stack)
constexpr float kSunOffset = 1.8f;        // world units a stack's lookup moves toward the sun (more than a layer step)
// The terrain's lookup barely moves and has almost no bias: its own depth is pushed back instead (kTerrainSlope),
// so the foot of every caster (a person, a trunk) still shadows the ground right under it.
constexpr float kGroundSunOffset = 0.05f;
constexpr float kGroundSunBias = 0.05f;   // world units
constexpr float kSunSoft = 0.3f;          // world units the shadow filter's taps reach up the map (a stack's height: the
                                          // view's up is z), filling the gaps between a low sun's layer strips; across
                                          // they stay at 0.75 texel, so a thin caster (a person) keeps its full shadow
constexpr float kTerrainSlope = 2.0f, kTerrainUnits = 4.0f;   // glPolygonOffset on the terrain in the sun map: a low sun
                                                              // sees flat ground change depth fast across a texel
constexpr float kSunBias = 0.6f;          // world units of depth bias
constexpr float kSunStep = 64.0f;         // the map's extent grows in these steps (world units), so it holds still
constexpr float kSunAngleStep = 0.25f;    // degrees the sun map's sun moves at a time (its bearing and its height)
constexpr float kContactTop = 255.0f;     // ground heights up to this fit a contact footprint's g (terrain.fs)
constexpr float kAirborneZ = 2.0f;        // stacks higher than this get ambient light only
constexpr float kPadTexels = 2.0f;        // an erased box reaches this much past what it erases

float SunEvery(StaticSunMode mode)        // texels the tallest static tip moves between redraws
{
    return mode == StaticSunMode::Smooth ? 0.25f : mode == StaticSunMode::Step ? 1.0f : 0.0f;
}

Rectangle Square(float half)    // +-half world units on both axes
{
    return Rectangle{ -half, -half, 2.0f * half, 2.0f * half };
}

// items as they land in a buffer of k texels per world unit: positions, heights and size times k.
void Scaled(const std::vector<SpriteStackItem>& in, float k, std::vector<SpriteStackItem>& out)
{
    out = in;
    if (k == 1.0f) return;
    for (SpriteStackItem& item : out) {
        item.X *= k;
        item.Y *= k;
        item.Z *= k;
        item.Scale *= k;
    }
}

// Sun shadows: every layer lands on the ground, pushed away from the sun as far as it is high.
void SunPass(const StackLighting& fx, const std::vector<SpriteStackItem>& items, float layerHeight,
             float direction, float length)
{
    static std::vector<SpriteStackItem> s_scaled;
    const float k = fx.Settings.ShadowScale, half = fx.Settings.GroundHalf;
    Scaled(items, k, s_scaled);
    SpriteStackPass pass;
    pass.ViewRotation = direction;
    pass.HeightScale = length;
    pass.LayerHeight = layerHeight;
    pass.Offset = Vector2{ half * k, half * k };
    pass.OverrideTint = true;
    pass.Tint = BLACK;
    pass.Sort = false;
    DrawSpriteStacks(s_scaled, pass);
}

// Terrain contact (StackLighting.h): a ground to measure casters on, and the shaders that need it.
bool TerrainContact(const StackLighting& fx)
{
    return fx.Ground && IsShaderValid(fx.Settings.SunDepth) && IsShaderValid(fx.Settings.Terrain);
}

// The contact buffer follows the camera (Settings.ContactMapSize, with terrain contact).
bool LocalContact(const StackLighting& fx)
{
    return fx.Settings.ContactMapSize > 0 && TerrainContact(fx);
}

// The camera-local buffers' area for the area the camera sees: a square in sqrt(2) steps of size on
// a grid of an eighth of it (whole texels), so it holds still while the camera moves within a step.
Rectangle ViewAreaFor(Rectangle seen)
{
    const float need = std::max({ seen.width, seen.height, kContactMapMin }) * 8.0f / 7.0f;
    const float size = kContactMapMin * std::pow(2.0f, std::ceil(2.0f * std::log2(need / kContactMapMin)) * 0.5f);
    const float grid = size / 8.0f;
    return Rectangle{ std::floor(seen.x / grid) * grid, std::floor(seen.y / grid) * grid, size, size };
}

// Contact: the footprint of everything standing on the ground (all layers flattened) over
// fx.ContactArea, black; with terrain contact flat (0, 1 - ground height / kContactTop, 1) instead,
// through sun_depth.fs. A camera-local map looks straight down on its area in 3D, so the static set
// goes through the instanced path.
void ContactPass(const StackLighting& fx, const std::vector<SpriteStackItem>& all)
{
    static std::vector<SpriteStackItem> s_scaled, s_inside;
    const Rectangle a = fx.ContactArea;
    const float k = fx.ContactRt->texture.width / a.width;
    const bool terrain = TerrainContact(fx), local = LocalContact(fx);
    if (local) {   // only what can touch the area: heights and the instanced split cost per item
        ProfileScope cull("area");
        s_inside.clear();
        for (const SpriteStackItem& item : all) {
            if (!item.Stack) continue;
            const float r = static_cast<float>(std::max(item.Stack->Width, item.Stack->Height)) * item.Scale;
            if (item.X + r >= a.x && item.X - r <= a.x + a.width && item.Y + r >= a.y && item.Y - r <= a.y + a.height)
                s_inside.push_back(item);
        }
    }
    const std::vector<SpriteStackItem>& items = local ? s_inside : all;
    Scaled(items, local ? 1.0f : k, s_scaled);
    SpriteStackPass pass;
    pass.HeightScale = 0.0f;
    pass.Offset = local ? Vector2{} : Vector2{ -a.x * k, -a.y * k };
    pass.OverrideTint = !terrain;
    pass.Tint = BLACK;
    pass.MaxZ = local ? 1.0f : k;
    pass.Sort = false;
    if (!terrain) {
        DrawSpriteStacks(s_scaled, pass);
        return;
    }
    {
        ProfileScope heights("heights");
        for (size_t i = 0; i < items.size(); ++i) {
            s_scaled[i].Ground = CellTerrainHeight(*fx.Ground, items[i].X, items[i].Y);   // the pass need not look it up again
            const float h = std::clamp(s_scaled[i].Ground / kContactTop, 0.0f, 1.0f);
            s_scaled[i].Tint = Color{ 0, static_cast<unsigned char>(std::lround(255.0f * (1.0f - h))), 255, 255 };
        }
    }
    if (local) {
        // World y down the buffer, as the 2D passes draw it (texel row 0 at a.y); no depth test: paint.
        rlDrawRenderBatchActive();
        rlMatrixMode(RL_PROJECTION);
        rlPushMatrix();
        rlLoadIdentity();
        rlMultMatrixf(MatrixToFloat(MatrixOrtho(a.x, a.x + a.width, a.y + a.height, a.y, -4000.0, 4000.0)));
        rlMatrixMode(RL_MODELVIEW);
        rlPushMatrix();
        rlLoadIdentity();
        rlDisableDepthTest();
        rlDisableBackfaceCulling();
        pass.ZUp = true;
    }
    BeginShaderMode(fx.Settings.SunDepth);
    if (local) DrawSpriteStacksInstanced(s_scaled, pass, fx.Settings.SunDepth);
    DrawSpriteStacks(s_scaled, pass);
    EndShaderMode();
    if (local) {
        rlDrawRenderBatchActive();
        rlMatrixMode(RL_PROJECTION);
        rlPopMatrix();
        rlMatrixMode(RL_MODELVIEW);
        rlPopMatrix();
        rlEnableBackfaceCulling();
    }
}

// Draw rt over the current target texel for texel (both upside down: a copy), alpha 0..255.
void Copy(const RenderTexture2D& rt, unsigned char alpha)
{
    const float w = static_cast<float>(rt.texture.width), h = static_cast<float>(rt.texture.height);
    DrawTexturePro(rt.texture, Rectangle{ 0.0f, 0.0f, w, -h }, Rectangle{ 0.0f, 0.0f, w, h }, Vector2{ 0.0f, 0.0f }, 0.0f,
                   Color{ 255, 255, 255, alpha });
}

// What a frame's buffer starts from: white, or the static snapshot (newer), cross-faded from the
// older one by t (alpha blending: older * (1 - t) + newer * t).
struct Base
{
    const RenderTexture2D* Older = nullptr;
    const RenderTexture2D* Newer = nullptr;
    float T = 1.0f;
};

void StartFrom(const Base& base)
{
    ClearBackground(WHITE);
    if (!base.Newer) return;
    if (!base.Older) {
        Copy(*base.Newer, 255);
        return;
    }
    Copy(*base.Older, 255);
    Copy(*base.Newer, static_cast<unsigned char>(std::lround(255.0f * std::clamp(base.T, 0.0f, 1.0f))));
}

// The passes run on the render side, replayed from the frame packet's offscreen pass, so they bind
// their targets directly: the game-side chokepoint (Application::BeginUserTextureMode) records instead.
bool BeginBuffer(const RenderTexture2D* rt)
{
    if (!rt) return false;
    BeginTextureMode(*rt);
    return true;
}

void EndBuffer()
{
    EndTextureMode();
}

// A square target owned by fx (stable address, unloaded with it); nullptr when it cannot be made.
RenderTexture2D* Buffer(StackLighting& fx, int size)
{
    static bool warned = false;
    RenderTexture2D target = size > 0 ? LoadRenderTexture(size, size) : RenderTexture2D{};
    if (!IsRenderTextureValid(target)) {
        if (!warned) LogWrite(LogLevel::Warn, "lighting", "Failed to create render texture, size " + std::to_string(size));
        warned = true;
        return nullptr;
    }
    fx.Targets.push_back(target);
    return &fx.Targets.back();
}

void SetFloat(const Shader& shader, int loc, float v);
void SetVec2(const Shader& shader, int loc, Vector2 v);
void SetVec3(const Shader& shader, int loc, Vector3 v);

// A depth-only target for the sun map (raylib's shadowmap example), its depth texture sampled with
// hardware depth compares (sampler2DShadow, linear filtering = PCF over four texels).
RenderTexture2D LoadSunMap(int size)
{
    RenderTexture2D map{};
    map.id = rlLoadFramebuffer();
    map.texture.width = map.texture.height = size;     // what BeginTextureMode reads for the viewport
    if (map.id == 0) return map;
    rlEnableFramebuffer(map.id);
    map.depth.id = rlLoadTextureDepth(size, size, false);
    map.depth.width = map.depth.height = size;
    map.depth.format = 19;                             // DEPTH_COMPONENT_24BIT
    map.depth.mipmaps = 1;
    rlFramebufferAttach(map.id, map.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    const bool complete = rlFramebufferComplete(map.id);
    rlDisableFramebuffer();
    using BindTexture = void (IYI_GLAPI*)(unsigned int, unsigned int);
    using TexParameteri = void (IYI_GLAPI*)(unsigned int, unsigned int, int);
    const auto bind = reinterpret_cast<BindTexture>(IYI_GL_PROC(glBindTexture));
    const auto param = reinterpret_cast<TexParameteri>(IYI_GL_PROC(glTexParameteri));
    if (!complete || !bind || !param) {
        LogWrite(LogLevel::Warn, "render", "sun map unavailable: no depth framebuffer");
        UnloadRenderTexture(map);
        map = {};
        return map;
    }
    constexpr unsigned int kTexture2D = 0x0DE1;
    bind(kTexture2D, map.depth.id);
    param(kTexture2D, 0x884C, 0x884E);   // TEXTURE_COMPARE_MODE = COMPARE_REF_TO_TEXTURE
    param(kTexture2D, 0x884D, 0x0203);   // TEXTURE_COMPARE_FUNC = LEQUAL
    param(kTexture2D, 0x2801, 0x2601);   // MIN_FILTER = LINEAR
    param(kTexture2D, 0x2800, 0x2601);   // MAG_FILTER = LINEAR
    param(kTexture2D, 0x2802, 0x812F);   // WRAP_S = CLAMP_TO_EDGE
    param(kTexture2D, 0x2803, 0x812F);   // WRAP_T
    bind(kTexture2D, 0);
    return map;
}

using BindFramebuffer = void (IYI_GLAPI*)(unsigned int, unsigned int);
using BlitFramebuffer = void (IYI_GLAPI*)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);

bool HasSunDepthBlit()
{
    static const bool available = IYI_GL_PROC(glBindFramebuffer) && IYI_GL_PROC(glBlitFramebuffer);
    return available;
}

// Copy the static depth into the frame target before moving casters draw. A colour-texture copy
// cannot preserve depth, and sampling a second shadow texture would change the shader's result.
bool CopySunDepth(const RenderTexture2D& from, const RenderTexture2D& to)
{
    static const auto bind = reinterpret_cast<BindFramebuffer>(IYI_GL_PROC(glBindFramebuffer));
    static const auto blit = reinterpret_cast<BlitFramebuffer>(IYI_GL_PROC(glBlitFramebuffer));
    if (!bind || !blit || from.depth.width != to.depth.width || from.depth.height != to.depth.height) return false;
    constexpr unsigned int kReadFramebuffer = 0x8CA8, kDepthBufferBit = 0x00000100, kNearest = 0x2600;
    rlDrawRenderBatchActive();
    bind(kReadFramebuffer, from.id);
    blit(0, 0, from.depth.width, from.depth.height, 0, 0, to.depth.width, to.depth.height,
         kDepthBufferBit, kNearest);
    bind(kReadFramebuffer, to.id);    // BeginTextureMode bound the frame target for both read and draw.
    return true;
}

bool BeginSunDepth(const RenderTexture2D& target, const Matrix& proj, const Matrix& view, const Shader& shader)
{
    if (!BeginBuffer(&target)) return false;
    ClearBackground(WHITE);
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPushMatrix();
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(proj));
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlMultMatrixf(MatrixToFloat(view));
    rlEnableDepthTest();
    rlDisableBackfaceCulling();
    BeginShaderMode(shader);
    return true;
}

void EndSunDepth()
{
    EndShaderMode();
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlDisableDepthTest();
    rlEnableBackfaceCulling();
    EndBuffer();
}

// The static set as a list, kept while its key holds (the sun map's casters, the contact's footprints).
const std::vector<SpriteStackItem>& StaticItems(StackLighting& fx)
{
    if (fx.SunCastersKey != fx.StaticKey) {
        fx.SunCasters.clear();
        for (const auto& [id, e] : fx.Static) fx.SunCasters.push_back(e.Item);
        fx.SunCastersKey = fx.StaticKey;
    }
    return fx.SunCasters;
}

// The sun map for this frame: an orthographic camera looking along the sun's rays at the area (and
// everything up to kSunCasterTop over it), the terrain and every stack drawn into its depth.
void SunMapPass(StackLighting& fx, const std::vector<SpriteStackItem>& moving, float layerHeight, float direction,
                float length, const CellTerrain& ground, Rectangle area)
{
    ProfileScope scope("sun_map");
    // The sun moves in kSunAngleStep steps here, bearing and height: between steps the projection
    // holds, so the static casters come from their cached depth (below) instead of being drawn again
    // every frame while the clock runs. At 4x a step comes about 15 times a second.
    direction = std::round(direction / kSunAngleStep) * kSunAngleStep;
    const float height = std::round(std::atan2(1.0f, length) * RAD2DEG / kSunAngleStep) * kSunAngleStep;
    length = 1.0f / std::max(std::tan(height * DEG2RAD), 1e-4f);
    // The rays: per unit of drop they move `length` along the shadows' direction (as SunPass lifts layers).
    const Vector2 along{ -std::sin(direction * DEG2RAD), -std::cos(direction * DEG2RAD) };
    const Vector3 ray = Vector3Normalize(Vector3{ along.x * length, along.y * length, -1.0f });
    const Vector3 centre{ area.x + area.width * 0.5f, area.y + area.height * 0.5f, 0.0f };
    const Vector3 up = std::fabs(ray.z) > 0.999f ? Vector3{ 0.0f, -1.0f, 0.0f } : Vector3{ 0.0f, 0.0f, 1.0f };
    const Matrix view = MatrixLookAt(Vector3Subtract(centre, Vector3Scale(ray, 4000.0f)), centre, up);
    Vector3 lo{ INFINITY, INFINITY, INFINITY }, hi{ -INFINITY, -INFINITY, -INFINITY };
    for (float z : { 0.0f, kSunCasterTop }) {
        for (float x : { area.x, area.x + area.width }) {
            for (float y : { area.y, area.y + area.height }) {
                const Vector3 v = Vector3Transform(Vector3{ x, y, z }, view);
                lo = Vector3Min(lo, v);
                hi = Vector3Max(hi, v);
            }
        }
    }
    // The shaders fade shadows over the map's outer 4% (lit_sprite.fs, terrain.fs): pad the box so that
    // fade falls outside the area asked for.
    const Vector3 pad = Vector3Scale(Vector3Subtract(hi, lo), 0.045f);
    lo = Vector3{ lo.x - pad.x, lo.y - pad.y, lo.z };
    hi = Vector3{ hi.x + pad.x, hi.y + pad.y, hi.z };
    // Stable: the extent grows in kSunStep steps and the box moves a whole texel at a time.
    const float size = static_cast<float>(fx.SunMap.texture.width);
    const float extentX = std::ceil((hi.x - lo.x) / kSunStep) * kSunStep;
    const float extentY = std::ceil((hi.y - lo.y) / kSunStep) * kSunStep;
    const float texelX = extentX / size, texelY = extentY / size;
    const float left = std::floor(lo.x / texelX) * texelX, bottom = std::floor(lo.y / texelY) * texelY;
    const float nearZ = -hi.z - 10.0f, farZ = -lo.z + 10.0f;
    const Matrix proj = MatrixOrtho(left, left + extentX + texelX, bottom, bottom + extentY + texelY, nearZ, farZ);
    fx.SunViewProj = MatrixMultiply(view, proj);
    fx.SunOffset = Vector3Scale(ray, -kSunOffset);
    fx.SunBias = kSunBias / (farZ - nearZ);
    fx.GroundSunBias = kGroundSunBias / (farZ - nearZ);
    fx.SunSoft = Vector2{ 0.0f, kSunSoft / extentY };

    // Casters: the static set and the moving stacks.
    StaticItems(fx);
    const bool sameStaticView = fx.StaticSunKey == fx.StaticKey && fx.StaticSunGround == &ground &&
                                fx.StaticSunLayerHeight == layerHeight &&
                                std::memcmp(&fx.StaticSunVP, &fx.SunViewProj, sizeof(Matrix)) == 0;
    if (!sameStaticView) {
        fx.StaticSunReady = false;
        fx.StaticSunKey = fx.StaticKey;
        fx.StaticSunGround = &ground;
        fx.StaticSunLayerHeight = layerHeight;
        fx.StaticSunVP = fx.SunViewProj;
    }
    // Wait for one unchanged projection before filling the cache. While the sun or camera moves,
    // drawing the original pass once is cheaper than drawing static depth and then copying it.
    if (sameStaticView && !fx.StaticSunAttempted && HasSunDepthBlit()) {
        fx.StaticSunAttempted = true;
        fx.StaticSunMap = LoadSunMap(fx.SunMap.depth.width);
    }
    const bool useCache = sameStaticView && fx.StaticSunMap.id != 0 && HasSunDepthBlit();
    const float reach = std::min(kSunCasterTop * length, 3000.0f);
    const Texture2D white{ rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    SpriteStackPass pass;
    pass.HeightScale = 0.0f;
    pass.LayerHeight = layerHeight;
    pass.Sort = false;
    pass.ZUp = true;
    pass.Ground = &ground;
    auto drawTerrain = [&] {
        // Terrain under the area and toward the sun, its depth pushed back (kTerrainSlope).
        using Enable = void (IYI_GLAPI*)(unsigned int);
        using PolygonOffset = void (IYI_GLAPI*)(float, float);
        static const auto enable = reinterpret_cast<Enable>(IYI_GL_PROC(glEnable));
        static const auto disable = reinterpret_cast<Enable>(IYI_GL_PROC(glDisable));
        static const auto offset = reinterpret_cast<PolygonOffset>(IYI_GL_PROC(glPolygonOffset));
        constexpr unsigned int kPolygonOffsetFill = 0x8037;
        rlDrawRenderBatchActive();
        if (enable && offset) { offset(kTerrainSlope, kTerrainUnits); enable(kPolygonOffsetFill); }
        DrawCellTerrain(ground, white,
                        Rectangle{ area.x - reach, area.y - reach, area.width + 2 * reach, area.height + 2 * reach },
                        WHITE, false, &fx.Settings.SunDepth);
        rlDrawRenderBatchActive();
        if (disable) disable(kPolygonOffsetFill);
    };
    auto drawStatic = [&] {
        drawTerrain();
        static std::vector<SpriteStackItem> s_static;
        s_static = fx.SunCasters;     // DrawSpriteStacks culls its input in place.
        DrawSpriteStacksInstanced(s_static, pass, fx.Settings.SunDepth);
        DrawSpriteStacks(s_static, pass);
    };
    if (useCache && !fx.StaticSunReady &&
        BeginSunDepth(fx.StaticSunMap, proj, view, fx.Settings.SunDepth)) {
        ProfileScope staticScope("static_depth");
        drawStatic();
        EndSunDepth();
        fx.StaticSunReady = true;
    }
    if (!BeginSunDepth(fx.SunMap, proj, view, fx.Settings.SunDepth)) return;
    const bool copied = useCache && fx.StaticSunReady && CopySunDepth(fx.StaticSunMap, fx.SunMap);
    if (!copied) {
        fx.StaticSunReady = false;
        drawTerrain();
    }
    static std::vector<SpriteStackItem> s_items;
    s_items = copied ? moving : fx.SunCasters;
    if (!copied) s_items.insert(s_items.end(), moving.begin(), moving.end());
    DrawSpriteStacksInstanced(s_items, pass, fx.Settings.SunDepth);
    DrawSpriteStacks(s_items, pass);
    EndSunDepth();
    fx.SunMapReady = true;
}

// The sun map uniforms of shader `which` (0 lit_sprite, 1 terrain); the map texture is set last.
void SetSunMap(const StackLighting& fx, const Shader& shader, int which)
{
    SetFloat(shader, fx.UseSunMapLoc[which], fx.SunMapReady ? 1.0f : 0.0f);
    if (!fx.SunMapReady) return;
    if (fx.SunVPLoc[which] >= 0) SetShaderValueMatrix(shader, fx.SunVPLoc[which], fx.SunViewProj);
    SetVec3(shader, fx.SunOffsetLoc[which], which == 1 ? Vector3Scale(fx.SunOffset, kGroundSunOffset / kSunOffset) : fx.SunOffset);
    SetFloat(shader, fx.SunBiasLoc[which], which == 1 ? fx.GroundSunBias : fx.SunBias);
    if (fx.SunSoftLoc[which] >= 0) SetShaderValue(shader, fx.SunSoftLoc[which], &fx.SunSoft, SHADER_UNIFORM_VEC2);
}

void SetSunMapTexture(const StackLighting& fx, const Shader& shader, int which)
{
    if (!fx.SunMapReady || fx.SunMapLoc[which] < 0) return;
    const Texture2D depth{ fx.SunMap.depth.id, fx.SunMap.depth.width, fx.SunMap.depth.height, 1, 19 };
    SetShaderValueTexture(shader, fx.SunMapLoc[which], depth);
}

// The 5x5 Gaussian of a raw buffer (white = lit) into soft (r = occlusion), as two 5-tap passes
// through half; spread is texels between taps. Same weights the composites once used per pixel.
void BlurBuffer(const StackLighting& fx, const RenderTexture2D* raw, RenderTexture2D* half, RenderTexture2D* soft,
                float spread, float falloff)
{
    const Shader& shader = fx.Settings.Blur;
    if (!raw || !half || !soft || !IsShaderValid(shader)) return;
    const Vector2 along[2] = { { spread / raw->texture.width, 0.0f }, { 0.0f, spread / raw->texture.height } };
    const RenderTexture2D* from[2] = { raw, half };
    RenderTexture2D* to[2] = { half, soft };
    for (int pass = 0; pass < 2; ++pass) {
        if (!BeginBuffer(to[pass])) return;
        BeginShaderMode(shader);
        rlDrawRenderBatchActive();
        SetVec2(shader, fx.BlurStepLoc, along[pass]);
        SetFloat(shader, fx.BlurFalloffLoc, falloff);
        SetFloat(shader, fx.BlurInvertLoc, pass == 0 ? 1.0f : 0.0f);
        Copy(*from[pass], 255);
        EndShaderMode();
        EndBuffer();
    }
}

StackLighting::StaticEntry Entry(const SpriteStackItem& item, float layerHeight)
{
    StackLighting::StaticEntry e;
    e.Item = item;
    if (item.Stack) {
        e.Radius = std::hypot(static_cast<float>(item.Stack->Width), static_cast<float>(item.Stack->Height)) * item.Scale;
        e.Top = item.Z + item.Stack->Layers * item.Stack->LayerStep * layerHeight * item.Scale;
    }
    return e;
}

// Erase round what the removed stacks drew in rt (k texels per unit), then redraw every static stack
// whose reach meets an erased box, and the added ones. reach: shadow length per unit of height (0 for
// the footprints). removedBoxes are the removed entries, gone from fx.Static already.
template <typename DrawFn>
void Patch(const StackLighting& fx, RenderTexture2D* rt, Rectangle area, const std::vector<StackLighting::StaticEntry>& removed,
           const std::vector<long long>& added, float reach, DrawFn draw)
{
    const float k = rt->texture.width / area.width, pad = kPadTexels / k;
    std::vector<Rectangle> erased;                       // world boxes
    for (const StackLighting::StaticEntry& e : removed) {
        const float r = e.Radius + e.Top * reach + pad;
        erased.push_back(Rectangle{ e.Item.X - r, e.Item.Y - r, 2.0f * r, 2.0f * r });
    }
    std::vector<SpriteStackItem> redraw;
    for (long long id : added) redraw.push_back(fx.Static.at(id).Item);
    if (!erased.empty()) {
        for (const auto& [id, e] : fx.Static) {
            if (std::find(added.begin(), added.end(), id) != added.end()) continue;
            const float r = e.Radius + e.Top * reach;
            const Rectangle box{ e.Item.X - r, e.Item.Y - r, 2.0f * r, 2.0f * r };
            for (const Rectangle& b : erased) {
                if (CheckCollisionRecs(box, b)) {
                    redraw.push_back(e.Item);
                    break;
                }
            }
        }
    }
    if (!BeginBuffer(rt)) return;
    for (const Rectangle& b : erased) {
        DrawRectangleRec(Rectangle{ (b.x - area.x) * k, (b.y - area.y) * k, b.width * k, b.height * k }, WHITE);
    }
    draw(redraw);
    EndBuffer();
}

// Bring the static set's buffers up to date; returns the bases the frame's passes start from.
void StaticPasses(StackLighting& fx, const StackLightingStatic& stat, float layerHeight, Vector3 sun,
                  Base& contactBase, Base& shadowBase)
{
    const float direction = sun.x, length = sun.y, strength = sun.z;
    if (fx.StaticKey != stat.Key) {
        fx.ContactStill = false;
        // What changed, by id; with no ids, or most of it changed, everything is redrawn.
        std::vector<long long> added;
        std::vector<StackLighting::StaticEntry> removed;
        bool whole = !stat.Ids || fx.StaticKey < 0 || !fx.StaticContactRt;
        std::unordered_map<long long, StackLighting::StaticEntry> next;
        next.reserve(stat.Items->size());
        for (size_t i = 0; i < stat.Items->size(); ++i) {
            const long long id = stat.Ids ? (*stat.Ids)[i] : static_cast<long long>(i);
            auto old = fx.Static.find(id);
            if (old != fx.Static.end()) {
                next.emplace(id, old->second);
            } else {
                next.emplace(id, Entry((*stat.Items)[i], layerHeight));
                added.push_back(id);
            }
        }
        for (const auto& [id, e] : fx.Static) {
            if (!next.count(id)) removed.push_back(e);
        }
        whole = whole || added.size() + removed.size() > next.size() / 4;
        fx.Static = std::move(next);
        fx.StaticKey = stat.Key;
        fx.StaticLights = stat.Lights ? *stat.Lights : std::vector<StackLight>{};
        if (whole) {
            fx.HasSun = false;
            fx.HasOlder = false;
            fx.StaticTop = 0.0f;
            for (const auto& [id, e] : fx.Static) fx.StaticTop = std::max(fx.StaticTop, e.Top);
            fx.ContactStale = true;
        } else {
            ProfileScope scope("static_patch");
            for (long long id : added) fx.StaticTop = std::max(fx.StaticTop, fx.Static.at(id).Top);
            if (!fx.ContactStale) {
                Patch(fx, fx.StaticContactRt, fx.ContactArea, removed, added, 0.0f,
                      [&](const std::vector<SpriteStackItem>& items) { ContactPass(fx, items); });
            }
            if (fx.HasSun) {
                const int live[2] = { fx.Newer, 1 - fx.Newer };
                for (int n = 0; n < (fx.HasOlder ? 2 : 1); ++n) {
                    const int i = live[n];
                    const float d = fx.RtSun[i].x, l = fx.RtSun[i].y;
                    Patch(fx, fx.StaticShadowRt[i], Square(fx.Settings.GroundHalf), removed, added, l,
                          [&](const std::vector<SpriteStackItem>& items) { SunPass(fx, items, layerHeight, d, l); });
                }
            }
        }
    }
    if (fx.ContactStale) {
        // Every static footprint in the contact area (a new set, or a camera-local area moved).
        if (!fx.StaticContactRt) fx.StaticContactRt = Buffer(fx, fx.ContactRt->texture.width);
        if (BeginBuffer(fx.StaticContactRt)) {
            ProfileScope scope("static_contact");
            fx.ContactStill = false;
            ClearBackground(WHITE);
            ContactPass(fx, StaticItems(fx));   // the pass culls to the area
            EndBuffer();
            fx.ContactStale = false;
        }
    }
    contactBase = Base{ nullptr, fx.StaticContactRt, 1.0f };
    if (strength <= 0.0f || !fx.ShadowRt) return;

    const float every = SunEvery(fx.Look.SunMode);
    const Vector2 tip{ length * std::cos(direction * DEG2RAD), length * std::sin(direction * DEG2RAD) };
    float moved = fx.HasSun ? std::hypot(tip.x - fx.SunTip.x, tip.y - fx.SunTip.y) * fx.StaticTop * fx.Settings.ShadowScale
                            : INFINITY;
    if (moved >= every) {
        if (!fx.StaticShadowRt[0]) {
            const int size = fx.ShadowRt->texture.width;
            fx.StaticShadowRt[0] = Buffer(fx, size);
            fx.StaticShadowRt[1] = Buffer(fx, size);
            fx.Newer = 0;
        }
        // Smooth: the snapshot so far becomes the older one, unless the sun jumped (a new day, a new
        // set, another mode), when there is nothing to fade from.
        const bool keep = fx.Look.SunMode == StaticSunMode::Smooth && fx.HasSun && moved < 2.0f * every;
        fx.HasOlder = keep;
        if (keep) fx.Newer = 1 - fx.Newer;
        if (BeginBuffer(fx.StaticShadowRt[fx.Newer])) {
            ProfileScope scope("static_sun_shadows");
            ClearBackground(WHITE);
            std::vector<SpriteStackItem> all;
            all.reserve(fx.Static.size());
            for (const auto& [id, e] : fx.Static) all.push_back(e.Item);
            SunPass(fx, all, layerHeight, direction, length);
            EndBuffer();
            fx.HasSun = true;
            fx.SunTip = tip;
            fx.RtSun[fx.Newer] = Vector2{ direction, length };
            moved = 0.0f;
        }
    }
    if (!fx.HasSun) return;
    const RenderTexture2D* newer = fx.StaticShadowRt[fx.Newer];
    if (fx.Look.SunMode != StaticSunMode::Smooth || !fx.HasOlder) {
        shadowBase = Base{ nullptr, newer, 1.0f };
    } else {
        shadowBase = Base{ fx.StaticShadowRt[1 - fx.Newer], newer, every > 0.0f ? moved / every : 1.0f };
    }
}

unsigned char Alpha(float k)
{
    return static_cast<unsigned char>(std::lround(255.0f * std::clamp(k, 0.0f, 1.0f)));
}

bool Night(const StackLighting& fx)
{
    return fx.Ambient.x < 1.0f || fx.Ambient.y < 1.0f || fx.Ambient.z < 1.0f;
}

bool Cloudy(const StackLighting& fx)
{
    return fx.Look.Clouds.Cover > 0.0f && fx.Look.Clouds.Strength > 0.0f && fx.ShadowStrength > 0.0f;
}

Vector3 Rgb(Color c)
{
    return Vector3{ c.r / 255.0f, c.g / 255.0f, c.b / 255.0f };
}

// Draw a world-fixed buffer covering the world rectangle `area` over `cover` (area itself unless
// given; past area the clamped edge texels) through the current shader (its uniforms set already);
// tint reaches it as fragColor (the composites take their colour and strength from it).
void Composite(const RenderTexture2D* rt, Rectangle area, Color tint, const Rectangle* cover = nullptr)
{
    if (!rt) return;
    const Rectangle c = cover ? *cover : area;
    const float kx = rt->texture.width / area.width, ky = rt->texture.height / area.height;
    // a render texture holds the area's top row at v = 1, so the flipped source starts from the area's bottom
    const Rectangle source{ (c.x - area.x) * kx, (area.y + area.height - c.y - c.height) * ky, c.width * kx, -c.height * ky };
    DrawTexturePro(rt->texture, source, c, Vector2{ 0.0f, 0.0f }, 0.0f, tint);
}

void SetRect(const Shader& shader, int loc, Rectangle r)
{
    const float v[4] = { r.x, r.y, r.width, r.height };
    if (loc >= 0) SetShaderValue(shader, loc, v, SHADER_UNIFORM_VEC4);
}

// Fog uniforms of shader `which` (0 lit_sprite, 1 terrain); the eye from the camera's view matrix.
void SetFog(const StackLighting& fx, const Shader& shader, int which)
{
    const Color f = fx.Look.Fog;
    const float fog[4] = { f.r / 255.0f, f.g / 255.0f, f.b / 255.0f, fx.Look.FogEnd > fx.Look.FogStart ? f.a / 255.0f : 0.0f };
    if (fx.FogLoc[which] >= 0) SetShaderValue(shader, fx.FogLoc[which], fog, SHADER_UNIFORM_VEC4);
    SetVec2(shader, fx.FogRangeLoc[which], Vector2{ fx.Look.FogStart, fx.Look.FogEnd });
    const Matrix eye = MatrixInvert(rlGetMatrixModelview());
    SetVec3(shader, fx.EyeLoc[which], Vector3{ eye.m12, eye.m13, eye.m14 });
}

void SetFloat(const Shader& shader, int loc, float v)
{
    if (loc >= 0) SetShaderValue(shader, loc, &v, SHADER_UNIFORM_FLOAT);
}

void SetVec2(const Shader& shader, int loc, Vector2 v)
{
    if (loc >= 0) SetShaderValue(shader, loc, &v, SHADER_UNIFORM_VEC2);
}

void SetVec3(const Shader& shader, int loc, Vector3 v)
{
    if (loc >= 0) SetShaderValue(shader, loc, &v, SHADER_UNIFORM_VEC3);
}

void SetClouds(const StackLighting& fx, const Shader& shader, int cloudsLoc, int scaleLoc)
{
    const float strength = Cloudy(fx) ? fx.Look.Clouds.Strength : 0.0f;
    const float v[4] = { fx.Look.Clouds.Cover, strength, fx.Look.Clouds.Offset.x, fx.Look.Clouds.Offset.y };
    if (cloudsLoc >= 0) SetShaderValue(shader, cloudsLoc, v, SHADER_UNIFORM_VEC4);
    SetFloat(shader, scaleLoc, fx.Look.Clouds.Scale);
}

// Paints the marks not yet in MarkRt (all of them, cleared first, when stale).
void MarkPass(StackLighting& fx)
{
    if (!fx.MarkRt || fx.MarkArea.width <= 0.0f || (!fx.MarksStale && fx.MarksDrawn == fx.Marks.size())) return;
    ProfileScope scope("marks");
    const Rectangle a = fx.MarkArea;
    const float k = fx.MarkRt->texture.width / a.width;
    const Texture2D white{ rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    BeginBuffer(fx.MarkRt);
    if (fx.MarksStale) {
        ClearBackground(BLANK);
        fx.MarksDrawn = 0;
        fx.MarksStale = false;
    }
    BeginShaderMode(fx.Settings.MarkFs);
    BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
    for (size_t i = fx.MarksDrawn; i < fx.Marks.size(); ++i) {
        const StackMark& m = fx.Marks[i];
        const float r = 0.5f * std::hypot(m.Length, m.Width);
        if (m.X + r < a.x || m.X - r > a.x + a.width || m.Y + r < a.y || m.Y - r > a.y + a.height) continue;
        const float w = m.Length * k, h = m.Width * k;
        // marks.fs: r = kind, g = a seed for its noise (from its place, so pruning the list never reshapes it), a = strength
        const int seed = static_cast<int>(std::abs(m.X * 7.0f + m.Y * 13.0f)) % 251;
        const Color code{ static_cast<unsigned char>(m.Kind), static_cast<unsigned char>(seed), 0, Alpha(m.Strength) };
        DrawTexturePro(white, Rectangle{ 0.0f, 0.0f, 1.0f, 1.0f }, Rectangle{ (m.X - a.x) * k, (m.Y - a.y) * k, w, h },
                       Vector2{ w * 0.5f, h * 0.5f }, m.Rotation, code);
    }
    EndBlendMode();
    EndShaderMode();
    EndBuffer();
    fx.MarksDrawn = fx.Marks.size();
}

} // namespace

StackLighting::~StackLighting()
{
    if (StaticSunMap.id) UnloadRenderTexture(StaticSunMap);
    if (SunMap.id) UnloadRenderTexture(SunMap);
    for (RenderTexture2D& rt : Targets) UnloadRenderTexture(rt);
}

void SetStackLightingMarks(StackLighting& fx, const std::vector<StackMark>& marks, bool replace)
{
    if (!fx.MarkRt) return;
    if (replace) {
        fx.Marks = marks;
        fx.MarksStale = true;
    } else {
        fx.Marks.insert(fx.Marks.end(), marks.begin(), marks.end());
    }
}

void InitStackLighting(StackLighting& fx, const StackLightingSettings& settings)
{
    fx.Settings = settings;
    const int shadow = static_cast<int>(std::lround(2.0f * settings.GroundHalf * settings.ShadowScale));
    const int contact = settings.ContactMapSize > 0 ? settings.ContactMapSize
                                                    : static_cast<int>(std::lround(2.0f * settings.GroundHalf * settings.ContactScale));
    fx.ContactArea = Square(settings.GroundHalf);
    const int light = settings.LightMapSize > 0 ? settings.LightMapSize
                                                : static_cast<int>(std::lround(2.0f * settings.LightHalf * settings.LightScale));
    fx.LightArea = Square(settings.LightHalf);
    fx.ContactRt = Buffer(fx, contact);
    fx.LightRt = Buffer(fx, light);
    fx.ContactBlurRt = Buffer(fx, contact);
    fx.ContactSoftRt = Buffer(fx, contact);
    // Filtered: the composites sample the soft buffers between texels, smooth as the blur is.
    if (fx.ContactSoftRt) SetTextureFilter(fx.ContactSoftRt->texture, TEXTURE_FILTER_BILINEAR);
    // Filtered: the light map is coarse (light pools are soft), and every reader samples it between
    // texels (ground composite, lit stacks, wet streaks, glow); point sampling shows its squares.
    if (fx.LightRt) SetTextureFilter(fx.LightRt->texture, TEXTURE_FILTER_BILINEAR);
    // Clamped: a camera-local map's readers past its edge get the edge (see the header).
    if (fx.LightRt) SetTextureWrap(fx.LightRt->texture, TEXTURE_WRAP_CLAMP);
    if (fx.ContactSoftRt) SetTextureWrap(fx.ContactSoftRt->texture, TEXTURE_WRAP_CLAMP);
    if (settings.MarkMapSize > 0 && IsShaderValid(settings.MarkFs)) {
        fx.MarkRt = Buffer(fx, settings.MarkMapSize);
        if (fx.MarkRt) SetTextureFilter(fx.MarkRt->texture, TEXTURE_FILTER_BILINEAR);
    }
    fx.BlurStepLoc = GetShaderLocation(settings.Blur, "uStep");
    fx.BlurFalloffLoc = GetShaderLocation(settings.Blur, "uFalloff");
    fx.BlurInvertLoc = GetShaderLocation(settings.Blur, "uInvert");
    fx.LightAreaLoc = GetShaderLocation(settings.Sprite, "uLightArea");
    fx.AmbientLoc = GetShaderLocation(settings.Sprite, "uAmbient");
    fx.LightMapLoc = GetShaderLocation(settings.Sprite, "uLightMap");
    fx.ShadowCloudsLoc = GetShaderLocation(settings.ShadowFs, "uClouds");
    fx.ShadowCloudScaleLoc = GetShaderLocation(settings.ShadowFs, "uCloudScale");
    fx.ShadowHalfLoc = GetShaderLocation(settings.ShadowFs, "uGroundHalf");
    fx.UseLightLoc = GetShaderLocation(settings.Sprite, "uUseLight");
    fx.CloudsLoc = GetShaderLocation(settings.Sprite, "uClouds");
    fx.CloudScaleLoc = GetShaderLocation(settings.Sprite, "uCloudScale");
    fx.SunLoc = GetShaderLocation(settings.Sprite, "uSun");
    fx.ShadowTintLoc = GetShaderLocation(settings.Sprite, "uShadowTint");
    if (IsShaderValid(settings.Wet)) {
        fx.WetLightMapLoc = GetShaderLocation(settings.Wet, "uLightMap");
        fx.WetLightAreaLoc = GetShaderLocation(settings.Wet, "uLightArea");
        fx.WetAmbientLoc = GetShaderLocation(settings.Wet, "uAmbient");
        fx.WetUseLightLoc = GetShaderLocation(settings.Wet, "uUseLight");
        fx.WetUpLoc = GetShaderLocation(settings.Wet, "uUp");
        fx.WetEyeLoc = GetShaderLocation(settings.Wet, "uEye");
        fx.WetLoc = GetShaderLocation(settings.Wet, "uWet");
        fx.WetSkyLoc = GetShaderLocation(settings.Wet, "uSky");
    }
    if (IsShaderValid(settings.Glow)) fx.GlowAmbientLoc = GetShaderLocation(settings.Glow, "uAmbient");
    fx.SilhouetteLoc = GetShaderLocation(settings.Sprite, "uSilhouette");
    fx.WashLoc = GetShaderLocation(settings.Sprite, "uWash");
    if (settings.SunMapSize > 0 && IsShaderValid(settings.SunDepth)) {
        fx.SunMap = LoadSunMap(settings.SunMapSize);
    }
    // The flat sun buffers only without a sun map (it replaces them).
    if (fx.SunMap.id == 0) {
        fx.ShadowRt = Buffer(fx, shadow);
        fx.ShadowBlurRt = Buffer(fx, shadow);
        fx.ShadowSoftRt = Buffer(fx, shadow);
        if (fx.ShadowSoftRt) SetTextureFilter(fx.ShadowSoftRt->texture, TEXTURE_FILTER_BILINEAR);
    }
    const Shader* sunShaders[2] = { &settings.Sprite, &settings.Terrain };
    for (int i = 0; i < 2; ++i) {
        if (!IsShaderValid(*sunShaders[i])) continue;
        fx.SunMapLoc[i] = GetShaderLocation(*sunShaders[i], "uSunMap");
        fx.SunVPLoc[i] = GetShaderLocation(*sunShaders[i], "uSunVP");
        fx.SunOffsetLoc[i] = GetShaderLocation(*sunShaders[i], "uSunOffset");
        fx.SunBiasLoc[i] = GetShaderLocation(*sunShaders[i], "uSunBias");
        fx.SunSoftLoc[i] = GetShaderLocation(*sunShaders[i], "uSunSoft");
        fx.UseSunMapLoc[i] = GetShaderLocation(*sunShaders[i], "uUseSunMap");
        fx.FogLoc[i] = GetShaderLocation(*sunShaders[i], "uFog");
        fx.FogRangeLoc[i] = GetShaderLocation(*sunShaders[i], "uFogRange");
        fx.EyeLoc[i] = GetShaderLocation(*sunShaders[i], "uEye");
    }
    if (IsShaderValid(settings.Terrain)) {
        fx.TerrainCloudsLoc = GetShaderLocation(settings.Terrain, "uClouds");
        fx.TerrainCloudScaleLoc = GetShaderLocation(settings.Terrain, "uCloudScale");
        fx.TerrainSunLoc = GetShaderLocation(settings.Terrain, "uSun");
        fx.TerrainTintLoc = GetShaderLocation(settings.Terrain, "uShadowTint");
        fx.TerrainLightMapLoc = GetShaderLocation(settings.Terrain, "uLightMap");
        fx.TerrainLightAreaLoc = GetShaderLocation(settings.Terrain, "uLightArea");
        fx.TerrainUseLightLoc = GetShaderLocation(settings.Terrain, "uUseLight");
        fx.TerrainContactLoc = GetShaderLocation(settings.Terrain, "uContact");
        fx.TerrainContactAreaLoc = GetShaderLocation(settings.Terrain, "uContactArea");
        fx.TerrainContactStrengthLoc = GetShaderLocation(settings.Terrain, "uContactStrength");
        fx.TerrainTimeLoc = GetShaderLocation(settings.Terrain, "uTime");
        fx.TerrainMarksLoc = GetShaderLocation(settings.Terrain, "uMarks");
        fx.TerrainMarkAreaLoc = GetShaderLocation(settings.Terrain, "uMarkArea");
        fx.TerrainUseMarksLoc = GetShaderLocation(settings.Terrain, "uUseMarks");
        fx.TerrainWaterLoc = GetShaderLocation(settings.Terrain, "uWater");
        fx.TerrainWaterAreaLoc = GetShaderLocation(settings.Terrain, "uWaterArea");
        fx.TerrainUseWaterLoc = GetShaderLocation(settings.Terrain, "uUseWater");
    }
}

void RenderStackLighting(StackLighting& fx, const std::vector<SpriteStackItem>& moving, float layerHeight,
                         Vector3 sun, Vector3 ambient, float lamps, const std::vector<StackLight>& lights,
                         const StackLightingStatic* stat, const CellTerrain* ground, Rectangle sunArea)
{
    const float direction = sun.x, length = sun.y, strength = sun.z;
    fx.ShadowStrength = strength;
    fx.Ambient = ambient;
    fx.SunMapReady = false;
    fx.Ground = ground;
    const bool sunMap = fx.SunMap.id != 0;
    Base contactBase, shadowBase;
    if (sunArea.width > 0.0f && sunArea.height > 0.0f) {
        const Rectangle area = ViewAreaFor(sunArea);
        if (LocalContact(fx) && std::memcmp(&area, &fx.ContactArea, sizeof(Rectangle)) != 0) {
            fx.ContactArea = area;
            fx.ContactStale = true;
        }
        if (fx.Settings.LightMapSize > 0) fx.LightArea = area;
        if (fx.MarkRt && std::memcmp(&area, &fx.MarkArea, sizeof(Rectangle)) != 0) {
            fx.MarkArea = area;
            fx.MarksStale = true;
        }
    }
    MarkPass(fx);
    if (stat && (stat->Key == fx.StaticKey || stat->Items)) {
        StaticPasses(fx, *stat, layerHeight, sunMap ? Vector3{ direction, length, 0.0f } : sun, contactBase, shadowBase);
    }

    if (sunMap) {
        if (strength > 0.0f && ground && sunArea.width > 0.0f && sunArea.height > 0.0f) {
            SunMapPass(fx, moving, layerHeight, direction, length, *ground, sunArea);
        }
    } else if (strength > 0.0f && BeginBuffer(fx.ShadowRt)) {
        ProfileScope scope("sun_shadows");
        StartFrom(shadowBase);
        SunPass(fx, moving, layerHeight, direction, length);
        EndBuffer();
        ProfileScope blur("blur");
        BlurBuffer(fx, fx.ShadowRt, fx.ShadowBlurRt, fx.ShadowSoftRt, kShadowSpread * fx.Settings.ShadowScale, kShadowFalloff);
    }
    // Nothing moving and the static set unchanged: last frame's soft contact still holds.
    if (!(moving.empty() && fx.ContactStill) && BeginBuffer(fx.ContactRt)) {
        ProfileScope scope("contact");
        fx.ContactStill = moving.empty();
        StartFrom(contactBase);
        ContactPass(fx, moving);
        EndBuffer();
        ProfileScope blur("blur");
        const float spread = LocalContact(fx) ? kContactMapSpread : kContactSpread;
        BlurBuffer(fx, fx.ContactRt, fx.ContactBlurRt, fx.ContactSoftRt,
                   spread * fx.ContactRt->texture.width / fx.ContactArea.width, kContactFalloff);
    }

    // Light map: the ambient colour, light sprites alpha-blended toward their own colour.
    if (Night(fx) && BeginBuffer(fx.LightRt)) {
        ProfileScope scope("light_map");
        ClearBackground(Color{ Alpha(ambient.x), Alpha(ambient.y), Alpha(ambient.z), 255 });
        const Rectangle a = fx.LightArea;
        const float ls = fx.LightRt->texture.width / a.width;
        static std::vector<SpriteStackItem> s_sprites;
        s_sprites.clear();
        const std::vector<StackLight>* lists[2] = { &fx.StaticLights, &lights };
        for (const std::vector<StackLight>* list : lists) {
            for (const StackLight& light : *list) {
                if (!light.Sprite || light.Sprite->Width <= 0) continue;
                SpriteStackItem item;
                item.Stack = light.Sprite;
                item.X = (light.X - a.x) * ls;
                item.Y = (light.Y - a.y) * ls;
                item.Rotation = light.Rotation;
                item.Tint = Color{ light.Tint.r, light.Tint.g, light.Tint.b, Alpha(light.Power * lamps) };
                item.Scale = light.Size / static_cast<float>(light.Sprite->Width) * ls;
                s_sprites.push_back(item);
            }
        }
        SpriteStackPass pass;
        pass.HeightScale = 0.0f;
        pass.Sort = false;
        DrawSpriteStacks(s_sprites, pass);
        EndBuffer();
    }
}

// Shadows and contact occlusion blend toward ShadowTint (black: plain darkening; a dark blue reads
// as the sky lighting what the sun does not). Clouds join the sun shadows in soft_shadow.fs.
void DrawStackLightingGround(const StackLighting& fx)
{
    const StackLightingSettings& s = fx.Settings;
    const Color tint = fx.Look.ShadowTint;
    if (!TerrainContact(fx)) {                               // else the lit terrain draws it
        BeginShaderMode(s.ContactFs);
        Composite(fx.ContactSoftRt, fx.ContactArea, Color{ tint.r, tint.g, tint.b, Alpha(s.ContactStrength) });
        EndShaderMode();
    }
    if (fx.ShadowStrength > 0.0f && fx.SunMap.id == 0) {     // a sun map shades in the lit passes instead
        BeginShaderMode(s.ShadowFs);
        rlDrawRenderBatchActive();
        SetFloat(s.ShadowFs, fx.ShadowHalfLoc, s.GroundHalf);
        SetClouds(fx, s.ShadowFs, fx.ShadowCloudsLoc, fx.ShadowCloudScaleLoc);
        Composite(fx.ShadowSoftRt, Square(s.GroundHalf), Color{ tint.r, tint.g, tint.b, Alpha(fx.ShadowStrength) });
        EndShaderMode();
    }
}

// The light map multiplies the ground, colour by colour (raylib's multiplied blend: dst * src).
void DrawStackLightingGroundLight(const StackLighting& fx)
{
    if (!Night(fx)) return;
    BeginBlendMode(BLEND_MULTIPLIED);
    BeginShaderMode(fx.Settings.LightFs);
    const Rectangle cover = Square(fx.Settings.LightHalf);
    Composite(fx.LightRt, fx.LightArea, WHITE, &cover);
    EndShaderMode();
    EndBlendMode();
}

// Premultiplied blending (dst * (1 - a) + rgb) lets one draw both darken the ground and add its
// reflections: the shader returns a = how much darker, rgb = what it reflects.
void DrawStackLightingWet(const StackLighting& fx, const std::vector<WetPatch>& patches, float wetness,
                          float viewRotation, Color sky, const Vector3* eye)
{
    const Shader& shader = fx.Settings.Wet;
    if (patches.empty() || wetness <= 0.0f || !IsShaderValid(shader) || !fx.LightRt) return;
    const float r = viewRotation * DEG2RAD;
    BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
    BeginShaderMode(shader);
    rlDrawRenderBatchActive();
    SetRect(shader, fx.WetLightAreaLoc, fx.LightArea);
    SetVec3(shader, fx.WetAmbientLoc, fx.Ambient);
    SetFloat(shader, fx.WetUseLightLoc, Night(fx) ? 1.0f : 0.0f);
    SetVec2(shader, fx.WetUpLoc, Vector2{ -std::sin(r), -std::cos(r) });
    SetVec3(shader, fx.WetEyeLoc, eye ? *eye : Vector3{ 0.0f, 0.0f, -1.0f });   // z < 0: no eye
    SetFloat(shader, fx.WetLoc, std::clamp(wetness, 0.0f, 1.0f));
    SetVec3(shader, fx.WetSkyLoc, Rgb(sky));
    if (fx.WetLightMapLoc >= 0) SetShaderValueTexture(shader, fx.WetLightMapLoc, fx.LightRt->texture);
    for (const WetPatch& p : patches) DrawTexturePro(*p.Texture, p.Source, p.Dest, Vector2{ 0.0f, 0.0f }, 0.0f, WHITE);
    EndShaderMode();
    EndBlendMode();
}

// Additive blend: what the shader returns (light above the ambient, times the tint's alpha) is added.
void DrawStackLightingGlow(const StackLighting& fx, float strength)
{
    if (!Night(fx) || strength <= 0.0f || !IsShaderValid(fx.Settings.Glow)) return;
    // Over everything: with a 3D camera the ground plane it lies on must not be hidden by what stands on it.
    const bool depth = ReplayingWorld3D();
    if (depth) {
        rlDrawRenderBatchActive();
        rlDisableDepthTest();
    }
    BeginBlendMode(BLEND_ADDITIVE);
    BeginShaderMode(fx.Settings.Glow);
    SetVec3(fx.Settings.Glow, fx.GlowAmbientLoc, fx.Ambient);
    const Rectangle cover = Square(fx.Settings.LightHalf);
    Composite(fx.LightRt, fx.LightArea, Color{ 255, 255, 255, Alpha(strength) }, &cover);
    EndShaderMode();
    EndBlendMode();
    if (depth) {
        rlDrawRenderBatchActive();
        rlEnableDepthTest();
    }
}

void DrawLitStacks(const StackLighting& fx, std::vector<SpriteStackItem>& items, const SpriteStackPass& pass,
                   std::vector<StaticStacksDraw>* statics)
{
    SpriteStackPass lit = pass;
    lit.BaseShade = fx.Look.BaseShade;
    lit.ShadeReach = std::max(fx.Look.ShadeReach, 1e-3f);
    lit.SwayTime = fx.Look.WindTime;
    lit.SwayStrength = fx.Look.WindStrength;
    lit.SwayDir = fx.Look.WindDir;
    const bool night = Night(fx) && fx.LightRt;
    if (!night && !Cloudy(fx) && !pass.ZUp) {   // a 3D camera always needs the shader: it cuts out clear texels and takes the sun map
        if (statics) ExpandStaticStacks(*statics, lit, items);
        DrawSpriteStacks(items, lit);
        return;
    }
    const Shader& shader = fx.Settings.Sprite;
    BeginShaderMode(shader);
    // Value setters apply at once while drawing is batched: flush first, and the texture last (a
    // flush drops raylib's extra sampler slots).
    rlDrawRenderBatchActive();
    SetRect(shader, fx.LightAreaLoc, fx.LightArea);
    SetFog(fx, shader, 0);
    SetVec3(shader, fx.AmbientLoc, fx.Ambient);
    SetFloat(shader, fx.UseLightLoc, night ? 1.0f : 0.0f);
    SetClouds(fx, shader, fx.CloudsLoc, fx.CloudScaleLoc);
    SetFloat(shader, fx.SunLoc, fx.ShadowStrength);
    SetVec3(shader, fx.ShadowTintLoc, Rgb(fx.Look.ShadowTint));
    SetSunMap(fx, shader, 0);
    if (fx.WashLoc >= 0) SetShaderValueV(shader, fx.WashLoc, fx.Look.Wash, SHADER_UNIFORM_VEC4, 4);
    lit.GroundInNormal = true;
    lit.GroundMaxZ = kAirborneZ;
    // Instances first. They draw at once, outside the batch, so their maps go to texture units here
    // (as in DrawLitTerrain); the batch binds its own below.
    int unit = 1;
    auto bind = [&](int loc, unsigned int id) {
        if (loc < 0 || id == 0) return;
        rlActiveTextureSlot(unit);
        rlEnableTexture(id);
        SetShaderValue(shader, loc, &unit, SHADER_UNIFORM_INT);
        ++unit;
    };
    if (fx.SunMapReady) bind(fx.SunMapLoc[0], fx.SunMap.depth.id);
    if (fx.LightRt) bind(fx.LightMapLoc, fx.LightRt->texture.id);
    rlActiveTextureSlot(0);
    DrawSpriteStacksInstanced(items, lit, shader, statics);
    if (fx.LightMapLoc >= 0) SetShaderValueTexture(shader, fx.LightMapLoc, fx.LightRt->texture);
    SetSunMapTexture(fx, shader, 0);
    DrawSpriteStacks(items, lit);
    EndShaderMode();
}

void DrawStackSilhouettes(const StackLighting& fx, std::vector<SpriteStackItem>& items, const SpriteStackPass& pass,
                          float opacity)
{
    // rlgl has no depth function setter; GL_GREATER, then back to rlgl's GL_LEQUAL.
    static const auto depthFunc = reinterpret_cast<void (*)(unsigned int)>(IYI_GL_PROC(glDepthFunc));
    const Shader& shader = fx.Settings.Sprite;
    if (!depthFunc || !pass.ZUp || items.empty() || fx.SilhouetteLoc < 0 || opacity <= 0.0f) return;
    ProfileScope scope("silhouettes");
    SpriteStackPass flat = pass;
    flat.GroundInNormal = true;
    flat.GroundMaxZ = kAirborneZ;
    BeginShaderMode(shader);
    rlDrawRenderBatchActive();
    SetFog(fx, shader, 0);   // its uEye: lit_sprite.vs pulls the silhouettes toward the eye
    SetFloat(shader, fx.SilhouetteLoc, opacity);
    rlDisableDepthMask();
    depthFunc(0x0204);
    DrawSpriteStacksInstanced(items, flat, shader);
    DrawSpriteStacks(items, flat);
    rlDrawRenderBatchActive();
    depthFunc(0x0203);
    rlEnableDepthMask();
    SetFloat(shader, fx.SilhouetteLoc, 0.0f);
    EndShaderMode();
}

void DrawLitTerrain(const StackLighting& fx, const CellTerrain& terrain, const Texture2D& texture, Rectangle area,
                    Color wall, const Texture2D* water, Rectangle waterArea)
{
    const Shader& shader = fx.Settings.Terrain;
    if (!IsShaderValid(shader)) {
        DrawCellTerrain(terrain, texture, area, wall, true);
        return;
    }
    BeginShaderMode(shader);
    rlDrawRenderBatchActive();
    SetClouds(fx, shader, fx.TerrainCloudsLoc, fx.TerrainCloudScaleLoc);
    SetFloat(shader, fx.TerrainSunLoc, fx.ShadowStrength);
    SetVec3(shader, fx.TerrainTintLoc, Rgb(fx.Look.ShadowTint));
    SetRect(shader, fx.TerrainLightAreaLoc, fx.LightArea);
    SetFog(fx, shader, 1);
    SetFloat(shader, fx.TerrainUseLightLoc, Night(fx) ? 1.0f : 0.0f);
    SetRect(shader, fx.TerrainContactAreaLoc, fx.ContactArea);
    SetFloat(shader, fx.TerrainContactStrengthLoc, TerrainContact(fx) ? fx.Settings.ContactStrength : 0.0f);
    SetFloat(shader, fx.TerrainTimeLoc, fx.Look.WindTime);
    SetRect(shader, fx.TerrainMarkAreaLoc, fx.MarkArea);
    SetFloat(shader, fx.TerrainUseMarksLoc, fx.MarkRt && !fx.Marks.empty() ? 1.0f : 0.0f);
    SetFloat(shader, fx.TerrainUseWaterLoc, water ? 1.0f : 0.0f);
    const float wa[4] = { waterArea.x, waterArea.y, waterArea.width, waterArea.height };
    if (fx.TerrainWaterAreaLoc >= 0) SetShaderValue(shader, fx.TerrainWaterAreaLoc, wa, SHADER_UNIFORM_VEC4);
    SetSunMap(fx, shader, 1);
    // The terrain is drawn as meshes, which skip the batch's sampler binding: each map goes to its
    // own texture unit here (unit 0 is the ground picture).
    int unit = 1;
    auto bind = [&](int loc, unsigned int id) {
        if (loc < 0 || id == 0) return;
        rlActiveTextureSlot(unit);
        rlEnableTexture(id);
        SetShaderValue(shader, loc, &unit, SHADER_UNIFORM_INT);
        ++unit;
    };
    if (fx.SunMapReady) bind(fx.SunMapLoc[1], fx.SunMap.depth.id);
    if (fx.LightRt) bind(fx.TerrainLightMapLoc, fx.LightRt->texture.id);
    if (fx.ContactSoftRt) bind(fx.TerrainContactLoc, fx.ContactSoftRt->texture.id);
    if (water) bind(fx.TerrainWaterLoc, water->id);
    if (fx.MarkRt) bind(fx.TerrainMarksLoc, fx.MarkRt->texture.id);
    rlActiveTextureSlot(0);
    DrawCellTerrain(terrain, texture, area, wall, true, &shader);
    EndShaderMode();
}

StackLightingHandle::StackLightingHandle(StackLightingSettings settings, const StackLightingShaders& shaders,
                                         std::unordered_map<std::string, const SpriteStack*> lightSprites)
    : Fx(RenderOwned(new StackLighting())), LightSprites(std::move(lightSprites))
{
    // The shaders compile in this frame's Setup too, before this: read them there (cache entries).
    RecordSetup([fx = Fx, settings, shaders]() mutable {
        auto shader = [](const Shader* s) { return s ? *s : Shader{}; };
        settings.ShadowFs = shader(shaders.ShadowFs);
        settings.ContactFs = shader(shaders.ContactFs);
        settings.LightFs = shader(shaders.LightFs);
        settings.Sprite = shader(shaders.Sprite);
        settings.Blur = shader(shaders.Blur);
        settings.Wet = shader(shaders.Wet);
        settings.Glow = shader(shaders.Glow);
        settings.SunDepth = shader(shaders.SunDepth);
        settings.Terrain = shader(shaders.Terrain);
        settings.MarkFs = shader(shaders.Marks);
        InitStackLighting(*fx, settings);
    });
}

void StackLightingHandle::SetMarks(std::vector<StackMark> marks, bool replace)
{
    for (StackMark& m : marks) m.Strength = std::clamp(m.Strength, 0.0f, 1.0f);
    Record([fx = Fx, marks = std::move(marks), replace]() { SetStackLightingMarks(*fx, marks, replace); });
}

void StackLightingHandle::Render(std::vector<SpriteStackItem> moving, float layerHeight, Vector3 sun, Vector3 ambient,
                                 float lamps, std::vector<StackLight> lights, std::shared_ptr<StaticSetData> stat,
                                 std::shared_ptr<CellTerrain> ground, Rectangle sunArea)
{
    if (stat && stat->Changed) SentStaticKey = stat->Key;
    Record([fx = Fx, look = Look, moving = std::move(moving), lights = std::move(lights), stat, layerHeight, sun, ambient,
            lamps, ground, sunArea]() {
        static const std::vector<SpriteStackItem> none;
        fx->Look = look;
        StackLightingStatic st;
        if (stat) {
            st.Key = stat->Key;
            st.Items = stat->Changed ? &stat->Items : &none;
            st.Ids = stat->HasIds ? &stat->Ids : nullptr;
            st.Lights = stat->Changed ? &stat->Lights : nullptr;
        }
        RenderStackLighting(*fx, moving, layerHeight, sun, ambient, lamps, lights, stat ? &st : nullptr, ground.get(), sunArea);
    });
}

void StackLightingHandle::DrawTerrain(std::shared_ptr<CellTerrain> terrain, const Texture2D& texture, Rectangle area,
                                      Color wall, const Texture2D* water, Rectangle waterArea)
{
    Record([fx = Fx, look = Look, terrain, tex = &texture, area, wall, water, waterArea]() {
        fx->Look = look;
        DrawLitTerrain(*fx, *terrain, *tex, area, wall, water, waterArea);
    });
}

void StackLightingHandle::DrawGround()
{
    Record([fx = Fx, look = Look]() { fx->Look = look; DrawStackLightingGround(*fx); });
}

void StackLightingHandle::DrawGroundLight()
{
    Record([fx = Fx, look = Look]() { fx->Look = look; DrawStackLightingGroundLight(*fx); });
}

void StackLightingHandle::DrawGlow(float strength)
{
    Record([fx = Fx, look = Look, strength]() { fx->Look = look; DrawStackLightingGlow(*fx, strength); });
}

void StackLightingHandle::DrawWet(std::vector<WetPatch> patches, float wetness, float viewRotation, Color sky,
                                  std::optional<Vector3> eye)
{
    Record([fx = Fx, look = Look, patches = std::move(patches), wetness, viewRotation, sky, eye]() {
        fx->Look = look;
        DrawStackLightingWet(*fx, patches, wetness, viewRotation, sky, eye ? &*eye : nullptr);
    });
}

void StackLightingHandle::DrawSilhouettes(std::vector<SpriteStackItem> items, float viewRotation, float layerHeight,
                                          float opacity, std::shared_ptr<CellTerrain> ground)
{
    SpriteStackPass pass;
    pass.ViewRotation = viewRotation;
    pass.HeightScale = 0.0f;
    pass.LayerHeight = layerHeight;
    pass.ZUp = true;
    Record([fx = Fx, look = Look, items = std::move(items), pass, opacity, ground]() mutable {
        fx->Look = look;
        pass.Ground = ground.get();
        DrawStackSilhouettes(*fx, items, pass, opacity);
    });
}

void StackLightingHandle::DrawStacks(std::vector<SpriteStackItem> items, std::vector<StaticStacksDraw> statics,
                                     float viewRotation, float heightScale, float layerHeight, bool zUp,
                                     std::shared_ptr<CellTerrain> ground)
{
    SpriteStackPass pass;
    pass.ViewRotation = viewRotation;
    pass.HeightScale = heightScale;
    pass.LayerHeight = layerHeight;
    pass.ZUp = zUp;
    Record([fx = Fx, look = Look, items = std::move(items), statics = std::move(statics), pass, ground]() mutable {
        ProfileScope scope("sprite_stacks");
        fx->Look = look;
        pass.Ground = ground.get();
        DrawLitStacks(*fx, items, pass, &statics);
    });
}

void StackLightingHandle::SetClouds(float cover, float strength, Vector2 offset, float scale)
{
    Look.Clouds = StackClouds{ std::clamp(cover, 0.0f, 1.0f), std::clamp(strength, 0.0f, 1.0f), offset, std::max(1.0f, scale) };
}

void StackLightingHandle::SetHeightShade(float strength, float reach)
{
    Look.BaseShade = std::clamp(strength, 0.0f, 1.0f);
    Look.ShadeReach = std::max(1e-3f, reach);
}

void StackLightingHandle::SetWind(float time, float directionDegrees, float strength)
{
    const float d = directionDegrees * DEG2RAD;
    Look.WindTime = time;
    Look.WindDir = Vector2{ std::cos(d), std::sin(d) };
    Look.WindStrength = std::clamp(strength, 0.0f, 1.0f);
}

float StackLightingHandle::WindDegrees() const
{
    return std::atan2(Look.WindDir.y, Look.WindDir.x) * RAD2DEG;
}

void StackLightingHandle::SetWash(const std::vector<Vector4>& points)
{
    for (Vector4& w : Look.Wash) w = Vector4{};
    for (size_t i = 0; i < points.size() && i < 4; ++i) {
        Look.Wash[i] = Vector4{ points[i].x, points[i].y, points[i].z, std::clamp(points[i].w, 0.0f, 1.0f) };
    }
}

std::vector<Vector4> StackLightingHandle::Wash() const
{
    std::vector<Vector4> out;
    for (const Vector4& w : Look.Wash) if (w.w > 0.0f) out.push_back(w);
    return out;
}

} // namespace IYI
