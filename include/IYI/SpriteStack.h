// Declares sprite stacks: a pseudo-3D object drawn as horizontal slices (layers), bottom first, each
// raised along the camera's screen-up by its height. A stack's layers are packed into one texture when
// it is loaded, and DrawSpriteStacks draws a whole list of stacks in one call: depth-sorted back to
// front, every layer a quad in raylib's batch, no per-object state changes. Python decides what to
// draw (engine.draw_sprite_stacks takes plain tuples); this file does the per-layer work.
// The asset manager packs every stack's block of layers into shared texture pages (AtlasX, AtlasY), so
// a depth-sorted list that interleaves many stacks still draws as a few batches, not one per stack.
// Culling is part of the draw and always on: a stack whose quads all fall outside the render target
// (the screen through the camera, or a buffer in texture mode) is skipped before sorting, so a game
// passes whole lists and never culls in Python.
//
// Connected to:
// - AssetManager::GetSpriteStack loads and caches stacks (engine.load_sprite_stack).
// - RaylibBindings exposes SpriteStack and engine.draw_sprite_stacks.

#pragma once

#include <cmath>
#include <memory>
#include <utility>
#include <vector>
#include "IYI/CellTerrain.h"
#include "raylib.h"

namespace IYI {

struct SpriteStackMesh;   // SpriteStack.cpp: a stack's layers as one GPU mesh, for instanced draws

struct SpriteStack
{
    Texture2D Texture{};     // the layers in columns, bottom layer first, 1 texel apart (a shared page)
    int AtlasX = 0, AtlasY = 0;   // where this stack's block starts in Texture
    int Layers = 0;
    int Width = 0;           // one layer, in texels
    int Height = 0;
    int PerColumn = 1;       // layers per atlas column
    Vector3 Origin{};        // pivot: x, y in layer texels, z in layers (default: centre, bottom)
    float LayerStep = 1.0f;  // layer heights per layer: 2 for a stack of every second layer, at full height
    mutable std::shared_ptr<SpriteStackMesh> Mesh;   // render side: made on the stack's first instanced draw

    Rectangle LayerRect(int layer) const;
};

// Packs equally sized RGBA layer images (bottom first) into a near-square block `atlas` and fills
// `out` (its Texture's size, id 0, AtlasX/Y 0: the caller places and uploads it). False when the
// layers are missing or differ in size.
bool BuildSpriteStack(const std::vector<Image>& layers, SpriteStack& out, Image& atlas);

struct SpriteStackItem
{
    const SpriteStack* Stack = nullptr;
    float X = 0.0f, Y = 0.0f;    // world position of the pivot
    float Z = 0.0f;              // pivot height above the ground, world units
    float Rotation = 0.0f;       // degrees
    Color Tint = WHITE;
    float Scale = 1.0f;          // footprint and layer spacing
    float Lean = 0.0f;           // degrees added to the pass's ViewRotation for this stack's climb
    float Build = 1.0f;          // 0..1: how far the stack is built (BuildLayer); 1 draws every layer
    float Sway = 0.0f;           // degrees this stack leans at full wind (SwayLean); 0 = rigid
    float Damage = 0.0f;         // 0..1: texels crumble away, the top first, the rest sooty (instanced path only)
    float Ground = 0.0f;         // height of the ground under the pivot, set by DrawSpriteStacks from the pass's Ground
};

// A static list's render-side copy, chunked (SpriteStack.cpp): every item's instance record and ground
// height kept, the items in square chunks of the world, so an instanced draw culls and picks the LOD
// per chunk instead of per item, and a change (SpriteStackList.set) redoes only that item.
struct StaticStacks;
std::shared_ptr<StaticStacks> MakeStaticStacks();

// One SpriteStackList in a draw command: its render-side copy, and what the game side changed since
// the list's previous draw (the whole list the first time, else the items set() since).
struct StaticStacksDraw
{
    std::shared_ptr<StaticStacks> Index;
    std::vector<SpriteStackItem> All;
    std::vector<std::pair<unsigned int, SpriteStackItem>> Edits;
};

// Items read from Python once and drawn as often as wanted (engine.SpriteStackList): stacks that do
// not change (buildings, trees) are not converted from tuples again every frame.
struct SpriteStackList
{
    std::vector<SpriteStackItem> Items;
    std::shared_ptr<StaticStacks> Index = MakeStaticStacks();   // touched only by draw commands (render side)
    std::vector<std::pair<unsigned int, SpriteStackItem>> Edits;   // set() since the last draw that took them
    bool Sent = false;                                          // Index has the whole list

