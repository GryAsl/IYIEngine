// Declares the navigation simulation: vehicles, walkers and helicopters moving on a map whose roads,
// bridges and buildings change while it is played. Route searches go to NavPlanner's worker
// thread. Live play (Wait false) steps everything else on the nav thread (StartStep) while the game
// reads the Shown* copies of the step before; Wait true steps inline (reproducible runs).
//
// It began as a line-by-line port of the Python nav library, proven bit for bit on seeded runs (2026-10-27);
// Python's evaluation order, its random.Random per unit, its float
// helpers (PyExact.h) and its dict orders are kept on purpose. Change behaviour freely, but know
// that any change moves seeded runs' fingerprints.
//
// Connected to:
// - NavSimBindings exposes engine.Nav and its Walker, Vehicle and Heli objects.
// - Nav.h: the planner (A* on a worker thread).
// - libraries/nav/README.md: the model, the contract with the game, the events.

#pragma once

#include "IYI/Nav.h"
#include "IYI/PyExact.h"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace IYI {

struct NavXY
{
    double x = 0.0, y = 0.0;
    bool operator==(const NavXY&) const = default;
};

// Grid bits of a geo_world map.
enum NavBits : uint8_t { NAV_WATER = 1, NAV_STEEP = 2, NAV_ROAD = 4, NAV_BRIDGE = 8, NAV_BUILDING = 16, NAV_RUBBLE = 32, NAV_WALL = 64 };

// The map as it was built (read once): grid, terrain, road graph, buildings.
struct NavMap
{
    int Columns = 0, Rows = 0;
    double Cell = 1.0;
    NavXY Origin;
    int TerrainColumns = 0, TerrainRows = 0;
    double TerrainCell = 1.0;
    NavXY TerrainOrigin;
    std::vector<float> Heights;                  // (columns + 1) * (rows + 1) vertices
    std::vector<std::string> NodeIds;
    std::vector<NavXY> Nodes;
    struct Road
    {
        std::string Id, Kind;
        int A = 0, B = 0;                        // node indices
        double Length = 0.0, Width = 0.0;
        bool Oneway = false, Bridge = false;
        std::vector<NavXY> Points;
    };
    std::vector<Road> Roads;
    struct Building
    {
        std::string Id;
        std::vector<NavXY> Footprint;
        std::vector<std::array<double, 5>> Boxes; // cx, cy, length, width, degrees
    };
    std::vector<Building> Buildings;
};

// What the navigation reads from the map while it is played (the game's world).
class NavWorld
{
public:
    virtual ~NavWorld() = default;
    virtual const uint8_t* Bits() = 0;                                       // the grid's bits now
    virtual bool Destroyed(const std::string& id) = 0;                       // a building or a road
    virtual int Revision() = 0;
    virtual std::vector<std::pair<std::string, std::string>> ChangesSince(int revision) = 0;  // (id, state)
};

// Movement classes. kind: foot, wheeled, tracked, air. speed m/s; accel, brake m/s2; length, width m;
// turn: smallest radius m (0: turns on the spot).
struct NavClass
{
    std::string Kind;
    double Speed = 0.0, Run = 0.0, Radius = 0.0, Accel = 0.0, Brake = 0.0, Length = 0.0, Width = 0.0, Turn = 0.0, Climb = 0.0, Pad = 0.0;
};
const std::vector<std::pair<std::string, NavClass>>& NavClasses();
const NavClass& NavClassOf(const std::string& cls);   // throws std::out_of_range

// An event value: None, bool, int, float (rounded like Python's round(x, digits) when digits >= 0), str, [float],
// or any other JSON value kept as its text (Json).
struct NavValue
{
    enum Kind : uint8_t { None, Bool, Int, Real, Text, Reals, Json } kind = None;
    bool flag = false;
    int64_t integer = 0;
    double real = 0.0;
    int digits = -1;
    std::string text;
    std::vector<double> reals;
};
struct NavEvent
{
    std::string Type;
    double Time = 0.0;                           // reported rounded to 2 digits
    std::vector<std::pair<std::string, NavValue>> Data;
};

