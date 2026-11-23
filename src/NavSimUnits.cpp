// Moved verbatim out of NavSim.cpp.

#include "NavSimInternal.h"

namespace IYI {

namespace {

// walk
constexpr double SEPARATION = 0.7, STUCK = 3.0;
// air
constexpr double CRUISE = 60.0, CHECK_BELOW = 25.0;


} // namespace

// --- helicopters' landing sites (core.landing_site) -----------------------------------------

std::vector<NavXY> NavSim::Occupants() const
{
    std::vector<NavXY> out;
    for (const auto& v : Vehicles.All()) if (v->State != "hidden") out.push_back({ v->X, v->Y });
    for (const auto& w : Walkers.All()) if (w->State != "hidden") out.push_back({ w->X, w->Y });
    return out;
}

std::optional<NavXY> NavSim::LandingSite(double x, double y, double pad, double search)
{
    if (pad == 0.0) pad = NavClassOf("heli").Pad;
    const uint8_t bad = NAV_WATER | NAV_STEEP | NAV_BUILDING | NAV_RUBBLE;
    const std::vector<NavXY> occupants = Occupants();
    const double step = pad * 0.75;
    for (int ring = 0; ring <= static_cast<int>(search / step); ++ring) {
        const int n = std::max(1, static_cast<int>(2 * K_PI * ring));
        for (int k = 0; k < n; ++k) {
            const double a = 2 * K_PI * k / n;
            const double cx = x + std::cos(a) * ring * step, cy = y + std::sin(a) * ring * step;
            if (PadOk(cx, cy, pad, bad, occupants)) return NavXY{ cx, cy };
        }
    }
    return std::nullopt;
}

bool NavSim::PadOk(double cx, double cy, double pad, uint8_t bad, const std::vector<NavXY>& occupants)
{
    for (const NavXY& o : occupants) if (Hyp(o.x - cx, o.y - cy) < pad) return false;
    const int r = static_cast<int>(pad / Map.Cell);
    const int64_t c0 = Py::ifloor((cx - Map.Origin.x) / Map.Cell), r0 = Py::ifloor((cy - Map.Origin.y) / Map.Cell);
    if (c0 - r < 0 || r0 - r < 0 || c0 + r >= Map.Columns || r0 + r >= Map.Rows) return false;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r) {
                const size_t i = static_cast<size_t>((r0 + dy) * Map.Columns + c0 + dx);
                if (Base[i] & bad || Extra[i]) return false;
            }
    const double hs[5] = { Height(cx, cy), Height(cx + pad, cy), Height(cx - pad, cy), Height(cx, cy + pad), Height(cx, cy - pad) };
    double lo = hs[0], hi = hs[0];
    for (double h : hs) { lo = std::min(lo, h); hi = std::max(hi, h); }
    return hi - lo < pad * std::tan(Py::radians(8));
}

// --- walkers (walk.py) ----------------------------------------------------------------------

NavWalker& NavSim::SpawnWalker(const std::string& id, const std::string& cls, double x, double y, std::optional<std::string> side)
{
    auto w = std::make_shared<NavWalker>();
    w->Id = id;
    w->Cls = cls;
    w->C = &NavClassOf(cls);
    w->Side = Has(side) ? *side : cls == "civilian" ? "civilian" : "military";
    w->X = x;
    w->Y = y;
    w->Rng = Py::Random(std::to_string(Seed) + ":" + id);
    w->React = w->Rng.uniform(0.4, 1.4);
    w->Noticed = Time;
    w->Rev = Revision;
    Walkers.Put(w);
    return *w;
}

void NavSim::Walk(const std::string& id, NavXY goal, bool run, std::optional<std::string> then)
{
    NavWalker* w = Walkers.Get(id);
    if (!w) throw std::out_of_range("nav: no walker " + id);
    w->Goal = goal;
    w->Run = run;
    w->Then = std::move(then);
    WalkerRequest(*w);
}

