// Declares lighting for sprite stacks: sun shadows, contact shadows and a light map in world-fixed
// render textures, filled in Update, plus the world-pass composites and the lit stack pass (the
// sprite_stack library's fx.py is a thin wrapper over it).
//
// The buffers are fixed to the world, not the screen: world point (x, y) is texel
// ((x + half) * scale, (y + half) * scale), so filling them needs no camera. Stacks that never move
// (buildings, trees, lamps) may be given apart as the static set, with an integer id each: their
// contact footprints do not depend on the sun, so they are drawn once into a buffer of their own;
// their sun shadows are redrawn only when the sun has moved the tallest one's tip a set number of
// texels (StaticSunMode); a change of a few of them only erases round the removed ones and redraws
// what reaches there. Each frame's passes start from a copy of those buffers and add the moving
// stacks. Static sun modes:
//     Smooth  snapshots a quarter texel of sun apart, the last two cross-faded as the sun moves on
//     Step    one snapshot, jumping a texel at a time (the cheapest)
//     Exact   redrawn every frame, as without the cache
//
// Sun map (Settings.SunMapSize > 0, for a 3D camera): instead of the flat sun buffer, the sun
// shadows come from a depth map drawn from the sun each frame (RenderStackLighting with a ground
// and an area): the terrain and every stack, seen along the sun's rays over the area the camera
// sees. The lit stacks and the lit terrain (DrawLitTerrain) look it up per pixel at their own 3D
// point, so stacks shadow each other and the hills, hills shadow what lies below them, and shadows
// fall right across cliffs and slopes. Edges are softened by filtered depth compares (PCF). The
// map follows the camera's area in steps of its own texels, so panning does not make it shimmer.
// The flat sun buffer, its blur and its static snapshots are skipped then.
//
// Terrain contact (a sun map, a ground and the terrain shader): each contact footprint also keeps
// its caster's ground height (in g, as occlusion * height / kContactTop), and the lit terrain draws
// the contact halo itself, only where its own height is near that: a halo stops at a cliff edge
// instead of spilling onto the level below or above. At the edge the shade comes from the cliff's
// own geometry instead (CellTerrain): a wall shades its own foot from its low height in its texture
// coordinates, and the ground below it is shaded from the terrain's height map (terrain.fs
// wallFoot, cliffShade). The ground composite then skips contact.
//
// Camera-local contact (Settings.ContactMapSize > 0, with terrain contact): the contact buffers
// cover the camera's area (a square in sqrt(2) steps of size on a grid of an eighth of it, so it
// holds still within a step) instead of +-GroundHalf, so their texels shrink as the camera zooms
// in. The static footprints are redrawn whole when the area steps, looking straight down in 3D
// through the instanced path; while nothing moves and the static set holds, nothing is redrawn.
// The sun map and the contact fade out toward their edge (a short area: the shadow distance).
//
// Camera-local light map (Settings.LightMapSize > 0, with an area each frame): the light map covers
// the same camera area instead of +-LightHalf; past its edge readers get its edge texel (clamped),
// which is the ambient colour unless a light sits on the edge.
//
// Ground marks (Settings.MarkMapSize > 0, with an area each frame): craters, tyre tracks, rubble and
// blood painted by marks.fs into a buffer over the same camera area, which DrawLitTerrain lays over
// the ground picture. The buffer is redrawn whole when the area steps or the marks are replaced, and
// otherwise only the marks added since are painted over it.
//
// Fog (Look.Fog, FogStart, FogEnd): the lit stacks and the lit terrain blend toward the fog colour
// with their distance from the eye (taken from the camera's view matrix while they draw).
//
// Connected to:
// - SpriteStack.h: DrawSpriteStacks does every stack pass (DrawSpriteStacksInstanced first in the 3D
//   ones); SpriteStackList holds the static set.
// - CellTerrain.h: the terrain the sun map draws and DrawLitTerrain shades.
// - The passes run on the render side and bind their buffers directly; StackLighting owns them
//   (Targets, SunMap, StaticSunMap) and frees them in its destructor.
// - RaylibBindings exposes engine.StackLighting; libraries/sprite_stack fx.py wraps it.

#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "IYI/SpriteStack.h"
#include "raylib.h"