struct NavRoute
{
    bool Found = false;
    std::vector<NavXY> Points;
    std::vector<double> Speeds;                  // speed factor per point
    std::vector<int> Edges;
    std::vector<std::pair<double, int>> Nodes;   // (distance along, node)
    std::string Mode;
    double Cost = 0.0;
};

class NavSim;

// Every unit is two objects: the live one the nav thread steps and the shown copy a game reads (NavSim::Shown*).
// A shown copy's Live is its live unit; Sim lets a write to the copy reach the live one (NavSim::Write).
struct NavWalker
{
    std::string Id, Cls, Side;
    const NavClass* C = nullptr;
    double X = 0.0, Y = 0.0;
    std::string State = "idle";
    std::vector<NavXY> Path;
    int I = 0;                                   // the waypoint it walks to
    std::optional<NavXY> Goal;
    bool Run = false;
    int Ticket = 0;                              // 0: none
    double ReplanAt = 0.0, React = 0.0;
    Py::Random Rng;
    double Panic = 0.0, Noticed = 0.0, CalmAt = 0.0;
    int Rev = 0;
    double Acc = 0.0;
    int Stride = 0;
    std::optional<std::string> Building, Then;
    bool Steady = false;                         // under orders: an alarm raises panic only
    std::optional<NavXY> Way;                    // the way it makes for the vehicle WayFor waiting behind it, till that passed
    std::string WayFor;
    double Pace = 1.0;                           // speed factor (a wounded soldier)
    std::optional<int> BestI;                    // stuck detection
    double Best = 0.0, BestAt = 0.0;
    std::shared_ptr<NavWalker> Live;
    std::weak_ptr<NavSim> Sim;
};

struct NavSample { double T, X, Y, Vel, H; };

struct NavVehicle
{
    std::string Id, Cls, Side;
    const NavClass* C = nullptr;
    double X = 0.0, Y = 0.0, H = 0.0, Vel = 0.0, Len = 0.0, Wid = 0.0;
    std::string State = "idle", Mode = "road", Prefer = "road", Doctrine, Want;
    int Priority = 0;
    double React = 0.0, Nerve = 0.0;
    Py::Random Rng;
    double Alarm = 0.0, Noticed = 0.0;
    std::vector<NavXY> Path;
    std::vector<double> Cum, Speeds;
    std::vector<int> Edges;
    std::vector<std::pair<double, int>> Nodes;
    int Seg = 0;
    double S = 0.0, Lat = 0.0, LatGoal = 0.0, PassUntil = -1.0;
    std::optional<NavXY> Goal, Danger, FleeGoal;
    int Ticket = 0, TailTicket = 0;              // TailTicket: the grid route from the road's end to a goal off the road
    double ReplanAt = 0.0, Blocked = 0.0, Queued = 0.0, Still = 0.0;   // Still: seconds under 0.3 m/s
    std::optional<double> Ahead;
    double ScanAt = 0.0;
    int Rev = 0;
    std::deque<NavSample> Hist;                  // at most 40, every SAMPLE seconds: what others see
    double Sampled = 0.0;
    std::optional<std::string> Convoy, BlockedBy;
    double Spacing = 0.0, ReverseTo = 0.0, FreeSince = 0.0, Acc = 0.0;
    int Stride = 0;
    double SwerveAt = 0.0;
    // The junction ahead (a node of three roads or more) and its box along the path: asked for at JAsked,
    // held once JGranted until the rear is out of the box; JPoly is the way through the box.
    std::optional<int> JNode;
    double JIn = 0.0, JOut = 0.0, JAsked = 0.0;
    bool JGranted = false;
    std::vector<NavXY> JPoly;
    std::string WaitFor;                         // the vehicle it waits for (in front, or holding its junction): deadlock cycles
    std::string WaitWalker;                      // the walker close in front of it (that walker makes way when it stands)
    double Yielded = -1e9;                       // when it last backed off to break a deadlock or give way
    int AvoidNode = -1;                          // the junction it backed off from in a deadlock: routes keep off it until AvoidUntil
    double AvoidUntil = 0.0;
    bool Round = false;                          // its route asked is one round a deadlock (failing, it backs off instead)
    std::pair<int, double> Setback{ -1, 0.0 };   // (junction, metres): it stood in the way there, so it waits farther back
    NavXY Anchor;                                // where it was when it last made 3 m of headway, and when
    double AnchorAt = 0.0;
    std::shared_ptr<NavVehicle> Live;
    std::weak_ptr<NavSim> Sim;
};

