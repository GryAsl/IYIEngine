// Implements cell terrain (CellTerrain.h): the height lookup and the draw. The geometry (tops,
// walls) is made by one set of functions below; with a shader it is built once into meshes, a chunk
// of cells each, and the chunks that overlap the area are drawn from them; without one it goes into
// raylib's batch as quads every call, only for the cells under the area.

#include "IYI/CellTerrain.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include "IYI/FramePacket.h"
#include "IYI/Profiler.h"
#include "raymath.h"
#include "rlgl.h"

namespace IYI {

namespace {

const float* CornersOf(const CellTerrain& t, int cx, int cy)
{
    return &t.Corners[(static_cast<size_t>(cy) * t.Columns + cx) * 4];
}

constexpr int kChunk = 32;            // cells per side of a mesh chunk (its vertices stay under 65536)
constexpr int kHeightUnit = 15;       // the height map's texture unit: the last one GL 3.3 promises, clear of the caller's

// One vertex of a quad: position, texture coordinates, normal. The tops carry world x, y as their
// coordinates (mapped onto the area when drawn); a wall carries (0, its low height).
struct TerrainVertex
{
    Vector3 P;
    Vector2 Uv;
    Vector3 N;
};
using TerrainQuad = std::array<TerrainVertex, 4>;

constexpr Vector3 kUp{ 0.0f, 0.0f, 1.0f };

TerrainVertex Flat(float x, float y, float z, Vector3 n) { return { { x, y, z }, { x, y }, n }; }

// The tops of cells [x0, x1] x [y0, y1].
template <class Out>
void EmitTops(const CellTerrain& t, int x0, int y0, int x1, int y1, Out&& out)
{
    for (int cy = y0; cy <= y1; ++cy) {
        for (int cx = x0; cx <= x1; ++cx) {
            const float* c = CornersOf(t, cx, cy);
            const float wx0 = t.Origin.x + cx * t.Cell, wy0 = t.Origin.y + cy * t.Cell;
            const float wx1 = wx0 + t.Cell, wy1 = wy0 + t.Cell;
            out(TerrainQuad{ Flat(wx0, wy0, c[0], kUp), Flat(wx0, wy1, c[3], kUp), Flat(wx1, wy1, c[2], kUp),
                             Flat(wx1, wy0, c[1], kUp) });
        }
    }
}

// A wall, facing the lower side (terrain.fs lights walls by it). Its uv's y carries the low height at
// that corner: terrain.fs shades a wall's foot from fragWorld.z minus it. Its x is 0, or on a
// waterfall 1 plus the wall's height there, so terrain.fs knows where the fall's lip is.
TerrainQuad WallQuad(const CellTerrainWall& w)
{
    const Vector3 n{ w.Down.x, w.Down.y, 0.0f };
    const float ua = w.Water ? 1.0f + w.HighA - w.LowA : 0.0f, ub = w.Water ? 1.0f + w.HighB - w.LowB : 0.0f;
    return { TerrainVertex{ { w.A.x, w.A.y, w.HighA }, { ua, w.LowA }, n },
             TerrainVertex{ { w.B.x, w.B.y, w.HighB }, { ub, w.LowB }, n },
             TerrainVertex{ { w.B.x, w.B.y, w.LowB }, { ub, w.LowB }, n },
             TerrainVertex{ { w.A.x, w.A.y, w.LowA }, { ua, w.LowA }, n } };
}

// Quads (as rlgl draws them: triangles 0 1 2 and 0 2 3) into an uploaded mesh; empty: no mesh.
Mesh ToMesh(const std::vector<TerrainVertex>& v)
{
    Mesh m{};
    if (v.empty()) return m;
    const int n = static_cast<int>(v.size());
    m.vertexCount = n;
    m.triangleCount = n / 2;
    m.vertices = static_cast<float*>(MemAlloc(sizeof(float) * 3 * n));
    m.texcoords = static_cast<float*>(MemAlloc(sizeof(float) * 2 * n));
    m.normals = static_cast<float*>(MemAlloc(sizeof(float) * 3 * n));
    m.indices = static_cast<unsigned short*>(MemAlloc(sizeof(unsigned short) * 3 * m.triangleCount));
    for (int i = 0; i < n; ++i) {
        const TerrainVertex& t = v[i];
        m.vertices[3 * i] = t.P.x; m.vertices[3 * i + 1] = t.P.y; m.vertices[3 * i + 2] = t.P.z;
        m.texcoords[2 * i] = t.Uv.x; m.texcoords[2 * i + 1] = t.Uv.y;
        m.normals[3 * i] = t.N.x; m.normals[3 * i + 1] = t.N.y; m.normals[3 * i + 2] = t.N.z;
    }
    for (int q = 0; q < n / 4; ++q) {
        const auto b = static_cast<unsigned short>(4 * q);
        const unsigned short tri[6] = { b, static_cast<unsigned short>(b + 1), static_cast<unsigned short>(b + 2),
                                        b, static_cast<unsigned short>(b + 2), static_cast<unsigned short>(b + 3) };
        std::copy(tri, tri + 6, m.indices + 6 * q);
    }
    UploadMesh(&m, false);
    return m;
}

} // namespace

// The meshes of one terrain: per chunk its tops (textured), its walls (plain), and the world
// rectangle all of it lies in; and the height map (one RGBA float texel per cell, its four corners).
struct CellTerrainMeshes
{
    struct Chunk
    {
        Rectangle Bounds{};
        Mesh Ground{}, Walls{};
        int Cells = 0;
        bool Cliff = false;    // terrain.fs's cliff shade can show in it (HasCliff)
    };
    std::vector<Chunk> Chunks;
    unsigned int Heights = 0;

