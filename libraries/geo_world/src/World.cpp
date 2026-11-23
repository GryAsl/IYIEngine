// Implements GeoWorld (World.h). A port of the former geo_world/world.py: same rounding, same evaluation order.

#include "geo_world/src/World.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace IYI {

namespace {

constexpr double A = 6378137.0, E2 = 6.69437999014e-3;   // WGS84 semi-major axis, first eccentricity squared
constexpr double Pi = 3.14159265358979323846, RAD = Pi / 180.0, DEG = 180.0 / Pi;   // math.radians / math.degrees factors

double PyRound(double v, int digits)   // Python's round(v, digits): correctly rounded decimal
{
    char buf[400];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return std::strtod(buf, nullptr);
}

int64_t BucketKey(int64_t i, int64_t j) { return i * 1000003 + j; }

double SegmentDistance(double x, double y, const NavXY& a, const NavXY& b)
{
    const double dx = b.x - a.x, dy = b.y - a.y, ll = dx * dx + dy * dy;
    const double t = std::max(0.0, std::min(1.0, ((x - a.x) * dx + (y - a.y) * dy) / (ll == 0.0 ? 1.0 : ll)));
    return std::hypot(x - a.x - t * dx, y - a.y - t * dy);
}

std::vector<NavXY> Points(const nlohmann::json& j)
{
    std::vector<NavXY> out;
    for (const auto& p : j) out.push_back({ p[0].get<double>(), p[1].get<double>() });
    return out;
}

GeoWorld::Grid ReadGrid(const nlohmann::json& j)
{
    return { j["cell"].get<double>(), j["columns"].get<int>(), j["rows"].get<int>(), { j["origin"][0].get<double>(), j["origin"][1].get<double>() } };
}

} // namespace

GeoWorld::GeoWorld(const std::string& name, const std::string& worldJson, const std::string& heights, const std::string& grid)
{
    const nlohmann::json j = nlohmann::json::parse(worldJson);
    if (j.value("format", "") != "geo_world" || j.value("version", 0) != 1) throw std::invalid_argument(name + ": not a geo_world v1 package");
    m_name = j["name"].get<std::string>();
    m_hash = j["sources"]["hash"].get<std::string>();
    m_frame = { j["frame"]["lon"].get<double>(), j["frame"]["lat"].get<double>() };
    m_terrain = ReadGrid(j["terrain"]);
    m_grid = ReadGrid(j["grid"]);
    if (heights.size() != sizeof(float) * (m_terrain.Columns + 1) * (m_terrain.Rows + 1) ||
        grid.size() != static_cast<size_t>(m_grid.Columns) * m_grid.Rows)
        throw std::invalid_argument(name + ": heights or grid size does not match world.json");
    m_heights.resize(heights.size() / sizeof(float));
    std::memcpy(m_heights.data(), heights.data(), heights.size());   // little-endian host
    m_ownBits.assign(grid.begin(), grid.end());
    m_bits = m_ownBits.data();

    for (const auto& b : j["buildings"]) {
        Building o{ b["id"].get<std::string>(), b["height"].get<double>(), Points(b["footprint"]), { b["obb"][0].get<double>(), b["obb"][1].get<double>() } };
        m_buildingIds[o.Id] = static_cast<int>(m_buildings.size());
        m_buildings.push_back(std::move(o));
    }
    for (const auto& r : j["roads"]) {
        Road o{ r["id"].get<std::string>(), r["bridge"].get<bool>(), r["width"].get<double>(), Points(r["points"]) };
        m_roadIds[o.Id] = static_cast<int>(m_roads.size());
        m_bridgeCells.push_back(o.Bridge ? LineCells(o.Points, o.Width) : Cells{});
        m_roads.push_back(std::move(o));
    }
    for (const auto& w : j.value("walls", nlohmann::json::array())) m_walls.push_back({ Points(w["points"]), w["height"].get<double>() });
    for (const auto& h : j.value("hedges", nlohmann::json::array())) m_hedges.push_back({ Points(h["points"]), h["height"].get<double>() });
    for (const auto& t : j.value("trees", nlohmann::json::array()))
        m_trees.push_back({ t[0].get<double>(), t[1].get<double>(), t[2].get<double>(), t[3].get<double>(), t[4].get<double>() });
    for (const auto& r : j.value("tall_grass", nlohmann::json::array())) m_tallGrass.push_back(Points(r));
    for (size_t i = 0; i < m_buildings.size(); ++i) {
        m_cells.push_back(BuildingCells(m_buildings[i]));
        for (int64_t key : BucketKeys(m_buildings[i].Footprint)) m_buckets[key].push_back(static_cast<int>(i));
    }
}

