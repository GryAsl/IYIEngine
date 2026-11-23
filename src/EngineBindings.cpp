// Registers core engine types, actor properties, and engine management functions with pybind11.
// Defines the 'GameActor' Python class and binds engine-level utilities like spawn_actor, find_actor,
// change_scene, camera controls, background color settings, and asset loading functions into the 'engine' module.
// Exposes GameActor::WorldPos directly for in-place component mutation and links actor.state to engine-owned
// dictionary storage that persists across script hot reloads.
// Development helpers that every game gets for free (SPEC §3, §12.3):
//   engine.profile_begin("sim") ... engine.profile_end()   own sections in the frame profiler
//   engine.profile_count("trips", n)                       a counter of the innermost open section
//   engine.get_profile()                                   last second: (name, avg_ms, max_ms, calls)
//   engine.take_screenshot(path)                           the finished frame, HUD included
//   engine.load_sprite_stack(paths)                        layers packed into one texture
// and, outside Python: the agent channel (agentio/request.json, AgentChannel.cpp) and the
// --frames / --screenshot / --profile command line flags (Application.cpp).
//
// Connected to:
// - Invoked by PYBIND11_EMBEDDED_MODULE(engine, m) in PythonRuntime.cpp during interpreter initialization.
// - Interacts with Application::CurrentApp(), Scene, ActorRegistry, and AssetManager.
// - Operates on GameActor instances passed between C++ and Python scripts.
//
// Future evolution (per ROADMAP.md):
// engine.quit(), engine.all_actors() and the window controls are in (ROADMAP Tier 1).
// Deep inheritance hierarchies and complex component binding systems are intentionally avoided.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "raylib.h"
#include "IYI/Api.h"
#include "IYI/Application.h"
#include "IYI/FrameInput.h"
#include "IYI/FramePacket.h"
#include "IYI/GameActor.h"
#include "IYI/Scene.h"
#include "IYI/AssetManager.h"
#include "IYI/Log.h"
#include "IYI/Profiler.h"

namespace py = pybind11;

