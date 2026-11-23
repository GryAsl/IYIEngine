// Implements navigation (Nav.h): A* over the grid (8 neighbours, no corner cutting, then string
// pulling) and over the road graph (many starts and goals), and the planner's worker thread.

#include "IYI/Nav.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>

namespace IYI {

void BuildNavLinks(NavGraph& graph)
{
    graph.Links.assign(graph.Nodes.size(), {});
    for (int e = 0; e < static_cast<int>(graph.Edges.size()); ++e) {
        graph.Links[graph.Edges[e].A].push_back(e);
        graph.Links[graph.Edges[e].B].push_back(e);
    }
}

// --- grid --------------------------------------------------------------------------------------

namespace {

using Open = std::priority_queue<std::pair<float, int>, std::vector<std::pair<float, int>>, std::greater<>>;

// Price of one cell per metre, 0 when closed.
float CellCost(const NavGrid& g, const NavGridQuery& q, int i)
{
    if (!q.Avoid.empty() && std::binary_search(q.Avoid.begin(), q.Avoid.end(), i)) return 0.0f;
    float c = q.Costs[g.Base[i]];
    for (int b = 0, x = g.Extra[i]; x && c > 0.0f; ++b, x >>= 1)
        if (x & 1) c *= q.ExtraCosts[b];
    return c;
}

// The cell's price when it and every cell within Clearance are open, else 0.
float StepCost(const NavGrid& g, const NavGridQuery& q, int col, int row)
{
    if (col < 0 || row < 0 || col >= g.Columns || row >= g.Rows) return 0.0f;
    const float c = CellCost(g, q, row * g.Columns + col);
    for (int dy = -q.Clearance; dy <= q.Clearance && c > 0.0f; ++dy)
        for (int dx = -q.Clearance; dx <= q.Clearance; ++dx) {
            const int x = col + dx, y = row + dy;
            if (x < 0 || y < 0 || x >= g.Columns || y >= g.Rows || CellCost(g, q, y * g.Columns + x) <= 0.0f) return 0.0f;
        }
    return c;
}

// The straight line between two cell centres crosses only open cells no dearer than `limit`.
bool Clear(const NavGrid& g, const NavGridQuery& q, int a, int b, float limit)
{
    const float ax = a % g.Columns + 0.5f, ay = a / g.Columns + 0.5f, bx = b % g.Columns + 0.5f, by = b / g.Columns + 0.5f;
    const int steps = static_cast<int>(std::ceil(std::max(std::fabs(bx - ax), std::fabs(by - ay)) * 4.0f));
    for (int s = 1; s < steps; ++s) {
        const float t = static_cast<float>(s) / steps;
        const float c = StepCost(g, q, static_cast<int>(ax + (bx - ax) * t), static_cast<int>(ay + (by - ay) * t));
        if (c <= 0.0f || c > limit) return false;
    }
    return true;
}

} // namespace

NavResult FindNavGridPath(const NavGrid& g, const NavGridQuery& q)
{
    NavResult r;
    const int n = g.Columns * g.Rows;
    auto cellOf = [&](Vector2 p) {
        const int c = static_cast<int>(std::floor((p.x - g.Origin.x) / g.Cell)), w = static_cast<int>(std::floor((p.y - g.Origin.y) / g.Cell));
        return c < 0 || w < 0 || c >= g.Columns || w >= g.Rows ? -1 : w * g.Columns + c;
    };
    const int start = cellOf(q.Start), goal = cellOf(q.Goal);
    if (n <= 0 || start < 0 || goal < 0 || static_cast<int>(g.Base.size()) != n || StepCost(g, q, goal % g.Columns, goal / g.Columns) <= 0.0f)
        return r;

    float minCost = 1e30f;
    for (float c : q.Costs) if (c > 0.0f) minCost = std::min(minCost, c);
    for (float c : q.ExtraCosts) if (c > 0.0f) minCost *= std::min(1.0f, c);

    // Scratch per thread, reused between searches: a generation stamp marks what this search wrote.
    static thread_local std::vector<float> G;
    static thread_local std::vector<uint8_t> From;       // the step (0..7) that reached the cell
    static thread_local std::vector<uint16_t> Stamp;
    static thread_local uint16_t gen = 0;
    if (static_cast<int>(G.size()) != n) { G.assign(n, 0.0f); From.assign(n, 0); Stamp.assign(n, 0); gen = 0; }
    if (++gen == 0) { std::fill(Stamp.begin(), Stamp.end(), uint16_t{ 0 }); gen = 1; }

    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 }, DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    const int gc = goal % g.Columns, gr = goal / g.Columns;
    auto heuristic = [&](int c, int w) {
        const float dx = static_cast<float>(std::abs(c - gc)), dy = static_cast<float>(std::abs(w - gr));
        return (std::max(dx, dy) + 0.41421356f * std::min(dx, dy)) * g.Cell * minCost;
    };
    Open open;
    G[start] = 0.0f; Stamp[start] = gen; From[start] = 255;
    open.push({ heuristic(start % g.Columns, start / g.Columns), start });
    bool found = false;
    while (!open.empty() && r.Expanded < q.MaxNodes) {
        const auto [f, i] = open.top();
        open.pop();
        const int c = i % g.Columns, w = i / g.Columns;
        if (f > G[i] + heuristic(c, w) + 1e-2f) continue;   // a stale entry
        if (i == goal) { found = true; break; }
        ++r.Expanded;
        const float here = i == start ? StepCost(g, q, c, w) : CellCost(g, q, i);
        for (int d = 0; d < 8; ++d) {
            const int nc = c + DX[d], nw = w + DY[d];
            const float cost = StepCost(g, q, nc, nw);
            if (cost <= 0.0f) continue;
            if (d >= 4 && (StepCost(g, q, c + DX[d], w) <= 0.0f || StepCost(g, q, c, w + DY[d]) <= 0.0f)) continue;
            const int j = nw * g.Columns + nc;
            const float step = (d < 4 ? 1.0f : 1.41421356f) * g.Cell * 0.5f * ((here > 0.0f ? here : cost) + cost);
            const float ng = G[i] + step;
            if (Stamp[j] == gen && ng >= G[j]) continue;
            G[j] = ng; Stamp[j] = gen; From[j] = static_cast<uint8_t>(d);
            open.push({ ng + heuristic(nc, nw), j });
        }
    }
    if (!found) return r;