GeoWorld GeoWorld::LoadDirectory(const std::string& dir)
{
    auto read = [&](const std::string& file) {
        std::ifstream in(dir + "/" + file, std::ios::binary);
        if (!in) throw std::runtime_error(dir + "/" + file + ": cannot read");
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    const std::string json = read("world.json");
    const nlohmann::json j = nlohmann::json::parse(json);
    return GeoWorld(dir, json, read(j["terrain"]["file"].get<std::string>()), read(j["grid"]["file"].get<std::string>()));
}

void GeoWorld::ShareBits(uint8_t* memory)
{
    std::copy(m_bits, m_bits + static_cast<size_t>(m_grid.Columns) * m_grid.Rows, memory);
    m_bits = memory;
    m_ownBits = {};
}

const char* GeoWorld::ConditionName(Condition c)
{
    static const char* const names[] = { "intact", "damaged", "destroyed", "cleared" };
    return names[static_cast<int>(c)];
}

std::optional<GeoWorld::Condition> GeoWorld::ParseCondition(const std::string& name)
{
    for (int i = 0; i < 4; ++i)
        if (name == ConditionName(static_cast<Condition>(i))) return static_cast<Condition>(i);
    return std::nullopt;
}

// --- frame ---------------------------------------------------------------------------------

NavXY GeoWorld::FromLonLat(double lon, double lat) const
{
    const double s = std::sin(m_frame.y * RAD), m = A * (1 - E2) / std::pow(1 - E2 * s * s, 1.5), n = A / std::sqrt(1 - E2 * s * s) * std::cos(m_frame.y * RAD);
    return { (lon - m_frame.x) * RAD * n, -(lat - m_frame.y) * RAD * m };
}

NavXY GeoWorld::ToLonLat(double x, double y) const
{
    const double s = std::sin(m_frame.y * RAD), m = A * (1 - E2) / std::pow(1 - E2 * s * s, 1.5), n = A / std::sqrt(1 - E2 * s * s) * std::cos(m_frame.y * RAD);
    return { m_frame.x + x / n * DEG, m_frame.y - y / m * DEG };
}

// --- rasterizing ---------------------------------------------------------------------------

std::vector<int> GeoWorld::PolygonCells(const std::vector<std::vector<NavXY>>& rings) const
{
    const double c = m_grid.Cell, ox = m_grid.Origin.x, oy = m_grid.Origin.y;
    std::vector<std::pair<NavXY, NavXY>> edges;
    double lo = INFINITY, hi = -INFINITY;
    for (const auto& r : rings)
        for (size_t i = 0; i < r.size(); ++i) {
            edges.push_back({ r[(i + r.size() - 1) % r.size()], r[i] });
            lo = std::min(lo, r[i].y);
            hi = std::max(hi, r[i].y);
        }
    std::vector<int> out;
    if (edges.empty()) return out;
    const int r0 = static_cast<int>(std::max(0.0, std::ceil((lo - oy) / c - 0.5)));
    const int r1 = static_cast<int>(std::min<double>(m_grid.Rows - 1, std::floor((hi - oy) / c - 0.5)));
    std::vector<double> xs;
    for (int row = r0; row <= r1; ++row) {
        const double y = oy + (row + 0.5) * c;
        xs.clear();
        for (const auto& [a, b] : edges)
            if ((a.y <= y) != (b.y <= y)) xs.push_back(a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y));
        std::sort(xs.begin(), xs.end());
        for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            const int c0 = static_cast<int>(std::max(0.0, std::ceil((xs[k] - ox) / c - 0.5)));
            const int c1 = static_cast<int>(std::min<double>(m_grid.Columns - 1, std::floor((xs[k + 1] - ox) / c - 0.5)));
            for (int col = c0; col <= c1; ++col) out.push_back(row * m_grid.Columns + col);
        }
    }
    return out;
}