struct NavHeli
{
    std::string Id, Cls;
    const NavClass* C = nullptr;
    double X = 0.0, Y = 0.0, Z = 0.0, Vel = 0.0, H = 0.0;
    std::string State = "landed";
    std::optional<NavXY> Target;
    bool Land = false;
    double Len = 0.0, Wid = 0.0;
    std::shared_ptr<NavHeli> Live;
    std::weak_ptr<NavSim> Sim;
};

// Units by id in insertion order (as a Python dict): Put keeps a known id's place, a new one goes last.
template <class T>
class NavUnits
{
public:
    T* Get(const std::string& id) const { auto it = m_ids.find(id); return it == m_ids.end() ? nullptr : it->second.get(); }
    std::shared_ptr<T> Shared(const std::string& id) const { auto it = m_ids.find(id); return it == m_ids.end() ? nullptr : it->second; }
    const std::vector<std::shared_ptr<T>>& All() const { return m_order; }
    size_t Size() const { return m_order.size(); }
    void Put(std::shared_ptr<T> u)
    {
        auto [it, added] = m_ids.try_emplace(u->Id, u);
        if (added) { m_order.push_back(u); return; }
        for (auto& o : m_order) if (o == it->second) o = u;
        it->second = u;
    }
    void Assign(std::vector<std::shared_ptr<T>> order)            // the same units in this order (a copy of another NavUnits)
    {
        if (order == m_order) return;
        m_ids.clear();
        for (const auto& u : order) m_ids[u->Id] = u;
        m_order = std::move(order);
    }
    std::shared_ptr<T> Pop(const std::string& id)
    {
        auto it = m_ids.find(id);
        if (it == m_ids.end()) return nullptr;
        std::shared_ptr<T> u = it->second;
        m_ids.erase(it);
        std::erase(m_order, u);
        return u;
    }

private:
    std::vector<std::shared_ptr<T>> m_order;
    std::unordered_map<std::string, std::shared_ptr<T>> m_ids;
};

class NavSim : public std::enable_shared_from_this<NavSim>
{
public:
    static constexpr double MAX_DT = 0.1;        // a long frame steps at most this
    static constexpr double WALK_FAR = 500.0;    // calm walkers farther from the focus step every 4th frame
    static constexpr double DRIVE_FAR = 700.0;   // the same for vehicles

    NavSim(NavMap map, std::unique_ptr<NavWorld> world, int64_t seed = 1, bool wait = false);
    ~NavSim();
    NavSim(const NavSim&) = delete;
    NavSim& operator=(const NavSim&) = delete;

    const NavMap Map;
    NavPlanner Planner;
    double Time = 0.0;
    bool Wait = false;                           // every update waits for the worker (reproducible runs)
    int64_t Seed = 1;
    NavUnits<NavWalker> Walkers;
    NavUnits<NavVehicle> Vehicles;
    NavUnits<NavHeli> Helis;
    struct EdgeState { bool Open = true; double Clear = 0.0, Hazard = 0.0, Delay = 0.0; bool operator==(const EdgeState&) const = default; };
    std::vector<EdgeState> Edges;
    std::vector<uint8_t> Base, Extra;            // the grid as navigation sees it (culverts added) and what play put on it
    struct Obstacle { std::vector<int> Cells; int Bit = 0; std::vector<std::array<double, 5>> Boxes; std::string Kind; };
    std::unordered_map<std::string, Obstacle> Obstacles;
    int Revision = 0;
    int Culverts = 0;
    struct Counts { int Routes = 0, Vehicles = 0, Walkers = 0, Near = 0; } Count;

