// Implements the living-world pieces (Life.h): scheduler, event log, facts, population and its pool.

#include "IYI/Life.h"
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace IYI {

namespace {

double Hyp(double x, double y) { return Py::hypot(x, y); }

// Share of a neighbourhood's people outdoors at each hour (the game day's shape: dawn prayer, morning
// work and market, the midday heat indoors, the busy evening, night).
constexpr double OUTDOORS[24] = { 0.01, 0.01, 0.01, 0.01, 0.02, 0.08, 0.25, 0.40, 0.45, 0.45, 0.40, 0.35,
                                  0.30, 0.25, 0.25, 0.35, 0.45, 0.55, 0.55, 0.40, 0.25, 0.12, 0.05, 0.02 };

double Outdoors(double hour)
{
    const double h = Py::mod(hour, 24.0);
    const int a = static_cast<int>(h);
    const double t = h - a;
    return OUTDOORS[a] * (1.0 - t) + OUTDOORS[(a + 1) % 24] * t;
}

// No vehicle within `reach` of p: someone stepping out of a door there is not run over, a car put there hits nobody.
bool Clear(NavSim& nav, NavXY p, double reach)
{
    for (const auto& v : nav.Vehicles.All())
        if (Hyp(v->X - p.x, v->Y - p.y) < reach) return false;
    return true;
}

bool Calm(const NavWalker& w) { return w.Panic < 0.1 && !w.Ticket && (w.State == "idle" || w.State == "walk") && !(w.Then && *w.Then == "hidden"); }

} // namespace

// ---- Scheduler ----

void Scheduler::At(const std::string& id, double time)
{
    m_live[id] = { ++m_seq, time };
    m_heap.emplace(time, m_seq, id);
}

std::vector<std::string> Scheduler::Due(double now, int budget)
{
    std::vector<std::string> out;
    while (!m_heap.empty() && static_cast<int>(out.size()) < budget && std::get<0>(m_heap.top()) <= now) {
        const auto [t, seq, id] = m_heap.top();
        m_heap.pop();
        auto it = m_live.find(id);
        if (it == m_live.end() || it->second.first != seq) continue;   // rescheduled or cancelled since
        m_live.erase(it);
        out.push_back(id);
    }
    return out;
}

std::vector<std::pair<std::string, double>> Scheduler::Items() const
{
    std::vector<std::pair<std::string, double>> out;
    for (const auto& [id, at] : m_live) out.emplace_back(id, at.second);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return std::tie(a.second, a.first) < std::tie(b.second, b.first); });
    return out;
}

// ---- EventLog ----

int64_t EventLog::Add(WorldEvent e)
{
    e.Id = m_next++;
    m_events.push_back(std::move(e));
    while (m_events.size() > Limit) m_events.pop_front();
    return m_events.back().Id;
}