namespace IYI {

// A light sprite in world space: "radial" (Size = diameter) or "cone" (Size = beam length).
struct StackLight
{
    const SpriteStack* Sprite = nullptr;
    float X = 0.0f, Y = 0.0f, Size = 0.0f, Rotation = 0.0f;
    float Power = 0.0f;          // scaled by the lamps level of the frame
    Color Tint = WHITE;          // the light's colour (alpha unused)
};

// A ground mark (SetStackLightingMarks): Kind 0 crater, 1 tyre track, 2 rubble, 3 blood, a Length x
// Width rectangle turned Rotation degrees round (X, Y), Strength 0..1.
struct StackMark
{
    int Kind = 0;
    float X = 0.0f, Y = 0.0f, Length = 0.0f, Width = 0.0f, Rotation = 0.0f, Strength = 1.0f;
};

enum class StaticSunMode { Smooth, Step, Exact };

struct StackLightingSettings
{
    float GroundHalf = 640.0f;       // the shadow and contact buffers cover +-GroundHalf world units
    float ShadowScale = 1.0f;        // texels per world unit in the sun shadow buffer
    float ContactScale = 1.0f;       // ... in the contact buffer
    float LightHalf = 1536.0f;       // the light map covers +-LightHalf
    float LightScale = 0.5f;         // ... at this many texels per unit (light pools are soft)
    float ContactStrength = 0.6f;
    Shader ShadowFs{}, ContactFs{}, LightFs{}, Sprite{};   // soft_shadow.fs, contact_ao.fs, lightmap.fs, lit_sprite
    Shader Blur{};                   // blur.fs: the shadow and contact buffers' blur, once per frame
    int SunMapSize = 0;              // > 0: sun shadows from a depth map this many texels square (above)
    int ContactMapSize = 0;          // > 0 (with terrain contact): the contact buffers this many texels square over the camera's area
    int LightMapSize = 0;            // > 0: the light map this many texels square over the camera's area (LightHalf, LightScale unused)
    int MarkMapSize = 0;             // > 0 (with MarkFs): the ground marks buffer this many texels square over the camera's area
    Shader MarkFs{};                 // marks.fs: paints the marks
    Shader SunDepth{};               // sun_depth.fs: the sun map pass
    Shader Terrain{};                // terrain.vs/fs: DrawLitTerrain
    Shader Wet{};                    // wet_ground (vs + fs); invalid: DrawStackLightingWet draws nothing
    Shader Glow{};                   // glow.fs; invalid: DrawStackLightingGlow draws nothing
};

// Clouds drifting over the ground: value noise in world space, cut at a cover level. Their shade
// joins the sun shadows (never darker than either alone) and falls on the stacks too; scaled by the
// sun, so there are none at night.
struct StackClouds
{
    float Cover = 0.0f;              // 0 clear .. 1 overcast
    float Strength = 0.0f;           // shade of a cloud relative to a sun shadow
    Vector2 Offset{};                // world units the cloud field has drifted
    float Scale = 900.0f;            // world units across one noise cell (cloud size)
};

// The look, set by the game whenever it likes (the bindings' properties). The game side keeps its own
// copy and every recorded lighting command carries the one in force when it was recorded.
struct StackLightingLook
{
    Color ShadowTint = BLACK;        // colour shadows and contact occlusion blend toward (alpha unused)
    StackClouds Clouds;
    float BaseShade = 0.0f, ShadeReach = 1.0f;          // the camera pass's height shade (SpriteStackPass)
    float WindTime = 0.0f, WindStrength = 0.0f;         // sway of items with Sway (SpriteStackPass)
    Vector2 WindDir{ 1.0f, 0.0f };
    Vector4 Wash[4]{};               // rotor downwash: (x, y, radius, strength 0..1); swaying instances lean away
    Color Fog = BLANK;               // distance haze of the lit stacks and terrain: its colour, alpha the most it covers
    float FogStart = 0.0f, FogEnd = 0.0f;               // world units from the eye where it begins and is full
    StaticSunMode SunMode = StaticSunMode::Smooth;
};

// Render side: the targets, caches and passes. Only replayed commands touch it (FramePacket.h); Python
// holds a game-side handle (RaylibBindings.cpp) that shares ownership with those commands.
struct StackLighting
{
    ~StackLighting();
    StackLighting() = default;
    StackLighting(const StackLighting&) = delete;
    StackLighting& operator=(const StackLighting&) = delete;