    std::vector<int> cells{ goal };
    while (cells.back() != start) {
        const int i = cells.back(), d = From[i];
        cells.push_back(i - DY[d] * g.Columns - DX[d]);
    }
    std::reverse(cells.begin(), cells.end());

    // String pulling: from each kept cell, the farthest later cell in a straight, no dearer line.
    std::vector<int> kept{ cells.front() };
    for (size_t a = 0; a + 1 < cells.size();) {
        size_t best = a + 1;
        float limit = CellCost(g, q, cells[a + 1]);
        for (size_t j = a + 2, misses = 0; j < cells.size() && j < a + 160 && misses < 8; ++j) {
            limit = std::max(limit, CellCost(g, q, cells[j - 1]));
            if (Clear(g, q, cells[a], cells[j], limit)) { best = j; misses = 0; }
            else ++misses;
        }
        kept.push_back(cells[best]);
        a = best;
    }
    r.Found = true;
    r.Cost = G[goal];
    r.Points.push_back(q.Start);
    for (size_t k = 1; k + 1 < kept.size(); ++k)
        r.Points.push_back({ g.Origin.x + (kept[k] % g.Columns + 0.5f) * g.Cell, g.Origin.y + (kept[k] / g.Columns + 0.5f) * g.Cell });
    r.Points.push_back(q.Goal);
    return r;
}

// --- graph -------------------------------------------------------------------------------------

