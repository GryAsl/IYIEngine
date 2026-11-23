// A geo_world package in memory (libraries/geo_world/FORMAT.md): the immutable imported geometry (terrain
// heights, passability grid, buildings, roads, walls, trees, hedges, tall grass) and the mutable part kept
// beside it (building and road conditions, damage, blast damage to trees, ground marks, a revision counter).
// Rasterizing, ground height, building lookup, conditions with their grid bits, line of sight inputs (blocker
// heights, foliage bands) and save/load state live here; the Python module geo_world.world wraps it
// (src/GeoWorldBindings.cpp) and a native game uses it directly. World frame: metres, x east, y south, z up.
//
// Part of IYIEngineCore (plain C++ and raylib types; implements NavWorld so a Nav can follow its conditions).
//
// Connected to:
// - GeoWorldBindings exposes engine.geo_world; libraries/geo_world/scripts/geo_world/world.py wraps that.
// - Sight.h: MakeSightGrid / UpdateSight keep a SightGrid in step. CellTerrain.h: TerrainCorners feed Api::MakeCellTerrain.
// - NavSim.h: NavXY, NavWorld.

#pragma once

#include "IYI/NavSim.h"
#include "IYI/Sight.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace IYI {

class GeoWorld : public NavWorld
{
public:
    enum Bit : uint8_t { WATER = 1, STEEP = 2, ROAD = 4, BRIDGE = 8, BUILDING = 16, RUBBLE = 32, WALL = 64 };   // grid cell bits
    enum class Condition { Intact, Damaged, Destroyed, Cleared };
    enum class TreeState { Broken, Burnt };
    static constexpr double BUCKET = 64.0;         // metres across a building lookup bucket
    static constexpr double RUBBLE_HEIGHT = 1.0;   // metres a destroyed building still blocks sight
    static constexpr double GRASS_HEIGHT = 0.9;    // metres of tall grass: hides a person lying down, not one standing
    static constexpr double BROKEN_TRUNK = 0.4;    // share of its height a tree snapped by a blast keeps

    struct Grid { double Cell = 1.0; int Columns = 0, Rows = 0; NavXY Origin; };
    struct Building { std::string Id; double Height = 0.0; std::vector<NavXY> Footprint; NavXY Centre; };   // Centre: the obb's
    struct Road { std::string Id; bool Bridge = false; double Width = 0.0; std::vector<NavXY> Points; };
    struct Line { std::vector<NavXY> Points; double Height = 0.0; };                                       // a wall or a hedge
    struct Tree { double X = 0.0, Y = 0.0, Height = 0.0, Crown = 0.0, Base = 0.0; };                        // Base: the crown base
    struct Scorch { double X = 0.0, Y = 0.0, Radius = 0.0; };
    struct BlastSite { int Revision = 0; double X = 0.0, Y = 0.0, Radius = 0.0; };                              // sight redone round it
    struct Change { int Revision = 0; std::string Id; Condition State = Condition::Intact; };
    using Mark = std::array<double, 8>;   // kind (index into MarkKinds), x, y, length, width, rotation, strength, time
    struct Box { double X0, Y0, X1, Y1; };
    // What a save game keeps of the mutable part.
    struct State
    {
        std::map<std::string, Condition> Conditions;
        std::map<std::string, double> Damage;
        std::map<int, TreeState> Trees;
        std::vector<Scorch> Scorches;
        std::vector<Mark> Marks;
    };
    // Where a SightGrid-like object hears about changes (a SightGrid, or a native module's own).
    struct SightSink
    {
        std::function<void(const std::vector<int>&, double)> SetBlockerCells;   // cells, blocker height
        std::function<void(const std::vector<float>&, const std::vector<float>&)> SetFoliage;   // empty: not kept
    };

