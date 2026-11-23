// Moved verbatim out of NavSim.cpp.

#include "NavSimInternal.h"

namespace IYI {

namespace {

constexpr double FOOT_ROAD = 1.3, FOOT_MIDDLE = 1.8; // on foot a road costs a little more than open ground (1.25), its middle
                                                     // much more: people walk beside the road where they can, else at its edge
constexpr double LANE_MIN_WIDTH = 4.0;               // narrower roads have one lane down the middle
constexpr double UTURN = 40.0;                       // metres a route pays for turning round
constexpr double TIGHT_LINK = 15.0, TIGHT_COST = 30.0, AVOID_COST = 120.0; // a link between junction boxes shorter than this (m) costs civilians this much more
constexpr double EDGE_BUCKET = 32.0;                 // metres across an edge lookup bucket
constexpr double SNAP = 60.0;                        // farthest a road route looks for its road
double RoadSpeed(const std::string& kind)
{
    static const std::unordered_map<std::string, double> speeds = {
        { "motorway", 1.0 }, { "trunk", 1.0 }, { "primary", 1.0 }, { "secondary", 0.95 }, { "tertiary", 0.85 },
        { "unclassified", 0.75 }, { "residential", 0.6 }, { "service", 0.5 }, { "track", 0.5 }, { "path", 0.3 } };
    auto it = speeds.find(kind);
    return it == speeds.end() ? 0.7 : it->second;
}

double OffroadSpeed(const std::string& kind)
{
    if (kind == "wheeled") return 0.4;
    if (kind == "tracked") return 0.65;
    if (kind == "foot") return 1.0;
    throw std::out_of_range("nav: no off-road speed for kind " + kind);
}

double HazardWeight(const std::string& side)
{
    if (side == "civilian") return 25.0;
    if (side == "medical") return 8.0;
    if (side == "logistics") return 6.0;
    return 2.0;
}

// geo_world.inside for one ring (even-odd).
bool Inside(const std::vector<NavXY>& r, double x, double y)
{
    bool hit = false;
    for (size_t k = 0; k < r.size(); ++k) {
        const NavXY& a = r[k];
        const NavXY& b = r[(k + 1) % r.size()];
        if ((a.y <= y) != (b.y <= y) && x < a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y)) hit = !hit;
    }
    return hit;
}

// core._lane: a polyline moved onto its right-hand lane (x east, y south: right of (dx, dy) is (-dy, dx)).
std::vector<NavXY> Lane(const std::vector<NavXY>& points, double width)
{
    if (width < LANE_MIN_WIDTH || points.size() < 2) return points;
    const double k = width / 4;
    std::vector<NavXY> out;
    const int n = static_cast<int>(points.size());
    for (int i = 0; i < n; ++i) {
        const NavXY& a = points[std::max(i - 1, 0)];
        const NavXY& b = points[std::min(i + 1, n - 1)];
        const double dx = b.x - a.x, dy = b.y - a.y;
        double d = Hyp(dx, dy);
        if (d == 0.0) d = 1.0;
        out.push_back({ points[i].x - dy / d * k, points[i].y + dx / d * k });
    }
    return out;
}


} // namespace

// --- load (core.load) -----------------------------------------------------------------------