    CellTerrainMeshes() = default;
    CellTerrainMeshes(const CellTerrainMeshes&) = delete;
    CellTerrainMeshes& operator=(const CellTerrainMeshes&) = delete;
    ~CellTerrainMeshes()
    {
        if (!IsWindowReady()) return;          // after CloseWindow the GL context went with them
        for (Chunk& c : Chunks) {
            if (c.Ground.vertexCount > 0) UnloadMesh(c.Ground);
            if (c.Walls.vertexCount > 0) UnloadMesh(c.Walls);
        }
        if (Heights) rlUnloadTexture(Heights);
    }
};

namespace {

// terrain.fs cliffShade: a neighbour shades a point only when it rises over 1 world unit (smoothstep(1.0, 4.0,
// rise)) within max(3, 0.6 * rise) of it; on smooth ground that rise needs a slope steeper than 1 in 3.
constexpr float kCliffRise = 1.0f, kCliffReach = 3.0f;

// Whether that shade can show anywhere in cells [x0, x1] x [y0, y1]: some neighbour steps up over a cell
// where they meet (corners are per cell) by kCliffRise, less what the cell's own slope gives within
// kCliffReach of the edge. Corners: top-left, top-right, bottom-right, bottom-left (x right, y down).
bool HasCliff(const CellTerrain& t, int x0, int y0, int x1, int y1)
{
    static const int cx[4] = { 0, 1, 1, 0 }, cy[4] = { 0, 0, 1, 1 };
    auto corners = [&](int x, int y) { return &t.Corners[(static_cast<size_t>(y) * t.Columns + x) * 4]; };
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float* k = corners(x, y);
            const float range = *std::max_element(k, k + 4) - *std::min_element(k, k + 4);
            const float slope = range / t.Cell * 1.5f * kCliffReach;   // the most the cell climbs over kCliffReach (with room)
            for (int c = 0; c < 4; ++c) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int lx = cx[c] - dx, ly = cy[c] - dy, nx = x + dx, ny = y + dy;
                        if ((dx == 0 && dy == 0) || lx < 0 || lx > 1 || ly < 0 || ly > 1 || nx < 0 || ny < 0 ||
                            nx >= t.Columns || ny >= t.Rows) {
                            continue;                          // not a neighbour sharing this corner
                        }
                        const int nc = ly == 0 ? lx : 3 - lx;
                        if (corners(nx, ny)[nc] - k[c] + slope >= kCliffRise) return true;
                    }
                }
            }
        }
    }
    return false;
}