std::vector<int> GeoWorld::LineCells(const std::vector<NavXY>& points, double width) const
{
    const double c = m_grid.Cell, ox = m_grid.Origin.x, oy = m_grid.Origin.y, half = width / 2;
    const int cols = m_grid.Columns, rows = m_grid.Rows;
    std::set<int> out;
    for (size_t k = 0; k + 1 < points.size(); ++k) {
        const NavXY a = points[k], b = points[k + 1];
        const double dx = b.x - a.x, dy = b.y - a.y, ll = dx * dx + dy * dy == 0.0 ? 1e-9 : dx * dx + dy * dy;
        const int r0 = static_cast<int>(std::max(0.0, std::floor((std::min(a.y, b.y) - half - oy) / c)));
        const int r1 = static_cast<int>(std::min<double>(rows, std::ceil((std::max(a.y, b.y) + half - oy) / c)));
        const int c0 = static_cast<int>(std::max(0.0, std::floor((std::min(a.x, b.x) - half - ox) / c)));
        const int c1 = static_cast<int>(std::min<double>(cols, std::ceil((std::max(a.x, b.x) + half - ox) / c)));
        for (int row = r0; row < r1; ++row) {
            const double py = oy + (row + 0.5) * c;
            for (int col = c0; col < c1; ++col) {
                const double px = ox + (col + 0.5) * c;
                const double t = std::max(0.0, std::min(1.0, ((px - a.x) * dx + (py - a.y) * dy) / ll));
                const double ex = a.x + t * dx - px, ey = a.y + t * dy - py;
                if (ex * ex + ey * ey <= half * half) out.insert(row * cols + col);
            }
        }
    }
    return { out.begin(), out.end() };
}

std::vector<int> GeoWorld::BuildingCells(const Building& b) const
{
    std::vector<int> cells = PolygonCells({ b.Footprint });
    if (cells.empty()) {   // smaller than a cell: the cell under its centre, if that is on the grid
        const int row = static_cast<int>(std::floor((b.Centre.y - m_grid.Origin.y) / m_grid.Cell));
        const int col = static_cast<int>(std::floor((b.Centre.x - m_grid.Origin.x) / m_grid.Cell));
        if (row >= 0 && row < m_grid.Rows && col >= 0 && col < m_grid.Columns) cells.push_back(row * m_grid.Columns + col);
    }
    return cells;
}

bool GeoWorld::Inside(const std::vector<NavXY>& ring, double x, double y)
{
    bool hit = false;
    for (size_t i = 0; i < ring.size(); ++i) {
        const NavXY a = ring[i], b = ring[(i + 1) % ring.size()];
        if ((a.y <= y) != (b.y <= y) && x < a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y)) hit = !hit;
    }
    return hit;
}

// --- queries -------------------------------------------------------------------------------

std::vector<int64_t> GeoWorld::BucketKeys(const std::vector<NavXY>& points)
{
    double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    for (const NavXY& p : points) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
    std::vector<int64_t> keys;
    for (int64_t i = static_cast<int64_t>(std::floor(x0 / BUCKET)); i <= static_cast<int64_t>(std::floor(x1 / BUCKET)); ++i)
        for (int64_t j = static_cast<int64_t>(std::floor(y0 / BUCKET)); j <= static_cast<int64_t>(std::floor(y1 / BUCKET)); ++j) keys.push_back(BucketKey(i, j));
    return keys;
}

std::vector<int> GeoWorld::NearBuildings(const std::vector<NavXY>& area) const
{
    std::vector<int> out;
    std::unordered_set<int> seen;
    for (int64_t key : BucketKeys(area)) {
        const auto it = m_buckets.find(key);
        if (it != m_buckets.end())
            for (int b : it->second)
                if (seen.insert(b).second) out.push_back(b);
    }
    return out;
}

double GeoWorld::Height(double x, double y) const
{
    if (!std::isfinite(x) || !std::isfinite(y)) throw std::invalid_argument("GeoWorld::Height: point is not finite");
    const int cols = m_terrain.Columns, rows = m_terrain.Rows;
    const double fx = std::min(std::max((x - m_terrain.Origin.x) / m_terrain.Cell, 0.0), static_cast<double>(cols));
    const double fy = std::min(std::max((y - m_terrain.Origin.y) / m_terrain.Cell, 0.0), static_cast<double>(rows));
    const int c = std::min(static_cast<int>(fx), cols - 1), r = std::min(static_cast<int>(fy), rows - 1);
    const double u = fx - c, v = fy - r;
    const size_t i = static_cast<size_t>(r) * (cols + 1) + c;
    const std::vector<float>& h = m_heights;
    const double top = h[i] + (static_cast<double>(h[i + 1]) - h[i]) * u;
    const double bottom = h[i + cols + 1] + (static_cast<double>(h[i + cols + 2]) - h[i + cols + 1]) * u;
    return top + (bottom - top) * v;
}