void NavSim::Gather(const std::vector<std::string>& ids, double x, double y, double radius)
{
    for (const std::string& id : ids) {
        NavWalker* w = Walkers.Get(id);
        if (!w) throw std::out_of_range("nav: no walker " + id);
        const double a = w->Rng.uniform(0, 2 * K_PI), r = radius * std::sqrt(w->Rng.random());
        Walk(id, { x + std::cos(a) * r, y + std::sin(a) * r });
    }
}

void NavSim::WalkerRequest(NavWalker& w, bool roundVehicles)
{
    w.ReplanAt = Time + 2.0;
    Forget(w.Ticket);
    w.Ticket = Request({ w.X, w.Y }, *w.Goal, w.Cls, "free", w.Side, std::nullopt, false,
                       roundVehicles ? Standing({ w.X, w.Y }, *w.Goal) : std::vector<int>{});
    if (w.State != "down" && w.State != "hidden") w.State = "wait";
}

void NavSim::AlarmWalker(NavWalker& w, NavXY at, double level)
{
    if (w.State == "down" || w.State == "hidden") return;
    w.Panic = std::min(1.0, w.Panic + level);
    w.CalmAt = Time + w.Rng.uniform(40.0, 90.0);
    if (w.Steady) return;
    if (w.Cls != "civilian") {
        if (auto spot = Shelter(w, 15.0)) Walk(w.Id, spot->first, true, "cover");
        else { w.State = "cover"; w.Path.clear(); }
        return;
    }
    std::optional<std::pair<NavXY, std::string>> spot;
    if (w.Rng.random() < 0.55) spot = Shelter(w, 40.0);
    if (spot) {
        Walk(w.Id, spot->first, true, "hidden");
        w.Building = spot->second;
        return;
    }
    const double dx = w.X - at.x, dy = w.Y - at.y;
    double d = Hyp(dx, dy);
    if (d == 0.0) d = 1.0;
    const double a = std::atan2(dy / d, dx / d) + w.Rng.uniform(-0.6, 0.6);
    const double far = w.Rng.uniform(120.0, 250.0);
    Walk(w.Id, { w.X + std::cos(a) * far, w.Y + std::sin(a) * far }, true);
    w.State = "flee";
}

std::optional<std::pair<NavXY, std::string>> NavSim::Shelter(const NavWalker& w, double reach)
{
    std::optional<std::tuple<double, NavXY, std::string>> best;
    for (int64_t i = Py::ifloor((w.X - reach) / BUILDING_BUCKET); i <= Py::ifloor((w.X + reach) / BUILDING_BUCKET); ++i)
        for (int64_t j = Py::ifloor((w.Y - reach) / BUILDING_BUCKET); j <= Py::ifloor((w.Y + reach) / BUILDING_BUCKET); ++j) {
            auto it = m_buildingBuckets.find(Key(i, j));
            if (it == m_buildingBuckets.end()) continue;
            for (int bi : it->second) {
                const auto& b = Map.Buildings[bi];
                if (Destroyed(b.Id)) continue;
                const auto& fp = b.Footprint;
                Py::Sum sx, sy;
                for (const NavXY& p : fp) sx.add(p.x);
                for (const NavXY& p : fp) sy.add(p.y);
                const double cx = sx.value() / static_cast<double>(fp.size()), cy = sy.value() / static_cast<double>(fp.size());
                for (size_t k = 0; k < fp.size(); ++k) {
                    const NavXY& p = fp[k];
                    const NavXY& q = fp[(k + 1) % fp.size()];
                    const double mx = (p.x + q.x) / 2, my = (p.y + q.y) / 2;
                    const double d = Hyp(mx - w.X, my - w.Y);
                    if (d < reach && (!best || d < std::get<0>(*best))) {
                        double kk = Hyp(mx - cx, my - cy);
                        if (kk == 0.0) kk = 1.0;
                        best = std::make_tuple(d, NavXY{ mx + (mx - cx) / kk * 1.2, my + (my - cy) / kk * 1.2 }, b.Id);
                    }
                }
            }
        }
    if (!best) return std::nullopt;
    return std::make_pair(std::get<1>(*best), std::get<2>(*best));
}

