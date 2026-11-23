// Moved verbatim out of NavSim.cpp.

#include "NavSimInternal.h"

namespace IYI {

namespace {

std::vector<double> Cumulative(const std::vector<NavXY>& pts)
{
    std::vector<double> cum{ 0.0 };
    for (size_t k = 0; k + 1 < pts.size(); ++k) cum.push_back(cum.back() + Hyp(pts[k + 1].x - pts[k].x, pts[k + 1].y - pts[k].y));
    return cum;
}

// drive._approach
double Approach(double a, double b, double step) { return std::fabs(b - a) <= step ? b : a + std::copysign(step, b - a); }


} // namespace

// --- vehicles (drive.py) --------------------------------------------------------------------

NavVehicle& NavSim::SpawnVehicle(const std::string& id, const std::string& cls, double x, double y, double heading, const std::string& side,
                                 std::optional<std::string> doctrine, std::optional<int> priority)
{
    const NavClass& c = NavClassOf(cls);
    auto v = std::make_shared<NavVehicle>();
    v->Rng = Py::Random(std::to_string(Seed) + ":" + id);
    v->Id = id; v->Cls = cls; v->C = &c; v->Side = side;
    v->X = x; v->Y = y; v->H = heading; v->Len = c.Length; v->Wid = c.Width;
    v->Doctrine = Has(doctrine) ? *doctrine : side == "civilian" ? "flee" : side == "medical" || side == "logistics" ? "cautious" : "push";
    v->Priority = priority ? *priority : side == "civilian" ? 0 : 1;
    v->React = side == "civilian" ? v->Rng.uniform(0.6, 1.3) : v->Rng.uniform(0.45, 0.9);
    v->Nerve = v->Rng.random();
    v->Noticed = Time;
    v->Rev = Revision;
    v->Hist.push_back({ Time, x, y, 0.0, heading });
    v->Sampled = Time;
    Vehicles.Put(v);
    return *v;
}

void NavSim::Drive(const std::string& id, NavXY goal, const std::string& mode)
{
    NavVehicle* v = Vehicles.Get(id);
    if (!v) throw std::out_of_range("nav: no vehicle " + id);
    v->Goal = goal;
    v->Prefer = v->Mode = mode;
    Replan(*v, true);
}

void NavSim::Convoy(const std::string& cid, const std::vector<std::string>& ids, NavXY goal, double spacing)
{
    m_convoys[cid] = { ids, goal, spacing };
    for (const std::string& id : ids) {
        NavVehicle* v = Vehicles.Get(id);
        if (!v) throw std::out_of_range("nav: no vehicle " + id);
        v->Convoy = cid;
        v->Spacing = spacing;
        Drive(id, goal);
    }
}

void NavSim::Disable(const std::string& id)
{
    NavVehicle* v = Vehicles.Get(id);
    if (!v) throw std::out_of_range("nav: no vehicle " + id);
    v->State = "disabled";
    v->Vel = 0.0;
    SetObstacle(id, { Box(*v) }, "blocked");
    Emit("disabled", { { "vehicle", V(id) }, { "x", R(v->X, 1) }, { "y", R(v->Y, 1) } });
}

void NavSim::Park(const std::string& id)
{
    NavVehicle* v = Vehicles.Get(id);
    if (!v) throw std::out_of_range("nav: no vehicle " + id);
    v->State = "parked";
    v->Vel = 0.0;
    SetObstacle(id, { Box(*v) }, "blocked");
}

void NavSim::Remove(const std::string& id)
{
    ClearObstacle(id);
    Vehicles.Pop(id);
}

void NavSim::Replan(NavVehicle& v, bool force, const std::string& mode, std::optional<NavXY> goal, bool round)
{
    if (StaticState(v.State) || !v.Goal || (!force && Time < v.ReplanAt)) return;
    v.ReplanAt = Time + 1.5;
    v.Want = mode.empty() ? v.Mode : mode;
    const NavXY to = goal ? *goal : *v.Goal;
    Forget(v.Ticket);
    v.Ticket = Request({ v.X, v.Y }, to, v.Cls, v.Want, v.Side, NavXY{ std::cos(v.H), std::sin(v.H) }, false,
                       (v.Side != "civilian" || round) && v.Want == "free" ? Standing({ v.X, v.Y }, to, &v) : std::vector<int>{},
                       Time < v.AvoidUntil ? v.AvoidNode : -1);
    if (!Has(v.Convoy)) return;
    const std::vector<NavVehicle*> column = Column(*v.Convoy);
    if (column.empty() || column[0] != &v) return;
    for (size_t k = 1; k < column.size(); ++k)               // the column follows its head's decision
        if (column[k]->State != "reverse") {
            column[k]->ReplanAt = 0.0;
            Replan(*column[k], false, mode, goal);
        }
}

// The cells under vehicles standing within 40 m of `at` (not self or its column, not at the goal): a soldier's grid route
// goes round a jam, a stuck walker round the vehicle it cannot pass.
std::vector<int> NavSim::Standing(NavXY at, NavXY goal, const NavVehicle* self) const
{
    std::vector<int> out;
    for (const auto& o : Vehicles.All()) {
        if (o.get() == self || std::fabs(o->Vel) > 0.5 || Hyp(o->X - at.x, o->Y - at.y) > 40.0 || Hyp(o->X - goal.x, o->Y - goal.y) < o->Len) continue;
        if (self && Has(self->Convoy) && o->Convoy == self->Convoy && !StaticState(o->State)) continue;
        const double c = std::cos(o->H), s = std::sin(o->H), hl = o->Len / 2 + 0.5, hw = o->Wid / 2 + 0.5, r = Hyp(hl, hw);
        for (double y = o->Y - r; y <= o->Y + r; y += Map.Cell)
            for (double x = o->X - r; x <= o->X + r; x += Map.Cell) {
                const double dx = x - o->X, dy = y - o->Y;
                if (std::fabs(dx * c + dy * s) <= hl && std::fabs(-dx * s + dy * c) <= hw)
                    if (const auto i = CellIndex(x, y)) out.push_back(*i);
            }
    }
    return out;
}

std::vector<NavVehicle*> NavSim::Column(const std::string& cid)
{
    std::vector<NavVehicle*> out;
    for (const std::string& id : m_convoys.at(cid).Ids)
        if (NavVehicle* v = Vehicles.Get(id); v && !StaticState(v->State)) out.push_back(v);
    return out;
}

void NavSim::TakeRoute(NavVehicle& v)
{
    if (!v.Ticket) return;
    auto r = TakeResult(v.Ticket);
    if (!r) return;
    v.Ticket = 0;
    const bool round = std::exchange(v.Round, false);
    if (!r->Found && round) { Reverse(v, 6.0); return; }  // no way round the deadlock: back off after all, on the old route
    if (!r->Found) {
        if (v.Want == "road") Replan(v, true, "free");      // no road: across open ground (calm civilians keep off walls)
        else { v.State = "stuck"; v.ReplanAt = Time + 5.0; }
        return;
    }
    const std::optional<int> held = v.JGranted ? v.JNode : std::nullopt;
    const double asked = v.JAsked;
    const auto& pts = r->Points;
    if (v.State == "idle" && pts.size() > 1) v.H = std::atan2(pts[1].y - pts[0].y, pts[1].x - pts[0].x);   // a new vehicle starts facing its way
    Forget(v.TailTicket);                                  // a tail asked for an earlier route is not this one's
    v.TailTicket = 0;
    if (r->Mode == "road" && v.Side != "civilian" && v.Goal && r->Points.size() > 1) {   // sent somewhere off the road: drive on to it
        const NavXY end = r->Points.back(), goal = OpenNear(v.Cls, *v.Goal);
        const double d = Hyp(goal.x - end.x, goal.y - end.y);
        if (d > 3.0) {
            if (PathClosed(v.Cls, { end, goal })) v.TailTicket = Request(end, goal, v.Cls, "free", v.Side, std::nullopt, true);   // round things: on the grid
            else for (int k = 1; k <= static_cast<int>(d / 2.0); ++k) {
                r->Points.push_back({ end.x + (goal.x - end.x) * k * 2.0 / d, end.y + (goal.y - end.y) * k * 2.0 / d });
                r->Speeds.push_back(0.5);
            }
            if (!v.TailTicket) {
                r->Points.push_back(goal);
                r->Speeds.push_back(0.5);
            }
        }
    }
    v.Path = std::move(r->Points);
    v.Speeds = std::move(r->Speeds);
    v.Edges = std::move(r->Edges);
    v.Nodes = std::move(r->Nodes);
    v.Mode = r->Mode;
    v.Cum = Cumulative(v.Path);
    v.Seg = 0; v.S = 0.0; v.Lat = 0.0; v.LatGoal = 0.0; v.PassUntil = -1.0; v.JNode.reset();
    v.State = "drive"; v.Blocked = 0.0; v.Ahead.reset(); v.ScanAt = 0.0; v.Rev = Revision;
    if (r->Mode == "free") v.FreeSince = 0.0;
    for (const auto& [s, node] : v.Nodes)                  // a new route through the junction it holds keeps it
        if (held && m_cluster[node] == *held && s - m_box[node] < v.Len) {
            SetJunction(v, s, node);
            v.JGranted = true;
            v.JAsked = asked;
            break;
        }
    Emit("route", { { "vehicle", V(v.Id) }, { "mode", V(r->Mode) }, { "length", R(v.Cum.back(), 1) } });
}

namespace {

// drive._point: ((x, y), (ux, uy)) on the vehicle's path at distance s.
std::pair<NavXY, NavXY> Point(const NavVehicle& v, double s)
{
    const auto& pts = v.Path;
    const auto& cum = v.Cum;
    if (pts.size() < 2) return { { v.X, v.Y }, { std::cos(v.H), std::sin(v.H) } };
    s = std::min(std::max(s, 0.0), cum.back());
    size_t i = cum[v.Seg] <= s ? static_cast<size_t>(v.Seg) : 0;
    while (i + 2 < pts.size() && cum[i + 1] < s) ++i;
    double d = cum[i + 1] - cum[i];
    if (d == 0.0) d = 1.0;
    const double t = (s - cum[i]) / d;
    const NavXY a = pts[i], b = pts[i + 1];
    return { { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t }, { (b.x - a.x) / d, (b.y - a.y) / d } };
}

// drive._project: (s, lateral offset right positive, distance) of (x, y) on the path between v's s and sMax.
std::optional<std::array<double, 3>> Project(const NavVehicle& v, double x, double y, double sMax)
{
    std::optional<std::array<double, 3>> best;
    for (int i = std::max(v.Seg - 1, 0); i + 1 < static_cast<int>(v.Path.size()); ++i) {
        if (v.Cum[i] > sMax) break;
        const NavXY a = v.Path[i], b = v.Path[i + 1];
        const double dx = b.x - a.x, dy = b.y - a.y;
        double d2 = dx * dx + dy * dy;
        if (d2 == 0.0) d2 = 1e-9;
        const double t = std::min(std::max(((x - a.x) * dx + (y - a.y) * dy) / d2, 0.0), 1.0);
        const double px = a.x + dx * t, py = a.y + dy * t;
        const double dist = Hyp(x - px, y - py);
        if (!best || dist < (*best)[2]) {
            const double d = std::sqrt(d2);
            best = std::array<double, 3>{ v.Cum[i] + t * d, ((x - a.x) * -dy + (y - a.y) * dx) / d, dist };
        }
    }
    return best;
}

void Advance(NavVehicle& v)
{
    if (v.Path.size() < 2) return;
    std::optional<double> best;
    int bi = v.Seg;
    double bs = v.S;
    const int end = std::min(v.Seg + 6, static_cast<int>(v.Path.size()) - 1);
    for (int i = std::max(v.Seg - 1, 0); i < end; ++i) {
        const NavXY a = v.Path[i], b = v.Path[i + 1];
        const double dx = b.x - a.x, dy = b.y - a.y;
        double d2 = dx * dx + dy * dy;
        if (d2 == 0.0) d2 = 1e-9;
        const double t = std::min(std::max(((v.X - a.x) * dx + (v.Y - a.y) * dy) / d2, 0.0), 1.0);
        const double dist = Hyp(a.x + dx * t - v.X, a.y + dy * t - v.Y);
        if (!best || dist < *best) { best = dist; bi = i; bs = v.Cum[i] + t * std::sqrt(d2); }
    }
    v.Seg = bi;
    v.S = std::max(bs, v.S - 0.5);
}

// Segments ab and cd cross.
bool SegCross(NavXY a, NavXY b, NavXY c, NavXY d)
{
    const auto side = [](NavXY p, NavXY q, NavXY r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
    return side(a, b, c) * side(a, b, d) < 0 && side(c, d, a) * side(c, d, b) < 0;
}

// Polylines a and b come closer than d somewhere.
bool PolyNear(const std::vector<NavXY>& a, const std::vector<NavXY>& b, double d)
{
    for (size_t i = 0; i + 1 < a.size(); ++i)
        for (size_t j = 0; j + 1 < b.size(); ++j)
            if (SegCross(a[i], a[i + 1], b[j], b[j + 1]) || SegDist(a[i].x, a[i].y, b[j], b[j + 1]) < d || SegDist(a[i + 1].x, a[i + 1].y, b[j], b[j + 1]) < d
                || SegDist(b[j].x, b[j].y, a[i], a[i + 1]) < d || SegDist(b[j + 1].x, b[j + 1].y, a[i], a[i + 1]) < d) return true;
    return false;
}

// drive._seen: o's (x, y, speed, heading) as it was at time t.
const NavSample& Seen(const NavVehicle& o, double t)
{
    for (auto it = o.Hist.rbegin(); it != o.Hist.rend(); ++it) if (it->T <= t) return *it;
    return o.Hist.front();
}

// drive._extent: (length along, width across) direction (ux, uy) of o's box.
std::pair<double, double> Extent(const NavVehicle& o, double ux, double uy)
{
    const double c = std::fabs(std::cos(o.H) * ux + std::sin(o.H) * uy), s = std::fabs(-std::sin(o.H) * ux + std::cos(o.H) * uy);
    return { o.Len * c + o.Wid * s, o.Len * s + o.Wid * c };
}

double CurveSpeed(const NavVehicle& v, double hurry)
{
    const double look = 8.0 + std::fabs(v.Vel) * 2.0;
    const NavXY a = Point(v, v.S).second, b = Point(v, v.S + look).second;
    const double turn = std::acos(std::max(-1.0, std::min(1.0, a.x * b.x + a.y * b.y)));
    if (turn < 0.05) return 99.0;
    return std::sqrt((3.0 + 2.5 * hurry) * look / turn);
}

void Move(NavVehicle& v, double dt, bool reverse)
{
    if (v.Path.size() < 2) return;
    const double speed = std::fabs(v.Vel);
    const auto [p, u] = Point(v, v.S + std::max(3.0, speed * 0.9 + 2.0));
    const double tx = p.x - u.y * v.Lat, ty = p.y + u.x * v.Lat;
    double want = std::atan2(ty - v.Y, tx - v.X);
    if (reverse) want += K_PI;
    const double diff = Py::mod(want - v.H + K_PI, 2 * K_PI) - K_PI;
    double rate = v.C->Turn != 0.0 ? speed / v.C->Turn : 0.8;
    if (speed < 1.5) rate = std::max(rate, 0.45);             // a slow multi-point turn, drawn as one
    v.H += std::max(-rate * dt, std::min(rate * dt, diff));
    if (std::fabs(diff) > 2.0 && !reverse) v.Vel = std::min(v.Vel, 1.5);   // facing away from the route: crawl while turning
    v.X += std::cos(v.H) * v.Vel * dt;
    v.Y += std::sin(v.H) * v.Vel * dt;
}

} // namespace

std::optional<NavSim::Lead> NavSim::Leader(NavVehicle& v, const VehicleGrid& grid, const WalkerGrid& walkers, double delay)
{
    const double reach = std::min(LOOK, 12.0 + std::fabs(v.Vel) * 3.5);
    const double sMax = v.S + reach;
    std::optional<Lead> best;
    const double tSeen = Time - delay;
    const int64_t gx = Py::ifloordiv(v.X, 20), gy = Py::ifloordiv(v.Y, 20), r = static_cast<int64_t>(reach + 30) / 20 + 1;
    const double hx = std::cos(v.H), hy = std::sin(v.H);
    for (int64_t i = gx - r; i <= gx + r; ++i)
        for (int64_t j = gy - r; j <= gy + r; ++j) {
            auto it = grid.find(Key(i, j));
            if (it == grid.end()) continue;
            for (NavVehicle* o : it->second) {
                const double dx = o->X - v.X, dy = o->Y - v.Y;
                if (o == &v || std::fabs(dx) > reach + 30 || std::fabs(dy) > reach + 30 || dx * hx + dy * hy < -25.0) continue;
                const NavSample& seen = Seen(*o, tSeen);
                const double ox = seen.X, oy = seen.Y, ov = seen.Vel, oh = seen.H;
                if (Hyp(ox - v.X, oy - v.Y) > reach + 10) continue;
                // where it is, and where it will be in a second (crossing traffic about to cut in; standing: once)
                const NavXY spots[2] = { { ox, oy }, { ox + std::cos(oh) * ov, oy + std::sin(oh) * ov } };
                for (int k = 0; k < (ov != 0.0 ? 2 : 1); ++k) {
                    const auto p = Project(v, spots[k].x, spots[k].y, sMax);
                    if (!p || (*p)[0] <= v.S) continue;
                    const NavXY u = Point(v, (*p)[0]).second;
                    const auto [along, across] = Extent(*o, u.x, u.y);
                    const bool oncoming = std::cos(oh - v.H) < -0.5;          // in its own lane it passes; only a real overlap stops
                    if (std::fabs((*p)[1] - v.Lat) > (v.Wid + across) / 2 + (oncoming ? -0.1 : 0.3)) continue;
                    const double gap = (*p)[0] - v.S - (v.Len + along) / 2;
                    const double vl = ov * (std::cos(oh) * u.x + std::sin(oh) * u.y);
                    if (!best || gap < best->Gap) best = Lead{ gap, k ? 0.0 : vl, o, nullptr, (*p)[0] };
                }
            }
        }
    const int64_t wx = Py::ifloordiv(v.X, 100), wy = Py::ifloordiv(v.Y, 100);
    for (int64_t i = wx - 1; i <= wx + 1; ++i)
        for (int64_t j = wy - 1; j <= wy + 1; ++j) {
            auto it = walkers.find(Key(i, j));
            if (it == walkers.end()) continue;
            for (NavWalker* w : it->second) {
                if (w->State == "hidden" || w->State == "down" || Hyp(w->X - v.X, w->Y - v.Y) > reach + 5) continue;
                const auto p = Project(v, w->X, w->Y, sMax);
                if (p && (*p)[0] > v.S && std::fabs((*p)[1] - v.Lat) < v.Wid / 2 + 1.0) {
                    const double gap = (*p)[0] - v.S - v.Len / 2 - 0.4;
                    if (!best || gap < best->Gap) best = Lead{ gap, 0.0, nullptr, w, (*p)[0] };
                }
            }
        }
    if (v.Ahead && *v.Ahead > v.S) {
        const double gap = *v.Ahead - v.S - v.Len / 2;
        if (!best || gap < best->Gap) best = Lead{ gap, 0.0, nullptr, nullptr, *v.Ahead };
    }
    return best;
}

std::optional<double> NavSim::ScanGround(NavVehicle& v)
{
    const double reach = std::min(LOOK, 15.0 + std::fabs(v.Vel) * 4.0);
    double s = v.S + v.Len / 2;
    const double end = std::min(v.S + reach, v.Cum.empty() ? 0.0 : v.Cum.back());
    const bool road = v.Mode == "road";
    while (s < end) {
        const auto [p, u] = Point(v, s);
        const double lat = s < v.PassUntil ? v.LatGoal : v.Lat;
        const double x = p.x - u.y * lat, y = p.y + u.x * lat;
        bool closed;
        if (road) {
            const auto i = CellIndex(x, y);
            closed = i && (Extra[*i] & (1 << BLOCKED | 1 << DEBRIS) || (Base[*i] & NAV_WATER && !(Base[*i] & NAV_BRIDGE)));   // a lane past the map's edge is open
        } else closed = !Passable(v.Cls, x, y);
        if (closed) return s;
        s += 2.0;
    }
    return std::nullopt;
}

bool NavSim::StripClear(NavVehicle& v, double s0, double s1, double lat, const VehicleGrid& grid, bool careful)
{
    for (double s = s0; s <= s1; s += 1.5) {
        const auto [p, u] = Point(v, s);
        for (double k : { lat - v.Wid / 2 - 0.5, lat, lat + v.Wid / 2 + 0.5 })
            if (!Passable(v.Cls, p.x - u.y * k, p.y + u.x * k)) return false;
        if (careful) {
            const double cx = p.x - u.y * lat, cy = p.y + u.x * lat;
            const int64_t gx = Py::ifloordiv(cx, 20), gy = Py::ifloordiv(cy, 20);
            for (int64_t i = -1; i <= 1; ++i)
                for (int64_t j = -1; j <= 1; ++j) {
                    auto it = grid.find(Key(gx + i, gy + j));
                    if (it == grid.end()) continue;
                    for (const NavVehicle* o : it->second)
                        if (o != &v && Hyp(o->X - cx, o->Y - cy) < (v.Wid + std::max(o->Wid, Extent(*o, u.x, u.y).second)) / 2 + 0.4) return false;
                }
        }
    }
    return true;
}

std::optional<std::pair<double, double>> NavSim::Swerve(NavVehicle& v, const Lead& lead, const VehicleGrid& grid)
{
    double along = 4.0;
    if (lead.V) {
        const NavXY u = Point(v, lead.S).second;
        along = Extent(*lead.V, u.x, u.y).first;
    }
    const double s0 = lead.S - along / 2 - 4.0, s1 = lead.S + along / 2 + 6.0;
    const bool careful = v.Alarm < 0.5 || v.Nerve < 0.5;      // a hurried bold driver does not look for oncoming traffic
    for (int k : { 1, -1, 2, -2, 3, -3, 4, -4 }) {   // right side first, at most 3 m off its lane
        const double off = k * 0.75;
        if (StripClear(v, s0, s1, off, grid, careful)) return std::make_pair(off, s1);
    }
    return std::nullopt;
}

void NavSim::StepVehicles(double dt, const NavXY* focus)
{
    const std::vector<std::shared_ptr<NavVehicle>> keep = Vehicles.All();
    const std::vector<std::shared_ptr<NavWalker>> keepWalkers = Walkers.All();   // walkers spawned or replaced meanwhile stay valid
    std::vector<NavVehicle*> vehicles;
    for (const auto& v : keep) vehicles.push_back(v.get());
    VehicleGrid grid;
    for (NavVehicle* v : vehicles) grid[Key(Py::ifloordiv(v->X, 20), Py::ifloordiv(v->Y, 20))].push_back(v);
    WalkerGrid walkers;
    for (const auto& w : keepWalkers) walkers[Key(Py::ifloordiv(w->X, 100), Py::ifloordiv(w->Y, 100))].push_back(w.get());
    NoticeVehicles(vehicles);
    Count.Vehicles = 0;
    for (NavVehicle* v : vehicles) {
        double vdt;
        if (focus && Hyp(v->X - focus->x, v->Y - focus->y) > DRIVE_FAR && v->Alarm < 0.1) {
            v->Acc += dt;
            v->Stride = (v->Stride + 1) % FAR_STRIDE;
            if (v->Stride) continue;
            vdt = v->Acc;
        } else vdt = dt + v->Acc;
        v->Acc = 0.0;
        ++Count.Vehicles;
        DriveOne(*v, vdt, grid, walkers);
    }
    Collide(vehicles, walkers);
    if (Time >= m_jamAt) Deadlocks(vehicles);   // every 2 s, with Jams
    Jams(vehicles);
    for (NavVehicle* v : vehicles)
        if (Time - v->Sampled >= SAMPLE) {
            v->Hist.push_back({ Time, v->X, v->Y, v->Vel, v->H });
            if (v->Hist.size() > 40) v->Hist.pop_front();
            v->Sampled = Time;
        }
}

void NavSim::NoticeVehicles(const std::vector<NavVehicle*>& vehicles)
{
    for (const Alarm& a : m_alarms)
        for (NavVehicle* v : vehicles) {
            if (StaticState(v->State) || v->Noticed >= a.Time) continue;
            const double d = Hyp(v->X - a.X, v->Y - a.Y);
            const double hear = a.Radius * 6;
            if (d < hear && Time - a.Time >= v->React) {
                v->Noticed = a.Time;
                const double level = a.Severity * (1.0 - d / hear) * (v->Side == "civilian" ? 1.3 : 1.0);
                v->Alarm = std::min(1.0, v->Alarm + level);
                v->Danger = NavXY{ a.X, a.Y };
                Emit("alarmed", { { "vehicle", V(v->Id) }, { "level", R(v->Alarm, 2) } });
                if (v->Side == "civilian" && d < a.Radius * 2 && v->Alarm > 0.3) Flee(*v);
            }
        }
}

void NavSim::DriveOne(NavVehicle& v, double dt, const VehicleGrid& grid, const WalkerGrid& walkers)
{
    TakeRoute(v);
    if (v.TailTicket) {
        if (auto tail = TakeResult(v.TailTicket)) {         // the way on from the road's end, if there is one
            v.TailTicket = 0;
            if (tail->Found && !v.Path.empty() && v.Mode == "road" && v.State == "drive")
                for (const NavXY& p : tail->Points)
                    if (Hyp(p.x - v.Path.back().x, p.y - v.Path.back().y) > 0.3) {
                        v.Path.push_back(p);
                        v.Speeds.push_back(0.5);
                    }
            v.Cum = Cumulative(v.Path);
        }
    }
    if (StaticState(v.State)) return;
    const NavClass& c = *v.C;
    const double t = Time;
    v.Alarm = std::max(0.0, v.Alarm - dt / 90.0);
    const double hurry = v.Alarm;
    if (v.State == "stuck" && t >= v.ReplanAt) Replan(v, true, v.Prefer);
    if ((v.State != "drive" && v.State != "reverse") || v.Path.size() < 2) {
        v.Vel = Approach(v.Vel, 0.0, c.Brake * dt);
        Move(v, dt, false);
        return;
    }
    if (v.Rev != Revision) CheckRoute(v);
    if (t >= v.ScanAt) {
        v.ScanAt = t + SCAN_EVERY;
        v.Ahead = ScanGround(v);
    }
    const bool reverse = v.State == "reverse";
    const double delay = v.React * (reverse ? 1.0 + 2.5 * hurry : 1.0);
    std::optional<Lead> lead = Leader(v, grid, walkers, delay);
    v.WaitFor.clear();

    // A stopped thing ahead: steer round it if there is room, else stop behind it.
    if (lead && !reverse && lead->Gap < 35.0 && std::fabs(lead->Vl) < 0.5 && v.S > v.PassUntil && t >= v.SwerveAt) {
        const NavVehicle* thing = lead->V;
        const bool mate = thing && Has(thing->Convoy) && thing->Convoy == v.Convoy && !StaticState(thing->State);
        const bool queue = thing && (thing->State == "drive" || thing->State == "reverse");   // standing (arrived, idle, stuck): overtake it
        if (!mate && (!queue || hurry > 0.5 || v.Queued > 20.0 || thing->Priority < v.Priority)) {   // a soldier overtakes a civilian queue
            const auto sw = Swerve(v, *lead, grid);
            v.SwerveAt = t + (sw ? 0.0 : 0.5);
            if (sw) {
                v.LatGoal = sw->first;
                v.PassUntil = sw->second;
                lead = Leader(v, grid, walkers, delay);
            }
        }
    }
    const bool giving = v.Priority == 0 && hurry < 0.5 && GiveWay(v, grid);
    if (v.S > v.PassUntil) v.LatGoal = 0.0;
    v.Lat = Approach(v.Lat, v.LatGoal, (1.2 + 2.0 * hurry) * dt);

    // Desired speed: the road, curves, hurry; the intelligent driver model toward the leader.
    double v0;
    if (reverse) v0 = 2.5 + 3.5 * hurry;
    else {
        const size_t i = std::min(static_cast<size_t>(v.Seg), v.Speeds.size() - 1);
        const double ground = v.Mode == "road" ? v.Speeds[i] : GroundSpeed(v);
        v0 = c.Speed * ground * (1.0 + 0.3 * hurry);
        v0 = std::min(v0, CurveSpeed(v, hurry));
        if (v.S < v.PassUntil) v0 = std::min(v0, 4.0 + 6.0 * hurry);
        v0 = std::min(v0, Cohesion(v, v0));
        if (giving) v0 = 0.0;                                   // pulled over: the soldier goes round it
    }
    const double left = v.Cum.back() - v.S;
    double speed = std::fabs(v.Vel);
    const double aMax = c.Accel * (1.0 + 0.6 * hurry), bComf = 3.0;
    const bool walker = lead && lead->W;
    const bool mate = lead && lead->V && Has(v.Convoy) && lead->V->Convoy == v.Convoy && !StaticState(lead->V->State);
    const double gapMin = mate ? v.Spacing : (walker ? 0.8 : 2.5);       // a knocked-out mate is an obstacle
    const double headway = (Has(v.Convoy) ? 1.0 : 1.4) * (1.0 - 0.45 * hurry);
    double acc = aMax * (1.0 - Py::pow(speed / std::max(v0, 0.1), 4));
    std::vector<std::pair<double, double>> targets;                        // (gap, leader's speed)
    if (lead) targets.push_back({ lead->Gap, lead->Vl });
    targets.push_back({ left + 1.5, 0.0 });                                // stop with its middle at the end (the IDM keeps 2.5 m)
    if (const auto stop = JunctionGap(v, grid)) targets.push_back({ *stop, 0.0 });
    if (!reverse && v.State == "reverse") return;                          // it just began to back off (give way)
    for (const auto& [gap, vl] : targets) {
        const double sStar = gapMin + std::max(0.0, speed * headway + speed * (speed - vl) / (2 * std::sqrt(aMax * bComf)));
        acc = std::min(acc, aMax * (1.0 - Py::pow(speed / std::max(v0, 0.1), 4) - Py::pow(sStar / std::max(gap, 0.1), 2)));
    }
    acc = std::max(-c.Brake, acc);
    speed = std::max(0.0, speed + acc * dt);
    v.Vel = reverse ? -speed : speed;

    const bool blocked = lead && lead->Gap < (lead->V || lead->W ? 6.0 : 12.0) && speed < 0.3;   // shut ground: it stops farther off
    v.Blocked = blocked && std::fabs(lead->Vl) < 0.3 && ((!lead->V && !lead->W) || (lead->V ? StaticState(lead->V->State) : lead->W && StaticState(lead->W->State)))
        ? v.Blocked + dt : 0.0;
    if (blocked && v.Blocked == dt) v.BlockedBy = lead->V ? lead->V->Id : lead->W ? lead->W->Id : "ground";
    v.Queued = blocked ? v.Queued + dt : 0.0;
    if (lead && lead->V && lead->Gap < 8.0) v.WaitFor = lead->V->Id;
    v.WaitWalker = lead && lead->W && lead->Gap < 8.0 ? lead->W->Id : std::string();
    v.Still = speed < 0.3 ? v.Still + dt : 0.0;
    Move(v, dt, reverse);
    Advance(v);

    if (reverse) {
        NavVehicle* back = lead && lead->V && lead->Gap < 3.0 ? lead->V : nullptr;   // the car it backs into waits for it: it makes room
        if (back && back->State == "drive" && back->Still > 1.0 && back->Yielded < Time - 5.0 && v.S < v.ReverseTo - 1.0) {
            back->Yielded = Time;
            Reverse(*back, v.ReverseTo - v.S + 2.0);
            v.Still = 0.0;
        }
        const double wait = lead && lead->V && lead->V->State == "reverse" ? 3.0 : 1.0;   // the car behind is making room
        if (v.S >= v.ReverseTo || (blocked && v.Blocked > 0.5) || v.Blocked > 1.0 || v.Still > 2.0 * wait || Time - v.AnchorAt > 5.0 * wait) {     // backed off, or cannot back further
            v.State = "drive";
            v.Path.clear();
            Replan(v, true, v.Doctrine == "push" ? "free" : "road", v.FleeGoal);
        }
        return;
    }
    if (left < 2.0 && speed < 0.5 && !v.TailTicket) {   // the end target stops it about 1 m short of the goal
        if (v.Side != "civilian" && v.Mode == "road" && v.Goal && Hyp(v.X - v.Goal->x, v.Y - v.Goal->y) > 3.0) {
            Replan(v, false, "free");                     // short of the clicked point (a lane, a folded tail): the last bit on the grid
            return;
        }
        v.State = "arrived";
        Emit("arrived", { { "vehicle", V(v.Id) } });
        return;
    }
    if (v.Mode == "free") {
        v.FreeSince += speed * dt;
        if (v.Prefer == "road" && v.FreeSince > 120.0 && hurry < 0.3) {
            v.FreeSince = -1e9;
            Replan(v, true, "road");
        }
    }
    if (v.Blocked > (hurry > 0.3 ? 1.0 : 5.0) || v.Queued > (Has(v.Convoy) ? 1e9 : v.Side == "civilian" ? 40.0 : 10.0)) {   // a soldier waits 10 s in a queue, then goes round
        v.Blocked = v.Queued = 0.0;
        if (hurry < 0.3 && v.Doctrine == "flee") {           // calm: another road, never reversing into the queue behind
            Emit("blocked", { { "vehicle", V(v.Id) }, { "doctrine", V("calm") }, { "by", V(v.BlockedBy) } });
            if (v.BlockedBy == "ground" && v.Ahead && *v.Ahead - v.S - v.Len / 2 < 0.5) Reverse(v, 6.0);   // its nose on a wall: back out first
            else Replan(v, true, v.BlockedBy == "ground" ? "free" : "road");   // shut ground (a ditch between it and the road): on the grid
        } else Decide(v);
    }
}

bool NavSim::GiveWay(NavVehicle& v, const VehicleGrid& grid)
{
    // A higher priority vehicle close behind on its way, or waiting for this one: pull over to the right and nearly stop.
    const int64_t gx = Py::ifloordiv(v.X, 20), gy = Py::ifloordiv(v.Y, 20);
    for (int64_t i = gx - 2; i <= gx + 2; ++i)
        for (int64_t j = gy - 2; j <= gy + 2; ++j) {
            auto it = grid.find(Key(i, j));
            if (it == grid.end()) continue;
            for (const NavVehicle* o : it->second) {
                if (o->Priority <= v.Priority || o->State != "drive") continue;
                const double dx = v.X - o->X, dy = v.Y - o->Y;
                const double along = dx * std::cos(o->H) + dy * std::sin(o->H), across = -dx * std::sin(o->H) + dy * std::cos(o->H);
                const bool behind = along > 0.0 && along < 30.0 && std::fabs(across) < 3.0 && std::cos(o->H - v.H) > 0.5 && o->Still < 3.0;   // standing (at a junction): not on its way
                if (!behind && (o->WaitFor != v.Id || v.JGranted)) continue;   // holding the junction it waits for: clears it instead
                for (double off : { 1.0, 0.6 })
                    if (StripClear(v, v.S, v.S + v.Len + 4.0, off, grid, false)) {
                        v.LatGoal = off;
                        v.PassUntil = v.S + 6.0;
                        return true;
                    }
                return false;                                    // no room to pull over: it drives on ahead of it
            }
        }
    return false;
}

double NavSim::GroundSpeed(const NavVehicle& v) const
{
    const auto i = CellIndex(v.X, v.Y);
    const bool onRoad = i && Base[*i] & (NAV_ROAD | NAV_BRIDGE);
    if (onRoad) return 0.75;
    const std::string& kind = v.C->Kind;
    return kind == "wheeled" ? 0.4 : kind == "tracked" ? 0.65 : 1.0;
}

void NavSim::Decide(NavVehicle& v)
{
    Emit("blocked", { { "vehicle", V(v.Id) }, { "doctrine", V(v.Doctrine) }, { "by", V(v.BlockedBy) } });
    if (v.Doctrine == "push" && v.Mode == "free" && v.BlockedBy == "ground") Replan(v, true, "road");   // the grid way is shut: by road
    else if (v.Doctrine == "push") {
        if (v.Alarm > 0.5 && v.Rng.random() < 0.25 * (1.0 - v.Nerve)) Reverse(v, 12.0 + 10.0 * v.Rng.random());   // backs off first, hastily
        else Replan(v, true, "free");                                                                                // across country round it
    } else if (v.Doctrine == "cautious") Replan(v, true, "road");
    else Flee(v);
}

void NavSim::Flee(NavVehicle& v)
{
    const NavXY danger = v.Danger ? *v.Danger : NavXY{ v.X - std::cos(v.H), v.Y - std::sin(v.H) };
    const double dx = v.X - danger.x, dy = v.Y - danger.y;
    double d = Hyp(dx, dy);
    if (d == 0.0) d = 1.0;
    v.FleeGoal = NavXY{ v.X + dx / d * 500.0, v.Y + dy / d * 500.0 };
    if (v.Alarm > 0.6 && v.Rng.random() < 0.3) {
        Park(v.Id);
        const int n = 1 + static_cast<int>(v.Rng.random() * 2);
        for (int k = 0; k < n; ++k) {
            NavWalker& w = SpawnWalker(v.Id + ".p" + std::to_string(k), "civilian", v.X - std::sin(v.H) * (1.5 + k), v.Y + std::cos(v.H) * (1.5 + k));
            AlarmWalker(w, danger, 1.0);
        }
        Emit("abandoned", { { "vehicle", V(v.Id) } });
        return;
    }
    if (v.Rng.random() < 0.4 + 0.4 * v.Alarm) Reverse(v, 15.0 + 15.0 * v.Rng.random());
    else {
        v.Path.clear();
        Replan(v, true, "road", v.FleeGoal);
    }
}

void NavSim::Reverse(NavVehicle& v, double distance)
{
    std::vector<NavXY> trail{ { v.X, v.Y } };
    for (auto it = v.Hist.rbegin(); it != v.Hist.rend(); ++it)
        if (Hyp(it->X - trail.back().x, it->Y - trail.back().y) > 1.0) trail.push_back({ it->X, it->Y });
    if (trail.size() < 3) {
        const double bx = -std::cos(v.H), by = -std::sin(v.H);
        trail.clear();
        for (int k = 0; k < static_cast<int>(distance) + 2; k += 2) trail.push_back({ v.X + bx * k, v.Y + by * k });
    }
    v.Path = trail;
    v.Speeds.assign(trail.size(), 1.0);
    v.Edges.clear();
    v.Nodes.clear();
    v.Cum = Cumulative(trail);
    v.Seg = 0; v.S = 0.0; v.Lat = 0.0; v.LatGoal = 0.0; v.PassUntil = -1.0; v.JNode.reset(); v.Ahead.reset(); v.Still = v.Blocked = 0.0;
    Forget(v.TailTicket);
    v.TailTicket = 0;
    v.ReverseTo = std::min(distance, v.Cum.back() - 1.0);
    v.State = "reverse";
    v.Anchor = { v.X, v.Y };                               // its headway backing off (a reverse into a wall ends)
    v.AnchorAt = Time;
    Emit("reversing", { { "vehicle", V(v.Id) }, { "distance", R(v.ReverseTo, 1) } });
}

void NavSim::CheckRoute(NavVehicle& v)
{
    const double w = v.C->Width + 0.6;
    std::vector<std::array<double, 4>> areas;
    for (const Dirty& d : m_dirty) if (d.Rev > v.Rev) areas.push_back({ d.X0, d.Y0, d.X1, d.Y1 });
    v.Rev = Revision;
    if (v.State == "reverse" || areas.empty()) return;
    bool bad = false;
    if (v.Mode == "road") {
        for (size_t k = 1; k + 1 < v.Edges.size() && !bad; ++k) bad = !Edges[v.Edges[k]].Open || Edges[v.Edges[k]].Clear < w;
    } else {
        for (size_t k = 0; k < areas.size() && !bad; ++k) bad = PathClosed(v.Cls, v.Path, v.Seg + 1, &areas[k]).has_value();
    }
    if (bad) {
        Emit("replan", { { "vehicle", V(v.Id) }, { "why", V("route closed") } });
        Replan(v, true);
    }
}

double NavSim::Cohesion(NavVehicle& v, double v0)
{
    if (!Has(v.Convoy)) return v0;
    const std::vector<NavVehicle*> alive = Column(*v.Convoy);
    if (alive.empty() || alive[0] != &v || alive.size() < 2) return v0;
    const double gap = Hyp(alive[1]->X - v.X, alive[1]->Y - v.Y);
    return v0 * (gap > 2.5 * v.Spacing ? 0.5 : 1.0);
}

std::optional<double> NavSim::JunctionGap(NavVehicle& v, const VehicleGrid& grid)
{
    const double front = v.S + v.Len / 2;
    if (v.JNode && v.S - v.Len / 2 > v.JOut) {                             // its rear is out of the box: the junction is free again
        v.JNode.reset();
        v.Setback = { -1, 0.0 };
    }
    if (!v.JNode) {
        const double reach = std::max(25.0, v.Vel * v.Vel / 3.0 + 12.0);   // a comfortable stop (1.5 m/s2) and a margin
        for (const auto& [s, node] : v.Nodes) {
            const double r = m_box[node];
            if (r == 0.0 || s + r < v.S - v.Len / 2) continue;              // a bend in the road, or behind (a junction left already)
            if (s - r - front > reach) break;
            SetJunction(v, s, node);
            v.JAsked = Time;
            v.JGranted = false;
            break;
        }
    }
    if (!v.JNode || v.JGranted) return std::nullopt;
    const NavXY at = Map.Nodes[*v.JNode];
    const bool inside = front > v.JIn + 0.5 || Hyp(v.X - at.x, v.Y - at.y) < m_box[*v.JNode];   // a car waiting at the box is not in it
    const NavVehicle* blocker = JunctionBlocker(v, grid, !inside);          // one already in it goes as soon as its way is free
    if (!blocker) {
        v.JGranted = true;
        return std::nullopt;
    }
    v.WaitFor = blocker->Id;
    if (!inside && blocker->JGranted && blocker->WaitFor == v.Id && blocker->Still > 1.0 && v.Yielded < Time - 5.0) {   // in the way of the one going
        const double back = std::min(15.0, (v.Setback.first == *v.JNode ? v.Setback.second : 0.0) + 4.0);
        v.Setback = { *v.JNode, back };
        if (front > v.JIn - back) {
            Emit("give_way", { { "vehicle", V(v.Id) }, { "to", V(blocker->Id) } });
            v.Yielded = Time;                                                      // once in 5 s: a car boxed in behind cannot keep trying
            Reverse(v, front - (v.JIn - back) + 1.0);
            return std::nullopt;
        }
    }
    const double setback = v.Setback.first == *v.JNode ? v.Setback.second : 0.0;
    return inside ? 0.1 : std::max(0.1, v.JIn - setback - front + 2.5);    // the IDM keeps 2.5 m: it stops at the box
}

void NavSim::SetJunction(NavVehicle& v, double s, int node)
{
    v.JNode = m_cluster[node];                             // the junction: the box of every node of it the path passes in a row
    v.JIn = s - m_box[node];
    v.JOut = s + m_box[node];
    bool after = false;
    for (const auto& [t, n] : v.Nodes) {
        if (after && m_cluster[n] != *v.JNode) break;
        if (after) v.JOut = std::max(v.JOut, t + m_box[n]);
        after = after || (n == node && t == s);
    }
    v.JPoly = Sub(v.Path, std::max(0.0, v.JIn - 1.0), std::min(v.Cum.back(), v.JOut));
}

const NavVehicle* NavSim::JunctionBlocker(const NavVehicle& v, const VehicleGrid& grid, bool exitRoom) const
{
    const NavXY at = Map.Nodes[*v.JNode];
    const NavXY in = v.JPoly.front();
    const int64_t gx = Py::ifloordiv(at.x, 20), gy = Py::ifloordiv(at.y, 20);
    for (int64_t i = gx - 2; i <= gx + 2; ++i)
        for (int64_t j = gy - 2; j <= gy + 2; ++j) {
            auto it = grid.find(Key(i, j));
            if (it == grid.end()) continue;
            for (const NavVehicle* o : it->second) {
                if (o == &v || o->JNode != v.JNode || o->JPoly.empty() || o->State != "drive" || (Has(v.Convoy) && o->Convoy == v.Convoy)) continue;
                if (Hyp(o->JPoly.front().x - in.x, o->JPoly.front().y - in.y) < 1.5) continue;   // the same lane in: following handles it
                if (!PolyNear(v.JPoly, o->JPoly, (v.Wid + o->Wid) / 2 + 0.5)) continue;          // their ways through do not cross
                const bool first = o->Priority != v.Priority ? o->Priority > v.Priority : o->JAsked != v.JAsked ? o->JAsked < v.JAsked : o->Id < v.Id;
                if (o->JGranted || first) return o;
            }
        }
    if (!exitRoom) return nullptr;
    // Keep the box clear: room for the whole vehicle past the exit, nothing slow in it.
    const double s0 = v.JOut - 1.0, s1 = std::min(v.Cum.back(), v.JOut + v.Len + 2.5);
    const NavXY out = Point(v, v.JOut).first;
    const int64_t ox = Py::ifloordiv(out.x, 20), oy = Py::ifloordiv(out.y, 20);
    for (int64_t i = ox - 1; i <= ox + 1; ++i)
        for (int64_t j = oy - 1; j <= oy + 1; ++j) {
            auto it = grid.find(Key(i, j));
            if (it == grid.end()) continue;
            for (const NavVehicle* o : it->second) {
                if (o == &v || std::fabs(o->Vel) > 3.0 || Hyp(o->X - out.x, o->Y - out.y) > v.Len + o->Len + 6.0) continue;
                if (o->JNode == v.JNode && !o->JGranted) continue;   // waiting for this junction too, outside its box: not the exit's queue
                const auto p = Project(v, o->X, o->Y, s1 + o->Len);
                if (p && (*p)[0] + o->Len / 2 > s0 && (*p)[0] - o->Len / 2 < s1 && std::fabs((*p)[1]) < (v.Wid + o->Wid) / 2) return o;
            }
        }
    return nullptr;
}

void NavSim::Deadlocks(const std::vector<NavVehicle*>& vehicles)
{
    // Vehicles standing 4 s that wait for each other in a ring (face to face, or round a junction) never move
    // on their own: the one of lowest priority (then the last asked) backs off a few metres and routes again.
    for (NavVehicle* v : vehicles) {
        if (Hyp(v->X - v->Anchor.x, v->Y - v->Anchor.y) > 3.0 || (v->State != "drive" && v->State != "reverse")) {
            v->Anchor = { v->X, v->Y };
            v->AnchorAt = Time;
        }
    }
    std::unordered_map<std::string, NavVehicle*> waiting;
    for (NavVehicle* v : vehicles) {
        if (v->State == "drive" && (v->Still > 4.0 || Time - v->AnchorAt > 20.0) && !v->WaitFor.empty()) waiting[v->Id] = v;   // or inching to and fro
        if (v->State == "drive" && (v->Still > 15.0 || Time - v->AnchorAt > 20.0) && !v->WaitWalker.empty() && v->Yielded < Time - 15.0) {   // a walker boxed in by it and others
            Emit("deadlock", { { "vehicle", V(v->Id) }, { "ring", R(1.0) } });
            v->Yielded = Time;
            Reverse(*v, 6.0);
        }
    }
    std::set<std::string> done;
    for (NavVehicle* v : vehicles) {
        if (!waiting.count(v->Id) || done.count(v->Id)) continue;
        std::vector<NavVehicle*> chain{ v };
        for (int k = 0; k < 16; ++k) {
            auto it = waiting.find(chain.back()->WaitFor);
            if (it == waiting.end()) break;
            const auto at = std::find(chain.begin(), chain.end(), it->second);
            if (at != chain.end()) {
                std::vector<NavVehicle*> ring(at, chain.end());
                NavVehicle* give = ring[0];
                for (NavVehicle* r : ring) {
                    const auto rank = [&](const NavVehicle* x) {   // not one that backed off a moment ago (it may be unable to)
                    return std::make_tuple(x->Yielded < Time - 10.0, -x->Priority, x->JGranted ? 0 : 1, x->JAsked, x->Id);
                };
                    if (rank(r) > rank(give)) give = r;
                }
                Emit("deadlock", { { "vehicle", V(give->Id) }, { "ring", R(static_cast<double>(ring.size())) } });
                const bool tried = give->Yielded > Time - 30.0;      // backing off did not untie it: round the others on open ground
                give->Yielded = Time;
                if (give->JNode) { give->AvoidNode = *give->JNode; give->AvoidUntil = Time + 60.0; }   // not back into the same knot
                if (tried) { Replan(*give, true, "free", std::nullopt, true); give->Round = true; }
                else Reverse(*give, give->JNode ? std::max(6.0, give->S + give->Len / 2 - give->JIn + 2.0) : 6.0);   // out of the box
                break;
            }
            chain.push_back(it->second);
        }
        for (NavVehicle* c : chain) done.insert(c->Id);
    }
}

} // namespace IYI
