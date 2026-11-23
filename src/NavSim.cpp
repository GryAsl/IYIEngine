// Implements the navigation simulation (NavSim.h): a line-by-line port of the Python nav library
// (core, drive, walk, air). Expressions keep Python's order and helpers (PyExact.h) so seeded runs
// give the same bits; the Python each part came from is named in its comment.

#include "NavSimInternal.h"

namespace IYI {

namespace {

constexpr int KERB_REACH = 5;                        // cells round a wander point searched for a spot off the road
constexpr double SPILL = 3.0;                        // metres of debris round a collapsed building

} // namespace

const std::vector<std::pair<std::string, NavClass>>& NavClasses()
{
    static const std::vector<std::pair<std::string, NavClass>> classes = {
        { "civilian", { .Kind = "foot", .Speed = 1.3, .Run = 3.6, .Radius = 0.3 } },
        { "infantry", { .Kind = "foot", .Speed = 1.5, .Run = 4.2, .Radius = 0.35 } },
        { "car", { .Kind = "wheeled", .Speed = 15.0, .Accel = 2.8, .Brake = 7.5, .Length = 4.4, .Width = 1.8, .Turn = 5.5 } },
        { "pickup", { .Kind = "wheeled", .Speed = 15.0, .Accel = 2.6, .Brake = 7.0, .Length = 5.2, .Width = 1.9, .Turn = 6.0 } },
        { "truck", { .Kind = "wheeled", .Speed = 12.0, .Accel = 1.4, .Brake = 5.5, .Length = 7.6, .Width = 2.5, .Turn = 9.0 } },
        { "apc", { .Kind = "wheeled", .Speed = 13.0, .Accel = 1.9, .Brake = 6.5, .Length = 6.9, .Width = 2.8, .Turn = 8.0 } },
        { "tank", { .Kind = "tracked", .Speed = 10.0, .Accel = 1.3, .Brake = 6.0, .Length = 7.7, .Width = 3.6, .Turn = 0.0 } },
        { "heli", { .Kind = "air", .Speed = 55.0, .Accel = 6.0, .Length = 15.0, .Width = 3.0, .Climb = 7.0, .Pad = 11.0 } },
    };
    return classes;
}

const NavClass& NavClassOf(const std::string& cls)
{
    for (const auto& [name, c] : NavClasses())
        if (name == cls) return c;
    throw std::out_of_range("nav: unknown class " + cls);
}

std::pair<NavXY, NavXY> NavPointAt(const std::vector<NavXY>& points, double s)
{
    for (size_t k = 0; k + 1 < points.size(); ++k) {
        const NavXY a = points[k], b = points[k + 1];
        const double d = Hyp(b.x - a.x, b.y - a.y);
        if (s <= d || b == points.back()) {
            const double t = d < 1e-9 ? 0.0 : std::min(std::max(s / d, 0.0), 1.0);
            return { { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }, d > 1e-9 ? NavXY{ (b.x - a.x) / d, (b.y - a.y) / d } : NavXY{ 1.0, 0.0 } };
        }
        s -= d;
    }
    return { points.back(), { 1.0, 0.0 } };
}

// --- per frame (core.sync, update) ----------------------------------------------------------

void NavSim::Sync()
{
    for (const auto& [oid, state] : m_world->ChangesSince(m_seenRevision)) {
        if (state == "destroyed") m_destroyed.insert(oid);
        else m_destroyed.erase(oid);
        auto b = m_buildingIndex.find(oid);
        if (b != m_buildingIndex.end()) {
            const auto& bd = Map.Buildings[b->second];
            CopyBase(PolygonCells(bd.Footprint));
            if (state == "destroyed") {
                std::vector<std::array<double, 5>> boxes;
                for (const auto& x : bd.Boxes) boxes.push_back({ x[0], x[1], x[2] + 2 * SPILL, x[3] + 2 * SPILL, x[4] });
                SetObstacle("debris:" + oid, boxes, "debris");
            } else ClearObstacle("debris:" + oid);
            Changed(bd.Footprint, SPILL + 2.0);
        } else {
            const auto& r = Map.Roads.at(m_edgeIndex.at(oid));
            if (r.Bridge) CopyBase(LineCells(r.Points, r.Width));
            Changed(r.Points, r.Width);
        }
    }
    m_seenRevision = m_world->Revision();
}

void NavSim::CopyBase(const std::vector<int>& cells)
{
    const uint8_t* bits = m_world->Bits();
    for (int i : cells) {
        const bool culvert = Base[i] & NAV_BRIDGE && !(bits[i] & NAV_BRIDGE) && !BridgeCell(i);
        Base[i] = static_cast<uint8_t>(bits[i] | (culvert ? NAV_BRIDGE : 0));
        Planner.Grid.Base[i] = Base[i];
    }
    Middle(cells);
    Planner.GridChanged = true;
}

void NavSim::Middle(const std::vector<int>& cells)
{
    const int cols = Map.Columns, rows = Map.Rows;
    auto road = [&](int c, int r) { return c >= 0 && r >= 0 && c < cols && r < rows && Base[static_cast<size_t>(r) * cols + c] & (NAV_ROAD | NAV_BRIDGE); };
    for (int i : cells)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const int c = i % cols + dx, r = i / cols + dy;
                if (c < 0 || r < 0 || c >= cols || r >= rows) continue;
                bool mid = road(c, r);
                for (int k = 0; k < 9 && mid; ++k) mid = road(c + k % 3 - 1, r + k / 3 - 1);
                uint8_t& e = Planner.Grid.Extra[static_cast<size_t>(r) * cols + c];
                e = static_cast<uint8_t>(mid ? e | 1 << ROAD_MIDDLE : e & ~(1 << ROAD_MIDDLE));
            }
}