    StackLightingSettings Settings;
    // Owns every buffer below (and the static ones): a deque keeps the pointers valid as it grows.
    std::deque<RenderTexture2D> Targets;
    RenderTexture2D* ShadowRt = nullptr;
    RenderTexture2D* ContactRt = nullptr;
    RenderTexture2D* LightRt = nullptr;
    // The shadow and contact buffers blurred (Update, after they are drawn), r = occlusion: the
    // ground composites read one filtered texel each instead of blurring per ground pixel.
    RenderTexture2D* ShadowSoftRt = nullptr;
    RenderTexture2D* ContactSoftRt = nullptr;
    RenderTexture2D* ShadowBlurRt = nullptr;      // the blurs' first (along x) half
    RenderTexture2D* ContactBlurRt = nullptr;
    Rectangle ContactArea{};         // the world rectangle the contact buffers cover (+-GroundHalf, or the camera's)
    bool ContactStale = true;        // the static contact must be redrawn whole (a new set or area)
    bool ContactStill = false;       // the soft contact holds the static set alone, unchanged: no redraw while nothing moves
    Rectangle LightArea{};           // the world rectangle the light map covers (+-LightHalf, or the camera's)
    RenderTexture2D* MarkRt = nullptr;   // the ground marks, premultiplied (rgb painted over, a covered)
    Rectangle MarkArea{};            // the world rectangle MarkRt covers
    std::vector<StackMark> Marks;
    size_t MarksDrawn = 0;           // Marks[0, MarksDrawn) are in MarkRt
    bool MarksStale = true;          // MarkRt must be cleared and painted whole (a new area or list)
    // The sun map: a depth-only target, its view-projection, and where lookups start from.
    RenderTexture2D SunMap{};
    RenderTexture2D StaticSunMap{};     // terrain and fixed casters before moving ones are added
    bool StaticSunAttempted = false;
    bool StaticSunReady = false;
    Matrix StaticSunVP{};
    long long StaticSunKey = -1;
    const CellTerrain* StaticSunGround = nullptr;
    float StaticSunLayerHeight = 0.0f;
    bool SunMapReady = false;        // drawn this frame (a sun is up)
    Matrix SunViewProj{};
    Vector3 SunOffset{};             // world units toward the sun added before a lookup
    float SunBias = 0.0f;            // in the map's 0..1 depth
    float GroundSunBias = 0.0f;      // the terrain's (StackLighting.cpp kGroundSunBias)
    Vector2 SunSoft{ 0.0f, 0.0f };   // the shadow filter taps' reach in the map's 0..1 (kSunSoft)
    const CellTerrain* Ground = nullptr;          // this frame's terrain (RenderStackLighting)
    std::vector<SpriteStackItem> SunCasters;      // the static set as a list, for the sun map
    long long SunCastersKey = -1;
    float ShadowStrength = 0.0f;     // this frame's sun, for the composites
    Vector3 Ambient{ 1.0f, 1.0f, 1.0f };   // light-map colour where no light reaches; all 1 = daylight, no light map
    StackLightingLook Look;          // the recorded command's look, set before each pass

    // The static set, as last given.
    struct StaticEntry
    {
        SpriteStackItem Item;
        float Radius = 0.0f;         // the sprite's whole diagonal: covers any pivot
        float Top = 0.0f;            // height of its top layer
    };
    long long StaticKey = -1;
    std::unordered_map<long long, StaticEntry> Static;
    std::vector<StackLight> StaticLights;
    float StaticTop = 0.0f;          // the tallest static stack, for how far its shadow moves
    RenderTexture2D* StaticContactRt = nullptr;
    RenderTexture2D* StaticShadowRt[2] = { nullptr, nullptr };
    int Newer = 0;                   // which snapshot is the newer one
    bool HasOlder = false;           // Smooth: the other one still holds the snapshot before
    bool HasSun = false;             // a sun snapshot exists (Newer)
    Vector2 SunTip{};                // length * direction of the newer snapshot
    Vector2 RtSun[2]{};              // (direction, length) each snapshot was drawn with