    // heights: float32 little-endian bytes, (terrain columns + 1) * (rows + 1); grid: one byte per cell.
    // Throws std::invalid_argument when it is not a geo_world v1 package or the sizes do not match (name: the message's prefix).
    GeoWorld(const std::string& name, const std::string& worldJson, const std::string& heights, const std::string& grid);
    static GeoWorld LoadDirectory(const std::string& dir);   // dir/world.json, heights and grid files; throws std::runtime_error when unreadable
    GeoWorld(GeoWorld&&) = default;
    // Make the grid bits live in `memory` (grid size bytes, e.g. a Python bytearray) from now on; the current bits are copied there.
    void ShareBits(uint8_t* memory);

    const std::string& Name() const { return m_name; }
    const std::string& SourceHash() const { return m_hash; }
    const Grid& GridInfo() const { return m_grid; }
    const Grid& TerrainInfo() const { return m_terrain; }
    const std::vector<float>& Heights() const { return m_heights; }
    const std::vector<Building>& Buildings() const { return m_buildings; }
    const std::vector<Road>& Roads() const { return m_roads; }
    const std::vector<Tree>& Trees() const { return m_trees; }
    int Revision() override { return m_revision; }
    int MarksGeneration() const { return m_marksGen; }
    const std::map<std::string, Condition>& Conditions() const { return m_state.Conditions; }
    const std::map<std::string, double>& DamageMap() const { return m_state.Damage; }
    const std::map<int, TreeState>& TreeStates() const { return m_state.Trees; }
    const std::vector<Scorch>& Scorches() const { return m_state.Scorches; }
    const std::vector<Mark>& Marks() const { return m_state.Marks; }
    const std::vector<BlastSite>& Blasts() const { return m_blasts; }
    const std::vector<Change>& Changes() const { return m_changes; }

    static const char* ConditionName(Condition c);                 // "intact", "damaged", "destroyed", "cleared"
    static std::optional<Condition> ParseCondition(const std::string& name);

    // Frame: WGS84 <-> world metres (equirectangular round the centre).
    NavXY FromLonLat(double lon, double lat) const;
    NavXY ToLonLat(double x, double y) const;   // (lon, lat)

    // Queries.
    double Height(double x, double y) const;               // DEM vertices interpolated bilinearly, clamped at the edge (the surface CellTerrain draws)
    int Flags(double x, double y) const;                    // grid bits under (x, y) with the conditions applied; -1 off the map
    const Building* BuildingById(const std::string& id) const;
    const Road* RoadById(const std::string& id) const;
    const Building* BuildingAt(double x, double y) const;   // the building whose footprint holds (x, y)
    std::vector<std::string> BuildingsNear(double x, double y, double radius) const;   // footprint within radius metres (holds it, or an edge is that close)
    Condition ConditionOf(const std::string& id) const;
    bool RoadOpen(const std::string& id) const { return ConditionOf(id) != Condition::Destroyed; }
    double Damage(const std::string& id) const;
    // [(id, state name)] after `revision`: what navigation and the renderer have not seen yet.
    std::vector<std::pair<std::string, std::string>> ChangesSince(int revision) override;
    const uint8_t* Bits() override { return m_bits; }
    bool Destroyed(const std::string& id) override { return ConditionOf(id) == Condition::Destroyed; }

    // Mutable part. The ids must exist (BuildingById / RoadById), else std::out_of_range.
    // SetCondition updates the grid (a destroyed building leaves RUBBLE, a destroyed bridge drops its BRIDGE bits); true if it changed.
    bool SetCondition(const std::string& id, Condition state);
    // A building's damage 0..1 beside its condition (how far its stack crumbles; looks only): raises the revision without a change entry.
    bool SetDamage(const std::string& id, double amount);
    // Trees within half the radius snap (BROKEN_TRUNK of their height left, no crown), the rest within it burn bare;
    // a scorch is left. Raises the revision (UpdateSight fixes the trunks and leaves). Returns how many trees changed.
    int Blast(double x, double y, double radius);
    // Replace the blast damage (a save game's, or {}, {} for a fresh scenario); sight is redone round every old and new scorch.
    void SetTrees(std::map<int, TreeState> trees, std::vector<Scorch> scorches);
    // A ground mark (rounded as Python's round); looks only. Marks only grow between SetMarks calls.
    static Mark MakeMark(int kind, double x, double y, double length, double width, double rotation, double strength, double time);
    void AddMark(const Mark& m) { m_state.Marks.push_back(m); }
    void SetMarks(std::vector<Mark> marks) { m_state.Marks = std::move(marks); ++m_marksGen; }
    static constexpr const char* MarkKinds[4] = { "crater", "track", "rubble", "blood" };
    State SaveState() const { return m_state; }
    // Restores SaveState's data; returns the ids it does not know (a package rebuilt from newer sources), which are skipped.
    std::vector<std::string> LoadState(const State& s);

