// Declares navigation: route searches over a passability grid and over a road graph, for games whose
// units cross a map whose obstacles change while it is played.
//
// The grid keeps two bytes per cell. Base is what the map says (the game's own bits, e.g. geo_world's
// water / road / building); Extra is what play added (up to 8 dynamic bits, e.g. a wreck, debris, a
// danger area). A grid query prices a cell as Costs[Base] (per metre, 0 = closed) times ExtraCosts[b]
// for each set Extra bit b (0 = closed), so every movement class asks with its own prices while the
// grid is stored once. The graph's edges carry a kind (priced per kind by the query), an open flag, a
// clear width (narrowed by obstacles; the query says how wide it is) and two added costs in metres.
//
// NavPlanner runs the searches on up to 4 worker threads. Edits change the game side's copy; the next
// request publishes a snapshot (a copy) when anything changed, so a running search never sees half an
// edit and the game side never waits for a search. Poll returns finished results in request order.
//
// Connected to:
// - NavBindings exposes engine.NavPlanner.
// - NavSim.h prices the bits per movement class and moves vehicles and walkers (NavSim::Planner).

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>
#include "raylib.h"

namespace IYI {

struct NavGrid
{
    int Columns = 0, Rows = 0;
    float Cell = 1.0f;                   // world units across a cell
    Vector2 Origin{};                    // world point of cell (0, 0)'s top-left corner
    std::vector<uint8_t> Base, Extra;    // per cell, row by row (x fastest)
};

struct NavEdge
{
    int A = 0, B = 0;                    // node indices
    float Length = 0.0f;
    int Kind = 0;                        // index into the query's KindCosts
    bool Oneway = false;                 // A to B only
    bool Open = true;
    float Clear = 0.0f;                  // width free of obstacles
    float Hazard = 0.0f;                 // added cost, times the query's HazardWeight
    float Delay = 0.0f;                  // added cost as it is (e.g. a traffic jam)
    float Tight = 0.0f;                  // added cost times the query's TightWeight (no room to queue: junctions close together)
};

struct NavGraph
{
    std::vector<Vector2> Nodes;
    std::vector<NavEdge> Edges;
    std::vector<std::vector<int>> Links; // per node: the edges touching it (BuildNavLinks)
};

void BuildNavLinks(NavGraph& graph);

struct NavGridQuery
{
    Vector2 Start{}, Goal{};
    std::array<float, 256> Costs{};      // per Base byte, per metre; 0 = closed
    std::array<float, 8> ExtraCosts{};   // factor per Extra bit; 0 = closed
    int Clearance = 0;                   // cells round each step that must be open too (a wide unit)
    int MaxNodes = 500000;               // gives up (not found) after expanding this many cells
    std::vector<int> Avoid;              // cells closed for this search only, sorted (vehicles standing in the way)
};

struct NavGraphQuery
{
    std::vector<std::pair<int, float>> Starts;   // node, cost already paid to reach it
    std::vector<std::pair<int, float>> Goals;    // node, cost still to pay from it
    std::vector<float> KindCosts;                // per edge kind, per metre; 0 or missing = closed
    float Width = 0.0f;                          // edges with less Clear are closed
    float HazardWeight = 1.0f;
    float TightWeight = 0.0f;
    int Avoid = -1;                              // a node that costs AvoidCost more to pass (a junction it backed off from)
    float AvoidCost = 0.0f;
    int MaxNodes = 200000;
};

struct NavResult
{
    int Ticket = 0;
    bool Found = false;
    float Cost = 0.0f;
    int Expanded = 0;
    std::vector<Vector2> Points;         // grid: the smoothed path, Start to Goal; graph: the nodes' points
    std::vector<int> Nodes, Edges;       // graph only
};

// The searches themselves (A*), synchronous; NavPlanner's worker runs them.
NavResult FindNavGridPath(const NavGrid& grid, const NavGridQuery& query);
NavResult FindNavGraphPath(const NavGraph& graph, const NavGraphQuery& query);

struct NavWorker;

class NavPlanner
{
public:
    NavPlanner();
    ~NavPlanner();                       // joins the worker; unfinished requests are dropped
    NavPlanner(const NavPlanner&) = delete;
    NavPlanner& operator=(const NavPlanner&) = delete;

    // The game side's copies. Edit them, then set the matching flag so the next request sees it.
    NavGrid Grid;
    NavGraph Graph;
    bool GridChanged = true, GraphChanged = true;

    // Edits of the copies above (set the matching flag). A bad argument throws std::invalid_argument, a bad index std::out_of_range.
    void InitGrid(int columns, int rows, float cell, Vector2 origin);        // an all-zero grid
    void SetBase(const std::vector<uint8_t>& bytes);                          // one byte per cell
    void SetBaseCells(const std::vector<int>& cells, const std::vector<uint8_t>& bytes);
    void SetExtra(const std::vector<int>& cells, int bit, bool on);          // bit 0..7
    std::pair<int, int> Cell(int i) const;                                    // (Base, Extra)
    void SetGraph(std::vector<Vector2> nodes, const std::vector<NavEdge>& edges);
    void SetEdge(int edge, bool open, float clear, float hazard, float delay);
    int FindGrid(Vector2 start, Vector2 goal, const std::vector<float>& costs, const std::vector<float>& extraCosts, int clearance = 0, int maxNodes = 500000);   // 256 costs, 8 extra

    int Request(const NavGridQuery& query);      // returns its ticket
    int Request(const NavGraphQuery& query);
    std::vector<NavResult> Poll(bool wait);      // wait: until every request so far is done
    int Pending() const;

private:
    std::unique_ptr<NavWorker> m_worker;
    std::shared_ptr<const NavGrid> m_grid;
    std::shared_ptr<const NavGraph> m_graph;
    int m_nextTicket = 1;
};

} // namespace IYI