NavSim::NavSim(NavMap map, std::unique_ptr<NavWorld> world, int64_t seed, bool wait)
    : Map(std::move(map)), Wait(wait), Seed(seed), m_world(std::move(world))
{
    const size_t cells = static_cast<size_t>(Map.Columns) * Map.Rows;
    Planner.Grid = { Map.Columns, Map.Rows, static_cast<float>(Map.Cell), { static_cast<float>(Map.Origin.x), static_cast<float>(Map.Origin.y) },
                     std::vector<uint8_t>(cells), std::vector<uint8_t>(cells) };
    for (size_t b = 0; b < Map.Buildings.size(); ++b) {
        const auto& fp = Map.Buildings[b].Footprint;
        m_buildingIndex[Map.Buildings[b].Id] = static_cast<int>(b);
        double x0 = fp[0].x, x1 = fp[0].x, y0 = fp[0].y, y1 = fp[0].y;
        for (const NavXY& p : fp) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
        for (int64_t i = Py::ifloor(x0 / BUILDING_BUCKET); i <= Py::ifloor(x1 / BUILDING_BUCKET); ++i)
            for (int64_t j = Py::ifloor(y0 / BUILDING_BUCKET); j <= Py::ifloor(y1 / BUILDING_BUCKET); ++j) m_buildingBuckets[Key(i, j)].push_back(static_cast<int>(b));
    }

    // core._with_culverts: a road over water that is no bridge crosses it on a culvert or a ford.
    const uint8_t* bits = m_world->Bits();
    Base.assign(bits, bits + cells);
    std::set<int> bridges;
    for (const auto& r : Map.Roads)
        if (r.Bridge) for (int i : LineCells(r.Points, r.Width)) bridges.insert(i);
    for (size_t i = 0; i < cells; ++i) {
        const uint8_t b = bits[i];
        if (b & NAV_WATER && b & NAV_ROAD && !(b & NAV_BRIDGE) && !bridges.count(static_cast<int>(i))) {
            Base[i] |= NAV_BRIDGE;
            ++Culverts;
        }
    }
    Extra.assign(cells, 0);
    Planner.Grid.Base = Base;
    std::vector<int> all(cells);
    for (size_t i = 0; i < cells; ++i) all[i] = static_cast<int>(i);
    Middle(all);
    Planner.GridChanged = true;

    std::set<std::string> kinds;
    for (const auto& r : Map.Roads) kinds.insert(r.Kind);
    m_kinds.assign(kinds.begin(), kinds.end());
    NavGraph g;
    for (const NavXY& p : Map.Nodes) g.Nodes.push_back({ static_cast<float>(p.x), static_cast<float>(p.y) });
    for (size_t e = 0; e < Map.Roads.size(); ++e) {
        const auto& r = Map.Roads[e];
        const int kind = static_cast<int>(std::find(m_kinds.begin(), m_kinds.end(), r.Kind) - m_kinds.begin());
        g.Edges.push_back({ r.A, r.B, static_cast<float>(r.Length), kind, r.Oneway, true, static_cast<float>(r.Width), 0.0f, 0.0f });
        m_edgeIndex[r.Id] = static_cast<int>(e);
        if (m_world->Destroyed(r.Id)) m_destroyed.insert(r.Id);
        Edges.push_back({ !Destroyed(r.Id), r.Width, 0.0, 0.0 });
        const double pad = r.Width / 2;
        double x0 = r.Points[0].x, x1 = x0, y0 = r.Points[0].y, y1 = y0;
        for (const NavXY& p : r.Points) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
        for (int64_t i = Py::ifloor((x0 - pad) / EDGE_BUCKET); i <= Py::ifloor((x1 + pad) / EDGE_BUCKET); ++i)
            for (int64_t j = Py::ifloor((y0 - pad) / EDGE_BUCKET); j <= Py::ifloor((y1 + pad) / EDGE_BUCKET); ++j) m_edgeBuckets[Key(i, j)].push_back(static_cast<int>(e));
    }
    Planner.Graph = std::move(g);
    Planner.GraphChanged = true;
    // Junction boxes: past its box a road is clear of the others, so the box grows where two roads meet at a sharp angle.
    struct Arm { NavXY U; double Width; };
    std::vector<std::vector<Arm>> arms(Map.Nodes.size());
    for (const auto& r : Map.Roads) {
        const double look = std::min(8.0, r.Length / 2);
        const NavXY a = NavPointAt(r.Points, look).first, b = NavPointAt(r.Points, r.Length - look).first;
        const NavXY na = Map.Nodes[r.A], nb = Map.Nodes[r.B];
        const double da = std::max(Hyp(a.x - na.x, a.y - na.y), 1e-6), db = std::max(Hyp(b.x - nb.x, b.y - nb.y), 1e-6);
        arms[r.A].push_back({ { (a.x - na.x) / da, (a.y - na.y) / da }, r.Width });
        arms[r.B].push_back({ { (b.x - nb.x) / db, (b.y - nb.y) / db }, r.Width });
    }
    m_box.assign(Map.Nodes.size(), 0.0);
    for (size_t n = 0; n < arms.size(); ++n) {
        if (arms[n].size() < 3) continue;                                    // a bend or a dead end: no junction
        for (const Arm& i : arms[n]) {
            m_box[n] = std::max(m_box[n], i.Width / 2 + 2.0);
            for (const Arm& j : arms[n]) {
                const double sine = std::fabs(i.U.x * j.U.y - i.U.y * j.U.x);
                if (&i != &j && i.U.x * j.U.x + i.U.y * j.U.y > 0.0)        // closer than 90 degrees: apart only farther out
                    m_box[n] = std::max(m_box[n], std::min(20.0, ((i.Width + j.Width) / 2 + 1.0) / std::max(sine, 0.05)));
            }
        }
    }
    m_cluster.assign(Map.Nodes.size(), -1);                                  // boxes that touch make one junction
    for (size_t n = 0; n < m_box.size(); ++n) if (m_box[n] > 0.0) m_cluster[n] = static_cast<int>(n);
    const auto root = [&](int n) { while (m_cluster[n] != n) n = m_cluster[n] = m_cluster[m_cluster[n]]; return n; };
    std::unordered_map<int64_t, std::vector<int>> buckets;
    for (size_t n = 0; n < m_box.size(); ++n)
        if (m_box[n] > 0.0) buckets[Key(Py::ifloordiv(Map.Nodes[n].x, 40), Py::ifloordiv(Map.Nodes[n].y, 40))].push_back(static_cast<int>(n));
    for (size_t n = 0; n < m_box.size(); ++n) {
        if (m_box[n] == 0.0) continue;
        const NavXY p = Map.Nodes[n];
        for (int64_t i = -1; i <= 1; ++i)
            for (int64_t j = -1; j <= 1; ++j) {
                auto it = buckets.find(Key(Py::ifloordiv(p.x, 40) + i, Py::ifloordiv(p.y, 40) + j));
                if (it == buckets.end()) continue;
                for (int o : it->second)
                    if (o != static_cast<int>(n) && Hyp(Map.Nodes[o].x - p.x, Map.Nodes[o].y - p.y) < m_box[n] + m_box[o]) m_cluster[root(o)] = root(static_cast<int>(n));
            }
    }
    for (size_t n = 0; n < m_cluster.size(); ++n) if (m_cluster[n] >= 0) m_cluster[n] = root(static_cast<int>(n));
    for (size_t e = 0; e < Map.Roads.size(); ++e) {      // a link between junctions too short to queue on: where gridlock starts
        const auto& r = Map.Roads[e];
        if (m_box[r.A] > 0.0 && m_box[r.B] > 0.0 && r.Length - m_box[r.A] - m_box[r.B] < TIGHT_LINK) Planner.Graph.Edges[e].Tight = 1.0f;
    }
    for (size_t e = 0; e < Edges.size(); ++e) {
        if (Edges[e].Open) continue;
        NavEdge& edge = Planner.Graph.Edges[e];
        edge.Open = false; edge.Clear = static_cast<float>(Edges[e].Clear); edge.Hazard = 0.0f; edge.Delay = 0.0f;
    }
    Sync();
}

