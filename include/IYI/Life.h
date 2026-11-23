// Declares the living-world pieces a game builds its people and their AI on: a low-rate scheduler, a
// log of world events with their causes, remembered facts that fade, and a population kept as
// numbers per neighbourhood with a recycled pool of drawn people and cars round the camera.
// Plain data, no game rules: the game decides what an event means, who learns a fact and when a
// neighbourhood is shocked.
//
// Connected to:
// - NavSimBindings exposes engine.Scheduler, engine.EventLog, engine.Facts and engine.Population.
// - NavSim.h: the pool's people and cars are NavSim walkers and vehicles.
// - libraries/geo_world/FORMAT.md: zones.json, the neighbourhoods Population is built from.

#pragma once

#include "IYI/NavSim.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <queue>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace IYI {

// Work that runs now and then: each id comes due at its time; Due hands out at most `budget`
// ids, earliest first, so far or idle things can be thought about less often.
class Scheduler
{
public:
    void At(const std::string& id, double time);            // (re)schedules id
    void Cancel(const std::string& id) { m_live.erase(id); }
    std::vector<std::string> Due(double now, int budget);
    std::vector<std::pair<std::string, double>> Items() const;  // live ids and their times (saving)
    size_t Size() const { return m_live.size(); }

private:
    using Item = std::tuple<double, uint64_t, std::string>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> m_heap;
    std::unordered_map<std::string, std::pair<uint64_t, double>> m_live;   // id -> (seq, time) of its live item
    uint64_t m_seq = 0;
};

// What happened, where, when and because of what (Cause: an earlier event's id, 0 none).
struct WorldEvent
{
    int64_t Id = 0;
    double Time = 0.0;
    std::string Kind;
    double X = 0.0, Y = 0.0;
    int64_t Cause = 0;
    std::vector<std::pair<std::string, NavValue>> Data;
};

class EventLog
{
public:
    size_t Limit = 4096;                                       // the oldest go first
    int64_t Add(WorldEvent e);                                 // gives it the next id
    std::vector<const WorldEvent*> Since(int64_t id) const;    // ids after `id`
    std::vector<const WorldEvent*> Near(double x, double y, double radius, double since, const std::string& kind = "") const;
    const WorldEvent* Get(int64_t id) const;
    int64_t Last() const { return m_next - 1; }

private:
    std::deque<WorldEvent> m_events;
    int64_t m_next = 1;
};

// What one side believes: a kind ("insurgent", "cache", "ied"...) about a subject (a unit or place id)
// seen at (X, Y). Confidence halves every HalfLife seconds of game time after Time, so old reports
// count less; a report can be wrong, the game decides that when it tells it.
struct Fact
{
    int64_t Id = 0;
    std::string Owner, Kind, Subject;
    double X = 0.0, Y = 0.0, Time = 0.0, Confidence = 0.0;
    std::string Source;
};

class Facts
{
public:
    double HalfLife = 1800.0;
    // The same owner, kind and subject is one fact: a newer report moves it and keeps the larger confidence.
    int64_t Tell(Fact f);
    double Confidence(const Fact& f, double now) const;
    // Facts of `owner` within radius of (x, y) (kind "" any), confidence as of now, at least `least`, nearest first.
    std::vector<Fact> Near(const std::string& owner, double x, double y, double radius, double now, const std::string& kind = "", double least = 0.05) const;
    std::vector<Fact> About(const std::string& owner, const std::string& subject, double now) const;
    void Forget(const std::string& owner, const std::string& subject, const std::string& kind = "");
    void Prune(double now, double least = 0.02);
    const std::vector<Fact>& All() const { return m_facts; }

private:
    // shortcut: linear scans, fine for hundreds of facts; bucket them if a game keeps thousands.
    std::vector<Fact> m_facts;
    int64_t m_next = 1;
};

// One neighbourhood: built offline (zones.json), its numbers kept by the game.
struct LifeZone
{
    std::string Id, Name;
    double X = 0.0, Y = 0.0;
    int Homes = 0;
    double People = 0.0;
    std::vector<NavXY> Markets, Mosques, Schools;
    std::vector<std::string> Buildings;
    std::vector<std::pair<NavXY, std::string>> Doors;   // filled by Population: where its people come out (point, building id)
    double Fear = 0.0, Support = 0.5, Displaced = 0.0;  // fear 0..1, support for the government 0..1, people gone
};

// The town as numbers per zone plus a pool of drawn people and cars kept round the camera: a calm,
// unpinned one far from the focus goes indoors (a car leaves the map) and comes out again at a door
// (a road) near it, as many as the zones' activity at that hour asks for. Ids stay the same; the game
// pins the ones it uses (a suspect, a crowd) so the pool leaves them alone.
class Population
{
public:
    static constexpr double FAR = 450.0;          // calm pool units farther than this from the focus are recycled (inside NavSim::WALK_FAR: fine steps)
    static constexpr double NEAR = 400.0;         // and come out within this of it
    static constexpr double DENSITY = 0.04;       // drawn people per person outdoors
    static constexpr double CARS = 0.3;           // drawn cars per drawn person
    static constexpr int PER_CALL = 20;           // units brought out per Manage

    struct Grid { double Cell = 50.0; NavXY Origin; int Columns = 0, Rows = 0; std::vector<int> Zones; };

    Population(std::vector<LifeZone> zones, Grid grid, NavSim& nav, int64_t seed, int walkers, int cars);

    std::vector<LifeZone> Zones;
    const Grid Cells;
    std::unordered_set<std::string> Pinned;

    int ZoneAt(double x, double y) const;                     // -1: none
    double Activity(int zone, double hour) const;             // share of its people outdoors now
    void Shock(double x, double y, double radius, double fear);
    void Step(double hours);                                   // fear fades, people leave and come back
    struct Counts { int Out = 0, Want = 0, Cars = 0, WantCars = 0; };
    Counts Manage(NavSim& nav, NavXY focus, double hour);
    // Brings out up to n indoor pool people at doors within radius of (x, y), pinned; returns their ids.
    std::vector<std::string> Summon(NavSim& nav, int n, double x, double y, double radius);
    int Home(const std::string& id) const;                     // the zone of a pool person, -1 if not one
    std::vector<std::string> Members() const;

private:
    struct Member { std::string Id; int Zone = -1; NavXY Home; std::string Building; };
    Py::Random m_rng;
    std::vector<Member> m_people;
    std::vector<std::string> m_cars;
    std::unordered_map<std::string, int> m_index;              // person id -> m_people index
    std::vector<int> m_carNodes;                               // road nodes a car fits through
    std::vector<NavXY> m_wander;                               // road nodes near the focus
    NavXY m_wanderAt{ 1e18, 1e18 };

    void Out(NavSim& nav, Member& m, const std::pair<NavXY, std::string>& door, int zone);
    std::vector<std::tuple<int, int, double>> DoorsNear(const NavSim& nav, double x, double y, double radius, double hour) const;  // (zone, door, weight)
};

} // namespace IYI