void NavSim::StepWalkers(double dt, const NavXY* focus)
{
    const std::vector<std::shared_ptr<NavWalker>> walkers = Walkers.All();
    WalkerGrid grid;
    for (const auto& w : walkers)
        if (w->State != "hidden") grid[Key(Py::ifloordiv(w->X, 2), Py::ifloordiv(w->Y, 2))].push_back(w.get());
    VehicleGrid movers;                             // vehicles by 50 m cell: moving ones to dodge, standing ones to walk round
    for (const auto& v : Vehicles.All())
        movers[Key(Py::ifloordiv(v->X, 50), Py::ifloordiv(v->Y, 50))].push_back(v.get());
    for (const Alarm& a : m_alarms)
        for (const auto& w : walkers) {
            if (w->Noticed >= a.Time || w->State == "down" || w->State == "hidden") continue;
            const double d = Hyp(w->X - a.X, w->Y - a.Y);
            const double hear = a.Radius * 5;
            if (d < hear && Time - a.Time >= w->React) {
                w->Noticed = a.Time;
                AlarmWalker(*w, { a.X, a.Y }, a.Severity * (1.0 - d / hear) * 1.5);
            }
        }
    std::unordered_map<std::string, const NavVehicle*> waited;   // walkers a vehicle has stood behind for 2 s
    for (const auto& v : Vehicles.All())
        if (!v->WaitWalker.empty() && v->State == "drive" && v->Still > 2.0) waited[v->WaitWalker] = v.get();
    Count.Walkers = Count.Near = 0;
    for (const auto& w : walkers) {
        const bool far = focus && w->Panic < 0.1 && Hyp(w->X - focus->x, w->Y - focus->y) > WALK_FAR;
        double wdt;
        if (far) {
            w->Acc += dt;
            w->Stride = (w->Stride + 1) % FAR_STRIDE;
            if (w->Stride) continue;
            wdt = w->Acc;
        } else wdt = dt + w->Acc;
        w->Acc = 0.0;
        ++Count.Walkers;
        const auto it = waited.find(w->Id);
        StepWalker(*w, wdt, far ? nullptr : &grid, movers, it == waited.end() ? nullptr : it->second);
    }
}

namespace {

// walk._in_way: (x, y) is in the strip vehicle v covers in the next `seconds`.
bool InWay(const NavVehicle& v, double x, double y, double seconds)
{
    const double dx = x - v.X, dy = y - v.Y;
    const double hx = std::cos(v.H), hy = std::sin(v.H);
    const double along = (dx * hx + dy * hy) * (v.Vel > 0 ? 1.0 : -1.0);
    return -v.Len / 2 < along && along < std::fabs(v.Vel) * seconds + v.Len && std::fabs(-dx * hy + dy * hx) < v.Wid / 2 + 1.2;
}

} // namespace

