// Declares cell terrain: ground made of square cells, each with its own height at its four corners,
// for a game drawn through a 3D camera (engine.set_camera_3d). A cell's top is the flat or sloped
// quad through its corners; where two neighbouring cells do not meet along their shared side, a
// vertical wall closes the gap (a cliff). Corners are per cell, not shared, so cliffs are exact.
//
// The tops are not textured per cell: a texture covering a world rectangle (a ground picture the
// game drew top-down, e.g. into a render texture) is laid over them by world x, y, so everything
// drawn flat on that picture follows the ground up and down. The walls are one plain colour. A
// shader drawing the terrain gets its corner heights as a texture (uTerrainHeights, one RGBA float
// texel per cell; uTerrainGrid = origin x, y, cell, 1), so terrain.fs shades the ground at a cliff's
// foot per pixel from how near and how much higher the neighbouring cells are. A mesh chunk with no
// such rise in it gets w = 0 instead, and the shader skips that work there.
//
// Cells may be marked as water (Water). A wall between two water cells is a waterfall: its uv's x
// is 1 plus the wall's height, and terrain.fs draws falling water on it where the stream crosses.
//
// The stack lighting's sun map (StackLighting.h) draws the terrain too, so hills cast shadows and
// receive them (DrawLitTerrain).
//
// With a shader, the terrain is drawn from meshes built once per terrain (chunks of cells, each
// drawn when it overlaps the area), not sent vertex by vertex every frame: the tops carry world x, y
// as their texture coordinates and the shader maps them onto the area (terrain.vs, uTerrainUv).
//
// Heights also lift what stands on the ground: a sprite stack pass or a particle draw given the
// terrain adds the height under each item to its z, so items keep z as height above their ground
// (what the shadow, contact and light passes expect) and only the 3D draw raises them.
//
// Connected to:
// - SpriteStack.h: SpriteStackPass::Ground; Particles.h: DrawParticles' ground.
// - RaylibBindings exposes engine.CellTerrain.

#pragma once

#include <memory>
#include <vector>
#include "raylib.h"

namespace IYI {

// A wall between two neighbouring cells that do not meet: their shared side from A to B, the
// higher cell's heights at A and B, the lower one's, and the unit direction toward the lower cell.
struct CellTerrainWall
{
    Vector2 A{}, B{};
    float HighA = 0.0f, HighB = 0.0f, LowA = 0.0f, LowB = 0.0f;
    Vector2 Down{};
    bool Water = false;              // both cells are water: a waterfall
};

struct CellTerrainMeshes;

struct CellTerrain
{
    int Columns = 0, Rows = 0;
    float Cell = 1.0f;               // world units across a cell
    Vector2 Origin{};                // world point of cell (0, 0)'s top-left corner
    // Four heights per cell, cells row by row (x fastest): its top-left, top-right, bottom-right
    // and bottom-left corner. World y grows downward on the ground, as in the 2D view.
    std::vector<float> Corners;
    std::vector<bool> Water;         // per cell, as Corners (empty: no water); read by BuildCellTerrainWalls
    std::vector<CellTerrainWall> Walls;   // BuildCellTerrainWalls, from Corners
    // The GPU meshes, built by the first draw with a shader (CellTerrain.cpp); freed with the last copy.
    mutable std::shared_ptr<CellTerrainMeshes> Meshes;
};

// Finds every wall (each shared side once). Call after filling Corners.
void BuildCellTerrainWalls(CellTerrain& terrain);

// Height of the ground at world (x, y): the cell's corners interpolated bilinearly; 0 off the grid.
float CellTerrainHeight(const CellTerrain& terrain, float x, float y);

// Draws the cells that overlap `area` (world rectangle): their tops with `texture`, which covers
// `area` (upsideDown: a render texture, stored bottom row first) and their walls in `wall`.
// World pass with a 3D camera. With `shader` (active, in BeginShaderMode) the cached meshes are
// drawn through it, whole chunks round the area: the shader maps the tops' world x, y onto the
// texture by its uTerrainUv (xy scale, zw offset), or samples a plain texture without one (a depth
// pass); walls take `wall` as colDiffuse; a uTerrainHeights sampler gets the height map on texture
// unit 15. Its other samplers must already be bound to their units (a mesh draw skips the batch's
// sampler binding). Without a shader: quads into raylib's batch.
void DrawCellTerrain(const CellTerrain& terrain, const Texture2D& texture, Rectangle area, Color wall, bool upsideDown,
                     const Shader* shader = nullptr);

namespace Api {
// Game side (engine.CellTerrain): validates and builds the terrain (columns, rows, cell > 0; four corners
// per cell; water empty or one flag per cell, else std::invalid_argument), shared with the commands that
// draw it and freed on the render side (RenderOwned).
std::shared_ptr<CellTerrain> MakeCellTerrain(int columns, int rows, float cell, Vector2 origin, std::vector<float> corners,
                                             std::vector<bool> water = {});
// Records DrawCellTerrain (no shader) in the current world pass; texture stays alive until the render side draws.
void DrawCellTerrain(std::shared_ptr<CellTerrain> terrain, const Texture2D& texture, Rectangle area, Color wall,
                     bool upsideDown = true);
} // namespace Api

} // namespace IYI