// --- geo_world helpers ------------------------------------------------------------------------

std::vector<int> NavSim::PolygonCells(const std::vector<NavXY>& ring) const
{
    const double c = Map.Cell, ox = Map.Origin.x, oy = Map.Origin.y;
    double ymin = ring[0].y, ymax = ring[0].y;
    for (const NavXY& p : ring) { ymin = std::min(ymin, p.y); ymax = std::max(ymax, p.y); }
    const int64_t r0 = std::max<int64_t>(0, Py::iceil((ymin - oy) / c - 0.5));
    const int64_t r1 = std::min<int64_t>(Map.Rows - 1, Py::ifloor((ymax - oy) / c - 0.5));
    std::vector<int> out;
    std::vector<double> xs;
    for (int64_t row = r0; row <= r1; ++row) {
        const double y = oy + (static_cast<double>(row) + 0.5) * c;
        xs.clear();
        for (size_t k = 0; k < ring.size(); ++k) {
            const NavXY& a = ring[(k + ring.size() - 1) % ring.size()];
            const NavXY& b = ring[k];
            if ((a.y <= y) != (b.y <= y)) xs.push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int64_t c0 = std::max<int64_t>(0, Py::iceil((xs[k] - ox) / c - 0.5));
            const int64_t c1 = std::min<int64_t>(Map.Columns - 1, Py::ifloor((xs[k + 1] - ox) / c - 0.5));
            for (int64_t col = c0; col <= c1; ++col) out.push_back(static_cast<int>(row * Map.Columns + col));
        }
    }
    return out;
}