NavXY NavSim::Kerb(NavXY p, double u) const
{
    const auto i = CellIndex(p.x, p.y);
    if (!i || !(Base[*i] & (NAV_ROAD | NAV_BRIDGE))) return p;
    const int cols = Map.Columns, c0 = *i % cols, r0 = *i / cols;
    constexpr uint8_t bad = NAV_ROAD | NAV_BRIDGE | NAV_BUILDING | NAV_WALL | NAV_WATER;
    for (int ring = 1; ring <= KERB_REACH; ++ring) {
        std::vector<int> found;
        for (int dy = -ring; dy <= ring; ++dy)
            for (int dx = -ring; dx <= ring; ++dx) {
                const int c = c0 + dx, r = r0 + dy;
                if (std::max(std::abs(dx), std::abs(dy)) == ring && c >= 0 && r >= 0 && c < cols && r < Map.Rows
                    && !(Base[static_cast<size_t>(r) * cols + c] & bad) && !Extra[static_cast<size_t>(r) * cols + c])
                    found.push_back(r * cols + c);
            }
        if (!found.empty()) {
            const int k = found[std::min(found.size() - 1, static_cast<size_t>(u * found.size()))];
            return { Map.Origin.x + (k % cols + 0.5) * Map.Cell, Map.Origin.y + (k / cols + 0.5) * Map.Cell };
        }
    }
    return p;
}

bool NavSim::BridgeCell(int i)
{
    if (!m_bridgeCells) {
        m_bridgeCells.emplace();
        for (const auto& r : Map.Roads)
            if (r.Bridge) for (int c : LineCells(r.Points, r.Width)) m_bridgeCells->insert(c);
    }
    return m_bridgeCells->count(i) > 0;
}

void NavSim::Update(double dt)
{
    Time += dt;
    Sync();
    Expire();
    Poll();
}

void NavSim::Expire()
{
    std::vector<std::string> expired;
    for (const Hazard& h : m_hazards) if (h.Until <= Time) expired.push_back(h.Id);
    for (const std::string& hid : expired) ClearHazard(hid);
}