std::shared_ptr<CellTerrainMeshes> BuildMeshes(const CellTerrain& t)
{
    auto meshes = std::make_shared<CellTerrainMeshes>();
    const int across = (t.Columns + kChunk - 1) / kChunk, down = (t.Rows + kChunk - 1) / kChunk;
    // Each wall goes with the chunk of the cell on its higher side (any one chunk would do: the
    // bounds below reach a cell past the chunk's cells).
    std::vector<std::vector<const CellTerrainWall*>> walls(static_cast<size_t>(across) * down);
    for (const CellTerrainWall& w : t.Walls) {
        const float mx = (w.A.x + w.B.x) * 0.5f - w.Down.x * t.Cell * 0.25f;
        const float my = (w.A.y + w.B.y) * 0.5f - w.Down.y * t.Cell * 0.25f;
        const int cx = std::clamp(static_cast<int>(std::floor((mx - t.Origin.x) / t.Cell)), 0, t.Columns - 1);
        const int cy = std::clamp(static_cast<int>(std::floor((my - t.Origin.y) / t.Cell)), 0, t.Rows - 1);
        walls[static_cast<size_t>(cy / kChunk) * across + cx / kChunk].push_back(&w);
    }
    std::vector<TerrainVertex> ground, wall;
    for (int j = 0; j < down; ++j) {
        for (int i = 0; i < across; ++i) {
            const int x0 = i * kChunk, y0 = j * kChunk;
            const int x1 = std::min(t.Columns, x0 + kChunk) - 1, y1 = std::min(t.Rows, y0 + kChunk) - 1;
            ground.clear();
            wall.clear();
            auto toGround = [&](const TerrainQuad& q) { ground.insert(ground.end(), q.begin(), q.end()); };
            EmitTops(t, x0, y0, x1, y1, toGround);
            for (const CellTerrainWall* w : walls[static_cast<size_t>(j) * across + i]) {
                const TerrainQuad q = WallQuad(*w);
                wall.insert(wall.end(), q.begin(), q.end());
            }
            if (ground.size() > 65535 || wall.size() > 65535) {
                TraceLog(LOG_WARNING, "CellTerrain: a chunk has too many vertices for one mesh; it is not drawn");
                continue;
            }
            const float reach = t.Cell;
            CellTerrainMeshes::Chunk c;
            c.Bounds = Rectangle{ t.Origin.x + x0 * t.Cell - reach, t.Origin.y + y0 * t.Cell - reach,
                                  (x1 - x0 + 1) * t.Cell + 2 * reach, (y1 - y0 + 1) * t.Cell + 2 * reach };
            c.Cells = (x1 - x0 + 1) * (y1 - y0 + 1);
            c.Cliff = HasCliff(t, x0, y0, x1, y1);
            c.Ground = ToMesh(ground);
            c.Walls = ToMesh(wall);
            meshes->Chunks.push_back(c);
        }
    }
    meshes->Heights = rlLoadTexture(t.Corners.data(), t.Columns, t.Rows, RL_PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, 1);
    return meshes;
}

// Without a shader: the cells under the area into raylib's batch, every call.
void DrawImmediate(const CellTerrain& t, const Texture2D& texture, Rectangle area, Color wall, bool upsideDown)
{
    const int x0 = std::max(0, static_cast<int>(std::floor((area.x - t.Origin.x) / t.Cell)));
    const int y0 = std::max(0, static_cast<int>(std::floor((area.y - t.Origin.y) / t.Cell)));
    const int x1 = std::min(t.Columns - 1, static_cast<int>(std::floor((area.x + area.width - t.Origin.x) / t.Cell)));
    const int y1 = std::min(t.Rows - 1, static_cast<int>(std::floor((area.y + area.height - t.Origin.y) / t.Cell)));
    // Walls on or by the area: a wall is found by its end A, so the bounds are one cell wider on
    // every side.
    const float x0w = area.x - t.Cell, y0w = area.y - t.Cell;
    const float x1w = area.x + area.width + t.Cell, y1w = area.y + area.height + t.Cell;
    auto near = [&](const CellTerrainWall& w) { return w.A.x >= x0w && w.A.x <= x1w && w.A.y >= y0w && w.A.y <= y1w; };
    ProfileCount("cells", static_cast<double>(std::max(0, x1 - x0 + 1)) * std::max(0, y1 - y0 + 1));

    auto mapped = [&](const TerrainQuad& q) {        // world x, y -> the texture over the area
        rlColor4ub(255, 255, 255, 255);
        for (const TerrainVertex& v : q) {
            const float k = (v.Uv.y - area.y) / area.height;
            rlNormal3f(v.N.x, v.N.y, v.N.z);
            rlTexCoord2f((v.Uv.x - area.x) / area.width, upsideDown ? 1.0f - k : k);
            rlVertex3f(v.P.x, v.P.y, v.P.z);
        }
    };
    rlSetTexture(texture.id);
    // Keep one quad stream open. rlgl flushes a full batch at quad boundaries; opening and closing
    // a primitive for every cell adds CPU work.
    rlBegin(RL_QUADS);
    EmitTops(t, x0, y0, x1, y1, mapped);
    rlEnd();
    // rlSetTexture(0) keeps the batch's current texture: the walls need the plain white one.
    rlSetTexture(rlGetTextureIdDefault());
    rlBegin(RL_QUADS);
    for (const CellTerrainWall& w : t.Walls) {
        if (!near(w)) continue;
        rlColor4ub(wall.r, wall.g, wall.b, wall.a);
        for (const TerrainVertex& v : WallQuad(w)) {
            rlNormal3f(v.N.x, v.N.y, v.N.z);
            rlTexCoord2f(v.Uv.x, v.Uv.y);
            rlVertex3f(v.P.x, v.P.y, v.P.z);
        }
    }
    rlEnd();
    rlSetTexture(0);
}

} // namespace