std::vector<int> NavSim::LineCells(const std::vector<NavXY>& points, double width) const
{
    const double c = Map.Cell, ox = Map.Origin.x, oy = Map.Origin.y, half = width / 2;
    std::set<int> out;
    for (size_t k = 0; k + 1 < points.size(); ++k) {
        const double ax = points[k].x, ay = points[k].y, bx = points[k + 1].x, by = points[k + 1].y;
        const double dx = bx - ax, dy = by - ay;
        double ll = dx * dx + dy * dy;
        if (ll == 0.0) ll = 1e-9;
        const int64_t rEnd = std::min<int64_t>(Map.Rows, Py::iceil((std::max(ay, by) + half - oy) / c));
        const int64_t cEnd = std::min<int64_t>(Map.Columns, Py::iceil((std::max(ax, bx) + half - ox) / c));
        for (int64_t row = std::max<int64_t>(0, Py::ifloor((std::min(ay, by) - half - oy) / c)); row < rEnd; ++row) {
            const double py = oy + (static_cast<double>(row) + 0.5) * c;
            for (int64_t col = std::max<int64_t>(0, Py::ifloor((std::min(ax, bx) - half - ox) / c)); col < cEnd; ++col) {
                const double px = ox + (static_cast<double>(col) + 0.5) * c;
                const double t = std::max(0.0, std::min(1.0, ((px - ax) * dx + (py - ay) * dy) / ll));
                if (Py::pow(ax + t * dx - px, 2) + Py::pow(ay + t * dy - py, 2) <= half * half) out.insert(static_cast<int>(row * Map.Columns + col));
            }
        }
    }
    return { out.begin(), out.end() };
}

double NavSim::Height(double x, double y) const
{
    const int cols = Map.TerrainColumns, rows = Map.TerrainRows;
    const double fx = std::min(std::max((x - Map.TerrainOrigin.x) / Map.TerrainCell, 0.0), static_cast<double>(cols));
    const double fy = std::min(std::max((y - Map.TerrainOrigin.y) / Map.TerrainCell, 0.0), static_cast<double>(rows));
    const int c = std::min(static_cast<int>(fx), cols - 1), r = std::min(static_cast<int>(fy), rows - 1);
    const double u = fx - c, v = fy - r;
    const size_t i = static_cast<size_t>(r) * (cols + 1) + c;
    auto h = [&](size_t k) { return static_cast<double>(Map.Heights[k]); };
    const double top = h(i) + (h(i + 1) - h(i)) * u;
    const double bottom = h(i + cols + 1) + (h(i + cols + 2) - h(i + cols + 1)) * u;
    return top + (bottom - top) * v;
}

std::optional<std::string> NavSim::BuildingAt(double x, double y) const
{
    auto it = m_buildingBuckets.find(Key(Py::ifloor(x / BUILDING_BUCKET), Py::ifloor(y / BUILDING_BUCKET)));
    if (it != m_buildingBuckets.end())
        for (int b : it->second)
            if (Inside(Map.Buildings[b].Footprint, x, y)) return Map.Buildings[b].Id;
    return std::nullopt;
}

// --- the grid as each class sees it (core.grid_costs, passable) ------------------------------

const NavSim::Costs& NavSim::CostsFor(const std::string& cls, const std::string& side)
{
    auto it = m_costs.find({ cls, side });
    if (it != m_costs.end()) return it->second;
    const std::string& kind = NavClassOf(cls).Kind;
    Costs c;
    for (int b = 0; b < 256; ++b) {
        double cost;
        if (b & (NAV_BUILDING | NAV_WALL) || (b & NAV_WATER && !(b & NAV_BRIDGE))) cost = 0.0;
        else if (kind == "foot") cost = (b & NAV_ROAD ? FOOT_ROAD : 1.25) * (b & NAV_RUBBLE ? 2.5 : 1.0) * (b & NAV_STEEP ? 3.0 : 1.0);
        else if (b & (NAV_RUBBLE | NAV_STEEP) && kind == "wheeled") cost = 0.0;
        else {
            cost = b & (NAV_ROAD | NAV_BRIDGE) ? 1.0 : 1.0 / OffroadSpeed(kind);
            cost *= (b & NAV_RUBBLE ? 3.0 : 1.0) * (b & NAV_STEEP ? 5.0 : 1.0);
        }
        c.Base[b] = cost;
    }
    c.Extra.fill(1.0);
    c.Extra[BLOCKED] = 0.0;
    c.Extra[DEBRIS] = kind == "foot" ? 2.5 : kind == "tracked" ? 3.0 : 0.0;
    c.Extra[HAZARD] = 1.0 + HazardWeight(side);
    c.Extra[ROAD_MIDDLE] = kind == "foot" ? FOOT_MIDDLE : 1.0;
    return m_costs.emplace(std::make_pair(cls, side), c).first->second;
}

std::optional<int> NavSim::CellIndex(double x, double y) const
{
    const int64_t col = Py::ifloor((x - Map.Origin.x) / Map.Cell), row = Py::ifloor((y - Map.Origin.y) / Map.Cell);
    if (0 <= col && col < Map.Columns && 0 <= row && row < Map.Rows) return static_cast<int>(row * Map.Columns + col);
    return std::nullopt;
}

