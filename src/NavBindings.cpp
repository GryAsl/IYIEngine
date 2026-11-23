// Exposes navigation (Nav.h) to Python as engine.NavPlanner: the grid's two layers, the road graph,
// route requests answered by the planner's worker thread, and polling for the answers.
// Game side only (Python's thread); the worker never touches Python. Points are (x, y) tuples.

#include "IYI/Nav.h"
#include <algorithm>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace IYI {

namespace {

py::list Points(const std::vector<Vector2>& points)
{
    py::list out;
    for (const Vector2& p : points) out.append(py::make_tuple(p.x, p.y));
    return out;
}

std::vector<uint8_t> Bytes(const py::bytes& data)
{
    const std::string s = data;
    return std::vector<uint8_t>(s.begin(), s.end());
}

} // namespace

void RegisterNavBindings(py::module_& m)
{
    py::class_<NavPlanner, std::unique_ptr<NavPlanner>>(m, "NavPlanner")
        .def(py::init([](int columns, int rows, float cell, Vector2 origin) {
            auto p = std::make_unique<NavPlanner>();
            p->InitGrid(columns, rows, cell, origin);
            return p;
        }), py::arg("columns"), py::arg("rows"), py::arg("cell"), py::arg("origin"))
        .def("set_base", [](NavPlanner& p, py::bytes data) { p.SetBase(Bytes(data)); }, py::arg("data"))
        .def("set_base_cells", [](NavPlanner& p, const std::vector<int>& cells, py::bytes data) { p.SetBaseCells(cells, Bytes(data)); }, py::arg("cells"), py::arg("data"))
        .def("set_extra", &NavPlanner::SetExtra, py::arg("cells"), py::arg("bit"), py::arg("on"))
        .def("cell", &NavPlanner::Cell, py::arg("i"))
        .def("set_graph", [](NavPlanner& p, const std::vector<std::pair<float, float>>& nodes, const std::vector<std::tuple<int, int, float, int, bool, float>>& edges) {
            std::vector<Vector2> points;
            std::vector<NavEdge> list;
            for (auto [x, y] : nodes) points.push_back({ x, y });
            for (auto [a, b, length, kind, oneway, width] : edges) list.push_back({ a, b, length, kind, oneway, true, width, 0.0f, 0.0f });
            p.SetGraph(std::move(points), list);
        }, py::arg("nodes"), py::arg("edges"))
        .def("set_edge", &NavPlanner::SetEdge, py::arg("edge"), py::arg("open"), py::arg("clear"), py::arg("hazard"), py::arg("delay"))
        .def("find_grid", [](NavPlanner& p, std::pair<float, float> start, std::pair<float, float> goal, const std::vector<float>& costs, const std::vector<float>& extra_costs, int clearance, int max_nodes) {
            return p.FindGrid({ start.first, start.second }, { goal.first, goal.second }, costs, extra_costs, clearance, max_nodes);
        }, py::arg("start"), py::arg("goal"), py::arg("costs"), py::arg("extra_costs"), py::arg("clearance") = 0, py::arg("max_nodes") = 500000)
        .def("find_graph", [](NavPlanner& p, const std::vector<std::pair<int, float>>& starts, const std::vector<std::pair<int, float>>& goals,
                              const std::vector<float>& kind_costs, float width, float hazard_weight, int max_nodes) -> int {
            NavGraphQuery q{ starts, goals, kind_costs, width, hazard_weight };
            q.MaxNodes = max_nodes;   // by name: positional init put it in a later-added float field
            return p.Request(q);
        }, py::arg("starts"), py::arg("goals"), py::arg("kind_costs"), py::arg("width") = 0.0f, py::arg("hazard_weight") = 1.0f,
            py::arg("max_nodes") = 200000)
        .def("poll", [](NavPlanner& p, bool wait) -> py::list {
            std::vector<NavResult> results;
            {
                py::gil_scoped_release release;
                results = p.Poll(wait);
            }
            py::list out;
            for (const NavResult& r : results) {
                py::dict d;
                d["ticket"] = r.Ticket; d["found"] = r.Found; d["cost"] = r.Cost; d["expanded"] = r.Expanded;
                d["points"] = Points(r.Points); d["nodes"] = r.Nodes; d["edges"] = r.Edges;
                out.append(d);
            }
            return out;
        }, py::arg("wait") = false)
        .def_property_readonly("pending", [](const NavPlanner& p) -> int { return p.Pending(); });
}

} // namespace IYI