    // Line of sight inputs.
    std::vector<float> BlockerHeights();                                    // columns * rows: tallest building (RUBBLE_HEIGHT destroyed), wall or tree trunk; 0 open
    // (lows, highs) per grid cell: the band above the ground that hides without blocking (leafy crowns, hedges, tall grass).
    // box: only the cells in it are redone, in the arrays of the last full call.
    std::pair<const std::vector<float>&, const std::vector<float>&> Foliage(std::optional<Box> box = std::nullopt);
    SightGrid MakeSightGrid();                                              // ground, blockers and leaves
    // Applies the building changes and blasts after `revision`; returns Revision().
    int UpdateSight(const SightSink& sink, int revision);
    int UpdateSight(SightGrid& sight, int revision);
    std::vector<float> TerrainCorners() const;                              // 4 per terrain cell (nw, ne, se, sw) for Api::MakeCellTerrain

    // Rasterizing onto a grid.
    std::vector<int> PolygonCells(const std::vector<std::vector<NavXY>>& rings) const;   // cells whose centres lie inside (rings: outer and holes, even-odd), row by row
    std::vector<int> LineCells(const std::vector<NavXY>& points, double width) const;    // cells whose centres lie within width / 2 of the polyline, sorted
    std::vector<int> BuildingCells(const Building& b) const;                              // its footprint's cells, or for a hut smaller than a cell the one under its centre
    static bool Inside(const std::vector<NavXY>& ring, double x, double y);              // even-odd point in polygon

private:
    using Cells = std::vector<int>;
    static std::vector<int64_t> BucketKeys(const std::vector<NavXY>& points);   // the buckets a bounding box of the points touches
    std::vector<int> NearBuildings(const std::vector<NavXY>& area) const;   // indices of the buildings in the buckets the area touches
    void RefreshBuilding(int index);
    void RefreshBridges();
    double BlockerHeight(const Building& b) const;
    double Trunk(int tree) const;
    const std::unordered_map<int, double>& StaticTops();
    std::map<double, Cells> Tops(const std::vector<int>& cells, const std::vector<NavXY>& area);

    std::string m_name, m_hash;
    NavXY m_frame;   // (lon, lat)
    Grid m_grid, m_terrain;
    std::vector<float> m_heights;
    std::vector<uint8_t> m_ownBits;
    uint8_t* m_bits = nullptr;
    std::vector<Building> m_buildings;
    std::vector<Cells> m_cells;   // per building
    std::vector<Road> m_roads;
    std::vector<Cells> m_bridgeCells;   // per road (empty unless a bridge)
    std::vector<Line> m_walls, m_hedges;
    std::vector<Tree> m_trees;
    std::vector<std::vector<NavXY>> m_tallGrass;
    std::unordered_map<std::string, int> m_buildingIds, m_roadIds;
    std::unordered_map<int64_t, std::vector<int>> m_buckets;

    State m_state;
    int m_revision = 0, m_marksGen = 0;
    std::vector<BlastSite> m_blasts;
    std::vector<Change> m_changes;
    std::optional<std::unordered_map<int, double>> m_staticTops;   // walls and trunks per cell; blasts reset it
    std::vector<float> m_lows, m_highs;
};

} // namespace IYI