void NavSim::StepWalker(NavWalker& w, double dt, const WalkerGrid* grid, const VehicleGrid& movers, const NavVehicle* waiting)
{
    const double t = Time;
    if (w.Ticket) {
        if (auto r = TakeResult(w.Ticket)) {
            w.Ticket = 0;
            if (r->Found) {
                w.Path = std::move(r->Points);
                if (w.Path.size() > 1 && PathClosed(w.Cls, { { w.X, w.Y }, w.Path[1] })) {   // off its cell's middle by a wall corner: via the middle
                    const double c = Map.Cell;
                    w.Path.insert(w.Path.begin() + 1, NavXY{ Map.Origin.x + (std::floor((w.X - Map.Origin.x) / c) + 0.5) * c,
                                                             Map.Origin.y + (std::floor((w.Y - Map.Origin.y) / c) + 0.5) * c });
                }
                w.I = 1;
                w.State = w.Run && w.Panic > 0.2 && !w.Then ? "flee" : "walk";
            } else {
                w.State = w.Then && *w.Then == "cover" ? "cover" : "idle";
                w.Path.clear();
            }
        }
    }
    const std::string st = w.State;
    if (st == "down") return;
    if (w.Panic > 0.0 && t >= w.CalmAt) {
        w.Panic = 0.0;
        if (st == "hidden") {
            w.State = "idle";
            Emit("emerged", { { "walker", V(w.Id) }, { "building", V(w.Building) } });
        } else if (st == "cover") w.State = "idle";
    }
    if (st == "hidden") return;
    if (st == "idle" && w.Cls == "civilian" && !m_wander.empty() && w.Rng.random() < dt / 25.0) Walk(w.Id, Kerb(w.Rng.choice(m_wander), w.Rng.random()));
    if (!w.Path.empty() && w.Rev != Revision) {
        std::vector<std::array<double, 4>> areas;
        for (const Dirty& d : m_dirty) if (d.Rev > w.Rev) areas.push_back({ d.X0, d.Y0, d.X1, d.Y1 });
        w.Rev = Revision;
        for (const auto& a : areas)
            if (PathClosed(w.Cls, w.Path, std::max(w.I - 1, 0), &a)) {
                Emit("replan", { { "walker", V(w.Id) }, { "why", V("route closed") } });
                WalkerRequest(w);
                break;
            }
    }
    double vx = 0.0, vy = 0.0;
    if (!w.Path.empty() && (w.State == "walk" || w.State == "flee")) {
        const NavXY target = w.Path[w.I];
        const double d = Hyp(target.x - w.X, target.y - w.Y);
        const bool last = w.I + 1 >= static_cast<int>(w.Path.size());
        if (d < 0.3 || (d < 1.0 && (last || !PathClosed(w.Cls, { { w.X, w.Y }, w.Path[w.I + 1] })))) {   // no cutting a wall corner
            if (!last) ++w.I;
            else {
                w.Path.clear();
                w.State = Has(w.Then) ? *w.Then : "idle";
                if (w.State == "hidden") Emit("sheltered", { { "walker", V(w.Id) }, { "building", V(w.Building) } });
                return;
            }
        } else {
            if (!w.BestI || *w.BestI != w.I || d < w.Best - 0.5) {
                w.BestI = w.I;
                w.Best = d;
                w.BestAt = t;
            } else if (t - w.BestAt > STUCK) {            // pushed onto a wall corner: route again from here
                w.BestI.reset();
                Emit("replan", { { "walker", V(w.Id) }, { "why", V("stuck") } });
                WalkerRequest(w, true);                      // a standing vehicle it cannot pass: round it
                return;
            }
            const double speed = (w.Run ? w.C->Run : w.C->Speed) * w.Pace;
            vx = (target.x - w.X) / d * speed;
            vy = (target.y - w.Y) / d * speed;
        }
    }
    // Keep apart; step out of a vehicle's way.
    if (grid) {
        const int64_t gx = Py::ifloordiv(w.X, 2), gy = Py::ifloordiv(w.Y, 2);
        for (int64_t i = gx - 1; i <= gx + 1; ++i)
            for (int64_t j = gy - 1; j <= gy + 1; ++j) {
                auto it = grid->find(Key(i, j));
                if (it == grid->end()) continue;
                Count.Near += static_cast<int>(it->second.size());
                for (const NavWalker* o : it->second) {
                    const double dx = w.X - o->X, dy = w.Y - o->Y;
                    const double d = Hyp(dx, dy);
                    if (o != &w && 1e-6 < d && d < SEPARATION) {
                        vx += dx / d * (SEPARATION - d) * 3.0;
                        vy += dy / d * (SEPARATION - d) * 3.0;
                    }
                }
            }
    }
    const bool calm = w.Panic < 0.3;
    const int64_t mx = Py::ifloordiv(w.X, 50), my = Py::ifloordiv(w.Y, 50);
    for (int64_t i = mx - 1; i <= mx + 1; ++i)
        for (int64_t j = my - 1; j <= my + 1; ++j) {
            auto it = movers.find(Key(i, j));
            if (it == movers.end()) continue;
            for (const NavVehicle* v : it->second) {
                if (std::fabs(w.X - v->X) > 45 || std::fabs(w.Y - v->Y) > 45) continue;
                if (std::fabs(v->Vel) < 0.5) {              // a standing vehicle is a box to walk round, not traffic to dodge
                    const double sp = Hyp(vx, vy);
                    if (sp < 1e-6 || (w.BestI && t - w.BestAt > 2.0)) continue;   // no headway for 2 s: squeezes past it
                    const double lx = w.X + vx / sp * 0.8 - v->X, ly = w.Y + vy / sp * 0.8 - v->Y;
                    const double c = std::cos(v->H), sn = std::sin(v->H);
                    const double along = lx * c + ly * sn, across = -lx * sn + ly * c;
                    if (std::fabs(along) < v->Len / 2 + 0.4 && std::fabs(across) < v->Wid / 2 + 0.4) {
                        const double here = -(w.X - v->X) * sn + (w.Y - v->Y) * c;
                        const double side = here >= 0 ? 1.0 : -1.0;
                        const double into = (-vx * sn + vy * c) * side;   // the part of its step toward the box's side
                        if (into < 0) { vx -= -sn * into * side; vy -= c * into * side; }
                        if (std::fabs(here) < v->Wid / 2 + 0.4) {          // in front of or behind it: step along its side
                            vx += -sn * side * sp * 0.7;
                            vy += c * side * sp * 0.7;
                        }
                    }
                    continue;
                }
                if (InWay(*v, w.X, w.Y, 2.5)) {
                    const double across = -(w.X - v->X) * std::sin(v->H) + (w.Y - v->Y) * std::cos(v->H);
                    const double side = across >= 0 ? 1.0 : -1.0;
                    const double run = w.C->Run;
                    vx += -std::sin(v->H) * side * run;
                    vy += std::cos(v->H) * side * run;
                } else if (calm && (vx != 0.0 || vy != 0.0) && std::fabs(v->Vel) > 1.0 && !(w.BestI && t - w.BestAt > 2.0)
                           && InWay(*v, w.X + vx, w.Y + vy, 3.0)) {   // traffic really going by, and not for ever
                    vx = vy = 0.0;                   // waits at the kerb for the vehicle to pass
                }
            }
        }
    if (waiting) w.WayFor = waiting->Id;
    const NavVehicle* yield = w.WayFor.empty() ? nullptr : Vehicles.Get(w.WayFor);
    const double along = yield ? (w.X - yield->X) * std::cos(yield->H) + (w.Y - yield->Y) * std::sin(yield->H) : 0.0;
    if (yield && (StaticState(yield->State) || (yield->WaitWalker != w.Id && (along < -yield->Len / 2 || Hyp(w.X - yield->X, w.Y - yield->Y) > 10.0))))
        yield = nullptr;                                   // it has passed (and its driver no longer waits for this walker)
    if (!yield) { w.WayFor.clear(); w.Way.reset(); }
    else if (w.State != "down") {                         // a vehicle waits behind it: out of its lane to the side, else on ahead of it, till it passed
        const double c = std::cos(yield->H), sn = std::sin(yield->H), speed = w.C->Speed;
        const double across = -(w.X - yield->X) * sn + (w.Y - yield->Y) * c;
        if (!w.Way || !Passable(w.Cls, w.X + w.Way->x * 0.5, w.Y + w.Way->y * 0.5)) {   // keeps its way while it is open
            const double side = across >= 0 ? 1.0 : -1.0;
            const NavXY aside{ -sn * side, c * side }, ahead{ c, sn };
            w.Way = Passable(w.Cls, w.X + aside.x * 1.5, w.Y + aside.y * 1.5) ? aside : ahead;
        }
        const bool clear = yield->WaitWalker != w.Id;     // the driver no longer has it in its way: it waits there till it passed
        vx = clear ? 0.0 : w.Way->x * speed;
        vy = clear ? 0.0 : w.Way->y * speed;
    }
    const NavXY was{ w.X, w.Y };
    if (vx != 0.0 || vy != 0.0) {
        const double nx = w.X + vx * dt, ny = w.Y + vy * dt;
        if (Passable(w.Cls, nx, ny) || !Passable(w.Cls, w.X, w.Y)) { w.X = nx; w.Y = ny; }   // pushed into a wall: walks out of it
        else if (Passable(w.Cls, nx, w.Y)) w.X = nx;
        else if (Passable(w.Cls, w.X, ny)) w.Y = ny;
        else if (!w.Path.empty() && w.I < static_cast<int>(w.Path.size())) {   // a route grazing a wall corner: squeeze on to its waypoint
            const NavXY target = w.Path[w.I];
            const double d = Hyp(target.x - w.X, target.y - w.Y), step = w.C->Speed * dt;
            if (d < 3.0 && d > 1e-6 && Passable(w.Cls, target.x, target.y)) { w.X += (target.x - w.X) / d * step; w.Y += (target.y - w.Y) / d * step; }
        }
    }
    if (yield && (w.X != was.x || w.Y != was.y)) w.BestAt = t;   // making way is not being stuck; held fast, it routes again round the vehicles
}

