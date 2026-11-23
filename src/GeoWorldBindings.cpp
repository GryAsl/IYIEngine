// Python face of GeoWorld (libraries/geo_world/src/World.h): engine.geo_world, which geo_world/world.py wraps.
// The world dict `w` stays what callers read: world.json's keys plus the mutable part (condition, damage,
// revision, changes, tree_state, scorches, blasts, marks), kept in step with the C++ truth after each change;
// the grid bits are one bytearray the C++ writes into. w["_cpp"] is the GeoWorld. All pybind11 here (rule 14).

#include "geo_world/src/World.h"
#include <array>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace IYI {

namespace {

GeoWorld& G(const py::dict& w) { return w["_cpp"].cast<GeoWorld&>(); }

py::object FloatArray(const std::vector<float>& v)
{
    return py::module_::import("array").attr("array")("f", py::bytes(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(float)));
}

py::list ScorchList(const GeoWorld::Scorch& s)
{
    py::list out;
    out.append(s.X); out.append(s.Y); out.append(s.Radius);
    return out;
}

py::list MarkList(const GeoWorld::Mark& m)
{
    py::list out;
    out.append(static_cast<int>(m[0]));
    for (int i = 1; i < 8; ++i) out.append(m[i]);
    return out;
}

// The dict's copy of what a change touched.
void MirrorChanges(const py::dict& w, GeoWorld& g)
{
    w["revision"] = g.Revision();
    py::list changes = w["changes"];
    for (size_t i = changes.size(); i < g.Changes().size(); ++i) {
        const GeoWorld::Change& c = g.Changes()[i];
        changes.append(py::make_tuple(c.Revision, c.Id, GeoWorld::ConditionName(c.State)));
    }
}

void MirrorTrees(const py::dict& w, GeoWorld& g)
{
    w["revision"] = g.Revision();
    py::dict trees = w["tree_state"];
    trees.clear();
    for (const auto& [i, st] : g.TreeStates()) trees[py::int_(i)] = st == GeoWorld::TreeState::Broken ? "broken" : "burnt";
    py::list scorches = w["scorches"];
    scorches.attr("clear")();
    for (const GeoWorld::Scorch& s : g.Scorches()) scorches.append(ScorchList(s));
    py::list blasts = w["blasts"];
    for (size_t i = blasts.size(); i < g.Blasts().size(); ++i) {
        const GeoWorld::BlastSite& b = g.Blasts()[i];
        blasts.append(py::make_tuple(b.Revision, b.X, b.Y, b.Radius));
    }
}

void SetMarks(const py::dict& w, const py::iterable& marks)
{
    py::list list;
    for (py::handle m : marks) list.append(py::list(py::reinterpret_borrow<py::object>(m)));
    w["marks"] = list;
    w["marks_gen"] = w["marks_gen"].cast<int>() + 1;
    if (w.contains("_cpp")) {
        std::vector<GeoWorld::Mark> out;
        for (py::handle m : list) {
            GeoWorld::Mark mark;
            for (int i = 0; i < 8; ++i) mark[i] = m[py::int_(i)].cast<double>();
            out.push_back(mark);
        }
        G(w).SetMarks(std::move(out));
    }
}

GeoWorld::Condition ParseState(const py::handle& s)
{
    const auto c = GeoWorld::ParseCondition(s.cast<std::string>());
    if (!c) throw py::value_error("unknown condition " + py::repr(s).cast<std::string>());
    return *c;
}

std::map<int, GeoWorld::TreeState> TreeStates(const py::dict& trees)
{
    std::map<int, GeoWorld::TreeState> out;
    for (auto [k, v] : trees) out[k.cast<int>()] = v.cast<std::string>() == "broken" ? GeoWorld::TreeState::Broken : GeoWorld::TreeState::Burnt;
    return out;
}

std::vector<GeoWorld::Scorch> Scorches(const py::iterable& list)
{
    std::vector<GeoWorld::Scorch> out;
    for (py::handle s : list) out.push_back({ s[py::int_(0)].cast<double>(), s[py::int_(1)].cast<double>(), s[py::int_(2)].cast<double>() });
    return out;
}

} // namespace

