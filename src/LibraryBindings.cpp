// Python faces of the engine libraries' runtime C++ (libraries/<lib>/src, built into IYIEngineCore):
// engine.<lib> submodules that each library's Python module wraps thinly, so Python games and native
// games share one implementation (SPEC §2c). All pybind11 stays here (rule 14).

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "sprite_stack/src/Daylight.h"
#include "sprite_stack/src/Fx.h"
#include "sprite_stack/src/Post.h"
#include "rts_select/src/Select.h"
#include "profiler_overlay/src/Overlay.h"

namespace py = pybind11;

namespace IYI {

// select.py's items: (id, x, y, tier, boxable) tuples; the C++ works on their indices.
static std::vector<Select::Item> SelectItems(const py::list& items)
{
    std::vector<Select::Item> out;
    for (const py::handle t : items) {
        const py::tuple e = py::reinterpret_borrow<py::tuple>(t);
        out.push_back({ e[1].cast<float>(), e[2].cast<float>(), e[3].cast<int>(), e[4].cast<bool>() });
    }
    return out;
}

static void RegisterToolLibraries(py::module_& m)
{
    // rts_select: the selection state stays the caller's dict (keys selected, groups, drag_from, box,
    // recalled, recalled_at); each call reads the keys it uses into Select::State and writes them back.
    py::module_ rs = m.def_submodule("rts_select", "rts_select library C++ (select)");
    rs.def("drag", [](py::dict st, const py::tuple& at, bool pressed, bool down, bool released) -> py::object {
        const Vector2 mouse{ at[0].cast<float>(), at[1].cast<float>() };
        Select::State s;
        if (st.contains("drag_from") && !st["drag_from"].is_none()) {
            const py::tuple f = st["drag_from"].cast<py::tuple>();
            s.DragFrom = Vector2{ f[0].cast<float>(), f[1].cast<float>() };
        }
        const auto hit = Select::Drag(s, mouse, pressed, down, released);
        if (!s.DragFrom && !hit) return py::none();   // no drag going on: st untouched
        st["drag_from"] = s.DragFrom ? py::object(py::make_tuple(s.DragFrom->x, s.DragFrom->y)) : py::none();
        st["box"] = s.DragBox ? py::object(py::make_tuple((*s.DragBox)[0], (*s.DragBox)[1], (*s.DragBox)[2], (*s.DragBox)[3])) : py::none();
        if (!hit) return py::none();
        if (hit->IsBox) return py::make_tuple("box", py::make_tuple(hit->Area[0], hit->Area[1], hit->Area[2], hit->Area[3]));
        return py::make_tuple("click", py::make_tuple(hit->At.x, hit->At.y));
    });
    rs.def("project", [](const Camera3D& camera, const py::list& entries) {
        std::vector<Vector3> points;
        std::vector<int> tiers, index;
        std::vector<bool> boxable;
        for (const py::handle t : entries) {
            const py::tuple e = py::reinterpret_borrow<py::tuple>(t);
            points.push_back({ e[1].cast<float>(), e[2].cast<float>(), e[3].cast<float>() });
            tiers.push_back(e[4].cast<int>());
            boxable.push_back(e[5].cast<bool>());
        }
        const std::vector<Select::Item> items = Select::Project(camera, points, tiers, boxable, index);
        py::list out;
        for (size_t i = 0; i < items.size(); ++i) {
            out.append(py::make_tuple(entries[static_cast<size_t>(index[i])].cast<py::tuple>()[0], items[i].X, items[i].Y,
                                      entries[static_cast<size_t>(index[i])].cast<py::tuple>()[4], items[i].Boxable));
        }
        return out;
    });
    rs.def("pick", [](const py::list& items, const py::tuple& at, float reach) -> py::object {
        const int i = Select::Pick(SelectItems(items), Vector2{ at[0].cast<float>(), at[1].cast<float>() }, reach);
        return i < 0 ? py::none() : py::object(items[static_cast<size_t>(i)].cast<py::tuple>()[0]);
    });
    rs.def("in_box", [](const py::list& items, const py::tuple& box) {
        py::list out;
        const Select::Box b{ box[0].cast<float>(), box[1].cast<float>(), box[2].cast<float>(), box[3].cast<float>() };
        for (const int i : Select::InBox(SelectItems(items), b)) out.append(items[static_cast<size_t>(i)].cast<py::tuple>()[0]);
        return out;
    });
    rs.def("apply", [](py::dict st, const std::vector<std::string>& ids, bool add) {
        Select::State s;
        py::list sel = st.attr("setdefault")("selected", py::list()).cast<py::list>();
        s.Selected = sel.cast<std::vector<std::string>>();
        Select::Apply(s, ids, add);
        sel.attr("clear")();                     // in place, as before: callers may hold the list
        for (const std::string& id : s.Selected) sel.append(id);
    }, py::arg("st"), py::arg("ids"), py::arg("add") = false);
    rs.def("group", [](py::dict st, int n, bool assign, double now) {
        Select::State s;
        py::dict groups = st.attr("setdefault")("groups", py::dict()).cast<py::dict>();
        if (st.contains("selected")) s.Selected = st["selected"].cast<std::vector<std::string>>();
        for (const auto& [k, v] : groups) s.Groups[k.cast<int>()] = v.cast<std::vector<std::string>>();
        if (st.contains("recalled") && !st["recalled"].is_none()) s.Recalled = st["recalled"].cast<int>();
        if (st.contains("recalled_at")) s.RecalledAt = st["recalled_at"].cast<double>();
        const bool again = Select::Group(s, n, assign, now);
        if (assign) {
            groups[py::int_(n)] = py::cast(s.Groups[n]);
        } else if (s.Recalled == n && s.RecalledAt == now) {   // recalled: the group existed
            st["selected"] = py::cast(s.Selected);
            st["recalled"] = n;
            st["recalled_at"] = now;
        }
        return again;
    });

    // profiler_overlay: actor.py keeps its state (mode, x, y, gpu) and calls these.
    py::module_ po = m.def_submodule("profiler_overlay", "profiler_overlay library C++ (the F3 table)");
    po.def("update", [](int mode, bool gpu) { ProfilerOverlay::Update(mode, gpu); return py::make_tuple(mode, gpu); });
    po.def("draw", &ProfilerOverlay::Draw, py::arg("mode"), py::arg("x"), py::arg("y"), py::arg("gpu"));
}

void RegisterLibraryBindings(py::module_& m)
{
    RegisterToolLibraries(m);
    py::module_ ss = m.def_submodule("sprite_stack", "sprite_stack library C++ (daylight, fx, post)");
    // fx.py and post.py (Fx.h, Post.h). Fx.lighting is the engine.StackLighting that fx.py calls.
    py::class_<Fx>(ss, "Fx")
        .def(py::init([](const py::dict& lights, float ground_half, float light_half, float light_scale, float contact_strength,
                         float shadow_scale, float contact_scale, int sun_map, int contact_map, int light_map, int mark_map,
                         bool light_tops) {
            std::unordered_map<std::string, const SpriteStack*> sprites;
            for (const auto& [name, stack] : lights) sprites[name.cast<std::string>()] = stack.cast<const SpriteStack*>();
            return std::make_unique<Fx>(std::move(sprites), FxOptions{ ground_half, light_half, light_scale, contact_strength,
                shadow_scale, contact_scale, sun_map, contact_map, light_map, mark_map, light_tops });
        }), py::arg("lights"), py::arg("ground_half") = 640.0f, py::arg("light_half") = 1536.0f, py::arg("light_scale") = 0.5f,
            py::arg("contact_strength") = 0.6f, py::arg("shadow_scale") = 1.0f, py::arg("contact_scale") = 1.0f,
            py::arg("sun_map") = 0, py::arg("contact_map") = 0, py::arg("light_map") = 0, py::arg("mark_map") = 0,
            py::arg("light_tops") = false)
        .def_property_readonly("lighting", [](Fx& fx) -> StackLightingHandle& { return *fx.Lighting; },
                               py::return_value_policy::reference_internal)
        .def("set_ground_detail", &Fx::SetGroundDetail, py::arg("strength") = 0.15f, py::arg("wobble") = 0.5f)
        .def("set_marks", [](Fx& fx, const py::list& marks, int gen) {
            fx.SetMarks(gen, marks.size(), [&marks](size_t i) {
                const py::sequence m = marks[i].cast<py::sequence>();
                if (m.size() < 7) throw py::value_error("StackLighting.set_marks: mark = (kind, x, y, length, width, rotation, strength)");
                return StackMark{ m[0].cast<int>(), m[1].cast<float>(), m[2].cast<float>(), m[3].cast<float>(),
                                  m[4].cast<float>(), m[5].cast<float>(), m[6].cast<float>() };
            });
        }, py::arg("marks"), py::arg("gen"));
    ss.def("light_sprites", &Fx::LightSprites, py::return_value_policy::reference);
    ss.def("at", &Fx::At, py::arg("x"), py::arg("y"), py::arg("rotation"), py::arg("lx"), py::arg("ly"));
    py::class_<Post>(ss, "Post")
        .def(py::init<>())
        .def("apply", [](Post& post, const py::dict& look) {   // keys of PostLook in snake_case; an unknown key is a KeyError
            PostLook l;
            const std::pair<const char*, float*> scalars[] = { { "exposure", &l.Exposure }, { "contrast", &l.Contrast },
                { "saturation", &l.Saturation }, { "mist", &l.Mist }, { "tilt", &l.Tilt }, { "tilt_centre", &l.TiltCentre },
                { "tilt_band", &l.TiltBand }, { "rain", &l.Rain }, { "heat", &l.Heat }, { "vignette", &l.Vignette },
                { "grain", &l.Grain } };
            const std::pair<const char*, std::array<float, 3>*> triples[] = { { "lift", &l.Lift }, { "gain", &l.Gain },
                { "mist_colour", &l.MistColour } };
            for (const auto& [k, v] : look) {
                const std::string key = k.cast<std::string>();
                bool known = false;
                for (const auto& [name, field] : scalars) if (key == name) { *field = v.cast<float>(); known = true; }
                for (const auto& [name, field] : triples) if (key == name) { *field = v.cast<std::array<float, 3>>(); known = true; }
                if (!known) throw py::key_error(key);
            }
            post.Apply(l);
        }, py::arg("look"));
    ss.def("post_unload", &Post::Unload);
    ss.def("daylight_elevation", &Daylight::Elevation, py::arg("hour"));
    ss.def("daylight_sun", [](double hour) { const Daylight::Sun s = Daylight::SunAt(hour); return py::make_tuple(s.Direction, s.Length, s.Strength); }, py::arg("hour"));
    ss.def("daylight_ambient", &Daylight::Ambient, py::arg("hour"));
    ss.def("daylight_lamps", &Daylight::Lamps, py::arg("hour"));
    ss.def("daylight_ambient_colour", [](double hour) { const auto c = Daylight::AmbientColour(hour); return py::make_tuple(c[0], c[1], c[2]); }, py::arg("hour"));
    ss.def("daylight_shadow_tint", [](double hour) { const auto c = Daylight::ShadowTint(hour); return py::make_tuple(c[0], c[1], c[2]); }, py::arg("hour"));
    ss.def("daylight_mist", &Daylight::Mist, py::arg("hour"));
    ss.def("daylight_haze", [](double hour) { const auto c = Daylight::Haze(hour); return py::make_tuple(c[0], c[1], c[2], c[3]); }, py::arg("hour"));
    ss.def("daylight_grade", [](double hour) {
        const Daylight::Grade g = Daylight::GradeAt(hour);
        py::dict d;
        d["exposure"] = g.Exposure;
        d["contrast"] = g.Contrast;
        d["saturation"] = g.Saturation;
        d["lift"] = py::make_tuple(g.Lift[0], g.Lift[1], g.Lift[2]);
        d["gain"] = py::make_tuple(g.Gain[0], g.Gain[1], g.Gain[2]);
        return d;
    }, py::arg("hour"));
}

} // namespace IYI