// --- helicopters (air.py) -------------------------------------------------------------------

NavHeli& NavSim::SpawnHeli(const std::string& id, double x, double y, const std::string& cls)
{
    auto h = std::make_shared<NavHeli>();
    h->Id = id; h->Cls = cls; h->C = &NavClassOf(cls); h->X = x; h->Y = y; h->Len = h->C->Length; h->Wid = h->C->Width;
    Helis.Put(h);
    return *h;
}

std::optional<NavXY> NavSim::FlyTo(const std::string& id, double x, double y, bool land)
{
    NavHeli* h = Helis.Get(id);
    if (!h) throw std::out_of_range("nav: no helicopter " + id);
    h->Land = land;
    h->Target = NavXY{ x, y };
    if (land) {
        const auto site = LandingSite(x, y);
        if (!site) {
            Emit("no_landing_site", { { "heli", V(id) }, { "x", R(x) }, { "y", R(y) } });
            h->Land = false;
        } else h->Target = site;
    }
    h->State = "fly";
    return h->Land ? h->Target : std::nullopt;
}

void NavSim::StepAir(double dt)
{
    const std::vector<std::shared_ptr<NavHeli>> helis = Helis.All();
    for (const auto& hp : helis) {
        NavHeli& h = *hp;
        const NavClass& c = *h.C;
        if (h.State == "fly") {
            if (h.Z < CRUISE * 0.5) {
                h.Z = std::min(CRUISE, h.Z + c.Climb * dt);
                continue;
            }
            h.Z = std::min(CRUISE, h.Z + c.Climb * dt);
            const double dx = h.Target->x - h.X, dy = h.Target->y - h.Y;
            const double d = Hyp(dx, dy);
            if (d > 0.5) h.H = std::atan2(dy, dx);
            const double stop = Py::pow(h.Vel, 2) / (2 * c.Accel);
            const double want = d > stop + 5.0 ? c.Speed : std::max(0.0, std::min(c.Speed, std::sqrt(2 * c.Accel * std::max(d - 1.0, 0.0))));
            h.Vel += std::max(-c.Accel * dt, std::min(c.Accel * dt, want - h.Vel));
            const double k = d > 1e-6 ? std::min(1.0, h.Vel * dt / d) : 0.0;
            h.X += dx * k;
            h.Y += dy * k;
            if (d < 2.0 && h.Vel < 1.0) {
                h.Vel = 0.0;
                h.State = h.Land ? "descend" : "hover";
            }
        } else if (h.State == "descend") {
            const double before = h.Z;
            h.Z = std::max(0.0, h.Z - c.Climb * 0.6 * dt);
            if (before > CHECK_BELOW && CHECK_BELOW >= h.Z && !PadOk(h.X, h.Y, c.Pad, NAV_WATER | NAV_STEEP | NAV_BUILDING | NAV_RUBBLE, Occupants())) {
                Emit("landing_aborted", { { "heli", V(h.Id) } });
                FlyTo(h.Id, h.X, h.Y);
            } else if (h.Z <= 0.0) {
                h.State = "landed";
                Emit("landed", { { "heli", V(h.Id) }, { "x", R(h.X, 1) }, { "y", R(h.Y, 1) } });
            }
        }
    }
}

} // namespace IYI