int GeoWorld::Flags(double x, double y) const
{
    const int col = static_cast<int>(std::floor((x - m_grid.Origin.x) / m_grid.Cell)), row = static_cast<int>(std::floor((y - m_grid.Origin.y) / m_grid.Cell));
    return col >= 0 && col < m_grid.Columns && row >= 0 && row < m_grid.Rows ? m_bits[row * m_grid.Columns + col] : -1;
}

const GeoWorld::Building* GeoWorld::BuildingById(const std::string& id) const
{
    const auto it = m_buildingIds.find(id);
    return it == m_buildingIds.end() ? nullptr : &m_buildings[it->second];
}

const GeoWorld::Road* GeoWorld::RoadById(const std::string& id) const
{
    const auto it = m_roadIds.find(id);
    return it == m_roadIds.end() ? nullptr : &m_roads[it->second];
}

const GeoWorld::Building* GeoWorld::BuildingAt(double x, double y) const
{
    const auto it = m_buckets.find(BucketKey(static_cast<int64_t>(std::floor(x / BUCKET)), static_cast<int64_t>(std::floor(y / BUCKET))));
    if (it != m_buckets.end())
        for (int b : it->second)
            if (Inside(m_buildings[b].Footprint, x, y)) return &m_buildings[b];
    return nullptr;
}

std::vector<std::string> GeoWorld::BuildingsNear(double x, double y, double radius) const
{
    std::vector<std::string> out;
    for (int b : NearBuildings({ { x - radius, y - radius }, { x + radius, y + radius } })) {
        const std::vector<NavXY>& r = m_buildings[b].Footprint;
        bool near = Inside(r, x, y);
        for (size_t i = 0; i < r.size() && !near; ++i) near = SegmentDistance(x, y, r[i], r[(i + 1) % r.size()]) <= radius;
        if (near) out.push_back(m_buildings[b].Id);
    }
    return out;
}

GeoWorld::Condition GeoWorld::ConditionOf(const std::string& id) const
{
    const auto it = m_state.Conditions.find(id);
    return it == m_state.Conditions.end() ? Condition::Intact : it->second;
}

double GeoWorld::Damage(const std::string& id) const
{
    const auto it = m_state.Damage.find(id);
    return it == m_state.Damage.end() ? 0.0 : it->second;
}

std::vector<std::pair<std::string, std::string>> GeoWorld::ChangesSince(int revision)
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const Change& c : m_changes)
        if (c.Revision > revision) out.push_back({ c.Id, ConditionName(c.State) });
    return out;
}

// --- mutable conditions --------------------------------------------------------------------

bool GeoWorld::SetCondition(const std::string& id, Condition state)
{
    const auto bi = m_buildingIds.find(id);
    const auto ri = m_roadIds.find(id);
    if (bi == m_buildingIds.end() && ri == m_roadIds.end()) throw std::out_of_range(id);
    if (ConditionOf(id) == state) return false;
    if (state == Condition::Intact) m_state.Conditions.erase(id);
    else m_state.Conditions[id] = state;
    if (bi != m_buildingIds.end()) RefreshBuilding(bi->second);
    else if (m_roads[ri->second].Bridge) RefreshBridges();
    m_changes.push_back({ ++m_revision, id, state });
    return true;
}

bool GeoWorld::SetDamage(const std::string& id, double amount)
{
    if (!BuildingById(id)) throw std::out_of_range(id);
    amount = PyRound(std::min(1.0, std::max(0.0, amount)), 3);
    if (Damage(id) == amount) return false;
    if (amount != 0.0) m_state.Damage[id] = amount;
    else m_state.Damage.erase(id);
    ++m_revision;
    return true;
}