    int BlurStepLoc = -1, BlurFalloffLoc = -1, BlurInvertLoc = -1, LightAreaLoc = -1, AmbientLoc = -1, LightMapLoc = -1;
    int ShadowCloudsLoc = -1, ShadowCloudScaleLoc = -1, ShadowHalfLoc = -1;
    int UseLightLoc = -1, CloudsLoc = -1, CloudScaleLoc = -1, SunLoc = -1, ShadowTintLoc = -1;
    int WetLightMapLoc = -1, WetLightAreaLoc = -1, WetAmbientLoc = -1, WetUseLightLoc = -1, WetUpLoc = -1, WetLoc = -1, WetSkyLoc = -1;
    int GlowAmbientLoc = -1;
    int SilhouetteLoc = -1, WashLoc = -1;                // lit_sprite: DrawStackSilhouettes, Look.Wash
    // Sun map uniforms: [0] lit_sprite, [1] terrain.
    int SunMapLoc[2] = { -1, -1 }, SunVPLoc[2] = { -1, -1 }, SunOffsetLoc[2] = { -1, -1 };
    int SunSoftLoc[2] = { -1, -1 };
    int SunBiasLoc[2] = { -1, -1 }, UseSunMapLoc[2] = { -1, -1 };
    int FogLoc[2] = { -1, -1 }, FogRangeLoc[2] = { -1, -1 }, EyeLoc[2] = { -1, -1 };   // [0] lit_sprite, [1] terrain
    int TerrainCloudsLoc = -1, TerrainCloudScaleLoc = -1, TerrainSunLoc = -1, TerrainTintLoc = -1;
    int TerrainLightMapLoc = -1, TerrainLightAreaLoc = -1, TerrainUseLightLoc = -1;
    int TerrainContactLoc = -1, TerrainContactAreaLoc = -1, TerrainContactStrengthLoc = -1;
    int TerrainMarksLoc = -1, TerrainMarkAreaLoc = -1, TerrainUseMarksLoc = -1;
    int TerrainTimeLoc = -1;                             // waterfalls move with the wind's clock
    int TerrainWaterLoc = -1, TerrainWaterAreaLoc = -1, TerrainUseWaterLoc = -1;   // the water map (terrain.fs)
    int WetEyeLoc = -1;
};

// A texture drawn as wet ground (DrawStackLightingWet): source rect of the texture, world dest.
struct WetPatch
{
    const Texture2D* Texture = nullptr;   // a cache entry: its GL id is read at replay
    Rectangle Source{}, Dest{};
};

// The static set of a frame: given every frame, read only when Key changes (Items may then be empty).
struct StackLightingStatic
{
    long long Key = -1;
    const std::vector<SpriteStackItem>* Items = nullptr;
    const std::vector<long long>* Ids = nullptr;           // one per item; null: no ids, always redraw whole
    const std::vector<StackLight>* Lights = nullptr;
};

void InitStackLighting(StackLighting& fx, const StackLightingSettings& settings);

// Offscreen passes; Update only. moving: the stacks that move (or every stack without a static set);
// sun: (direction, length, strength) from sprite_stack.daylight; lights: this frame's lights besides
// the static set's. stat may be null.
// ambient: the light-map colour where no light reaches (all 1 = daylight: no light map).
// ground and sunArea (world rectangle the camera sees): with a sun map, the terrain and area it
// is drawn for; no ground or an empty area leaves the frame without sun shadows.
void RenderStackLighting(StackLighting& fx, const std::vector<SpriteStackItem>& moving, float layerHeight,
                         Vector3 sun, Vector3 ambient, float lamps, const std::vector<StackLight>& lights,
                         const StackLightingStatic* stat, const CellTerrain* ground = nullptr,
                         Rectangle sunArea = Rectangle{});

// The ground marks (render side, from a recorded command): replace them all, or add to them.
void SetStackLightingMarks(StackLighting& fx, const std::vector<StackMark>& marks, bool replace);

// World pass, after the flat ground: contact and sun shadows.
void DrawStackLightingGround(const StackLighting& fx);
// World pass, after everything flat on the ground and before the stacks.
void DrawStackLightingGroundLight(const StackLighting& fx);
// World pass, after the ground light: patches of ground (their texture's alpha = how much is
// there) darkened by wetness 0..1 and reflecting the light map, smeared toward the viewer as wet
// asphalt stretches a light's reflection, plus a little sky by day. viewRotation: the camera's;
// eye: a perspective camera's world position (z up), whose view rays fan out, so each point's
// streak runs along its own ray; null for a 2D or orthographic camera (all along screen-up).
void DrawStackLightingWet(const StackLighting& fx, const std::vector<WetPatch>& patches, float wetness,
                          float viewRotation, Color sky, const Vector3* eye = nullptr);
// World pass, last: the night's light pools added once more, blurred wide, over everything (stacks
// included), as a glow round the lights: bloom from the light map the frame already has, no screen
// pass. strength 0..1; nothing by day.
void DrawStackLightingGlow(const StackLighting& fx, float strength);
// World pass: the stacks; by night every texel is lit at its own ground point, under clouds shaded,
// and with a sun map (ZUp) shadowed where it says so.
void DrawLitStacks(const StackLighting& fx, std::vector<SpriteStackItem>& items, const SpriteStackPass& pass,
                   std::vector<StaticStacksDraw>* statics = nullptr);
// World pass with a 3D camera, after everything opaque: items (their tint the colour) drawn flat at
// `opacity` only where something stands well in front of them (more than lit_sprite.vs SILHOUETTE_PULL world
// units nearer the eye), so units behind a building show through it and never over themselves.
void DrawStackSilhouettes(const StackLighting& fx, std::vector<SpriteStackItem>& items, const SpriteStackPass& pass,
                          float opacity);
// World pass with a 3D camera: DrawCellTerrain through terrain.vs/fs, sun-map shadows and clouds,
// terrain contact, and by night the light map on the cliff walls (the tops have it in the picture).
// water (optional): a map of the water over world rectangle waterArea, one texel per cell, that
// terrain.fs draws the water from per pixel (the library's sprite_stack.water).
void DrawLitTerrain(const StackLighting& fx, const CellTerrain& terrain, const Texture2D& texture, Rectangle area,
                    Color wall, const Texture2D* water = nullptr, Rectangle waterArea = {});

// The shaders of StackLightingSettings by address (null: none): cache entries, read when the recorded
// InitStackLighting replays, after this frame's Setup has compiled them.
struct StackLightingShaders
{
    const Shader *ShadowFs = nullptr, *ContactFs = nullptr, *LightFs = nullptr, *Sprite = nullptr, *Blur = nullptr;
    const Shader *Wet = nullptr, *Glow = nullptr, *SunDepth = nullptr, *Terrain = nullptr, *Marks = nullptr;
};

// The static set as one Render call sends it: read only when its key changed (Changed).
struct StaticSetData
{
    long long Key = -1;
    bool Changed = false;
    std::vector<SpriteStackItem> Items;
    bool HasIds = false;
    std::vector<long long> Ids;
    std::vector<StackLight> Lights;
};

// The game-side object (engine.StackLighting): a game owns one. The lighting itself (targets, caches,
// the static set's snapshots) is render-side state behind Fx, touched only by recorded commands
// (FramePacket.h); the look and the static key last sent are the game side's, set freely. Every method
// that draws or renders records the work (call it from the game thread, in the pass its twin above names).
struct StackLightingHandle
{
    std::shared_ptr<StackLighting> Fx;
    std::unordered_map<std::string, const SpriteStack*> LightSprites;   // by name, for reading lights
    StackLightingLook Look;
    long long SentStaticKey = -1;