namespace IYI {

py::dict GetActorStateDict(uint64_t actorId);
ResourceStore* GetActiveResourceStore();

static void EnsurePybindLocalInternals() {
    auto& l_ppmgr = py::detail::get_local_internals_pp_manager();
    auto* l_pp = l_ppmgr.get_pp();
    if (l_pp && !l_pp->get()) {
        l_pp->reset(new pybind11::detail::local_internals());
    }
}

// The one chokepoint for retained actor references, installed on GameActor and every bound subclass
// (UICanvas): once the actor has left its registry (Alive false) any attribute but `id` and dunders raises
// ReferenceError instead of acting on a dead actor. The memory itself stays valid while any wrapper exists
// (SetActorReferencedHook), so this check never reads freed storage. Bypass it (a new actor type without the
// guard) and a dead actor silently reads and writes parked state.
static bool DeadActorAccess(PyObject* self, PyObject* name)
{
    if (!PyUnicode_Check(name)) return false;
    const char* n = PyUnicode_AsUTF8(name);
    if (!n) { PyErr_Clear(); return false; }
    if (std::strcmp(n, "id") == 0 || std::strncmp(n, "__", 2) == 0) return false;
    GameActor* actor = nullptr;
    try { actor = py::handle(self).cast<GameActor*>(); } catch (const py::cast_error&) { return false; }
    if (!actor || actor->Alive) return false;
    PyErr_Format(PyExc_ReferenceError, "actor %llu '%s' was destroyed; re-resolve it with find_actor_by_id",
                 static_cast<unsigned long long>(actor->Id), actor->Name.c_str());
    return true;
}
static PyObject* ActorGetAttr(PyObject* self, PyObject* name)
{
    return DeadActorAccess(self, name) ? nullptr : PyObject_GenericGetAttr(self, name);
}
static int ActorSetAttr(PyObject* self, PyObject* name, PyObject* value)
{
    return DeadActorAccess(self, name) ? -1 : PyObject_GenericSetAttr(self, name, value);
}
void GuardDeadActorAccess(py::handle type)
{
    auto* t = reinterpret_cast<PyTypeObject*>(type.ptr());
    t->tp_getattro = &ActorGetAttr;
    t->tp_setattro = &ActorSetAttr;
    PyType_Modified(t);
}

void RegisterEngineBindings(py::module_& m)
{
    EnsurePybindLocalInternals();

    // GameActor binding
    py::class_<GameActor, std::unique_ptr<GameActor, py::nodelete>>(m, "GameActor")
        .def_readonly("id", &GameActor::Id)
        .def_readonly("name", &GameActor::Name)
        .def_readonly("script", &GameActor::Script)
        // WorldPos is exposed via def_readwrite so 'actor.world_pos.x += 5' mutates the Vector3 struct in place.
        .def_readwrite("world_pos", &GameActor::WorldPos)
        .def_readwrite("active", &GameActor::Active)
        .def_readwrite("visible", &GameActor::Visible)
        .def_readwrite("persistent", &GameActor::Persistent)
        .def_readwrite("screen_space", &GameActor::ScreenSpace)
        .def_readonly("pending_destroy", &GameActor::PendingDestroy)
        // actor.state is an engine-owned py::dict stored in PythonRuntime's m_actorState map, keyed by actor ID.
        // Because it lives in C++ rather than the script module's globals, state survives script hot reloads.
        .def_property_readonly("state", [](GameActor& self) -> py::dict {
            return GetActorStateDict(self.Id);
        })
        .def("destroy", &GameActor::Destroy)
        .def("__repr__", [](const GameActor& self) {
            return "<GameActor " + std::to_string(self.Id) + " '" + self.Name + "'>";
        });
    GuardDeadActorAccess(m.attr("GameActor"));

    // Engine functions
    m.def("spawn_actor", [](const std::string& name, const std::string& script, float x, float y, float z, bool persistent, bool screen_space) -> GameActor* {
        Vector3 pos{ x, y, z };
        GameActor* actor = CurrentApp().GetScene().Actors.SpawnScripted(name, script, pos);
        if (actor) {
            actor->Persistent = persistent;
            actor->ScreenSpace = screen_space;
        }
        return actor;
    }, py::arg("name") = "Actor", py::arg("script") = "", py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("z") = 0.0f, py::arg("persistent") = false, py::arg("screen_space") = false, py::return_value_policy::reference);

    m.def("find_actor", [](const std::string& name) -> GameActor* {
        return CurrentApp().GetScene().Actors.FindByName(name);
    }, py::arg("name"), py::return_value_policy::reference);

    m.def("find_actor_by_id", [](uint64_t id) -> GameActor* {
        return CurrentApp().GetScene().Actors.FindById(id);
    }, py::arg("id"), py::return_value_policy::reference);

    // Every live actor in spawn order (destroyed ones skipped, as find_actor_by_id does).
    m.def("all_actors", []() -> std::vector<GameActor*> {
        std::vector<GameActor*> actors;
        for (const auto& actor : CurrentApp().GetScene().Actors.GetActors()) {
            if (actor && !actor->PendingDestroy) actors.push_back(actor.get());
        }
        return actors;
    }, py::return_value_policy::reference);

    // The rest forwards to IYI::Api (Api.h), the one implementation native games call too.
    m.def("change_scene", &Api::ChangeScene, py::arg("script_path"));
    m.def("toggle_fullscreen", &Api::ToggleFullscreen);
    m.def("is_fullscreen", &Api::IsWindowFullscreen);
    m.def("set_vsync", &Api::SetVsync, py::arg("interval"));
    m.def("set_target_fps", &Api::SetTargetFPS, py::arg("fps"));
    m.def("get_monitor_refresh_rate", &Api::GetMonitorRefreshRate);
    m.def("is_window_focused", &Api::IsWindowFocused);
    m.def("quit", &Api::Quit);
    m.def("set_close_intercept", &Api::SetCloseIntercept, py::arg("enabled"));
    m.def("take_close_request", &Api::TakeCloseRequest);
    m.def("take_screenshot", &Api::TakeScreenshot, py::arg("path"));

    // vertical_scale squashes the view along screen-vertical (a tilted camera: sin of the pitch).
    m.def("set_camera", [](const Camera2D& camera, float vertical_scale) {
        if (!std::isfinite(vertical_scale) || vertical_scale <= 0.0f) {
            throw py::value_error("set_camera: vertical_scale must be a finite number > 0");
        }
        Api::SetCamera(camera, vertical_scale);
    }, py::arg("camera"), py::arg("vertical_scale") = 1.0f);

    // The world pass through a 3D camera (engine.Camera3D, world space with z = height up), with a
    // depth test; replaces set_camera's 2D camera until set_camera or clear_camera.
    m.def("set_camera_3d", &Api::SetCamera3D, py::arg("camera"));
    m.def("clear_camera", &Api::ClearCamera);

    m.def("set_background_color", &Api::SetBackgroundColor, py::arg("color"));
    m.def("load_texture", &Api::LoadTexture, py::arg("rel_path"), py::return_value_policy::reference);
    // Sprite-stack layers (bottom first, equal sizes) packed into one texture; cached per path list.
    m.def("load_sprite_stack", &Api::LoadSpriteStack, py::arg("layer_paths"), py::return_value_policy::reference);
    // Engine-owned offscreen target (cached per name and size, freed at shutdown). Draw into it
    // between begin_texture_mode/end_texture_mode from Update; refused inside Drawing.
    m.def("load_render_texture", &Api::LoadRenderTexture, py::arg("name"), py::arg("width"), py::arg("height"),
          py::return_value_policy::reference);
    m.def("begin_texture_mode", &Api::BeginTextureMode, py::arg("target"));
    m.def("end_texture_mode", &Api::EndTextureMode);
    m.def("load_font", &Api::LoadFont, py::arg("rel_path"), py::arg("font_size") = 32, py::arg("glyph_text") = "", py::arg("pixel") = false,
          py::return_value_policy::reference);
    m.def("add_pixel_font_size", &Api::AddPixelFontSize, py::arg("font"), py::arg("rel_path"), py::arg("font_size"));
    m.def("load_shader", &Api::LoadShader, py::arg("vs_rel_path"), py::arg("fs_rel_path"), py::return_value_policy::reference);
    m.def("set_postprocess_shader", &Api::SetPostprocessShader, py::arg("shader"), py::arg("strength") = 1.0f,
          py::arg("radial_warp") = 0.0f, py::arg("world_only") = false, py::arg("text_mask") = false);
    // Window/taskbar icon from a horizontal frame strip; call again with the next frame to animate.
    m.def("set_window_icon", &Api::SetWindowIcon, py::arg("rel_path"), py::arg("frame") = 0, py::arg("frame_count") = 1);
    m.def("project_path", &Api::ProjectPath, py::arg("rel_path") = "");
    m.def("user_data_path", &Api::UserDataPath, py::arg("subdir") = "");
    m.def("run_path", &Api::RunPath, py::arg("rel_path") = "");

    m.def("is_shipping", &Api::IsShipping);
    m.def("is_agent_run", &Api::IsAgentRun);
    m.def("run_seed", []() -> py::object {   // None without --seed
        long long seed = 0;
        return Api::RunSeed(seed) ? py::object(py::int_(seed)) : py::object(py::none());
    });

    m.def("_read_resource", [](const std::string& path) -> py::object {
        ResourceStore* store = GetActiveResourceStore();
        if (!store) {
            try {
                store = &CurrentApp().GetAssets().GetResourceStore();
            } catch (...) {
                return py::none();
            }
        }
        if (!store || !store->Exists(path)) {
            return py::none();
        }
        std::vector<unsigned char> bytes;
        if (!store->ReadBytes(path, bytes)) {
            return py::none();
        }
        return py::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }, py::arg("path"));

    // Frame profiler (Profiler.h): nest a section inside the engine stage and actor hook that is
    // running. A section left open (an exception) is closed when its hook returns.
    m.def("profile_begin", [](const std::string& name) -> void {
        ProfileBegin(name, true);
    }, py::arg("name"));

    m.def("profile_end", []() -> void {
        ProfileEnd(true);
    });

    m.def("profile_count", [](const std::string& counter, double n) -> void {
        ProfileCount(counter, n);
    }, py::arg("counter"), py::arg("n") = 1.0);

    // GPU timing for every section (Profiler.h): results gain a "gpu/..." tree. False if unsupported.
    m.def("set_gpu_profiling", [](bool enabled) -> bool {
        return ProfileSetGpu(enabled);
    }, py::arg("enabled"));

    // Fine engine sub-sections (ProfileAdd, e.g. sprite_stacks/emit/layers): on while F3 shows the overlay.
    m.def("set_profile_detail", [](bool enabled) -> void {
        ProfileSetDetail(enabled);
    }, py::arg("enabled"));

    // The last completed one-second window: (name, avg_ms, max_ms, calls_per_frame), parents first.
    m.def("get_profile", []() -> std::vector<std::tuple<std::string, float, float, float>> {
        std::vector<std::tuple<std::string, float, float, float>> out;
        for (const ProfileSection& section : ProfileResults()) {
            out.emplace_back(section.Name, section.AvgMs, section.MaxMs, section.Calls);
        }
        return out;
    });

    m.def("log_debug", [](const std::string& message, const std::string& category) {
        LogWrite(LogLevel::Debug, category, message);
    }, py::arg("message"), py::arg("category") = "game");

    m.def("log_info", [](const std::string& message, const std::string& category) {
        LogWrite(LogLevel::Info, category, message);
    }, py::arg("message"), py::arg("category") = "game");

    m.def("log_warn", [](const std::string& message, const std::string& category) {
        LogWrite(LogLevel::Warn, category, message);
    }, py::arg("message"), py::arg("category") = "game");

    m.def("log_error", [](const std::string& message, const std::string& category) {
        LogWrite(LogLevel::Error, category, message);
    }, py::arg("message"), py::arg("category") = "game");

    // One line of logs/events.jsonl; the fields must be JSON-serializable.
    m.def("log_event", [](const std::string& kind, py::kwargs fields) {
        for (const char* reserved : { "t", "frame", "kind" })   // the line's own keys: a duplicate breaks readers
            if (fields.contains(reserved)) throw py::value_error(std::string("log_event: field name '") + reserved + "' is reserved");
        LogEventJson(kind, py::module_::import("json").attr("dumps")(fields, py::arg("default") = py::module_::import("builtins").attr("repr")).cast<std::string>());
    }, py::arg("kind"), py::pos_only());
}

} // namespace IYI