    // The nav thread. StartStep: map changes are read here (the game thread), then one step runs on the nav thread
    // (inline when Wait). Join waits for it and applies the writes queued meanwhile. Publish copies the live
    // units, time and events to the Shown* ones a game reads; call it with the nav thread idle (after Join).
    void StartStep(double dt, std::optional<NavXY> focus);
    void Join();
    bool Busy() const { return m_step.valid(); }
    void Publish();
    void PublishEvents();                        // with no step in flight: events direct calls emitted (a route_failed) show at once
    template <class T> void Write(T& shown, std::function<void(T&)> write);   // a game's write to a shown copy
    void PublishWalker(const std::string& id);
    void PublishVehicle(const std::string& id);
    void PublishHeli(const std::string& id);
    NavUnits<NavWalker> ShownWalkers;
    NavUnits<NavVehicle> ShownVehicles;
    NavUnits<NavHeli> ShownHelis;
    std::vector<NavEvent> ShownEvents;          // drained by the game
    double ShownTime = 0.0;
    double WorkerMs = 0.0, ShownWorkerMs = 0.0;  // the last step's time on the nav thread (Shown: as published)
    Counts ShownCount;

    // core: requests, map changes, obstacles, hazards, events
    int Request(NavXY start, NavXY goal, const std::string& cls, std::string mode = "road", const std::string& side = "military",
                std::optional<NavXY> heading = {}, bool tight = false, std::vector<int> avoid = {}, int avoidNode = -1);   // tight: a grid route without room off walls
    void Poll();
    std::optional<NavRoute> TakeResult(int ticket);
    void Forget(int ticket);                     // a request nobody will take: its answer is dropped
    void Sync();
    void Update(double dt);                      // time, map changes, expired hazards, answers
    void Expire();                               // hazards whose time is up
    bool Passable(const std::string& cls, double x, double y);
    std::optional<int> CellIndex(double x, double y) const;
    NavXY OpenNear(const std::string& cls, NavXY p, double reach = 12.0);
    std::optional<int> PathClosed(const std::string& cls, const std::vector<NavXY>& points, int start = 0, const std::array<double, 4>* area = nullptr);
    void SetObstacle(const std::string& oid, const std::vector<std::array<double, 5>>& boxes, const std::string& kind = "blocked");
    void ClearObstacle(const std::string& oid);
    void ReportHazard(const std::string& hid, double x, double y, double radius, double severity = 1.0, double duration = 60.0,
                      std::optional<std::string> source = {});
    void ClearHazard(const std::string& hid);
    std::optional<std::tuple<int, double, double>> Snap(double x, double y, double reach = 60.0, bool openOnly = false) const;
    std::optional<NavXY> LandingSite(double x, double y, double pad = 0.0, double search = 250.0);
    std::vector<NavEvent> TakeEvents();
    void Emit(const std::string& kind, std::vector<std::pair<std::string, NavValue>> data) { m_events.push_back({ kind, Time, std::move(data) }); }   // at Time
    std::vector<int> PolygonCells(const std::vector<NavXY>& ring) const;   // geo_world.polygon_cells
    std::vector<int> LineCells(const std::vector<NavXY>& points, double width) const;
    double Height(double x, double y) const;                              // geo_world.height
    bool Destroyed(const std::string& id) const { return m_destroyed.count(id) > 0; }   // as of the last Sync

    // walkers
    NavWalker& SpawnWalker(const std::string& id, const std::string& cls, double x, double y, std::optional<std::string> side = {});
    void Walk(const std::string& id, NavXY goal, bool run = false, std::optional<std::string> then = {});
    void Gather(const std::vector<std::string>& ids, double x, double y, double radius);
    void Wander(std::vector<NavXY> points) { m_wander = std::move(points); }
    NavXY Kerb(NavXY p, double u) const;   // a spot beside the road near p (p itself off the road); u in [0, 1) picks among them
    void AlarmWalker(NavWalker& w, NavXY at, double level);
    void StepWalkers(double dt, const NavXY* focus);

