// Exposes SightGrid (Sight.h) to Python as engine.SightGrid. Heights come as any float32 buffer
// (array('f'), bytes, memoryview); points as engine.Vector3, except visible_pairs' plain tuples. Game side only.

#include "IYI/Sight.h"
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace IYI {

namespace {

std::vector<float> Floats(const py::buffer& data, const char* what)
{
    const py::buffer_info info = data.request();
    if (info.itemsize == 4 && info.format == py::format_descriptor<float>::format()) {
        const float* p = static_cast<const float*>(info.ptr);
        return std::vector<float>(p, p + info.size);
    }
    if (info.itemsize != 1 || info.size % 4) throw py::value_error(std::string(what) + ": float32 data expected");
    std::vector<float> out(static_cast<size_t>(info.size / 4));        // raw little-endian bytes
    std::memcpy(out.data(), info.ptr, static_cast<size_t>(info.size));
    return out;
}

template <typename F>
auto Checked(F&& f)
{
    try { return f(); }
    catch (const std::exception& e) { throw py::value_error(e.what()); }
}

} // namespace

void RegisterSightBindings(py::module_& m)
{
    py::class_<SightGrid, std::unique_ptr<SightGrid>>(m, "SightGrid")
        .def(py::init<>())
        .def("set_ground", [](SightGrid& g, int columns, int rows, float cell, Vector2 origin, py::buffer heights) {
            Checked([&] { g.SetGround(columns, rows, cell, origin, Floats(heights, "SightGrid.set_ground")); return 0; });
        }, py::arg("columns"), py::arg("rows"), py::arg("cell"), py::arg("origin"), py::arg("heights"))
        .def("set_blockers", [](SightGrid& g, int columns, int rows, float cell, Vector2 origin, py::buffer heights) {
            Checked([&] { g.SetBlockers(columns, rows, cell, origin, Floats(heights, "SightGrid.set_blockers")); return 0; });
        }, py::arg("columns"), py::arg("rows"), py::arg("cell"), py::arg("origin"), py::arg("heights"))
        .def("set_blocker_cells", [](SightGrid& g, const std::vector<int>& cells, float height) {
            Checked([&] { g.SetBlockerCells(cells, height); return 0; });
        }, py::arg("cells"), py::arg("height"))
        .def("ground", &SightGrid::Ground, py::arg("x"), py::arg("y"))
        .def("top", &SightGrid::Top, py::arg("x"), py::arg("y"))
        .def("set_foliage", [](SightGrid& g, py::buffer lows, py::buffer highs) {
            Checked([&] { g.SetFoliage(Floats(lows, "SightGrid.set_foliage"), Floats(highs, "SightGrid.set_foliage")); return 0; });
        }, py::arg("lows"), py::arg("highs"))
        .def("visible", [](const SightGrid& g, Vector3 a, Vector3 b) { return g.Visible(a, b); }, py::arg("a"), py::arg("b"))
        .def("foliage", [](const SightGrid& g, Vector3 a, Vector3 b) -> py::object {
            float f = 0.0f;
            return g.Visible(a, b, &f) ? py::cast(f) : py::none();
        }, py::arg("a"), py::arg("b"))
        .def("ray", [](const SightGrid& g, Vector3 origin, Vector3 direction, float max_distance) -> py::object {
            Vector3 hit;
            return g.Ray(origin, direction, max_distance, &hit) ? py::cast(hit) : py::none();
        }, py::arg("origin"), py::arg("direction"), py::arg("max_distance") = 100000.0f)
        .def("visible_pairs", [](const SightGrid& g, const std::vector<std::tuple<float, float, float, float>>& observers,
                                 const std::vector<std::tuple<float, float, float>>& targets) {
            std::vector<Vector4> o;
            std::vector<Vector3> t;
            for (const auto& [x, y, z, r] : observers) o.push_back({ x, y, z, r });
            for (const auto& [x, y, z] : targets) t.push_back({ x, y, z });
            py::list out;
            for (const SightPair& p : g.VisiblePairs(o, t)) out.append(py::make_tuple(p.Observer, p.Target, p.Distance));
            return out;
        }, py::arg("observers"), py::arg("targets"));
}

} // namespace IYI