void GeoWorld::RefreshBuilding(int index)
{
    // BUILDING / RUBBLE on its cells from every building covering them (they overlap sometimes: a standing neighbour keeps its cells blocked).
    const Cells& cells = m_cells[index];
    const std::unordered_set<int> mine(cells.begin(), cells.end());
    std::unordered_set<int> standing, rubble;
    for (int o : NearBuildings(m_buildings[index].Footprint)) {
        const Condition c = ConditionOf(m_buildings[o].Id);
        if (c == Condition::Cleared) continue;
        for (int i : m_cells[o])
            if (mine.count(i)) (c == Condition::Destroyed ? rubble : standing).insert(i);
    }
    const int n = m_grid.Columns * m_grid.Rows;
    for (int i : cells)
        if (i >= 0 && i < n) m_bits[i] = static_cast<uint8_t>(m_bits[i] & ~(BUILDING | RUBBLE) | (standing.count(i) ? BUILDING : rubble.count(i) ? RUBBLE : 0));
}

void GeoWorld::RefreshBridges()
{
    // BRIDGE bits from every bridge that is not destroyed (bridges are few: all of them).
    for (const Cells& cells : m_bridgeCells)
        for (int i : cells) m_bits[i] &= static_cast<uint8_t>(~BRIDGE);
    for (size_t r = 0; r < m_roads.size(); ++r)
        if (m_roads[r].Bridge && RoadOpen(m_roads[r].Id))
            for (int i : m_bridgeCells[r]) m_bits[i] |= BRIDGE;
}

int GeoWorld::Blast(double x, double y, double radius)
{
    int hit = 0;
    for (size_t i = 0; i < m_trees.size(); ++i) {
        const double d = std::hypot(m_trees[i].X - x, m_trees[i].Y - y);
        if (d >= radius) continue;
        const TreeState state = d < radius / 2 ? TreeState::Broken : TreeState::Burnt;
        const auto it = m_state.Trees.find(static_cast<int>(i));
        if (it == m_state.Trees.end() || (it->second != TreeState::Broken && it->second != state)) {
            m_state.Trees[static_cast<int>(i)] = state;
            ++hit;
        }
    }
    m_state.Scorches.push_back({ PyRound(x, 2), PyRound(y, 2), PyRound(radius / 3, 2) });
    m_blasts.push_back({ ++m_revision, x, y, radius });
    m_staticTops.reset();
    return hit;
}

void GeoWorld::SetTrees(std::map<int, TreeState> trees, std::vector<Scorch> scorches)
{
    std::vector<BlastSite> spots;   // a scorch is a third of its blast
    for (const std::vector<Scorch>* list : { &m_state.Scorches, &scorches })
        for (const Scorch& s : *list) spots.push_back({ 0, s.X, s.Y, 3 * s.Radius + 1.0 });
    if ((!m_state.Trees.empty() || !trees.empty()) && spots.empty()) spots.push_back({ 0, 0.0, 0.0, m_grid.Columns * m_grid.Cell });   // the whole map (slow)
    if (!spots.empty()) {   // the trunks and leaf bands round every old and new blast redone
        ++m_revision;
        for (BlastSite& s : spots) { s.Revision = m_revision; m_blasts.push_back(s); }
        m_staticTops.reset();
    }
    m_state.Trees = std::move(trees);
    m_state.Scorches = std::move(scorches);
}

GeoWorld::Mark GeoWorld::MakeMark(int kind, double x, double y, double length, double width, double rotation, double strength, double time)
{
    return { static_cast<double>(kind), PyRound(x, 2), PyRound(y, 2), PyRound(length, 2), PyRound(width, 2), PyRound(rotation, 1),
             PyRound(std::min(1.0, std::max(0.0, strength)), 3), PyRound(time, 1) };
}

std::vector<std::string> GeoWorld::LoadState(const State& s)
{
    std::vector<std::string> now;
    for (const auto& [id, c] : m_state.Conditions) now.push_back(id);
    for (const std::string& id : now) SetCondition(id, Condition::Intact);
    std::vector<std::string> unknown;
    for (const auto& [id, c] : s.Conditions) {
        if (m_buildingIds.count(id) || m_roadIds.count(id)) SetCondition(id, c);
        else unknown.push_back(id);
    }
    m_state.Damage.clear();
    for (const auto& [id, d] : s.Damage)
        if (m_buildingIds.count(id)) m_state.Damage[id] = d;
    ++m_revision;
    std::map<int, TreeState> trees;
    for (const auto& [i, st] : s.Trees)
        if (i < static_cast<int>(m_trees.size())) trees[i] = st;
    SetTrees(std::move(trees), s.Scorches);
    SetMarks(s.Marks);
    return unknown;
}

// --- line of sight -------------------------------------------------------------------------