    // vehicles
    NavVehicle& SpawnVehicle(const std::string& id, const std::string& cls, double x, double y, double heading = 0.0, const std::string& side = "civilian",
                             std::optional<std::string> doctrine = {}, std::optional<int> priority = {});
    void Drive(const std::string& id, NavXY goal, const std::string& mode = "road");
    void Convoy(const std::string& cid, const std::vector<std::string>& ids, NavXY goal, double spacing = 25.0);
    void Disable(const std::string& id);
    void Park(const std::string& id);
    void Remove(const std::string& id);
    void StepVehicles(double dt, const NavXY* focus);

    // helicopters
    NavHeli& SpawnHeli(const std::string& id, double x, double y, const std::string& cls = "heli");
    std::optional<NavXY> FlyTo(const std::string& id, double x, double y, bool land = true);
    void StepAir(double dt);

private:
    struct Costs { std::array<double, 256> Base{}; std::array<double, 8> Extra{}; };
    struct Snapped { int E; double S, Off; };
    struct End { int Node; double Cost, S; };
    struct Req { std::string Mode; NavXY Start, Goal; std::optional<Snapped> A, B; std::string Cls, Side; std::vector<End> Starts, Goals; std::optional<NavXY> Heading; };
    struct Hazard { std::string Id; double X, Y, Radius, Severity, Until; std::vector<int> Cells; };
    struct Alarm { double Time, X, Y, Radius, Severity; };
    struct Dirty { int Rev; double X0, Y0, X1, Y1; };
    struct Lead { double Gap, Vl; NavVehicle* V; NavWalker* W; double S; };
    using VehicleGrid = std::unordered_map<int64_t, std::vector<NavVehicle*>>;
    using WalkerGrid = std::unordered_map<int64_t, std::vector<NavWalker*>>;

    const Costs& CostsFor(const std::string& cls, const std::string& side = "military");
    void SetBits(const std::vector<int>& cells, int bit, bool on);
    void Changed(const std::vector<NavXY>& points, double pad);
    void RefreshEdges(double x0, double y0, double x1, double y1);
    std::vector<int> EdgesNear(double x0, double y0, double x1, double y1) const;
    bool BlockedAt(double x, double y) const;
    double SpanClear(int e, double s0, double s1) const;
    void SetDelay(int e, double delay);
    std::vector<End> Ends(const Snapped& at, double width, const std::optional<NavXY>& heading, bool isGoal) const;
    NavRoute RoadRoute(const Req& req, const NavResult& r) const;
    void CopyBase(const std::vector<int>& cells);
    void Middle(const std::vector<int>& cells);   // the planner's ROAD_MIDDLE bit for these cells and their neighbours
    bool BridgeCell(int i);
    bool PadOk(double cx, double cy, double pad, uint8_t bad, const std::vector<NavXY>& occupants);
    std::vector<NavXY> Occupants() const;
    std::optional<std::string> BuildingAt(double x, double y) const;

    void WalkerRequest(NavWalker& w, bool roundVehicles = false);
    std::optional<std::pair<NavXY, std::string>> Shelter(const NavWalker& w, double reach);
    void StepWalker(NavWalker& w, double dt, const WalkerGrid* grid, const VehicleGrid& movers, const NavVehicle* waiting);