NavSim::~NavSim()
{
    if (m_step.valid()) m_step.wait();
}

void NavSim::StartStep(double dt, std::optional<NavXY> focus)
{
    Join();
    Sync();                                                 // reads the world (Python): here, never on the nav thread
    m_step = std::async(std::launch::async, [this, dt, focus] {
        const auto t0 = std::chrono::steady_clock::now();
        const NavXY* f = focus ? &*focus : nullptr;
        Count.Routes = 0;
        Time += dt;
        Expire();
        Poll();
        StepVehicles(dt, f);
        StepWalkers(dt, f);
        StepAir(dt);
        WorkerMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    });
}

void NavSim::Join()
{
    if (m_step.valid()) m_step.get();                       // rethrows what the step threw
    for (auto& write : m_writes) write();
    m_writes.clear();
}

namespace {

void Show(NavWalker& s, const NavWalker& l)
{
    s.Side = l.Side; s.X = l.X; s.Y = l.Y; s.State = l.State; s.Path = l.Path; s.I = l.I; s.Goal = l.Goal; s.Run = l.Run;
    s.Ticket = l.Ticket; s.React = l.React; s.Panic = l.Panic; s.CalmAt = l.CalmAt; s.Building = l.Building; s.Then = l.Then;
    s.Steady = l.Steady; s.Pace = l.Pace;
}

void Show(NavVehicle& s, const NavVehicle& l)
{
    s.Side = l.Side; s.X = l.X; s.Y = l.Y; s.H = l.H; s.Vel = l.Vel; s.Len = l.Len; s.Wid = l.Wid; s.State = l.State; s.Mode = l.Mode;
    s.Doctrine = l.Doctrine; s.Priority = l.Priority; s.Alarm = l.Alarm; s.Path = l.Path; s.S = l.S; s.Seg = l.Seg; s.Lat = l.Lat;
    s.Goal = l.Goal; s.Convoy = l.Convoy; s.Ticket = l.Ticket; s.BlockedBy = l.BlockedBy; s.Blocked = l.Blocked; s.Queued = l.Queued;
    s.Still = l.Still; s.WaitFor = l.WaitFor; s.WaitWalker = l.WaitWalker; s.JNode = l.JNode; s.JGranted = l.JGranted; s.JIn = l.JIn; s.JOut = l.JOut;
}

void Show(NavHeli& s, const NavHeli& l)
{
    s.X = l.X; s.Y = l.Y; s.Z = l.Z; s.Vel = l.Vel; s.H = l.H; s.State = l.State; s.Target = l.Target; s.Land = l.Land; s.Len = l.Len; s.Wid = l.Wid;
}

template <class T>
std::shared_ptr<T> ShownOf(const std::shared_ptr<T>& live, const std::shared_ptr<T>& old, const std::weak_ptr<NavSim>& sim)
{
    std::shared_ptr<T> s = old && old->Live == live ? old : std::make_shared<T>();
    if (s != old) { s->Id = live->Id; s->Cls = live->Cls; s->C = live->C; s->Live = live; s->Sim = sim; }
    Show(*s, *live);
    return s;
}

template <class T>
void ShowAll(NavUnits<T>& shown, const NavUnits<T>& live, const std::weak_ptr<NavSim>& sim)
{
    std::vector<std::shared_ptr<T>> order;
    order.reserve(live.Size());
    for (const auto& l : live.All()) order.push_back(ShownOf(l, shown.Shared(l->Id), sim));
    shown.Assign(std::move(order));
}

template <class T>
void ShowOne(NavUnits<T>& shown, const NavUnits<T>& live, const std::string& id, const std::weak_ptr<NavSim>& sim)
{
    const auto l = live.Shared(id);
    if (!l) { shown.Pop(id); return; }
    shown.Put(ShownOf(l, shown.Shared(id), sim));
}

} // namespace