bool NavSim::Passable(const std::string& cls, double x, double y)
{
    const auto i = CellIndex(x, y);
    if (!i) return false;
    const Costs& c = CostsFor(cls);
    if (c.Base[Base[*i]] <= 0.0) return false;
    const uint8_t extra = Extra[*i];
    for (int b = 0; b < 8; ++b)
        if (extra >> b & 1 && !(c.Extra[b] > 0.0)) return false;
    return true;
}

void NavSim::SetBits(const std::vector<int>& cells, int bit, bool on)
{
    std::vector<int> changed;
    for (int i : cells) {
        const int64_t key = static_cast<int64_t>(i) * 8 + bit;
        auto it = m_cellBits.find(key);
        const int c = (it == m_cellBits.end() ? 0 : it->second) + (on ? 1 : -1);
        if (c <= 0) { if (it != m_cellBits.end()) m_cellBits.erase(it); }
        else m_cellBits[key] = c;
        const int was = Extra[i] >> bit & 1, now = c > 0 ? 1 : 0;
        if (was != now) {
            Extra[i] ^= static_cast<uint8_t>(1 << bit);
            changed.push_back(i);
        }
    }
    if (changed.empty()) return;
    for (int i : changed) Planner.Grid.Extra[i] = static_cast<uint8_t>(on ? Planner.Grid.Extra[i] | 1 << bit : Planner.Grid.Extra[i] & ~(1 << bit));
    Planner.GridChanged = true;
}

void NavSim::Changed(const std::vector<NavXY>& points, double pad)
{
    double x0 = points[0].x, x1 = x0, y0 = points[0].y, y1 = y0;
    for (const NavXY& p : points) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
    ++Revision;
    m_dirty.push_back({ Revision, x0 - pad, y0 - pad, x1 + pad, y1 + pad });
    if (m_dirty.size() > 64) m_dirty.erase(m_dirty.begin(), m_dirty.end() - 64);
    RefreshEdges(x0 - pad, y0 - pad, x1 + pad, y1 + pad);
}

// --- obstacles and hazards ------------------------------------------------------------------

void NavSim::SetObstacle(const std::string& oid, const std::vector<std::array<double, 5>>& boxes, const std::string& kind)
{
    ClearObstacle(oid);
    std::set<int> cells;
    std::vector<NavXY> corners;
    for (const auto& box : boxes) {
        const auto c = BoxCorners(box);
        for (int i : PolygonCells({ c.begin(), c.end() })) cells.insert(i);
    }
    for (const auto& box : boxes)
        for (const NavXY& p : BoxCorners(box)) corners.push_back(p);
    const int bit = kind == "blocked" ? BLOCKED : DEBRIS;
    Obstacle& o = Obstacles[oid] = { { cells.begin(), cells.end() }, bit, boxes, kind };
    SetBits(o.Cells, bit, true);
    Changed(corners, 2.0);
}

void NavSim::ClearObstacle(const std::string& oid)
{
    auto it = Obstacles.find(oid);
    if (it == Obstacles.end()) return;
    Obstacle o = std::move(it->second);
    Obstacles.erase(it);
    SetBits(o.Cells, o.Bit, false);
    std::vector<NavXY> corners;
    for (const auto& box : o.Boxes)
        for (const NavXY& p : BoxCorners(box)) corners.push_back(p);
    Changed(corners, 2.0);
}

void NavSim::ReportHazard(const std::string& hid, double x, double y, double radius, double severity, double duration, std::optional<std::string>)
{
    ClearHazard(hid);
    std::vector<NavXY> ring;
    for (int a = 0; a < 16; ++a) ring.push_back({ x + radius * std::cos(a * K_PI / 8), y + radius * std::sin(a * K_PI / 8) });
    std::vector<int> cells = PolygonCells(ring);
    SetBits(cells, HAZARD, true);
    m_hazards.push_back({ hid, x, y, radius, severity, Time + duration, std::move(cells) });
    m_alarms.push_back({ Time, x, y, radius, severity });
    if (m_alarms.size() > 32) m_alarms.erase(m_alarms.begin());
    Changed(ring, 0.0);
}

void NavSim::ClearHazard(const std::string& hid)
{
    auto it = std::find_if(m_hazards.begin(), m_hazards.end(), [&](const Hazard& h) { return h.Id == hid; });
    if (it == m_hazards.end()) return;
    Hazard h = std::move(*it);
    m_hazards.erase(it);
    SetBits(h.Cells, HAZARD, false);
    const double r = h.Radius;
    Changed({ { h.X - r, h.Y - r }, { h.X + r, h.Y + r } }, 0.0);
}

// --- the road graph -------------------------------------------------------------------------