float CellTerrainHeight(const CellTerrain& t, float x, float y)
{
    const float gx = (x - t.Origin.x) / t.Cell, gy = (y - t.Origin.y) / t.Cell;
    const int cx = static_cast<int>(std::floor(gx)), cy = static_cast<int>(std::floor(gy));
    if (cx < 0 || cy < 0 || cx >= t.Columns || cy >= t.Rows) return 0.0f;
    const float* c = CornersOf(t, cx, cy);
    const float fx = gx - cx, fy = gy - cy;
    const float top = c[0] + (c[1] - c[0]) * fx, bottom = c[3] + (c[2] - c[3]) * fx;
    return top + (bottom - top) * fy;
}

void BuildCellTerrainWalls(CellTerrain& t)
{
    t.Walls.clear();
    t.Meshes.reset();
    const float eps = 1e-3f;
    const bool water = t.Water.size() == static_cast<size_t>(t.Columns) * t.Rows;
    auto wet = [&](int cx, int cy) { return water && t.Water[static_cast<size_t>(cy) * t.Columns + cx]; };
    auto add = [&](Vector2 a, Vector2 b, float pa, float pb, float qa, float qb, Vector2 toward, bool fall) {
        if (std::fabs(pa - qa) <= eps && std::fabs(pb - qb) <= eps) return;
        if (pa + pb >= qa + qb) t.Walls.push_back({ a, b, pa, pb, qa, qb, toward, fall });
        else t.Walls.push_back({ a, b, qa, qb, pa, pb, Vector2{ -toward.x, -toward.y }, fall });
    };
    for (int cy = 0; cy < t.Rows; ++cy) {
        for (int cx = 0; cx < t.Columns; ++cx) {
            const float* c = CornersOf(t, cx, cy);
            const float x1 = t.Origin.x + (cx + 1) * t.Cell, y1 = t.Origin.y + (cy + 1) * t.Cell;
            const float x0 = x1 - t.Cell, y0 = y1 - t.Cell;
            if (cx + 1 < t.Columns) {                       // right side: our TR, BR against its TL, BL
                const float* n = CornersOf(t, cx + 1, cy);
                add({ x1, y0 }, { x1, y1 }, c[1], c[2], n[0], n[3], { 1.0f, 0.0f }, wet(cx, cy) && wet(cx + 1, cy));
            }
            if (cy + 1 < t.Rows) {                          // bottom side: our BL, BR against its TL, TR
                const float* n = CornersOf(t, cx, cy + 1);
                add({ x0, y1 }, { x1, y1 }, c[3], c[2], n[0], n[1], { 0.0f, 1.0f }, wet(cx, cy) && wet(cx, cy + 1));
            }
        }
    }
}