std::vector<const WorldEvent*> EventLog::Since(int64_t id) const
{
    std::vector<const WorldEvent*> out;
    for (auto it = m_events.rbegin(); it != m_events.rend() && it->Id > id; ++it) out.push_back(&*it);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<const WorldEvent*> EventLog::Near(double x, double y, double radius, double since, const std::string& kind) const
{
    std::vector<const WorldEvent*> out;
    for (auto it = m_events.rbegin(); it != m_events.rend() && it->Time >= since; ++it)
        if ((kind.empty() || it->Kind == kind) && Hyp(it->X - x, it->Y - y) <= radius) out.push_back(&*it);
    std::reverse(out.begin(), out.end());
    return out;
}

const WorldEvent* EventLog::Get(int64_t id) const
{
    if (m_events.empty() || id < m_events.front().Id || id > m_events.back().Id) return nullptr;
    return &m_events[static_cast<size_t>(id - m_events.front().Id)];   // ids are consecutive
}

// ---- Facts ----

double Facts::Confidence(const Fact& f, double now) const
{
    return f.Confidence * std::pow(0.5, std::max(0.0, now - f.Time) / HalfLife);
}

int64_t Facts::Tell(Fact f)
{
    for (Fact& old : m_facts)
        if (old.Owner == f.Owner && old.Kind == f.Kind && old.Subject == f.Subject) {
            if (f.Time < old.Time) return old.Id;                      // an older report changes nothing
            f.Confidence = std::max(f.Confidence, Confidence(old, f.Time));
            f.Id = old.Id;
            old = std::move(f);
            return old.Id;
        }
    f.Id = m_next++;
    m_facts.push_back(std::move(f));
    return m_facts.back().Id;
}

std::vector<Fact> Facts::Near(const std::string& owner, double x, double y, double radius, double now, const std::string& kind, double least) const
{
    std::vector<std::pair<double, Fact>> found;
    for (const Fact& f : m_facts) {
        if (f.Owner != owner || (!kind.empty() && f.Kind != kind)) continue;
        const double d = Hyp(f.X - x, f.Y - y), c = Confidence(f, now);
        if (d <= radius && c >= least) {
            found.emplace_back(d, f);
            found.back().second.Confidence = c;
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Fact> out;
    for (auto& [d, f] : found) out.push_back(std::move(f));
    return out;
}

std::vector<Fact> Facts::About(const std::string& owner, const std::string& subject, double now) const
{
    std::vector<Fact> out;
    for (const Fact& f : m_facts)
        if (f.Owner == owner && f.Subject == subject) {
            out.push_back(f);
            out.back().Confidence = Confidence(f, now);
        }
    return out;
}

void Facts::Forget(const std::string& owner, const std::string& subject, const std::string& kind)
{
    std::erase_if(m_facts, [&](const Fact& f) { return f.Owner == owner && f.Subject == subject && (kind.empty() || f.Kind == kind); });
}

void Facts::Prune(double now, double least)
{
    std::erase_if(m_facts, [&](const Fact& f) { return Confidence(f, now) < least; });
}

// ---- Population ----

Population::Population(std::vector<LifeZone> zones, Grid grid, NavSim& nav, int64_t seed, int walkers, int cars)
    : Zones(std::move(zones)), Cells(std::move(grid)), m_rng(std::to_string(seed) + ":population")
{
    if (Cells.Zones.size() != static_cast<size_t>(Cells.Columns) * Cells.Rows) throw std::invalid_argument("Population: grid is not columns * rows");
    std::unordered_map<std::string, const NavMap::Building*> byId;
    for (const auto& b : nav.Map.Buildings) byId[b.Id] = &b;
    std::vector<std::pair<int, int>> all;                            // (zone, door) of every door
    for (size_t z = 0; z < Zones.size(); ++z) {
        LifeZone& zone = Zones[z];
        for (const std::string& id : zone.Buildings) {
            auto it = byId.find(id);
            if (it == byId.end()) continue;
            const auto& fp = it->second->Footprint;
            double cx = 0.0, cy = 0.0;
            for (const NavXY& p : fp) { cx += p.x; cy += p.y; }
            cx /= static_cast<double>(fp.size()); cy /= static_cast<double>(fp.size());
            std::vector<std::pair<double, NavXY>> sides;              // the middle of each wall, longest first
            for (size_t k = 0; k < fp.size(); ++k) {
                const NavXY a = fp[k], b = fp[(k + 1) % fp.size()];
                const NavXY m{ (a.x + b.x) / 2, (a.y + b.y) / 2 };
                double d = Hyp(m.x - cx, m.y - cy);
                if (d == 0.0) d = 1.0;
                sides.push_back({ -Hyp(b.x - a.x, b.y - a.y), { m.x + (m.x - cx) / d * 1.5, m.y + (m.y - cy) / d * 1.5 } });
            }
            std::stable_sort(sides.begin(), sides.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [len, door] : sides)
                if (nav.Passable("civilian", door.x, door.y)) {
                    zone.Doors.push_back({ door, id });
                    all.push_back({ static_cast<int>(z), static_cast<int>(zone.Doors.size()) - 1 });
                    break;
                }
        }
    }
    std::vector<bool> fits(nav.Map.Nodes.size(), false);
    for (const auto& r : nav.Map.Roads)
        if (r.Width >= 3.5) fits[r.A] = fits[r.B] = true;
    for (size_t n = 0; n < fits.size(); ++n)
        if (fits[n]) m_carNodes.push_back(static_cast<int>(n));
    for (int k = 0; k < walkers && !all.empty(); ++k) {              // everyone starts indoors
        const auto [z, d] = m_rng.choice(all);
        Member m{ "p" + std::to_string(k), z, Zones[z].Doors[d].first, Zones[z].Doors[d].second };
        NavWalker& w = nav.SpawnWalker(m.Id, "civilian", m.Home.x, m.Home.y);
        w.State = "hidden";
        w.Building = m.Building;
        m_index[m.Id] = static_cast<int>(m_people.size());
        m_people.push_back(std::move(m));
    }
    for (int k = 0; k < cars; ++k) m_cars.push_back("pc" + std::to_string(k));
}

int Population::ZoneAt(double x, double y) const
{
    const int64_t c = Py::ifloor((x - Cells.Origin.x) / Cells.Cell), r = Py::ifloor((y - Cells.Origin.y) / Cells.Cell);
    if (c < 0 || r < 0 || c >= Cells.Columns || r >= Cells.Rows) return -1;
    return Cells.Zones[static_cast<size_t>(r * Cells.Columns + c)];
}

double Population::Activity(int zone, double hour) const
{
    return Outdoors(hour) * (1.0 - 0.8 * Zones.at(zone).Fear);
}

void Population::Shock(double x, double y, double radius, double fear)
{
    std::vector<double> nearest(Zones.size(), radius);
    const int64_t c0 = Py::ifloor((x - radius - Cells.Origin.x) / Cells.Cell), c1 = Py::ifloor((x + radius - Cells.Origin.x) / Cells.Cell);
    const int64_t r0 = Py::ifloor((y - radius - Cells.Origin.y) / Cells.Cell), r1 = Py::ifloor((y + radius - Cells.Origin.y) / Cells.Cell);
    for (int64_t r = std::max<int64_t>(r0, 0); r <= std::min<int64_t>(r1, Cells.Rows - 1); ++r)
        for (int64_t c = std::max<int64_t>(c0, 0); c <= std::min<int64_t>(c1, Cells.Columns - 1); ++c) {
            const int z = Cells.Zones[static_cast<size_t>(r * Cells.Columns + c)];
            if (z < 0) continue;
            const double d = Hyp(Cells.Origin.x + (c + 0.5) * Cells.Cell - x, Cells.Origin.y + (r + 0.5) * Cells.Cell - y);
            nearest[z] = std::min(nearest[z], d);
        }
    for (size_t z = 0; z < Zones.size(); ++z)
        if (nearest[z] < radius) Zones[z].Fear = std::min(1.0, Zones[z].Fear + fear * (1.0 - nearest[z] / radius));
}

void Population::Step(double hours)
{
    for (LifeZone& z : Zones) {
        if (z.Fear > 0.4) z.Displaced += (z.People - z.Displaced) * (z.Fear - 0.4) * std::min(1.0, hours / 24.0);   // leave within a day
        else z.Displaced -= z.Displaced * std::min(1.0, hours / 72.0);                                                // come back over days
        z.Fear -= z.Fear * std::min(1.0, hours / 24.0);
        z.Support = std::clamp(z.Support, 0.0, 1.0);
    }
}

std::vector<std::tuple<int, int, double>> Population::DoorsNear(const NavSim& nav, double x, double y, double radius, double hour) const
{
    std::vector<std::tuple<int, int, double>> out;
    for (size_t z = 0; z < Zones.size(); ++z) {
        const LifeZone& zone = Zones[z];
        if (zone.Doors.empty() || zone.People <= 0.0) continue;
        const double outdoors = Activity(static_cast<int>(z), hour) * (zone.People - zone.Displaced) / static_cast<double>(zone.Doors.size());
        for (size_t d = 0; d < zone.Doors.size(); ++d) {
            const NavXY p = zone.Doors[d].first;
            if (Hyp(p.x - x, p.y - y) <= radius) out.emplace_back(static_cast<int>(z), static_cast<int>(d), outdoors);
        }
    }
    (void)nav;
    return out;
}

void Population::Out(NavSim& nav, Member& m, const std::pair<NavXY, std::string>& door, int zone)
{
    NavWalker& w = *nav.Walkers.Get(m.Id);
    m.Zone = zone; m.Home = door.first; m.Building = door.second;
    w.X = door.first.x; w.Y = door.first.y;
    w.State = "idle"; w.Path.clear(); w.I = 0; w.Goal.reset(); w.Then.reset(); w.Run = false;
    w.Panic = 0.0; w.Noticed = nav.Time; w.Building = door.second;
    if (!m_wander.empty()) nav.Walk(m.Id, nav.Kerb(m_rng.choice(m_wander), m_rng.random()));
}

Population::Counts Population::Manage(NavSim& nav, NavXY focus, double hour)
{
    Counts c;
    if (Hyp(focus.x - m_wanderAt.x, focus.y - m_wanderAt.y) > 100.0) {   // idle people stroll between road nodes near the camera
        m_wanderAt = focus;
        m_wander.clear();
        for (int n : m_carNodes)
            if (Hyp(nav.Map.Nodes[n].x - focus.x, nav.Map.Nodes[n].y - focus.y) < NEAR) m_wander.push_back(nav.Map.Nodes[n]);
        if (!m_wander.empty()) nav.Wander(m_wander);
    }
    std::vector<std::tuple<int, int, double>> doors = DoorsNear(nav, focus.x, focus.y, NEAR, hour);
    std::vector<double> cum;
    double want = 0.0;
    for (auto& [z, d, weight] : doors)
        if (!nav.Destroyed(Zones[z].Doors[d].second)) cum.push_back(want += weight);
        else cum.push_back(want);
    c.Want = std::min(static_cast<int>(m_people.size()), static_cast<int>(want * DENSITY));

    std::vector<Member*> indoors;
    std::vector<std::pair<double, Member*>> calm;
    for (Member& m : m_people) {
        NavWalker* w = nav.Walkers.Get(m.Id);
        if (!w || w->State == "down") continue;                     // taken out of the pool by the game
        if (Pinned.count(m.Id)) { c.Out += w->State != "hidden"; continue; }
        if (w->State == "hidden") {
            if (w->Panic == 0.0 && !w->Ticket) indoors.push_back(&m);
            continue;
        }
        const double d = Hyp(w->X - focus.x, w->Y - focus.y);
        if (Calm(*w) && d > FAR) {                                   // out of sight: indoors
            w->State = "hidden"; w->Path.clear(); w->Building = m.Building;
            indoors.push_back(&m);
            continue;
        }
        ++c.Out;
        if (Calm(*w)) calm.emplace_back(d, &m);
    }
    const int excess = c.Out - c.Want - c.Want / 10 - 5;
    if (excess > 0) {                                                // evening: the farthest walk home
        std::stable_sort(calm.begin(), calm.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (int k = 0; k < std::min({ excess, PER_CALL, static_cast<int>(calm.size()) }); ++k) {
            Member& m = *calm[k].second;
            nav.Walk(m.Id, m.Home, false, "hidden");
            nav.Walkers.Get(m.Id)->Building = m.Building;
        }
    }
    for (int k = 0; c.Out < c.Want && k < PER_CALL && k < static_cast<int>(indoors.size()) && want > 0.0; ++k) {
        const size_t i = std::upper_bound(cum.begin(), cum.end(), m_rng.random() * want) - cum.begin();
        const auto& [z, d, weight] = doors[std::min(i, doors.size() - 1)];
        if (!Clear(nav, Zones[z].Doors[d].first, 15.0)) continue;
        Out(nav, *indoors[k], Zones[z].Doors[d], z);
        ++c.Out;
    }

    // cars: leave the map far away, come in on roads near the edge of the view
    c.WantCars = std::min(static_cast<int>(m_cars.size()), static_cast<int>(c.Want * CARS));
    std::vector<NavXY> near, edge;
    for (int n : m_carNodes) {
        const NavXY p = nav.Map.Nodes[n];
        const double d = Hyp(p.x - focus.x, p.y - focus.y);
        if (d < NEAR) near.push_back(p);
        if (d > 200.0 && d < NEAR) edge.push_back(p);
    }
    std::vector<std::string> free;
    for (const std::string& id : m_cars) {
        NavVehicle* v = nav.Vehicles.Get(id);
        if (!v) { free.push_back(id); continue; }
        if (Pinned.count(id)) { ++c.Cars; continue; }
        const bool calmCar = v->Alarm < 0.1 && (v->State == "drive" || v->State == "arrived" || v->State == "idle");
        if (calmCar && Hyp(v->X - focus.x, v->Y - focus.y) > FAR + 100.0) { nav.Remove(id); free.push_back(id); continue; }
        ++c.Cars;
        if (calmCar && v->State == "arrived" && !near.empty()) nav.Drive(id, m_rng.choice(near));
    }
    for (size_t k = 0; c.Cars < c.WantCars && k < free.size() && k < PER_CALL / 2 && !edge.empty(); ++k) {
        const NavXY p = m_rng.choice(edge);
        if (!Clear(nav, p, 30.0)) continue;
        static const std::vector<std::string> kinds = { "car", "car", "pickup", "truck" };
        nav.SpawnVehicle(free[k], m_rng.choice(kinds), p.x, p.y, m_rng.uniform(0.0, 2 * 3.141592653589793), "civilian");
        nav.Drive(free[k], m_rng.choice(near));
        ++c.Cars;
    }
    return c;
}

std::vector<std::string> Population::Summon(NavSim& nav, int n, double x, double y, double radius)
{
    std::vector<std::string> out;
    std::vector<std::tuple<int, int, double>> doors = DoorsNear(nav, x, y, radius, 12.0);
    std::erase_if(doors, [&](const auto& t) { return nav.Destroyed(Zones[std::get<0>(t)].Doors[std::get<1>(t)].second); });
    if (doors.empty()) return out;
    for (Member& m : m_people) {
        if (static_cast<int>(out.size()) >= n) break;
        NavWalker* w = nav.Walkers.Get(m.Id);
        if (!w || w->State != "hidden" || w->Panic != 0.0 || w->Ticket || Pinned.count(m.Id)) continue;
        const auto& [z, d, weight] = m_rng.choice(doors);
        if (!Clear(nav, Zones[z].Doors[d].first, 15.0)) continue;
        Out(nav, m, Zones[z].Doors[d], z);
        Pinned.insert(m.Id);
        out.push_back(m.Id);
    }
    return out;
}

int Population::Home(const std::string& id) const
{
    auto it = m_index.find(id);
    return it == m_index.end() ? -1 : m_people[it->second].Zone;
}

std::vector<std::string> Population::Members() const
{
    std::vector<std::string> out;
    for (const Member& m : m_people) out.push_back(m.Id);
    return out;
}

} // namespace IYI