double GeoWorld::BlockerHeight(const Building& b) const
{
    const Condition c = ConditionOf(b.Id);
    return c == Condition::Destroyed ? RUBBLE_HEIGHT : c == Condition::Cleared ? 0.0 : b.Height;
}

double GeoWorld::Trunk(int i) const
{
    // How high tree i's trunk blocks sight: to its crown base, BROKEN_TRUNK of its height when snapped.
    const Tree& t = m_trees[i];
    const auto it = m_state.Trees.find(i);
    return it != m_state.Trees.end() && it->second == TreeState::Broken && t.Base > 0 ? t.Height * BROKEN_TRUNK : t.Base;
}

const std::unordered_map<int, double>& GeoWorld::StaticTops()
{
    // What buildings never change: walls (their cells) and tree trunks (the trunk's cell, up to the crown; the crown
    // itself is foliage, which hides without blocking). Blasts reset it.
    if (!m_staticTops) {
        std::unordered_map<int, double> tops;
        const int n = m_grid.Columns * m_grid.Rows;
        for (const Line& w : m_walls)
            for (int i : LineCells(w.Points, m_grid.Cell)) tops[i] = std::max(tops[i], w.Height);
        for (size_t k = 0; k < m_trees.size(); ++k) {
            const Tree& t = m_trees[k];
            const int col = static_cast<int>(std::floor((t.X - m_grid.Origin.x) / m_grid.Cell)), row = static_cast<int>(std::floor((t.Y - m_grid.Origin.y) / m_grid.Cell));
            const int i = row * m_grid.Columns + col;
            if (t.Base > 0 && i >= 0 && i < n) tops[i] = std::max(tops[i], Trunk(static_cast<int>(k)));
        }
        m_staticTops = std::move(tops);
    }
    return *m_staticTops;
}

std::vector<float> GeoWorld::BlockerHeights()
{
    std::vector<float> out(static_cast<size_t>(m_grid.Columns) * m_grid.Rows, 0.0f);
    for (const auto& [i, h] : StaticTops()) out[i] = static_cast<float>(h);
    for (size_t b = 0; b < m_buildings.size(); ++b) {
        const double h = BlockerHeight(m_buildings[b]);
        for (int i : m_cells[b])
            if (i >= 0 && i < static_cast<int>(out.size())) out[i] = static_cast<float>(std::max(static_cast<double>(out[i]), h));
    }
    return out;
}

std::map<double, std::vector<int>> GeoWorld::Tops(const std::vector<int>& cells, const std::vector<NavXY>& area)
{
    // {blocker height: cells} for these cells: walls and trunks, and the buildings in the buckets the area touches.
    const auto& fixed = StaticTops();
    std::unordered_map<int, double> top;
    for (int i : cells) top[i] = fixed.count(i) ? fixed.at(i) : 0.0;   // a wall or tree on the cell still stands
    for (int o : NearBuildings(area)) {
        const double h = BlockerHeight(m_buildings[o]);
        for (int i : m_cells[o]) {
            const auto it = top.find(i);
            if (it != top.end()) it->second = std::max(it->second, h);
        }
    }
    std::map<double, std::vector<int>> byHeight;
    for (const auto& [i, h] : top) byHeight[h].push_back(i);
    return byHeight;
}