    // Makes the lighting and records its InitStackLighting (Setup).
    StackLightingHandle(StackLightingSettings settings, const StackLightingShaders& shaders,
                        std::unordered_map<std::string, const SpriteStack*> lightSprites);

    bool StaticChanged(long long key) const { return key != SentStaticKey; }
    // Ground marks: replace them all or add to them (strength clamped to 0..1).
    void SetMarks(std::vector<StackMark> marks, bool replace);
    // RenderStackLighting. stat: null for no static set, else its key and, when Changed, its content.
    void Render(std::vector<SpriteStackItem> moving, float layerHeight, Vector3 sun, Vector3 ambient, float lamps,
                std::vector<StackLight> lights, std::shared_ptr<StaticSetData> stat,
                std::shared_ptr<CellTerrain> ground = nullptr, Rectangle sunArea = Rectangle{});
    void DrawTerrain(std::shared_ptr<CellTerrain> terrain, const Texture2D& texture, Rectangle area, Color wall,
                     const Texture2D* water = nullptr, Rectangle waterArea = Rectangle{});
    void DrawGround();
    void DrawGroundLight();
    void DrawGlow(float strength);
    void DrawWet(std::vector<WetPatch> patches, float wetness, float viewRotation, Color sky, std::optional<Vector3> eye = std::nullopt);
    // DrawStackSilhouettes of the items (3D camera).
    void DrawSilhouettes(std::vector<SpriteStackItem> items, float viewRotation, float layerHeight, float opacity,
                         std::shared_ptr<CellTerrain> ground = nullptr);
    // DrawLitStacks. statics: the static lists among the items, as the Python binding collects them.
    void DrawStacks(std::vector<SpriteStackItem> items, std::vector<StaticStacksDraw> statics, float viewRotation,
                    float heightScale, float layerHeight, bool zUp, std::shared_ptr<CellTerrain> ground = nullptr);

    // Look setters that clamp (the rest of Look is plain fields).
    void SetClouds(float cover, float strength, Vector2 offset, float scale);
    void SetHeightShade(float strength, float reach);
    void SetWind(float time, float directionDegrees, float strength);
    float WindDegrees() const;
    void SetWash(const std::vector<Vector4>& points);        // up to 4 (x, y, radius, strength); the rest ignored
    std::vector<Vector4> Wash() const;                       // the ones with strength > 0
};

} // namespace IYI