void NavSim::Publish()
{
    const std::weak_ptr<NavSim> sim = weak_from_this();
    ShowAll(ShownWalkers, Walkers, sim);
    ShowAll(ShownVehicles, Vehicles, sim);
    ShowAll(ShownHelis, Helis, sim);
    PublishEvents();
    ShownTime = Time;
    ShownWorkerMs = WorkerMs;
    ShownCount = Count;
}

void NavSim::PublishEvents()
{
    if (Busy()) return;
    for (NavEvent& e : m_events) ShownEvents.push_back(std::move(e));
    m_events.clear();
}

void NavSim::PublishWalker(const std::string& id) { ShowOne(ShownWalkers, Walkers, id, weak_from_this()); }
void NavSim::PublishVehicle(const std::string& id) { ShowOne(ShownVehicles, Vehicles, id, weak_from_this()); }
void NavSim::PublishHeli(const std::string& id) { ShowOne(ShownHelis, Helis, id, weak_from_this()); }

std::optional<int> NavSim::PathClosed(const std::string& cls, const std::vector<NavXY>& points, int start, const std::array<double, 4>* area)
{
    for (int i = start; i + 1 < static_cast<int>(points.size()); ++i) {
        const double ax = points[i].x, ay = points[i].y, bx = points[i + 1].x, by = points[i + 1].y;
        if (area && (std::max(ax, bx) < (*area)[0] || std::min(ax, bx) > (*area)[2] || std::max(ay, by) < (*area)[1] || std::min(ay, by) > (*area)[3])) continue;
        const int n = std::max(1, static_cast<int>(Hyp(bx - ax, by - ay)));
        for (int k = 0; k <= n; ++k)
            if (!Passable(cls, ax + (bx - ax) * k / n, ay + (by - ay) * k / n)) return i;
    }
    return std::nullopt;
}

// --- collisions -----------------------------------------------------------------------------

namespace {

// drive._overlap: minimum translation (nx, ny, depth) separating box a from box b (separating axes).
std::optional<std::array<double, 3>> Overlap(const NavVehicle& a, const NavVehicle& b)
{
    std::optional<std::array<double, 3>> best;
    const auto ca = BoxCorners(Box(a)), cb = BoxCorners(Box(b));
    for (double h : { a.H, a.H + K_PI / 2, b.H, b.H + K_PI / 2 }) {
        const double nx = std::cos(h), ny = std::sin(h);
        double amin = 0, amax = 0, bmin = 0, bmax = 0;
        for (int k = 0; k < 4; ++k) {
            const double pa = ca[k].x * nx + ca[k].y * ny, pb = cb[k].x * nx + cb[k].y * ny;
            amin = k ? std::min(amin, pa) : pa; amax = k ? std::max(amax, pa) : pa;
            bmin = k ? std::min(bmin, pb) : pb; bmax = k ? std::max(bmax, pb) : pb;
        }
        const double depth = std::min(amax, bmax) - std::max(amin, bmin);
        if (depth <= 0) return std::nullopt;
        if (!best || depth < (*best)[2]) {
            const double sign = (a.X - b.X) * nx + (a.Y - b.Y) * ny >= 0 ? 1 : -1;
            best = std::array<double, 3>{ nx * sign, ny * sign, depth };
        }
    }
    return best;
}

} // namespace

bool NavSim::NewContact(const std::pair<std::string, std::string>& pair)
{
    // A touch is news unless the pair touched within the last 2 s (boxes part and touch again).
    auto it = m_touched.find(pair);
    const bool fresh = !m_contacts.count(pair) && Time - (it == m_touched.end() ? -1e9 : it->second) > 2.0;
    m_touched[pair] = Time;
    return fresh;
}