std::pair<const std::vector<float>&, const std::vector<float>&> GeoWorld::Foliage(std::optional<Box> box)
{
    const size_t n = static_cast<size_t>(m_grid.Columns) * m_grid.Rows;
    if (!box || m_lows.empty()) {
        m_lows.assign(n, 0.0f);
        m_highs.assign(n, 0.0f);
        box.reset();
    }
    std::unordered_set<int> only;
    if (box) {
        const Box& b = *box;
        for (int i : PolygonCells({ { { b.X0, b.Y0 }, { b.X1, b.Y0 }, { b.X1, b.Y1 }, { b.X0, b.Y1 } } })) {
            only.insert(i);
            m_lows[i] = m_highs[i] = 0.0f;
        }
    }
    auto near = [&](const std::vector<NavXY>& pts, double r) {
        if (!box) return true;
        double x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
        for (const NavXY& p : pts) { x0 = std::min(x0, p.x); x1 = std::max(x1, p.x); y0 = std::min(y0, p.y); y1 = std::max(y1, p.y); }
        return x1 + r >= box->X0 && x0 - r <= box->X1 && y1 + r >= box->Y0 && y0 - r <= box->Y1;
    };
    auto band = [&](int i, double low, double high) {
        if (box && !only.count(i)) return;
        m_lows[i] = static_cast<float>(m_highs[i] == 0 ? low : std::min(static_cast<double>(m_lows[i]), low));
        m_highs[i] = static_cast<float>(std::max(static_cast<double>(m_highs[i]), high));
    };
    for (size_t k = 0; k < m_trees.size(); ++k) {
        const Tree& t = m_trees[k];
        if (!m_state.Trees.count(static_cast<int>(k)) && near({ { t.X, t.Y } }, t.Crown))   // a broken or burnt tree has no leaves
            for (int i : LineCells({ { t.X, t.Y }, { t.X, t.Y } }, 2 * t.Crown)) band(i, t.Base, t.Height);
    }
    for (const Line& h : m_hedges)
        if (near(h.Points, m_grid.Cell / 2))   // its cells reach half a cell past the line
            for (int i : LineCells(h.Points, m_grid.Cell)) band(i, 0.0, h.Height);
    for (const auto& ring : m_tallGrass)
        if (near(ring, 0.0))
            for (int i : PolygonCells({ ring }))
                if (m_highs[i] == 0) band(i, 0.0, GRASS_HEIGHT);
    return { m_lows, m_highs };
}

SightGrid GeoWorld::MakeSightGrid()
{
    SightGrid s;
    s.SetGround(m_terrain.Columns, m_terrain.Rows, static_cast<float>(m_terrain.Cell), { static_cast<float>(m_terrain.Origin.x), static_cast<float>(m_terrain.Origin.y) }, m_heights);
    s.SetBlockers(m_grid.Columns, m_grid.Rows, static_cast<float>(m_grid.Cell), { static_cast<float>(m_grid.Origin.x), static_cast<float>(m_grid.Origin.y) }, BlockerHeights());
    const auto [lows, highs] = Foliage();
    s.SetFoliage(lows, highs);
    return s;
}

int GeoWorld::UpdateSight(const SightSink& sink, int revision)
{
    std::vector<BlastSite> blasts;
    for (const BlastSite& b : m_blasts)
        if (b.Revision > revision) blasts.push_back(b);
    double crown = m_grid.Cell;   // how far past a blast its leaf bands reach: the widest crown and a cell
    for (const Tree& t : m_trees) crown = std::max(crown, t.Crown + m_grid.Cell);
    for (const BlastSite& b : blasts) {   // the trunks within it, the leaf bands round it
        const double x = b.X, y = b.Y, r = b.Radius;
        for (const auto& [h, ids] : Tops(LineCells({ { x, y }, { x, y } }, 2 * r), { { x - r, y - r }, { x + r, y - r }, { x + r, y + r }, { x - r, y + r } }))
            sink.SetBlockerCells(ids, h);
        Foliage(Box{ x - r - crown, y - r - crown, x + r + crown, y + r + crown });
    }
    if (!blasts.empty() && sink.SetFoliage) sink.SetFoliage(m_lows, m_highs);
    for (const Change& c : m_changes) {
        const auto it = c.Revision > revision ? m_buildingIds.find(c.Id) : m_buildingIds.end();
        if (it == m_buildingIds.end()) continue;
        for (const auto& [h, ids] : Tops(m_cells[it->second], m_buildings[it->second].Footprint)) sink.SetBlockerCells(ids, h);
    }
    return m_revision;
}

int GeoWorld::UpdateSight(SightGrid& sight, int revision)
{
    return UpdateSight({ [&](const std::vector<int>& cells, double h) { sight.SetBlockerCells(cells, static_cast<float>(h)); },
                         [&](const std::vector<float>& lows, const std::vector<float>& highs) { sight.SetFoliage(lows, highs); } }, revision);
}

std::vector<float> GeoWorld::TerrainCorners() const
{
    std::vector<float> corners;
    const int cols = m_terrain.Columns;
    for (int r = 0; r < m_terrain.Rows; ++r)
        for (int c = 0; c < cols; ++c) {
            const size_t i = static_cast<size_t>(r) * (cols + 1) + c;
            corners.insert(corners.end(), { m_heights[i], m_heights[i + 1], m_heights[i + cols + 2], m_heights[i + cols + 1] });
        }
    return corners;
}

} // namespace IYI