NavResult FindNavGraphPath(const NavGraph& graph, const NavGraphQuery& q)
{
    NavResult r;
    const int n = static_cast<int>(graph.Nodes.size());
    if (q.Starts.empty() || q.Goals.empty() || static_cast<int>(graph.Links.size()) != n) return r;
    float minCost = 1e30f;
    for (float c : q.KindCosts) if (c > 0.0f) minCost = std::min(minCost, c);
    if (minCost > 1e29f) return r;
    std::vector<float> goalCost(n, -1.0f), g(n, 1e30f);
    std::vector<int> via(n, -1);
    for (auto [node, cost] : q.Goals)
        if (node >= 0 && node < n && (goalCost[node] < 0.0f || cost < goalCost[node])) goalCost[node] = cost;
    auto heuristic = [&](int i) {
        float h = 1e30f;
        for (auto [node, cost] : q.Goals)
            if (node >= 0 && node < n) h = std::min(h, std::hypot(graph.Nodes[i].x - graph.Nodes[node].x, graph.Nodes[i].y - graph.Nodes[node].y));
        return h * minCost;
    };
    Open open;
    for (auto [node, cost] : q.Starts)
        if (node >= 0 && node < n && cost < g[node]) { g[node] = cost; open.push({ cost + heuristic(node), node }); }
    float best = 1e30f;
    int end = -1;
    while (!open.empty() && r.Expanded < q.MaxNodes) {
        const auto [f, i] = open.top();
        open.pop();
        if (f >= best) break;
        if (f > g[i] + heuristic(i) + 1e-2f) continue;
        ++r.Expanded;
        if (goalCost[i] >= 0.0f && g[i] + goalCost[i] < best) { best = g[i] + goalCost[i]; end = i; }
        for (int e : graph.Links[i]) {
            const NavEdge& edge = graph.Edges[e];
            const float k = edge.Kind >= 0 && edge.Kind < static_cast<int>(q.KindCosts.size()) ? q.KindCosts[edge.Kind] : 0.0f;
            if (!edge.Open || k <= 0.0f || edge.Clear < q.Width || (edge.Oneway && edge.A != i)) continue;
            const int j = edge.A == i ? edge.B : edge.A;
            const float ng = g[i] + edge.Length * k + edge.Hazard * q.HazardWeight + edge.Delay + edge.Tight * q.TightWeight + (j == q.Avoid ? q.AvoidCost : 0.0f);
            if (ng >= g[j]) continue;
            g[j] = ng; via[j] = e;
            open.push({ ng + heuristic(j), j });
        }
    }
    if (end < 0) return r;
    r.Found = true;
    r.Cost = best;
    for (int i = end;;) {
        r.Nodes.push_back(i);
        const int e = via[i];
        if (e < 0) break;
        r.Edges.push_back(e);
        i = graph.Edges[e].A == i ? graph.Edges[e].B : graph.Edges[e].A;
    }
    std::reverse(r.Nodes.begin(), r.Nodes.end());
    std::reverse(r.Edges.begin(), r.Edges.end());
    for (int i : r.Nodes) r.Points.push_back(graph.Nodes[i]);
    return r;
}

// --- planner -----------------------------------------------------------------------------------

struct NavJob
{
    int Ticket = 0;
    std::shared_ptr<const NavGrid> Grid;     // set for a grid query
    std::shared_ptr<const NavGraph> Graph;   // set for a graph query
    NavGridQuery GridQuery;
    NavGraphQuery GraphQuery;
};

struct NavWorker
{
    std::mutex Mutex;
    std::condition_variable Wake, Done;
    std::deque<NavJob> Jobs;
    std::vector<NavResult> Results;
    int Busy = 0;
    bool Quit = false;
    std::vector<std::thread> Threads;   // a few searches at once; Poll sorts the answers by ticket, so the order never depends on them

    static NavResult Plan(const NavJob& job)
    {
        NavResult r = job.Grid ? FindNavGridPath(*job.Grid, job.GridQuery) : FindNavGraphPath(*job.Graph, job.GraphQuery);
        r.Ticket = job.Ticket;
        return r;
    }

    void Push(NavJob job)
    {
#ifdef __EMSCRIPTEN__
        Results.push_back(Plan(job));   // the browser build has no threads: planned at once, Poll finds it done
#else
        {
            std::lock_guard lock(Mutex);
            Jobs.push_back(std::move(job));
        }
        Wake.notify_one();
#endif
    }

    void Run()
    {
        std::unique_lock lock(Mutex);
        for (;;) {
            Wake.wait(lock, [&] { return Quit || !Jobs.empty(); });
            if (Quit) return;
            NavJob job = std::move(Jobs.front());
            Jobs.pop_front();
            ++Busy;
            lock.unlock();
            NavResult r = Plan(job);
            lock.lock();
            Results.push_back(std::move(r));
            --Busy;
            Done.notify_all();
        }
    }
};

NavPlanner::NavPlanner() : m_worker(std::make_unique<NavWorker>())
{
#ifndef __EMSCRIPTEN__
    const int n = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 3, 1, 4);   // leave the game, render and nav threads a core
    for (int k = 0; k < n; ++k) m_worker->Threads.emplace_back([w = m_worker.get()] { w->Run(); });
#endif
}

NavPlanner::~NavPlanner()
{
    if (m_worker->Threads.empty()) return;   // the browser build never started any
    {
        std::lock_guard lock(m_worker->Mutex);
        m_worker->Quit = true;
    }
    m_worker->Wake.notify_all();
    for (auto& t : m_worker->Threads) t.join();
}

void NavPlanner::InitGrid(int columns, int rows, float cell, Vector2 origin)
{
    if (columns <= 0 || rows <= 0 || cell <= 0.0f) throw std::invalid_argument("NavPlanner: columns, rows and cell must be > 0");
    const size_t n = static_cast<size_t>(columns) * rows;
    Grid = { columns, rows, cell, origin, std::vector<uint8_t>(n), std::vector<uint8_t>(n) };
}

void NavPlanner::SetBase(const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != Grid.Base.size()) throw std::invalid_argument("NavPlanner.set_base: one byte per cell");
    Grid.Base = bytes;
    GridChanged = true;
}