void NavSim::Collide(const std::vector<NavVehicle*>& vehicles, const WalkerGrid& walkers)
{
    VehicleGrid grid;
    for (NavVehicle* v : vehicles) grid[Key(Py::ifloordiv(v->X, 10), Py::ifloordiv(v->Y, 10))].push_back(v);
    std::set<std::pair<std::string, std::string>> now;
    for (NavVehicle* v : vehicles) {
        if (StaticState(v->State)) continue;
        const int64_t gx = Py::ifloordiv(v->X, 10), gy = Py::ifloordiv(v->Y, 10);
        for (int64_t i = gx - 1; i <= gx + 1; ++i)
            for (int64_t j = gy - 1; j <= gy + 1; ++j) {
                auto it = grid.find(Key(i, j));
                if (it == grid.end()) continue;
                for (NavVehicle* o : it->second) {
                    if (o == v || (!StaticState(o->State) && o->Id < v->Id)) continue;
                    const double reach = (v->Len + o->Len) / 2;
                    if (std::fabs(o->X - v->X) > reach || std::fabs(o->Y - v->Y) > reach) continue;
                    const auto m = Overlap(*v, *o);
                    if (!m) continue;
                    const auto pair = v->Id < o->Id ? std::make_pair(v->Id, o->Id) : std::make_pair(o->Id, v->Id);
                    now.insert(pair);
                    const double rvx = std::cos(v->H) * v->Vel - std::cos(o->H) * o->Vel;
                    const double rvy = std::sin(v->H) * v->Vel - std::sin(o->H) * o->Vel;
                    const double speed = std::fabs(rvx * (*m)[0] + rvy * (*m)[1]);
                    if (NewContact(pair) && speed >= TOUCH)
                        Emit("collision", { { "a", V(pair.first) }, { "b", V(pair.second) }, { "speed", R(speed, 2) },
                                              { "x", R((v->X + o->X) / 2, 1) }, { "y", R((v->Y + o->Y) / 2, 1) } });
                    const double share = StaticState(o->State) ? 1.0 : 0.5;
                    v->X += (*m)[0] * (*m)[2] * share;
                    v->Y += (*m)[1] * (*m)[2] * share;
                    v->Vel *= 0.2;
                    if (!StaticState(o->State)) {
                        v->WaitFor = o->Id;                  // touching: each holds the other (a ring the deadlock breaker sees)
                        o->WaitFor = v->Id;
                        o->X -= (*m)[0] * (*m)[2] * 0.5;
                        o->Y -= (*m)[1] * (*m)[2] * 0.5;
                        o->Vel *= 0.2;
                    }
                }
            }
        std::vector<std::pair<int64_t, int64_t>> added;
        for (int dx : { -6, 6 })
            for (int dy : { -6, 6 }) added.push_back({ Py::ifloordiv(v->X + dx, 100), Py::ifloordiv(v->Y + dy, 100) });
        std::vector<NavWalker*> near;
        for (const auto& [i, j] : Py::SetOrder(added)) {
            auto it = walkers.find(Key(i, j));
            if (it != walkers.end()) near.insert(near.end(), it->second.begin(), it->second.end());
        }
        HitWalkers(*v, now, near);
        HitBuildings(*v, now);
    }
    m_contacts = std::move(now);
}

