// Exposes the navigation simulation (NavSim.h) to Python as engine.Nav, built from a geo_world world
// dict, with typed Walker, Vehicle and Heli objects in dict-like containers (nav.walkers[id], in
// insertion order). The world dict stays the map's truth: the bindings read its grid bits,
// conditions and changes when the navigation asks (PyNavWorld). Points are (x, y) tuples.
// Also the living-world pieces of Life.h: engine.Scheduler, EventLog, Facts, Population.

#include "IYI/Life.h"
#include "IYI/NavSim.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

namespace pybind11::detail {

template <>
struct type_caster<IYI::NavXY>
{
    PYBIND11_TYPE_CASTER(IYI::NavXY, const_name("tuple[float, float]"));
    bool load(handle src, bool)
    {
        if (!src || !PySequence_Check(src.ptr()) || PyUnicode_Check(src.ptr()) || PySequence_Size(src.ptr()) != 2) return false;
        sequence s = reinterpret_borrow<sequence>(src);
        value = { s[0].cast<double>(), s[1].cast<double>() };
        return true;
    }
    static handle cast(const IYI::NavXY& p, return_value_policy, handle) { return py::make_tuple(p.x, p.y).release(); }
};

} // namespace pybind11::detail

namespace IYI {

namespace {

// The game's geo_world dict, read live.
class PyNavWorld : public NavWorld
{
public:
    explicit PyNavWorld(py::dict w) : m_w(std::move(w)) {}
    const uint8_t* Bits() override { return reinterpret_cast<const uint8_t*>(PyByteArray_AsString(m_w["grid_bits"].ptr())); }
    bool Destroyed(const std::string& id) override
    {
        py::dict condition = m_w["condition"];
        return condition.contains(id) && condition[py::str(id)].cast<std::string>() == "destroyed";
    }
    int Revision() override { return m_w["revision"].cast<int>(); }
    std::vector<std::pair<std::string, std::string>> ChangesSince(int revision) override
    {
        std::vector<std::pair<std::string, std::string>> out;
        for (py::handle c : m_w["changes"])
            if (c[py::int_(0)].cast<int>() > revision) out.push_back({ c[py::int_(1)].cast<std::string>(), c[py::int_(2)].cast<std::string>() });
        return out;
    }

private:
    py::dict m_w;
};

NavMap MapFrom(py::dict w)
{
    NavMap m;
    py::dict g = w["grid"], t = w["terrain"];
    m.Columns = g["columns"].cast<int>();
    m.Rows = g["rows"].cast<int>();
    m.Cell = g["cell"].cast<double>();
    m.Origin = g["origin"].cast<NavXY>();
    m.TerrainColumns = t["columns"].cast<int>();
    m.TerrainRows = t["rows"].cast<int>();
    m.TerrainCell = t["cell"].cast<double>();
    m.TerrainOrigin = t["origin"].cast<NavXY>();
    py::buffer_info heights = py::buffer(w["heights"]).request();
    if (heights.itemsize != 4) throw py::value_error("Nav: heights must be float32");
    const float* h = static_cast<const float*>(heights.ptr);
    m.Heights.assign(h, h + heights.size);
    std::unordered_map<std::string, int> index;
    for (auto [id, p] : py::dict(w["nodes"])) {
        index[id.cast<std::string>()] = static_cast<int>(m.NodeIds.size());
        m.NodeIds.push_back(id.cast<std::string>());
        m.Nodes.push_back(p.cast<NavXY>());
    }
    for (py::handle r : w["roads"])
        m.Roads.push_back({ r["id"].cast<std::string>(), r["kind"].cast<std::string>(), index.at(r["a"].cast<std::string>()), index.at(r["b"].cast<std::string>()),
                            r["length"].cast<double>(), r["width"].cast<double>(), r["oneway"].cast<bool>(), r["bridge"].cast<bool>(),
                            r["points"].cast<std::vector<NavXY>>() });
    for (py::handle b : w["buildings"])
        m.Buildings.push_back({ b["id"].cast<std::string>(), b["footprint"].cast<std::vector<NavXY>>(), b["boxes"].cast<std::vector<std::array<double, 5>>>() });
    return m;
}

py::object ToPython(const NavValue& v, const py::object& round)
{
    switch (v.kind) {
    case NavValue::Bool: return py::bool_(v.flag);
    case NavValue::Int: return py::int_(v.integer);
    case NavValue::Real: return v.digits < 0 ? py::object(py::float_(v.real)) : round(v.real, v.digits);
    case NavValue::Text: return py::str(v.text);
    case NavValue::Reals: return py::cast(v.reals);
    case NavValue::Json: return py::module_::import("json").attr("loads")(v.text);
    default: return py::none();
    }
}

NavValue FromPython(py::handle o)
{
    NavValue v;
    if (o.is_none()) return v;
    if (py::isinstance<py::bool_>(o)) { v.kind = NavValue::Bool; v.flag = o.cast<bool>(); }
    else if (py::isinstance<py::int_>(o)) { v.kind = NavValue::Int; v.integer = o.cast<int64_t>(); }
    else if (py::isinstance<py::float_>(o)) { v.kind = NavValue::Real; v.real = o.cast<double>(); }
    else if (py::isinstance<py::str>(o)) { v.kind = NavValue::Text; v.text = o.cast<std::string>(); }
    else {
        try { v.reals = o.cast<std::vector<double>>(); v.kind = NavValue::Reals; }
        catch (const py::cast_error&) { v.kind = NavValue::Json; v.text = py::module_::import("json").attr("dumps")(o).cast<std::string>(); }
    }
    return v;
}

py::object Route(const NavRoute& r)
{
    py::dict d;
    d["found"] = r.Found; d["points"] = r.Points; d["speeds"] = r.Speeds; d["edges"] = r.Edges;
    py::list nodes;
    for (const auto& [s, n] : r.Nodes) nodes.append(py::make_tuple(s, n));
    d["nodes"] = nodes; d["mode"] = r.Mode; d["cost"] = r.Cost;
    return d;
}

// nav.walkers / vehicles / helis: a dict-like view of the shown copies (NavSim::Shown*).
template <class T>
struct Units
{
    std::shared_ptr<NavSim> nav;
    NavUnits<T> NavSim::*shown;
    NavUnits<T> NavSim::*live;
    NavUnits<T>& u() const { return (*nav).*shown; }
};

template <class T>
void BindUnits(py::module_& m, const char* name)
{
    using U = Units<T>;
    py::class_<U>(m, name)
        .def("__len__", [](const U& s) { return s.u().Size(); })
        .def("__contains__", [](const U& s, const std::string& id) { return s.u().Get(id) != nullptr; })
        .def("__getitem__", [](const U& s, const std::string& id) {
            auto p = s.u().Shared(id);
            if (!p) throw py::key_error(id);
            return p;
        })
        .def("__setitem__", [](const U& s, const std::string& id, std::shared_ptr<T> unit) {   // a unit popped earlier goes back (last if new)
            if (unit->Id != id) throw py::value_error("Nav: the key must be the unit's id");
            s.nav->Join();
            ((*s.nav).*s.live).Put(unit->Live ? unit->Live : unit);
            s.nav->Publish();
        })
        .def("get", [](const U& s, const std::string& id, py::object fallback) -> py::object {
            auto p = s.u().Shared(id);
            return p ? py::cast(p) : fallback;
        }, py::arg("id"), py::arg("default") = py::none())
        .def("pop", [](const U& s, const std::string& id, py::object fallback) -> py::object {
            s.nav->Join();
            auto p = s.u().Shared(id);
            ((*s.nav).*s.live).Pop(id);
            s.nav->Publish();
            if (p) return py::cast(p);
            if (fallback.is(py::ellipsis())) throw py::key_error(id);
            return fallback;
        }, py::arg("id"), py::arg("default") = py::ellipsis())
        .def("keys", [](const U& s) { std::vector<std::string> out; for (const auto& p : s.u().All()) out.push_back(p->Id); return out; })
        .def("values", [](const U& s) { return s.u().All(); })
        .def("items", [](const U& s) { std::vector<std::pair<std::string, std::shared_ptr<T>>> out; for (const auto& p : s.u().All()) out.push_back({ p->Id, p }); return out; })
        .def("__iter__", [](const U& s) { std::vector<std::string> out; for (const auto& p : s.u().All()) out.push_back(p->Id); return py::iter(py::cast(out)); });
}

// A field of a shown unit: Get reads the copy; Set writes the copy and (queued while the nav thread steps) the live unit.
template <class T, class V>
auto Get(V T::*m) { return [m](const T& u) { return u.*m; }; }

template <class T, class V>
auto Set(V T::*m)
{
    return [m](T& u, V value) {
        const auto sim = u.Sim.lock();
        if (sim) sim->template Write<T>(u, [m, value](T& x) { x.*m = value; });
        else u.*m = value;
    };
}

// The nav after waiting for its step: what a call that changes or asks the live state needs.
NavSim& Live(NavSim& n)
{
    n.Join();
    return n;
}

py::dict EventDict(const WorldEvent& e, const py::object& round)
{
    py::dict d;
    d["id"] = e.Id; d["time"] = round(e.Time, 2); d["kind"] = e.Kind; d["x"] = round(e.X, 1); d["y"] = round(e.Y, 1); d["cause"] = e.Cause;
    for (const auto& [k, v] : e.Data) d[py::str(k)] = ToPython(v, round);
    return d;
}

py::list EventList(const std::vector<const WorldEvent*>& events)
{
    const py::object round = py::module_::import("builtins").attr("round");
    py::list out;
    for (const WorldEvent* e : events) out.append(EventDict(*e, round));
    return out;
}

py::dict FactDict(const Fact& f)
{
    py::dict d;
    d["id"] = f.Id; d["owner"] = f.Owner; d["kind"] = f.Kind; d["subject"] = f.Subject; d["x"] = f.X; d["y"] = f.Y;
    d["time"] = f.Time; d["confidence"] = f.Confidence; d["source"] = f.Source;
    return d;
}

py::list FactList(const std::vector<Fact>& facts)
{
    py::list out;
    for (const Fact& f : facts) out.append(FactDict(f));
    return out;
}

// engine.Population(zones, nav, ...): zones is the parsed zones.json (geo_world FORMAT.md).
std::shared_ptr<Population> MakePopulation(py::dict z, NavSim& nav, int64_t seed, int walkers, int cars)
{
    std::vector<LifeZone> zones;
    for (py::handle h : z["zones"]) {
        py::dict d = py::reinterpret_borrow<py::dict>(h);
        LifeZone zone;
        zone.Id = d["id"].cast<std::string>(); zone.Name = d["name"].cast<std::string>();
        zone.X = d["x"].cast<double>(); zone.Y = d["y"].cast<double>();
        zone.Homes = d["homes"].cast<int>(); zone.People = d["people"].cast<double>();
        zone.Markets = d["markets"].cast<std::vector<NavXY>>(); zone.Mosques = d["mosques"].cast<std::vector<NavXY>>();
        zone.Schools = d["schools"].cast<std::vector<NavXY>>(); zone.Buildings = d["buildings"].cast<std::vector<std::string>>();
        zones.push_back(std::move(zone));
    }
    Population::Grid grid{ z["cell"].cast<double>(), z["origin"].cast<NavXY>(), z["columns"].cast<int>(), z["rows"].cast<int>(), z["grid"].cast<std::vector<int>>() };
    nav.Join();
    auto pop = std::make_shared<Population>(std::move(zones), std::move(grid), nav, seed, walkers, cars);
    nav.Publish();
    return pop;
}

int Ticket(const py::object& t) { return t.is_none() ? 0 : t.cast<int>(); }
py::object Ticket(int t) { return t ? py::object(py::int_(t)) : py::none(); }

} // namespace

void RegisterNavSimBindings(py::module_& m)
{
    py::class_<Py::Random>(m, "PyRandom")
        .def("random", &Py::Random::random)
        .def("uniform", &Py::Random::uniform, py::arg("a"), py::arg("b"));

    py::class_<NavWalker, std::shared_ptr<NavWalker>>(m, "NavWalker")
        .def_readonly("id", &NavWalker::Id).def_readonly("cls", &NavWalker::Cls)
        .def_property("ticket", [](const NavWalker& w) { return Ticket(w.Ticket); }, [](NavWalker& w, py::object t) {
            const int k = Ticket(t);
            const auto sim = w.Sim.lock();
            if (sim) sim->Write<NavWalker>(w, [k](NavWalker& x) { x.Ticket = k; });
            else w.Ticket = k;
        })
        .def_property_readonly("rng", [](NavWalker& w) -> Py::Random& {     // the live unit's own stream (waits for the step)
            if (const auto sim = w.Sim.lock()) sim->Join();
            return w.Live ? w.Live->Rng : w.Rng;
        }, py::return_value_policy::reference_internal)
        .def_property("side", Get(&NavWalker::Side), Set(&NavWalker::Side))
        .def_property("x", Get(&NavWalker::X), Set(&NavWalker::X))
        .def_property("y", Get(&NavWalker::Y), Set(&NavWalker::Y))
        .def_property("state", Get(&NavWalker::State), Set(&NavWalker::State))
        .def_property("path", Get(&NavWalker::Path), Set(&NavWalker::Path))
        .def_property("i", Get(&NavWalker::I), Set(&NavWalker::I))
        .def_property("goal", Get(&NavWalker::Goal), Set(&NavWalker::Goal))
        .def_property("run", Get(&NavWalker::Run), Set(&NavWalker::Run))
        .def_property("react", Get(&NavWalker::React), Set(&NavWalker::React))
        .def_property("panic", Get(&NavWalker::Panic), Set(&NavWalker::Panic))
        .def_property("calm_at", Get(&NavWalker::CalmAt), Set(&NavWalker::CalmAt))
        .def_property("building", Get(&NavWalker::Building), Set(&NavWalker::Building))
        .def_property("then", Get(&NavWalker::Then), Set(&NavWalker::Then))
        .def_property("steady", Get(&NavWalker::Steady), Set(&NavWalker::Steady))
        .def_property("pace", Get(&NavWalker::Pace), Set(&NavWalker::Pace));

    py::class_<NavVehicle, std::shared_ptr<NavVehicle>>(m, "NavVehicle")
        .def_readonly("id", &NavVehicle::Id).def_readonly("cls", &NavVehicle::Cls)
        .def_readonly("len", &NavVehicle::Len).def_readonly("wid", &NavVehicle::Wid)
        .def_readonly("path", &NavVehicle::Path).def_readonly("s", &NavVehicle::S).def_readonly("seg", &NavVehicle::Seg).def_readonly("lat", &NavVehicle::Lat)
        .def_property_readonly("ticket", [](const NavVehicle& v) { return Ticket(v.Ticket); })
        .def_property_readonly("rng", [](NavVehicle& v) -> Py::Random& {
            if (const auto sim = v.Sim.lock()) sim->Join();
            return v.Live ? v.Live->Rng : v.Rng;
        }, py::return_value_policy::reference_internal)
        .def_property_readonly("why", [](const NavVehicle& v) -> py::dict {      // debugging: what holds it (the junction it waits for, its last blocker)
            py::dict d; d["blocked_by"] = v.BlockedBy; d["blocked"] = v.Blocked; d["queued"] = v.Queued; d["still"] = v.Still; d["wait_for"] = v.WaitFor; d["wait_walker"] = v.WaitWalker;
            d["junction"] = v.JNode ? py::object(py::make_tuple(*v.JNode, v.JGranted, v.JIn, v.JOut)) : py::object(py::none());
            return d;
        })
        .def_property("side", Get(&NavVehicle::Side), Set(&NavVehicle::Side))
        .def_property("x", Get(&NavVehicle::X), Set(&NavVehicle::X))
        .def_property("y", Get(&NavVehicle::Y), Set(&NavVehicle::Y))
        .def_property("h", Get(&NavVehicle::H), Set(&NavVehicle::H))
        .def_property("vel", Get(&NavVehicle::Vel), Set(&NavVehicle::Vel))
        .def_property("state", Get(&NavVehicle::State), Set(&NavVehicle::State))
        .def_property("mode", Get(&NavVehicle::Mode), Set(&NavVehicle::Mode))
        .def_property("doctrine", Get(&NavVehicle::Doctrine), Set(&NavVehicle::Doctrine))
        .def_property("priority", Get(&NavVehicle::Priority), Set(&NavVehicle::Priority))
        .def_property("alarm", Get(&NavVehicle::Alarm), Set(&NavVehicle::Alarm))
        .def_property("goal", Get(&NavVehicle::Goal), Set(&NavVehicle::Goal))
        .def_property("convoy", Get(&NavVehicle::Convoy), Set(&NavVehicle::Convoy));

    py::class_<NavHeli, std::shared_ptr<NavHeli>>(m, "NavHeli")
        .def_readonly("id", &NavHeli::Id).def_readonly("cls", &NavHeli::Cls).def_readonly("len", &NavHeli::Len).def_readonly("wid", &NavHeli::Wid)
        .def_property("x", Get(&NavHeli::X), Set(&NavHeli::X))
        .def_property("y", Get(&NavHeli::Y), Set(&NavHeli::Y))
        .def_property("z", Get(&NavHeli::Z), Set(&NavHeli::Z))
        .def_property("h", Get(&NavHeli::H), Set(&NavHeli::H))
        .def_property("vel", Get(&NavHeli::Vel), Set(&NavHeli::Vel))
        .def_property("state", Get(&NavHeli::State), Set(&NavHeli::State))
        .def_property("target", Get(&NavHeli::Target), Set(&NavHeli::Target))
        .def_property("land", Get(&NavHeli::Land), Set(&NavHeli::Land));

    BindUnits<NavWalker>(m, "NavWalkers");
    BindUnits<NavVehicle>(m, "NavVehicles");
    BindUnits<NavHeli>(m, "NavHelis");

    m.def("nav_classes", []() {
        py::dict out;
        for (const auto& [name, c] : NavClasses()) {
            py::dict d;
            d["kind"] = c.Kind; d["speed"] = c.Speed;
            if (c.Kind == "foot") { d["run"] = c.Run; d["radius"] = c.Radius; }
            else { d["accel"] = c.Accel; d["length"] = c.Length; d["width"] = c.Width; }
            if (c.Kind == "wheeled" || c.Kind == "tracked") { d["brake"] = c.Brake; d["turn"] = c.Turn; }
            if (c.Kind == "air") { d["climb"] = c.Climb; d["pad"] = c.Pad; }
            out[py::str(name)] = d;
        }
        return out;
    });
    m.def("nav_point_at", [](const std::vector<NavXY>& points, double s) {
        if (points.empty()) throw py::value_error("nav_point_at: no points");
        return NavPointAt(points, s);
    }, py::arg("points"), py::arg("s"));

    py::class_<NavSim, std::shared_ptr<NavSim>>(m, "Nav")
        .def(py::init([](py::dict w, int64_t seed, bool wait) {
            auto n = std::make_shared<NavSim>(MapFrom(w), std::make_unique<PyNavWorld>(w), seed, wait);
            n->Publish();
            return n;
        }), py::arg("world"), py::arg("seed") = 1, py::arg("wait") = false)
        .def_readonly_static("MAX_DT", &NavSim::MAX_DT).def_readonly_static("WALK_FAR", &NavSim::WALK_FAR).def_readonly_static("DRIVE_FAR", &NavSim::DRIVE_FAR)
        .def_readonly("time", &NavSim::ShownTime).def_readonly("seed", &NavSim::Seed)
        .def_property("wait", [](const NavSim& n) -> bool { return n.Wait; }, [](NavSim& n, bool w) { Live(n).Wait = w; })
        .def_property_readonly("revision", [](NavSim& n) -> int { return Live(n).Revision; })
        .def_readonly("culverts", &NavSim::Culverts)
        .def_property_readonly("node_ids", [](const NavSim& n) { return n.Map.NodeIds; })
        .def_property_readonly("kinds", [](const NavSim& n) { return n.Kinds(); })
        .def_property_readonly("pending", [](NavSim& n) -> int { return Live(n).Planner.Pending(); })
        .def_property_readonly("worker_ms", [](const NavSim& n) -> double { return n.ShownWorkerMs; })   // the last step's time on the nav thread
        .def_property_readonly("counts", [](const NavSim& n) -> py::dict {
            const auto& c = n.ShownCount;
            py::dict d; d["routes"] = c.Routes; d["vehicles"] = c.Vehicles; d["walkers"] = c.Walkers; d["near"] = c.Near; return d;
        })
        .def_property_readonly("walkers", [](std::shared_ptr<NavSim> n) { return Units<NavWalker>{ n, &NavSim::ShownWalkers, &NavSim::Walkers }; })
        .def_property_readonly("vehicles", [](std::shared_ptr<NavSim> n) { return Units<NavVehicle>{ n, &NavSim::ShownVehicles, &NavSim::Vehicles }; })
        .def_property_readonly("helis", [](std::shared_ptr<NavSim> n) { return Units<NavHeli>{ n, &NavSim::ShownHelis, &NavSim::Helis }; })
        .def("step", [](NavSim& n, double dt, std::optional<NavXY> focus) { Api::NavStep(n, dt, focus); }, py::arg("dt"), py::arg("focus") = py::none())
        .def("join", [](NavSim& n) { Live(n).Publish(); })   // waits for the step in flight and shows it (tests)
        .def("update", [](NavSim& n, double dt) { Live(n).Update(dt); n.Publish(); }, py::arg("dt"))
        .def("sync", [](NavSim& n) { Live(n).Sync(); })
        .def("poll", [](NavSim& n) { Live(n).Poll(); })
        .def("request", [](NavSim& n, NavXY start, NavXY goal, const std::string& cls, const std::string& mode, const std::string& side, std::optional<NavXY> heading) {
            return Live(n).Request(start, goal, cls, mode, side, heading);
        }, py::arg("start"), py::arg("goal"), py::arg("cls"), py::arg("mode") = "road", py::arg("side") = "military", py::arg("heading") = py::none())
        .def("take_result", [](NavSim& n, int ticket) -> py::object {
            auto r = Live(n).TakeResult(ticket);
            return r ? Route(*r) : py::none();
        }, py::arg("ticket"))
        .def("events", [](NavSim& n) {                     // the shown step's events (drained)
            const py::object round = py::module_::import("builtins").attr("round");
            py::list out;
            n.PublishEvents();
            for (const NavEvent& e : n.ShownEvents) {
                py::dict d;
                d["type"] = e.Type;
                d["time"] = round(e.Time, 2);
                for (const auto& [k, v] : e.Data) d[py::str(k)] = ToPython(v, round);
                out.append(d);
            }
            n.ShownEvents.clear();
            return out;
        })
        .def("emit", [](NavSim& n, const std::string& kind, py::kwargs data) {
            std::vector<std::pair<std::string, NavValue>> values;
            for (auto [k, v] : data) values.push_back({ k.cast<std::string>(), FromPython(v) });
            Live(n).Emit(kind, std::move(values));
        }, py::arg("kind"))
        .def("passable", [](NavSim& n, const std::string& cls, double x, double y) { return Live(n).Passable(cls, x, y); }, py::arg("cls"), py::arg("x"), py::arg("y"))
        .def("cell_index", &NavSim::CellIndex, py::arg("x"), py::arg("y"))
        .def("cell", [](NavSim& n, int i) {
            Live(n);
            if (i < 0 || i >= static_cast<int>(n.Base.size())) throw py::index_error("Nav.cell: off the grid");
            return std::make_pair(n.Base[i], n.Extra[i]);
        }, py::arg("i"))
        .def("open_near", [](NavSim& n, const std::string& cls, NavXY p, double reach) { return Live(n).OpenNear(cls, p, reach); },
             py::arg("cls"), py::arg("p"), py::arg("reach") = 12.0)
        .def("path_closed", [](NavSim& n, const std::string& cls, const std::vector<NavXY>& points, int start) { return Live(n).PathClosed(cls, points, start); },
             py::arg("cls"), py::arg("points"), py::arg("start") = 0)
        .def("set_obstacle", [](NavSim& n, const std::string& oid, const std::vector<std::array<double, 5>>& boxes, const std::string& kind) {
            Live(n).SetObstacle(oid, boxes, kind);
        }, py::arg("oid"), py::arg("boxes"), py::arg("kind") = "blocked")
        .def("clear_obstacle", [](NavSim& n, const std::string& oid) { Live(n).ClearObstacle(oid); }, py::arg("oid"))
        .def("obstacle", [](NavSim& n, const std::string& oid) -> py::object {
            auto it = Live(n).Obstacles.find(oid);
            if (it == n.Obstacles.end()) return py::none();
            py::dict d; d["cells"] = it->second.Cells; d["kind"] = it->second.Kind; d["boxes"] = it->second.Boxes;
            return d;
        }, py::arg("oid"))
        .def("report_hazard", [](NavSim& n, const std::string& hid, double x, double y, double radius, double severity, double duration, std::optional<std::string> source) {
            Live(n).ReportHazard(hid, x, y, radius, severity, duration, source);
        }, py::arg("hid"), py::arg("x"), py::arg("y"), py::arg("radius"), py::arg("severity") = 1.0, py::arg("duration") = 60.0, py::arg("source") = py::none())
        .def("clear_hazard", [](NavSim& n, const std::string& hid) { Live(n).ClearHazard(hid); }, py::arg("hid"))
        .def("snap", [](NavSim& n, double x, double y, double reach, bool openOnly) { return Live(n).Snap(x, y, reach, openOnly); },
             py::arg("x"), py::arg("y"), py::arg("reach") = 60.0, py::arg("open_only") = false)
        .def("edge", [](const NavSim& n, const std::string& rid) -> py::object {
            auto it = n.EdgeIndex().find(rid);
            return it == n.EdgeIndex().end() ? py::object(py::none()) : py::object(py::int_(it->second));
        }, py::arg("road_id"))
        .def("edge_state", [](NavSim& n, int e) {
            const auto& st = Live(n).Edges.at(e);
            return py::make_tuple(st.Open, st.Clear, st.Hazard, st.Delay);
        }, py::arg("edge"))
        .def("landing_site", [](NavSim& n, double x, double y, std::optional<double> pad, double search) { return Live(n).LandingSite(x, y, pad.value_or(0.0), search); },
             py::arg("x"), py::arg("y"), py::arg("pad") = py::none(), py::arg("search") = 250.0)
        .def("spawn_walker", [](NavSim& n, const std::string& id, const std::string& cls, double x, double y, std::optional<std::string> side) {
            Live(n).SpawnWalker(id, cls, x, y, side);
            n.PublishWalker(id);
            return n.ShownWalkers.Shared(id);
        }, py::arg("id"), py::arg("cls"), py::arg("x"), py::arg("y"), py::arg("side") = py::none())
        .def("walk", [](NavSim& n, const std::string& id, NavXY goal, bool run, std::optional<std::string> then) {
            Live(n).Walk(id, goal, run, then);
            n.PublishWalker(id);
        }, py::arg("id"), py::arg("goal"), py::arg("run") = false, py::arg("then") = py::none())
        .def("gather", [](NavSim& n, const std::vector<std::string>& ids, double x, double y, double radius) {
            Live(n).Gather(ids, x, y, radius);
            for (const auto& id : ids) n.PublishWalker(id);
        }, py::arg("ids"), py::arg("x"), py::arg("y"), py::arg("radius"))
        .def("wander", [](NavSim& n, std::vector<NavXY> points) { Live(n).Wander(std::move(points)); }, py::arg("points"))
        .def("spawn_vehicle", [](NavSim& n, const std::string& id, const std::string& cls, double x, double y, double heading, const std::string& side,
                                 std::optional<std::string> doctrine, std::optional<int> priority) {
            Live(n).SpawnVehicle(id, cls, x, y, heading, side, doctrine, priority);
            n.PublishVehicle(id);
            return n.ShownVehicles.Shared(id);
        }, py::arg("id"), py::arg("cls"), py::arg("x"), py::arg("y"), py::arg("heading") = 0.0, py::arg("side") = "civilian",
            py::arg("doctrine") = py::none(), py::arg("priority") = py::none())
        .def("drive", [](NavSim& n, const std::string& id, NavXY goal, const std::string& mode) {
            Live(n).Drive(id, goal, mode);
            n.PublishVehicle(id);
        }, py::arg("id"), py::arg("goal"), py::arg("mode") = "road")
        .def("convoy", [](NavSim& n, const std::string& cid, const std::vector<std::string>& ids, NavXY goal, double spacing) {
            Live(n).Convoy(cid, ids, goal, spacing);
            for (const auto& id : ids) n.PublishVehicle(id);
        }, py::arg("cid"), py::arg("ids"), py::arg("goal"), py::arg("spacing") = 25.0)
        .def("disable", [](NavSim& n, const std::string& id) { Live(n).Disable(id); n.PublishVehicle(id); }, py::arg("id"))
        .def("park", [](NavSim& n, const std::string& id) { Live(n).Park(id); n.PublishVehicle(id); }, py::arg("id"))
        .def("remove", [](NavSim& n, const std::string& id) {
            Live(n).Remove(id);
            n.PublishVehicle(id); n.PublishWalker(id); n.PublishHeli(id);
        }, py::arg("id"))
        .def("spawn_heli", [](NavSim& n, const std::string& id, double x, double y, const std::string& cls) {
            Live(n).SpawnHeli(id, x, y, cls);
            n.PublishHeli(id);
            return n.ShownHelis.Shared(id);
        }, py::arg("id"), py::arg("x"), py::arg("y"), py::arg("cls") = "heli")
        .def("fly_to", [](NavSim& n, const std::string& id, double x, double y, bool land) {
            auto r = Live(n).FlyTo(id, x, y, land);
            n.PublishHeli(id);
            return r;
        }, py::arg("id"), py::arg("x"), py::arg("y"), py::arg("land") = true);

    py::class_<Scheduler>(m, "Scheduler")
        .def(py::init<>())
        .def("at", &Scheduler::At, py::arg("id"), py::arg("time"))
        .def("cancel", &Scheduler::Cancel, py::arg("id"))
        .def("due", &Scheduler::Due, py::arg("now"), py::arg("budget") = 64)
        .def("items", &Scheduler::Items)
        .def("__len__", &Scheduler::Size);

    py::class_<EventLog>(m, "EventLog")
        .def(py::init<>())
        .def_readwrite("limit", &EventLog::Limit)
        .def_property_readonly("last", &EventLog::Last)
        .def("add", [](EventLog& log, const std::string& kind, double x, double y, double time, int64_t cause, py::kwargs data) {
            WorldEvent e{ 0, time, kind, x, y, cause, {} };
            for (auto [k, v] : data) e.Data.push_back({ k.cast<std::string>(), FromPython(v) });
            return log.Add(std::move(e));
        }, py::arg("kind"), py::arg("x"), py::arg("y"), py::arg("time"), py::arg("cause") = 0)
        .def("since", [](const EventLog& log, int64_t id) { return EventList(log.Since(id)); }, py::arg("id"))
        .def("near", [](const EventLog& log, double x, double y, double radius, double since, const std::string& kind) {
            return EventList(log.Near(x, y, radius, since, kind));
        }, py::arg("x"), py::arg("y"), py::arg("radius"), py::arg("since") = 0.0, py::arg("kind") = "")
        .def("get", [](const EventLog& log, int64_t id) -> py::object {
            const WorldEvent* e = log.Get(id);
            return e ? py::object(EventDict(*e, py::module_::import("builtins").attr("round"))) : py::object(py::none());
        }, py::arg("id"));

    py::class_<Facts>(m, "Facts")
        .def(py::init<>())
        .def_readwrite("half_life", &Facts::HalfLife)
        .def("tell", [](Facts& f, const std::string& owner, const std::string& kind, const std::string& subject, double x, double y, double time,
                        double confidence, const std::string& source) { return f.Tell({ 0, owner, kind, subject, x, y, time, confidence, source }); },
             py::arg("owner"), py::arg("kind"), py::arg("subject"), py::arg("x"), py::arg("y"), py::arg("time"), py::arg("confidence"), py::arg("source") = "")
        .def("near", [](const Facts& f, const std::string& owner, double x, double y, double radius, double now, const std::string& kind, double least) {
            return FactList(f.Near(owner, x, y, radius, now, kind, least));
        }, py::arg("owner"), py::arg("x"), py::arg("y"), py::arg("radius"), py::arg("now"), py::arg("kind") = "", py::arg("least") = 0.05)
        .def("about", [](const Facts& f, const std::string& owner, const std::string& subject, double now) { return FactList(f.About(owner, subject, now)); },
             py::arg("owner"), py::arg("subject"), py::arg("now"))
        .def("forget", &Facts::Forget, py::arg("owner"), py::arg("subject"), py::arg("kind") = "")
        .def("prune", &Facts::Prune, py::arg("now"), py::arg("least") = 0.02)
        .def("all", [](const Facts& f) { return FactList(f.All()); });

    py::class_<LifeZone>(m, "LifeZone")
        .def_readonly("id", &LifeZone::Id).def_readonly("name", &LifeZone::Name).def_readonly("x", &LifeZone::X).def_readonly("y", &LifeZone::Y)
        .def_readonly("homes", &LifeZone::Homes).def_readonly("people", &LifeZone::People)
        .def_readonly("markets", &LifeZone::Markets).def_readonly("mosques", &LifeZone::Mosques).def_readonly("schools", &LifeZone::Schools)
        .def_property_readonly("doors", [](const LifeZone& z) { return z.Doors.size(); })
        .def_readwrite("fear", &LifeZone::Fear).def_readwrite("support", &LifeZone::Support).def_readwrite("displaced", &LifeZone::Displaced);

    py::class_<Population, std::shared_ptr<Population>>(m, "Population")
        .def(py::init(&MakePopulation), py::arg("zones"), py::arg("nav"), py::arg("seed") = 1, py::arg("walkers") = 400, py::arg("cars") = 120)
        .def_readonly_static("FAR", &Population::FAR).def_readonly_static("NEAR", &Population::NEAR)
        .def_property_readonly("zones", [](py::object self) {
            Population& p = self.cast<Population&>();
            py::list out;
            for (LifeZone& z : p.Zones) out.append(py::cast(&z, py::return_value_policy::reference_internal, self));
            return out;
        })
        .def("zone_at", &Population::ZoneAt, py::arg("x"), py::arg("y"))
        .def("activity", &Population::Activity, py::arg("zone"), py::arg("hour"))
        .def("shock", &Population::Shock, py::arg("x"), py::arg("y"), py::arg("radius"), py::arg("fear"))
        .def("step", &Population::Step, py::arg("hours"))
        .def("manage", [](Population& p, NavSim& nav, NavXY focus, double hour) {
            const Population::Counts c = p.Manage(Live(nav), focus, hour);
            nav.Publish();
            py::dict d; d["out"] = c.Out; d["want"] = c.Want; d["cars"] = c.Cars; d["want_cars"] = c.WantCars; return d;
        }, py::arg("nav"), py::arg("focus"), py::arg("hour"))
        .def("summon", [](Population& p, NavSim& nav, int n, double x, double y, double radius) {
            auto out = p.Summon(Live(nav), n, x, y, radius);
            nav.Publish();
            return out;
        }, py::arg("nav"), py::arg("n"), py::arg("x"), py::arg("y"), py::arg("radius") = 150.0)
        .def("pin", [](Population& p, const std::string& id) { p.Pinned.insert(id); }, py::arg("id"))
        .def("unpin", [](Population& p, const std::string& id) { p.Pinned.erase(id); }, py::arg("id"))
        .def("pinned", [](const Population& p, const std::string& id) { return p.Pinned.count(id) > 0; }, py::arg("id"))
        .def("home", &Population::Home, py::arg("id"))
        .def_property_readonly("members", &Population::Members);
}

} // namespace IYI