std::vector<int> NavSim::EdgesNear(double x0, double y0, double x1, double y1) const
{
    std::set<int> found;
    for (int64_t i = Py::ifloor(x0 / EDGE_BUCKET); i <= Py::ifloor(x1 / EDGE_BUCKET); ++i)
        for (int64_t j = Py::ifloor(y0 / EDGE_BUCKET); j <= Py::ifloor(y1 / EDGE_BUCKET); ++j) {
            auto it = m_edgeBuckets.find(Key(i, j));
            if (it != m_edgeBuckets.end()) found.insert(it->second.begin(), it->second.end());
        }
    return { found.begin(), found.end() };
}

bool NavSim::BlockedAt(double x, double y) const
{
    const auto i = CellIndex(x, y);
    return !i || Extra[*i] & (1 << BLOCKED | 1 << DEBRIS);
}

double NavSim::SpanClear(int e, double s0, double s1) const
{
    const auto& r = Map.Roads[e];
    const double half = r.Width / 2;
    double clear = r.Width;
    const double lo = std::min(s0, s1), hi = std::max(s0, s1);
    for (double s = lo; s <= hi + 1e-6; s += 1.5) {
        const auto [p, u] = NavPointAt(r.Points, s);
        double run = 0.0, best = 0.0;
        for (double k = -half; k <= half + 1e-6; k += 0.5) {
            run = BlockedAt(p.x - u.y * k, p.y + u.x * k) ? 0.0 : run + 0.5;
            best = std::max(best, run);
        }
        clear = std::min(clear, best);
    }
    return clear;
}

void NavSim::RefreshEdges(double x0, double y0, double x1, double y1)
{
    for (int e : EdgesNear(x0, y0, x1, y1)) {
        const auto& r = Map.Roads[e];
        double hz = 0.0;
        for (const Hazard& h : m_hazards) {
            bool near = false;
            for (size_t k = 0; k + 1 < r.Points.size() && !near; ++k) near = SegDist(h.X, h.Y, r.Points[k], r.Points[k + 1]) < h.Radius;
            if (near) hz = std::max(hz, h.Severity * std::min(r.Length, 2 * h.Radius));
        }
        const EdgeState st{ !Destroyed(r.Id), SpanClear(e, 0.0, r.Length), hz, Edges[e].Delay };
        if (st == Edges[e]) continue;
        Edges[e] = st;
        NavEdge& edge = Planner.Graph.Edges[e];
        edge.Open = st.Open; edge.Clear = static_cast<float>(st.Clear); edge.Hazard = static_cast<float>(st.Hazard); edge.Delay = static_cast<float>(st.Delay);
        Planner.GraphChanged = true;
    }
}

void NavSim::SetDelay(int e, double delay)
{
    EdgeState& st = Edges[e];
    if (!(std::fabs(st.Delay - delay) > 1.0)) return;
    st.Delay = delay;
    Planner.Graph.Edges[e].Delay = static_cast<float>(delay);
    Planner.GraphChanged = true;
}

std::optional<std::tuple<int, double, double>> NavSim::Snap(double x, double y, double reach, bool openOnly) const
{
    std::optional<std::tuple<int, double, double>> best;
    for (int e : EdgesNear(x - reach, y - reach, x + reach, y + reach)) {
        if (openOnly && !Edges[e].Open) continue;
        const auto& pts = Map.Roads[e].Points;
        double along = 0.0;
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const double ax = pts[k].x, ay = pts[k].y, dx = pts[k + 1].x - ax, dy = pts[k + 1].y - ay;
            const double d2 = dx * dx + dy * dy;
            const double t = d2 < 1e-9 ? 0.0 : std::min(std::max(((x - ax) * dx + (y - ay) * dy) / d2, 0.0), 1.0);
            const double off = Hyp(ax + dx * t - x, ay + dy * t - y);
            if (off < reach && (!best || off < std::get<2>(*best))) best = std::make_tuple(e, along + t * std::sqrt(d2), off);
            along += std::sqrt(d2);
        }
    }
    return best;
}

// --- requests -------------------------------------------------------------------------------