void NavSim::HitWalkers(NavVehicle& v, std::set<std::pair<std::string, std::string>>& now, const std::vector<NavWalker*>& walkers)
{
    const double hx = std::cos(v.H), hy = std::sin(v.H);
    for (NavWalker* w : walkers) {
        if (w->State == "hidden" || w->State == "down" || std::fabs(w->X - v.X) > 6 || std::fabs(w->Y - v.Y) > 6) continue;
        const double dx = w->X - v.X, dy = w->Y - v.Y;
        const double across = -dx * hy + dy * hx;
        if (std::fabs(dx * hx + dy * hy) < v.Len / 2 + 0.3 && std::fabs(across) < v.Wid / 2 + 0.3) {
            if (std::fabs(v.Vel) < 1.0) {
                const double side = across >= 0 ? 1.0 : -1.0, along = dx * hx + dy * hy;   // shoved out of the box, never into a wall
                const double k = (v.Wid / 2 + 0.35 - std::fabs(across)) * side, k2 = -(v.Wid / 2 + 0.35 + std::fabs(across)) * side;
                const double f = (v.Len / 2 + 0.35) * (v.Vel >= 0 ? 1.0 : -1.0) - along;
                for (const NavXY& p : { NavXY{ w->X - hy * k, w->Y + hx * k }, NavXY{ w->X - hy * k2, w->Y + hx * k2 }, NavXY{ w->X + hx * f, w->Y + hy * f } })
                    if (Passable(w->Cls, p.x, p.y)) { w->X = p.x; w->Y = p.y; break; }
                continue;
            }
            const auto pair = std::make_pair(v.Id, w->Id);
            now.insert(pair);
            if (NewContact(pair)) {
                w->State = "down";
                w->Path.clear();
                Emit("hit", { { "vehicle", V(v.Id) }, { "walker", V(w->Id) }, { "speed", R(std::fabs(v.Vel), 2) } });
                v.Vel *= 0.6;
            }
        }
    }
}

void NavSim::HitBuildings(NavVehicle& v, std::set<std::pair<std::string, std::string>>& now)
{
    // Buildings stop a vehicle off its road (road routes trust the map's roads).
    if (v.Mode == "road" && std::fabs(v.Lat) < 1.5) return;
    for (const NavXY& p : BoxCorners(Box(v))) {
        const auto i = CellIndex(p.x, p.y);
        const auto bid = i && Base[*i] & NAV_BUILDING ? BuildingAt(p.x, p.y) : std::nullopt;   // Base: the world's bits as of Sync
        if (!Has(bid)) continue;                          // the footprint itself, not just its 2 m cells
        const auto pair = std::make_pair(v.Id, std::string("building"));
        now.insert(pair);
        const bool fresh = NewContact(pair);
        if (fresh) Emit("collision", { { "a", V(v.Id) }, { "b", V(*bid) }, { "speed", R(std::fabs(v.Vel), 2) }, { "x", R(p.x, 1) }, { "y", R(p.y, 1) } });
        if (fresh) {
            if (v.State == "drive") Reverse(v, 6.0);       // back off the wall, then plan again
            v.SwerveAt = Time + 10.0;
            v.LatGoal = 0.0;
            v.PassUntil = -1.0;
        } else if (v.Still > 3.0) {                        // still wedged against it: slide out, away from the corner inside
            const double d = Hyp(v.X - p.x, v.Y - p.y);
            v.X += (v.X - p.x) / d * 0.1;
            v.Y += (v.Y - p.y) / d * 0.1;
        }
        v.X -= std::cos(v.H) * v.Vel * 0.05;
        v.Y -= std::sin(v.H) * v.Vel * 0.05;
        v.Vel = 0.0;
        return;
    }
}

void NavSim::Jams(const std::vector<NavVehicle*>& vehicles)
{
    // Every 2 s: stopped vehicles add delay to their road, so others route round a jam.
    if (Time < m_jamAt) return;
    m_jamAt = Time + 2.0;
    std::map<int, int> count;
    for (const NavVehicle* v : vehicles)
        if (std::fabs(v->Vel) < 0.5 && (v->State == "drive" || v->State == "parked" || v->State == "disabled" || v->State == "stuck"))
            if (const auto hit = Snap(v->X, v->Y, 8.0)) ++count[std::get<0>(*hit)];
    std::set<int> edges = m_jamEdges;
    for (const auto& [e, n] : count) edges.insert(e);
    for (int e : edges) {
        auto it = count.find(e);
        SetDelay(e, 20.0 * (it == count.end() ? 0 : it->second));
    }
    m_jamEdges.clear();
    for (const auto& [e, n] : count) m_jamEdges.insert(e);
}

} // namespace IYI