    // Replaces one item (index < Items.size()). Draws copy the list when called, so the change shows
    // from the next draw; a lit static set takes it as an edit, or the whole list when too many.
    void Set(size_t index, const SpriteStackItem& item);
};

// What one pass does with a list of items. The camera pass uses the defaults plus the camera's
// rotation and height scale; an offscreen pass (sun shadows, footprints) overrides the rest.
struct SpriteStackPass
{
    float ViewRotation = 0.0f;   // the Camera2D rotation: layers climb toward screen-up
    float HeightScale = 1.0f;    // world height -> ground offset (a tilted camera: cot(pitch); 0 = flat)
    float LayerHeight = 1.0f;    // world units per layer
    Vector2 Offset{};            // added to every position (a world-anchored buffer's origin)
    bool OverrideTint = false;   // every item drawn in Tint (a shadow pass)
    Color Tint = WHITE;
    float MaxZ = INFINITY;       // items whose Z is above this are skipped (footprints of grounded things)
    bool Sort = true;            // back to front for the camera; an opaque one-colour pass needs no order
    // Each vertex carries the ground point under it (world x, y without the height lift and Offset)
    // in its normal, as normalize(x, y, kGroundNormalScale): a vertex shader recovers it with
    // vertexNormal.xy / vertexNormal.z * kGroundNormalScale. Lets a shader light every texel at its
    // own ground position (per-texel light maps) while the whole list stays one batch.
    bool GroundInNormal = false;
    // With GroundInNormal: items whose Z is above this get normal (0, 0, -1) instead, "no ground under
    // me" (an aircraft is not lit by light pools on the ground).
    float GroundMaxZ = INFINITY;
    // Height shade: a layer h world units above the ground is darkened by
    // BaseShade * (1 - h / ShadeReach)^2, so walls darken toward their foot (occlusion on the stack
    // itself; the contact pass darkens the ground round it). 0 = off: offscreen passes leave it off.
    float BaseShade = 0.0f;
    float ShadeReach = 1.0f;
    // Wind for items with Sway: SwayTime in seconds, SwayDir the world direction it blows toward
    // (unit), SwayStrength 0..1. Only the camera pass sways, so cached shadows never redraw for it.
    float SwayTime = 0.0f;
    float SwayStrength = 0.0f;
    Vector2 SwayDir{ 1.0f, 0.0f };
    // A 3D camera (set_camera_3d): layers sit at their real height on the world z axis instead of
    // being lifted toward screen-up, and HeightScale is not used: Lean and the sway tilt each stack
    // in the world (Lean toward screen-left, by ViewRotation, the camera's yaw), and the sort goes
    // by the camera's depth.
    bool ZUp = false;
    // With ZUp: the terrain the items stand on. Each item is raised by the ground height under its
    // pivot; its Z stays the height above that ground (MaxZ and GroundMaxZ compare it as given).
    const CellTerrain* Ground = nullptr;
    // Layer LOD: when a stack's layers lie closer than this many pixels apart on the target, only
    // every k-th layer (and the top one) is drawn, so a far stack costs what it shows. 0 = every layer.
    float LodPixels = 1.0f;
};

constexpr int kMaxLodStep = 8;   // a top-down view puts every layer in one place: still draw some

constexpr float kGroundNormalScale = 1024.0f;

// How layer `layer` of a stack being built shows: the layers come in bottom first, kBuildFlight of
// them in flight at once, each fading in while it settles from kBuildDrop layers above its place.
// Build 0 shows nothing, 1 the finished stack.
constexpr float kBuildFlight = 4.0f;
constexpr float kBuildDrop = 3.0f;
struct BuildState
{
    float Alpha = 1.0f;          // 0 = not drawn
    float Drop = 0.0f;           // layers above its place
};
BuildState BuildLayer(float build, int layer, int layers);

// Degrees the wind adds to an item's lean in this pass (0 without Sway or wind): gusts roll across
// the world along the wind, each item flutters on its own phase, and the lean follows the wind's
// sideways part on screen (layers climb screen-up, so a lean can only move them sideways).
float SwayLean(const SpriteStackItem& item, const SpriteStackPass& pass);

// True when nothing the item draws in this pass can land in clip space: its quads span the footprint
// at the bottom layer and at the top one (layers climb in a straight line), so those eight corners,
// through mvp (model-view times projection), bound everything it draws. Tested in homogeneous
// coordinates, so it holds for a perspective camera too.
bool SpriteStackOffTarget(const SpriteStackItem& item, const SpriteStackPass& pass, const Matrix& mvp);

// Culls (SpriteStackOffTarget against rlgl's current model-view and projection), sorts and draws
// items; they are reordered and the culled ones removed. Profiled as cull, sort and emit.
void DrawSpriteStacks(std::vector<SpriteStackItem>& items, const SpriteStackPass& pass);

// ZUp passes (a 3D camera, Offset 0): draws the items that can go as instances, grouped by stack, one
// instanced draw per stack, and removes them (and the culled ones) from items, leaving the rest for
// DrawSpriteStacks. Like Unity's or Unreal's automatic instancing: the game still gives one item per
// object (a building, a tree); each stack's layers are uploaded once as a mesh (SpriteStack::Mesh) and
// every item becomes a small instance record (position, rotation, scale, tint, build, lean, sway), so
// changing an object changes only its record. The vertex shader places the layers, shades them
// (BaseShade), raises and fades them (build) and tilts them (lean, the pass's sway): `shader` must be
// built from lit_sprite.vs (sprite_stack library) and active. Items with a see-through tint stay in
// items (blending needs DrawSpriteStacks' back-to-front order), and so does everything when the
// shader lacks the instance inputs. Profiled as "instanced" (stacks, draws, quads).
// statics: SpriteStackLists drawn the same way through their chunks (StaticStacks; counted as
// "chunks", the visible ones); what of them cannot go as instances is added to items.
void DrawSpriteStacksInstanced(std::vector<SpriteStackItem>& items, const SpriteStackPass& pass, const Shader& shader,
                               std::vector<StaticStacksDraw>* statics = nullptr);
// Brings the statics' copies up to date and adds all their items to items (a pass that draws them one
// by one: no 3D camera, no instancing).
void ExpandStaticStacks(std::vector<StaticStacksDraw>& statics, const SpriteStackPass& pass, std::vector<SpriteStackItem>& items);
// The statics' chunk choice for one frame, without drawing (tests): (item index, LOD level 0..3) of
// every item an instanced draw through mvp would draw, on a target half_size pixels across from its centre.
std::vector<std::pair<unsigned int, int>> SelectStaticStacks(StaticStacksDraw& statics, const SpriteStackPass& pass,
                                                             const Matrix& mvp, Vector2 half_size);

// Culling is on unless switched off here (engine.set_sprite_stack_culling): only to measure what it
// saves, with everything else the same.
void SetSpriteStackCulling(bool enabled);
// Instancing likewise (engine.set_sprite_stack_instancing): off, every item goes through DrawSpriteStacks.
void SetSpriteStackInstancing(bool enabled);

namespace Api {
// Game side (engine.draw_sprite_stacks): records DrawSpriteStacks of `items` with `pass` in the current
// pass. This is the one way a game draws sprite stacks (rule 25).
void DrawSpriteStacks(std::vector<SpriteStackItem> items, const SpriteStackPass& pass);
} // namespace Api

} // namespace IYI