int NavSim::Request(NavXY start, NavXY goal, const std::string& cls, std::string mode, const std::string& side, std::optional<NavXY> heading, bool tight,
                    std::vector<int> avoid, int avoidNode)
{
    const NavClass& c = NavClassOf(cls);
    ++Count.Routes;
    if (mode == "road" && c.Kind != "foot") {
        std::optional<Snapped> a, b;
        if (auto s = Snap(start.x, start.y, SNAP, true)) a = Snapped{ std::get<0>(*s), std::get<1>(*s), std::get<2>(*s) };
        if (auto s = Snap(goal.x, goal.y, SNAP, true)) b = Snapped{ std::get<0>(*s), std::get<1>(*s), std::get<2>(*s) };
        if (b) {                                       // a road goal stops short of a junction, never in its box
            const auto& r = Map.Roads[b->E];
            const double m = c.Length / 2 + 1.0, lo = m_box[r.A] + m, hi = r.Length - m_box[r.B] - m;
            if (lo < hi) b->S = std::min(std::max(b->S, lo), hi);
        }
        const double width = c.Width + 0.6;
        std::vector<End> starts = a ? Ends(*a, width, heading, false) : std::vector<End>{};
        std::vector<End> goals = b ? Ends(*b, width, std::nullopt, true) : std::vector<End>{};
        if (!starts.empty() && !goals.empty()) {
            NavGraphQuery q;
            for (const End& s : starts) q.Starts.push_back({ s.Node, static_cast<float>(s.Cost + (s.Node == avoidNode ? AVOID_COST : 0.0)) });
            for (const End& g : goals) q.Goals.push_back({ g.Node, static_cast<float>(g.Cost) });
            for (const std::string& k : m_kinds) q.KindCosts.push_back(static_cast<float>(1.0 / RoadSpeed(k)));
            q.Width = static_cast<float>(width);
            q.HazardWeight = static_cast<float>(HazardWeight(side));
            q.TightWeight = side == "civilian" ? static_cast<float>(TIGHT_COST) : 0.0f;   // calm traffic keeps out of tight junction clusters
            q.Avoid = avoidNode;
            q.AvoidCost = static_cast<float>(AVOID_COST);
            const int ticket = Planner.Request(q);
            m_requests[ticket] = { "road", start, goal, a, b, cls, side, std::move(starts), std::move(goals), heading };
            return ticket;
        }
        mode = "free";
    }
    const Costs& costs = CostsFor(cls, side);
    goal = OpenNear(cls, goal);
    start = OpenNear(cls, start, 4.0);                 // a vehicle squeezed against a wall searches from the open ground beside it
    // a vehicle's grid route keeps room for its width off walls (half its width beyond the middle cell)
    const int clearance = c.Kind == "foot" || tight ? 0 : std::max(1, static_cast<int>(std::ceil((c.Width / 2 - Map.Cell / 2) / Map.Cell)));
    NavGridQuery q;
    q.Start = { static_cast<float>(start.x), static_cast<float>(start.y) };
    q.Goal = { static_cast<float>(goal.x), static_cast<float>(goal.y) };
    for (int b = 0; b < 256; ++b) q.Costs[b] = static_cast<float>(costs.Base[b]);
    for (int b = 0; b < 8; ++b) q.ExtraCosts[b] = static_cast<float>(costs.Extra[b]);
    q.Clearance = clearance;
    std::sort(avoid.begin(), avoid.end());
    q.Avoid = std::move(avoid);
    const int ticket = Planner.Request(q);
    m_requests[ticket] = { "free", start, goal, std::nullopt, std::nullopt, cls, side, {}, {}, std::nullopt };
    return ticket;
}

NavXY NavSim::OpenNear(const std::string& cls, NavXY p, double reach)
{
    if (Passable(cls, p.x, p.y)) return p;
    for (int r = 2; r <= static_cast<int>(reach); r += 2) {
        const int n = 8 * r / 2;
        for (int k = 0; k < n; ++k) {
            const double a = 2 * K_PI * k / n;
            const NavXY q{ p.x + std::cos(a) * r, p.y + std::sin(a) * r };
            if (Passable(cls, q.x, q.y)) return q;
        }
    }
    return p;
}

std::vector<NavSim::End> NavSim::Ends(const Snapped& at, double width, const std::optional<NavXY>& heading, bool isGoal) const
{
    const auto& r = Map.Roads[at.E];
    std::vector<End> out;
    const std::pair<int, double> ends[2] = { { r.A, 0.0 }, { r.B, r.Length } };
    for (const auto& [node, sEnd] : ends) {
        if (!Edges[at.E].Open || SpanClear(at.E, at.S, sEnd) < width) continue;
        if (r.Oneway && (sEnd == 0.0) != isGoal) continue;
        double cost = std::fabs(sEnd - at.S) / RoadSpeed(r.Kind);
        if (heading) {
            const NavXY u = NavPointAt(r.Points, at.S).second;
            const double forward = (u.x * heading->x + u.y * heading->y) * (sEnd > at.S ? 1 : -1);
            if (forward < -0.3) cost += UTURN;
        }
        out.push_back({ node, cost, sEnd });
    }
    return out;
}

