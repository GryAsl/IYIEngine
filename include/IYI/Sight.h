// Declares SightGrid: line of sight and ray hits over a height field (the ground) with blockers standing
// on it (buildings, walls), for games that ask "can A see B" and "what does this ray hit" many times a
// frame (an RTS: who sees whom, what the cursor points at).
//
// The ground is a vertex grid interpolated bilinearly, the same surface CellTerrain draws. Blockers are a
// second, usually finer grid holding each cell's blocker height above the ground (0 = open); the class
// keeps one "top" per blocker cell (ground at its centre plus the blocker), so a sight line is one lookup
// per sample. A line is blocked where it passes under a top; samples are half the finer cell apart.
//
// Foliage (optional, same cells as the blockers) is a band from a low to a high height above the
// ground (a tree crown, a bush) that hides without blocking: Visible also reports how many metres of
// foliage the line passed through, and the game decides what that much leaf cover means.
//
// Part of IYIEngineSim (plain C++ and raylib types, no GL, no Python, no engine state), so a game's
// native module links it and calls it directly (docs/spec/project.md §2b).
//
// Connected to:
// - SightBindings exposes engine.SightGrid.
// - libraries/geo_world builds one from a map package (world.sight_grid, world.blocker_heights).

#pragma once

#include <vector>
#include "raylib.h"

namespace IYI {

struct SightPair
{
    int Observer = 0;
    int Target = 0;
    float Distance = 0.0f;
};

class SightGrid
{
public:
    // (columns + 1) * (rows + 1) vertex heights, row by row from origin (the smallest x and y corner).
    void SetGround(int columns, int rows, float cell, Vector2 origin, std::vector<float> heights);
    // columns * rows blocker heights above the ground, row by row from origin. Call after SetGround.
    void SetBlockers(int columns, int rows, float cell, Vector2 origin, const std::vector<float>& heights);
    // One blocker height for the given cells (a building destroyed, a wall built).
    void SetBlockerCells(const std::vector<int>& cells, float height);
    // columns * rows foliage bands above the ground (low, high; high <= low = none), on the blocker grid.
    // Call after SetBlockers; empty vectors clear it.
    void SetFoliage(const std::vector<float>& lows, const std::vector<float>& highs);

    float Ground(float x, float y) const;
    float Top(float x, float y) const;              // ground plus the blocker in that cell
    // Whether the segment a-b passes over every top (the last half metre before b is not tested, so a
    // target standing against a wall is not hidden by its own cell). With `foliage`: the metres of the
    // line inside foliage bands (counted up to where it is blocked).
    bool Visible(Vector3 a, Vector3 b, float* foliage = nullptr) const;
    // The first point along origin + t * direction (t <= maxDistance, direction any length) under a top.
    bool Ray(Vector3 origin, Vector3 direction, float maxDistance, Vector3* hit) const;
    // Every (observer, target) pair within the observer's range (x, y, z, range in w) with a clear line.
    std::vector<SightPair> VisiblePairs(const std::vector<Vector4>& observers, const std::vector<Vector3>& targets) const;

    int BlockerColumns() const { return m_bColumns; }
    int BlockerRows() const { return m_bRows; }

private:
    int m_columns = 0, m_rows = 0;
    float m_cell = 1.0f;
    Vector2 m_origin { 0.0f, 0.0f };
    std::vector<float> m_ground;
    int m_bColumns = 0, m_bRows = 0;
    float m_bCell = 1.0f;
    Vector2 m_bOrigin { 0.0f, 0.0f };
    std::vector<float> m_top;                       // per blocker cell; empty: tops are the ground
    std::vector<float> m_leafLow, m_leafHigh;       // per blocker cell, absolute heights; empty: no foliage
    float m_step = 0.5f;
    float m_lowest = 0.0f, m_highest = 0.0f;        // of all tops, bounding a ray march
};

} // namespace IYI