    void Replan(NavVehicle& v, bool force = false, const std::string& mode = "", std::optional<NavXY> goal = {}, bool round = false);   // round: a grid route goes round vehicles standing near
    std::vector<int> Standing(NavXY at, NavXY goal, const NavVehicle* self = nullptr) const;
    std::vector<NavVehicle*> Column(const std::string& cid);
    void TakeRoute(NavVehicle& v);
    std::optional<Lead> Leader(NavVehicle& v, const VehicleGrid& grid, const WalkerGrid& walkers, double delay);
    std::optional<double> ScanGround(NavVehicle& v);
    bool StripClear(NavVehicle& v, double s0, double s1, double lat, const VehicleGrid& grid, bool careful);
    std::optional<std::pair<double, double>> Swerve(NavVehicle& v, const Lead& lead, const VehicleGrid& grid);
    void NoticeVehicles(const std::vector<NavVehicle*>& vehicles);
    void DriveOne(NavVehicle& v, double dt, const VehicleGrid& grid, const WalkerGrid& walkers);
    double GroundSpeed(const NavVehicle& v) const;
    void Decide(NavVehicle& v);
    void Flee(NavVehicle& v);
    void Reverse(NavVehicle& v, double distance);
    void CheckRoute(NavVehicle& v);
    double Cohesion(NavVehicle& v, double v0);
    std::optional<double> JunctionGap(NavVehicle& v, const VehicleGrid& grid);
    const NavVehicle* JunctionBlocker(const NavVehicle& v, const VehicleGrid& grid, bool exitRoom) const;   // nullptr: it may go
    void SetJunction(NavVehicle& v, double s, int node);
    void Deadlocks(const std::vector<NavVehicle*>& vehicles);
    bool GiveWay(NavVehicle& v, const VehicleGrid& grid);   // a civilian pulls over for a higher priority vehicle
    void Collide(const std::vector<NavVehicle*>& vehicles, const WalkerGrid& walkers);
    bool NewContact(const std::pair<std::string, std::string>& pair);
    void HitWalkers(NavVehicle& v, std::set<std::pair<std::string, std::string>>& now, const std::vector<NavWalker*>& walkers);
    void HitBuildings(NavVehicle& v, std::set<std::pair<std::string, std::string>>& now);
    void Jams(const std::vector<NavVehicle*>& vehicles);

    std::unique_ptr<NavWorld> m_world;
    std::unordered_map<std::string, int> m_edgeIndex, m_buildingIndex;
    std::vector<std::string> m_kinds;
    std::unordered_map<int64_t, std::vector<int>> m_edgeBuckets, m_buildingBuckets;
    std::map<std::pair<std::string, std::string>, Costs> m_costs;
    std::unordered_map<int64_t, int> m_cellBits;            // (cell, bit) -> obstacles holding it
    std::vector<Hazard> m_hazards;                          // in the order they were reported
    std::vector<Alarm> m_alarms;
    std::vector<NavEvent> m_events;
    std::map<int, NavRoute> m_results;
    std::set<int> m_forgotten;                   // tickets a unit gave up before their answer came
    std::map<int, Req> m_requests;
    int m_seenRevision = 0;
    std::vector<Dirty> m_dirty;
    std::vector<NavXY> m_wander;
    struct ConvoyInfo { std::vector<std::string> Ids; NavXY Goal; double Spacing; };
    std::unordered_map<std::string, ConvoyInfo> m_convoys;
    std::optional<std::set<int>> m_bridgeCells;
    std::set<std::pair<std::string, std::string>> m_contacts;
    std::map<std::pair<std::string, std::string>, double> m_touched;
    double m_jamAt = 0.0;
    std::unordered_set<std::string> m_destroyed;            // the world's destroyed buildings and roads (Sync): the nav thread never asks the world
    std::future<void> m_step;
    std::vector<std::function<void()>> m_writes;
    std::vector<double> m_box;                              // per node: the junction box's radius (0: not a junction)
    std::vector<int> m_cluster;                             // per node: its junction (nodes whose boxes touch are one), -1: none
    std::set<int> m_jamEdges;

public:
    const std::unordered_map<std::string, int>& EdgeIndex() const { return m_edgeIndex; }
    const std::vector<std::string>& Kinds() const { return m_kinds; }
};

// ((x, y), unit direction) at distance s along a polyline (clamped).
std::pair<NavXY, NavXY> NavPointAt(const std::vector<NavXY>& points, double s);

template <class T>
void NavSim::Write(T& shown, std::function<void(T&)> write)
{
    write(shown);
    if (!shown.Live) return;
    if (!Busy()) { write(*shown.Live); return; }
    std::shared_ptr<T> live = shown.Live;
    m_writes.push_back([live, write] { write(*live); });
}

namespace Api {
// One step for everything (dt clamped to MAX_DT): map changes and answers, vehicles, walkers, helicopters, under
// frame profiler sections. Lives in NavApi.cpp (engine core), not in IYIEngineSim: it needs the profiler.
void NavStep(NavSim& nav, double dt, std::optional<NavXY> focus = std::nullopt);
} // namespace Api

} // namespace IYI