void RegisterGeoWorldBindings(py::module_& m)
{
    py::module_ gw = m.def_submodule("geo_world", "geo_world library C++ (libraries/geo_world/src/World.h)");

    // GeoWorld(path, world.json bytes, heights bytes, grid bytearray): the bytearray becomes the grid bits the C++ writes into.
    py::class_<GeoWorld>(gw, "World")
        .def(py::init([](const std::string& path, const py::bytes& json, const py::bytes& heights, py::bytearray grid) {
            auto g = std::make_unique<GeoWorld>(path, static_cast<std::string>(json), static_cast<std::string>(heights), static_cast<std::string>(grid));
            g->ShareBits(reinterpret_cast<uint8_t*>(PyByteArray_AsString(grid.ptr())));
            return g;
        }), py::arg("path"), py::arg("json"), py::arg("heights"), py::arg("grid"), py::keep_alive<1, 5>());

    gw.def("height", [](const py::dict& w, double x, double y) { return G(w).Height(x, y); });
    gw.def("flags", [](const py::dict& w, double x, double y) -> std::optional<int> {
        const int f = G(w).Flags(x, y);
        return f < 0 ? std::nullopt : std::optional<int>(f);
    });
    gw.def("building_at", [](const py::dict& w, double x, double y) -> std::optional<std::string> {
        const GeoWorld::Building* b = G(w).BuildingAt(x, y);
        return b ? std::optional<std::string>(b->Id) : std::nullopt;
    });
    gw.def("buildings_near", [](const py::dict& w, double x, double y, double radius) { return G(w).BuildingsNear(x, y, radius); });
    gw.def("to_lonlat", [](const py::dict& w, double x, double y) { const NavXY p = G(w).ToLonLat(x, y); return py::make_tuple(p.x, p.y); });
    gw.def("from_lonlat", [](const py::dict& w, double lon, double lat) { const NavXY p = G(w).FromLonLat(lon, lat); return py::make_tuple(p.x, p.y); });

    gw.def("set_condition", [](const py::dict& w, const std::string& oid, const std::string& state) {
        const auto c = GeoWorld::ParseCondition(state);
        if (!c) throw py::value_error("unknown condition " + py::repr(py::str(state)).cast<std::string>());
        GeoWorld& g = G(w);
        if (!g.BuildingById(oid) && !g.RoadById(oid)) throw py::key_error(oid);
        if (!g.SetCondition(oid, *c)) return false;
        py::dict conditions = w["condition"];
        if (*c == GeoWorld::Condition::Intact) conditions.attr("pop")(oid);
        else conditions[py::str(oid)] = state;
        MirrorChanges(w, g);
        return true;
    });
    gw.def("set_damage", [](const py::dict& w, const std::string& oid, double amount) {
        GeoWorld& g = G(w);
        if (!g.BuildingById(oid)) throw py::key_error(oid);
        if (!g.SetDamage(oid, amount)) return false;
        py::dict damage = w["damage"];
        if (const double d = g.Damage(oid); d != 0.0) damage[py::str(oid)] = d;
        else damage.attr("pop")(oid);
        w["revision"] = g.Revision();
        return true;
    });
    gw.def("blast", [](const py::dict& w, double x, double y, double radius) {
        GeoWorld& g = G(w);
        const int hit = g.Blast(x, y, radius);
        MirrorTrees(w, g);
        return hit;
    });
    gw.def("set_trees", [](const py::dict& w, const py::dict& trees, const py::iterable& scorches) {
        GeoWorld& g = G(w);
        g.SetTrees(TreeStates(trees), Scorches(scorches));
        MirrorTrees(w, g);
    });
    // marks work on a bare dict too (a game's self-check): only the dict's list is the truth there
    gw.def("add_mark", [](const py::dict& w, int kind, double x, double y, double length, double width, double rotation, double strength, double time) {
        const GeoWorld::Mark mark = GeoWorld::MakeMark(kind, x, y, length, width, rotation, strength, time);
        py::list marks = w["marks"];
        marks.append(MarkList(mark));
        if (w.contains("_cpp")) G(w).AddMark(mark);
    });
    gw.def("set_marks", &SetMarks);

    gw.def("save_state", [](const py::dict& w) {
        GeoWorld& g = G(w);
        py::dict d;
        d["format"] = "geo_world"; d["package"] = g.Name(); d["source_hash"] = g.SourceHash();
        py::dict conditions, damage, trees;
        for (const auto& [id, c] : g.Conditions()) conditions[py::str(id)] = GeoWorld::ConditionName(c);
        for (const auto& [id, v] : g.DamageMap()) damage[py::str(id)] = v;
        for (const auto& [i, st] : g.TreeStates()) trees[py::str(std::to_string(i))] = st == GeoWorld::TreeState::Broken ? "broken" : "burnt";
        py::list scorches, marks;
        for (const GeoWorld::Scorch& s : g.Scorches()) scorches.append(ScorchList(s));
        for (const GeoWorld::Mark& mk : g.Marks()) marks.append(MarkList(mk));
        d["condition"] = conditions; d["damage"] = damage; d["tree_state"] = trees; d["scorches"] = scorches; d["marks"] = marks;
        return d;
    });
    gw.def("load_state", [](const py::dict& w, const py::dict& data) {
        GeoWorld& g = G(w);
        if (!data.contains("format") || data["format"].cast<std::string>() != "geo_world" || !data.contains("package") || data["package"].cast<std::string>() != g.Name())
            throw py::value_error("save state is not for this package");
        GeoWorld::State st;
        for (auto [id, state] : py::dict(data["condition"])) st.Conditions[id.cast<std::string>()] = ParseState(state);
        const py::dict none;
        for (auto [id, d] : data.contains("damage") ? py::dict(data["damage"]) : none) st.Damage[id.cast<std::string>()] = d.cast<double>();
        const py::object builtin_int = py::module_::import("builtins").attr("int");
        if (data.contains("tree_state"))
            for (auto [i, state] : py::dict(data["tree_state"])) st.Trees[builtin_int(i).cast<int>()] = state.cast<std::string>() == "broken" ? GeoWorld::TreeState::Broken : GeoWorld::TreeState::Burnt;
        if (data.contains("scorches")) st.Scorches = Scorches(data["scorches"]);
        py::list marks = data.contains("marks") ? py::list(data["marks"]) : py::list();
        for (py::handle mk : marks) {
            GeoWorld::Mark mark;
            for (int i = 0; i < 8; ++i) mark[i] = mk[py::int_(i)].cast<double>();
            st.Marks.push_back(mark);
        }
        const std::vector<std::string> unknown = g.LoadState(st);
        py::dict conditions = w["condition"], damage = w["damage"];
        conditions.clear(); damage.clear();
        for (const auto& [id, c] : g.Conditions()) conditions[py::str(id)] = GeoWorld::ConditionName(c);
        for (const auto& [id, v] : g.DamageMap()) damage[py::str(id)] = v;
        MirrorChanges(w, g);
        MirrorTrees(w, g);
        py::list list;
        for (py::handle mk : marks) list.append(py::list(py::reinterpret_borrow<py::object>(mk)));
        w["marks"] = list;
        w["marks_gen"] = w["marks_gen"].cast<int>() + 1;
        return unknown;
    });

    gw.def("blocker_heights", [](const py::dict& w) { return FloatArray(G(w).BlockerHeights()); });
    gw.def("foliage", [](const py::dict& w, std::optional<std::array<double, 4>> box) {
        const auto [lows, highs] = G(w).Foliage(box ? std::optional<GeoWorld::Box>(GeoWorld::Box{ (*box)[0], (*box)[1], (*box)[2], (*box)[3] }) : std::nullopt);
        return py::make_tuple(FloatArray(lows), FloatArray(highs));
    }, py::arg("w"), py::arg("box") = py::none());
    gw.def("terrain_corners", [](const py::dict& w) { return G(w).TerrainCorners(); });
    // sight: anything with set_blocker_cells(cells, height), and set_foliage(lows, highs) for blasts
    gw.def("update_sight", [](const py::dict& w, const py::object& sight, int revision) {
        GeoWorld::SightSink sink;
        sink.SetBlockerCells = [&](const std::vector<int>& cells, double h) { sight.attr("set_blocker_cells")(cells, h); };
        if (py::hasattr(sight, "set_foliage"))
            sink.SetFoliage = [&](const std::vector<float>& lows, const std::vector<float>& highs) { sight.attr("set_foliage")(FloatArray(lows), FloatArray(highs)); };
        return G(w).UpdateSight(sink, revision);
    });
}

} // namespace IYI