static void CheckCell(const NavGrid& g, int i)
{
    if (i < 0 || i >= static_cast<int>(g.Base.size())) throw std::out_of_range("NavPlanner: cell " + std::to_string(i) + " is off the grid");
}

void NavPlanner::SetBaseCells(const std::vector<int>& cells, const std::vector<uint8_t>& bytes)
{
    if (bytes.size() != cells.size()) throw std::invalid_argument("NavPlanner.set_base_cells: one byte per cell");
    for (size_t k = 0; k < cells.size(); ++k) { CheckCell(Grid, cells[k]); Grid.Base[cells[k]] = bytes[k]; }
    GridChanged = true;
}

void NavPlanner::SetExtra(const std::vector<int>& cells, int bit, bool on)
{
    if (bit < 0 || bit > 7) throw std::invalid_argument("NavPlanner.set_extra: bit 0..7");
    for (int i : cells) {
        CheckCell(Grid, i);
        Grid.Extra[i] = static_cast<uint8_t>(on ? Grid.Extra[i] | 1 << bit : Grid.Extra[i] & ~(1 << bit));
    }
    GridChanged = true;
}

std::pair<int, int> NavPlanner::Cell(int i) const
{
    CheckCell(Grid, i);
    return { Grid.Base[i], Grid.Extra[i] };
}

void NavPlanner::SetGraph(std::vector<Vector2> nodes, const std::vector<NavEdge>& edges)
{
    for (const NavEdge& e : edges)
        if (e.A < 0 || e.B < 0 || e.A >= static_cast<int>(nodes.size()) || e.B >= static_cast<int>(nodes.size()))
            throw std::out_of_range("NavPlanner.set_graph: edge node out of range");
    Graph = { std::move(nodes), edges, {} };
    GraphChanged = true;
}

void NavPlanner::SetEdge(int edge, bool open, float clear, float hazard, float delay)
{
    if (edge < 0 || edge >= static_cast<int>(Graph.Edges.size())) throw std::out_of_range("NavPlanner.set_edge: no edge " + std::to_string(edge));
    NavEdge& e = Graph.Edges[edge];
    e.Open = open; e.Clear = clear; e.Hazard = hazard; e.Delay = delay;
    GraphChanged = true;
}

int NavPlanner::FindGrid(Vector2 start, Vector2 goal, const std::vector<float>& costs, const std::vector<float>& extraCosts, int clearance, int maxNodes)
{
    if (costs.size() != 256 || extraCosts.size() != 8) throw std::invalid_argument("NavPlanner.find_grid: 256 costs and 8 extra costs");
    NavGridQuery q;
    q.Start = start;
    q.Goal = goal;
    std::copy(costs.begin(), costs.end(), q.Costs.begin());
    std::copy(extraCosts.begin(), extraCosts.end(), q.ExtraCosts.begin());
    q.Clearance = clearance;
    q.MaxNodes = maxNodes;
    return Request(q);
}

int NavPlanner::Request(const NavGridQuery& query)
{
    if (GridChanged || !m_grid) { m_grid = std::make_shared<const NavGrid>(Grid); GridChanged = false; }
    NavJob job;
    job.Ticket = m_nextTicket++;
    job.Grid = m_grid;
    job.GridQuery = query;
    m_worker->Push(std::move(job));
    return m_nextTicket - 1;
}

int NavPlanner::Request(const NavGraphQuery& query)
{
    if (GraphChanged || !m_graph) {
        NavGraph copy = Graph;
        BuildNavLinks(copy);
        m_graph = std::make_shared<const NavGraph>(std::move(copy));
        GraphChanged = false;
    }
    NavJob job;
    job.Ticket = m_nextTicket++;
    job.Graph = m_graph;
    job.GraphQuery = query;
    m_worker->Push(std::move(job));
    return m_nextTicket - 1;
}

std::vector<NavResult> NavPlanner::Poll(bool wait)
{
    std::unique_lock lock(m_worker->Mutex);
    if (wait) m_worker->Done.wait(lock, [&] { return m_worker->Jobs.empty() && m_worker->Busy == 0; });
    std::vector<NavResult> out = std::move(m_worker->Results);
    m_worker->Results.clear();
    std::sort(out.begin(), out.end(), [](const NavResult& a, const NavResult& b) { return a.Ticket < b.Ticket; });
    return out;
}

int NavPlanner::Pending() const
{
    std::lock_guard lock(m_worker->Mutex);
    return static_cast<int>(m_worker->Jobs.size()) + m_worker->Busy + static_cast<int>(m_worker->Results.size());
}

} // namespace IYI