void DrawCellTerrain(const CellTerrain& t, const Texture2D& texture, Rectangle area, Color wall, bool upsideDown,
                     const Shader* shader)
{
    if (t.Columns <= 0 || t.Rows <= 0 || area.width <= 0.0f || area.height <= 0.0f) return;
    ProfileScope scope("terrain");
    if (!shader) {
        DrawImmediate(t, texture, area, wall, upsideDown);
        return;
    }
    if (!t.Meshes) t.Meshes = BuildMeshes(t);
    rlDrawRenderBatchActive();                         // what the batch holds goes first: meshes draw at once
    MaterialMap maps[12] = {};                        // raylib's MAX_MATERIAL_MAPS (config.h): DrawMesh reads them all
    const Material material{ *shader, maps, { 0.0f, 0.0f, 0.0f, 0.0f } };
    const int uvLoc = GetShaderLocation(*shader, "uTerrainUv");
    if (uvLoc >= 0) {                                  // world x, y -> the texture over the area
        const float uv[4] = { 1.0f / area.width, (upsideDown ? -1.0f : 1.0f) / area.height, -area.x / area.width,
                              upsideDown ? 1.0f + area.y / area.height : -area.y / area.height };
        SetShaderValue(*shader, uvLoc, uv, SHADER_UNIFORM_VEC4);
    }
    const int heightsLoc = GetShaderLocation(*shader, "uTerrainHeights");
    const int gridLoc = heightsLoc >= 0 && t.Meshes->Heights ? GetShaderLocation(*shader, "uTerrainGrid") : -1;
    float grid[4] = { t.Origin.x, t.Origin.y, t.Cell, 0.0f };
    if (gridLoc >= 0) {                                // the height map, for terrain.fs's cliff shade
        SetShaderValue(*shader, gridLoc, grid, SHADER_UNIFORM_VEC4);
        rlActiveTextureSlot(kHeightUnit);
        rlEnableTexture(t.Meshes->Heights);
        SetShaderValue(*shader, heightsLoc, &kHeightUnit, SHADER_UNIFORM_INT);
        rlActiveTextureSlot(0);
    }
    maps[MATERIAL_MAP_DIFFUSE].texture = texture;
    maps[MATERIAL_MAP_DIFFUSE].color = WHITE;
    int chunks = 0, cells = 0;
    for (const CellTerrainMeshes::Chunk& c : t.Meshes->Chunks) {
        if (c.Ground.vertexCount == 0 || !CheckCollisionRecs(c.Bounds, area)) continue;
        if (gridLoc >= 0 && grid[3] != (c.Cliff ? 1.0f : 0.0f)) {   // a chunk without cliffs skips the shade
            grid[3] = c.Cliff ? 1.0f : 0.0f;
            SetShaderValue(*shader, gridLoc, grid, SHADER_UNIFORM_VEC4);
        }
        DrawMesh(c.Ground, material, MatrixIdentity());
        ++chunks;
        cells += c.Cells;
    }
    if (uvLoc >= 0) {                                  // walls keep their own (0, low)
        const float identity[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
        SetShaderValue(*shader, uvLoc, identity, SHADER_UNIFORM_VEC4);
    }
    maps[MATERIAL_MAP_DIFFUSE].texture = Texture2D{ rlGetTextureIdDefault(), 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    maps[MATERIAL_MAP_DIFFUSE].color = wall;
    for (const CellTerrainMeshes::Chunk& c : t.Meshes->Chunks) {
        if (c.Walls.vertexCount > 0 && CheckCollisionRecs(c.Bounds, area)) DrawMesh(c.Walls, material, MatrixIdentity());
    }
    rlActiveTextureSlot(0);
    ProfileCount("cells", cells);
    ProfileCount("chunks", chunks);
}

namespace Api {

std::shared_ptr<CellTerrain> MakeCellTerrain(int columns, int rows, float cell, Vector2 origin, std::vector<float> corners,
                                             std::vector<bool> water)
{
    if (columns <= 0 || rows <= 0 || cell <= 0.0f) throw std::invalid_argument("CellTerrain: columns, rows and cell must be > 0");
    if (corners.size() != static_cast<size_t>(columns) * rows * 4) throw std::invalid_argument("CellTerrain: 4 corners per cell");
    if (!water.empty() && water.size() != static_cast<size_t>(columns) * rows) throw std::invalid_argument("CellTerrain: one water flag per cell");
    std::shared_ptr<CellTerrain> t = RenderOwned(new CellTerrain{ columns, rows, cell, origin, std::move(corners), std::move(water) });
    BuildCellTerrainWalls(*t);
    return t;
}

void DrawCellTerrain(std::shared_ptr<CellTerrain> terrain, const Texture2D& texture, Rectangle area, Color wall, bool upsideDown)
{
    Record([terrain, tex = &texture, area, wall, upsideDown]() { IYI::DrawCellTerrain(*terrain, *tex, area, wall, upsideDown); });
}

} // namespace Api

} // namespace IYI