void NavSim::Poll()
{
    for (const NavResult& r : Planner.Poll(Wait)) {
        auto it = m_requests.find(r.Ticket);
        if (it == m_requests.end()) continue;
        const Req req = std::move(it->second);
        m_requests.erase(it);
        NavRoute out;
        if (req.Mode == "road") out = RoadRoute(req, r);
        else {
            out.Found = r.Found;
            for (const Vector2& p : r.Points) out.Points.push_back({ p.x, p.y });
            out.Mode = "free";
            out.Speeds.assign(out.Points.size(), 1.0);
        }
        out.Cost = r.Cost;
        if (!out.Found) Emit("route_failed", { { "cls", V(req.Cls) }, { "start", V(req.Start) }, { "goal", V(req.Goal) }, { "mode", V(req.Mode) } });
        if (!m_forgotten.erase(r.Ticket)) m_results[r.Ticket] = std::move(out);
    }
}

void NavSim::Forget(int ticket)
{
    if (ticket && !m_results.erase(ticket)) m_forgotten.insert(ticket);
}

NavRoute NavSim::RoadRoute(const Req& req, const NavResult& r) const
{
    const Snapped a = *req.A, b = *req.B;
    const double w = NavClassOf(req.Cls).Width + 0.6;
    NavRoute out;
    out.Mode = "road";
    // Start and goal on the same edge, the way between them open: drive straight there.
    std::optional<double> direct;
    if (a.E == b.E && SpanClear(a.E, a.S, b.S) >= w && Edges[a.E].Open) {
        direct = std::fabs(b.S - a.S);
        if (req.Heading) {
            const NavXY u = NavPointAt(Map.Roads[a.E].Points, a.S).second;
            if ((u.x * req.Heading->x + u.y * req.Heading->y) * (b.S > a.S ? 1 : -1) < -0.3) *direct += UTURN;
        }
    }
    if (direct && (!r.Found || *direct <= static_cast<double>(r.Cost))) {
        const auto& e = Map.Roads[a.E];
        out.Found = true;
        out.Points = { req.Start };
        for (const NavXY& p : Lane(Sub(e.Points, a.S, b.S), e.Width)) out.Points.push_back(p);
        out.Speeds.assign(out.Points.size(), RoadSpeed(e.Kind));
        out.Edges = { a.E };
        return out;
    }
    if (!r.Found) return out;
    const std::vector<int>& nodes = r.Nodes;
    double sStart = 0.0, sGoal = 0.0;
    for (const End& s : req.Starts) if (s.Node == nodes.front()) { sStart = s.S; break; }
    for (const End& g : req.Goals) if (g.Node == nodes.back()) { sGoal = g.S; break; }
    struct Leg { int E; double S0, S1; };
    std::vector<Leg> legs{ { a.E, a.S, sStart } };   // leg k ends at nodes[k]; the last one at the goal
    for (size_t k = 0; k < r.Edges.size() && k < nodes.size(); ++k) {
        const auto& edge = Map.Roads[r.Edges[k]];
        const bool forward = edge.A == nodes[k];
        legs.push_back({ r.Edges[k], forward ? 0.0 : edge.Length, forward ? edge.Length : 0.0 });
    }
    legs.push_back({ b.E, sGoal, b.S });
    out.Points = { req.Start };
    out.Speeds = { RoadSpeed(Map.Roads[a.E].Kind) };
    double along = 0.0;
    for (size_t k = 0; k < legs.size(); ++k) {
        const auto& edge = Map.Roads[legs[k].E];
        for (const NavXY& p : Lane(Sub(edge.Points, legs[k].S0, legs[k].S1), edge.Width)) {
            const double d = Hyp(p.x - out.Points.back().x, p.y - out.Points.back().y);
            if (d > 0.3) {
                along += d;
                out.Points.push_back(p);
                out.Speeds.push_back(RoadSpeed(edge.Kind));
            }
        }
        if (k < nodes.size()) out.Nodes.push_back({ along, nodes[k] });
    }
    out.Found = true;
    out.Edges = { a.E };
    out.Edges.insert(out.Edges.end(), r.Edges.begin(), r.Edges.end());
    out.Edges.push_back(b.E);
    return out;
}

std::optional<NavRoute> NavSim::TakeResult(int ticket)
{
    auto it = m_results.find(ticket);
    if (it == m_results.end()) return std::nullopt;
    NavRoute r = std::move(it->second);
    m_results.erase(it);
    return r;
}

std::vector<NavEvent> NavSim::TakeEvents()
{
    std::vector<NavEvent> out;
    out.swap(m_events);
    return out;
}

} // namespace IYI
