// Exposes raylib data types, named colors, input key constants, and thin function forwards to Python.
// Binds core raylib structures (Vector2, Vector3, Rectangle, Color, Camera2D) with read/write attributes,
// as well as opaque handles for textures, fonts, sounds, music, and shaders. Provides direct 1:1 bindings
// for raylib drawing, keyboard/mouse/gamepad input queries, collision checks, and 2D camera coordinate math.
// Follows the principle of zero abstraction over raylib: if raylib solves it, bind it thinly without wrapping.
// Forwards therefore behave as the raylib this build uses: the current version (now 6.2) or a legacy one
// (now 5.5), SPEC §0.1. Check that version's raylib.h (build/<tree>/_deps/raylib-src/src), not memory of
// an older raylib; behaviour a version changed is noted in docs/CHEATSHEET.md, not hidden here.
//
// Connected to:
// - Invoked by PYBIND11_EMBEDDED_MODULE(engine, m) in PythonRuntime.cpp during interpreter initialization.
// - Calls raylib C API functions directly for rendering, input polling, audio playback, and collisions.
//
// Deliberate batch exceptions are draw_sprite_stacks (SpriteStack.cpp) and draw_billboards:
// many Python calls for small repeated shapes cost whole milliseconds. Both emit raylib batch quads.
// Sprite-stack usage in short:
//   stack = engine.load_sprite_stack(paths); stack.origin = engine.Vector3(px, py, pz)
//   engine.draw_sprite_stacks([(stack, x, y, z, rot, tint), ...], view_rotation=cam_rot, height_scale=cot_pitch)
//   lighting and shadows on top: the sprite_stack library (libraries/sprite_stack, SPEC §12.4)
// draw_spline_linear has mitered joints (raylib SUPPORT_SPLINE_MITERS, CMakeLists.txt).
//
// Future evolution (per ROADMAP.md):
// raylib forwards: shapes, gradients, window controls (ROADMAP Tier 1).
// Custom render pipelines, general draw batchers, and bespoke collision engines are explicitly rejected.

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include "IYI/Api.h"
#include "IYI/Application.h"
#include "IYI/FramePacket.h"
#include "IYI/FrameInput.h"
#include "IYI/Ime.h"
#include "IYI/Profiler.h"
#include "IYI/SpriteStack.h"
#include "IYI/StackLighting.h"
#include "IYI/Particles.h"
// raylib exposes only the monitor's current mode (GetMonitorWidth/Height), never the list of modes
// it supports (raylib issue #514), so the mode list comes from the GLFW raylib already bundles and
// links on desktop. No new dependency: this is the same GLFW that backs every raylib window call.
#ifndef IYI_MOBILE
#include <GLFW/glfw3.h>
#endif

namespace py = pybind11;

namespace IYI {

static void EnsurePybindLocalInternals() {
    auto& l_ppmgr = py::detail::get_local_internals_pp_manager();
    auto* l_pp = l_ppmgr.get_pp();
    if (l_pp && !l_pp->get()) {
        l_pp->reset(new pybind11::detail::local_internals());
    }
}

// Sprite-stack items from Python: (stack, x, y, z, rotation, tint[, scale[, lean[, build[, sway[, damage]]]]]) tuples and, where
// lists are allowed (a draw call), SpriteStackList entries, whose items are copied in as they are, or
// with statics given, go there as what changed since the list's previous draw (StaticStacksDraw).
static void ReadSpriteStackItems(const py::list& items, std::vector<SpriteStackItem>& out, bool lists,
                                 std::vector<StaticStacksDraw>* statics = nullptr)
{
    out.reserve(out.size() + items.size());
    for (py::handle h : items) {
        if (lists && py::isinstance<SpriteStackList>(h)) {
            SpriteStackList& list = h.cast<SpriteStackList&>();
            if (!statics) {
                out.insert(out.end(), list.Items.begin(), list.Items.end());
                continue;
            }
            StaticStacksDraw sd{ list.Index, {}, {} };
            if (!list.Sent) sd.All = list.Items;
            else sd.Edits = std::move(list.Edits);
            list.Edits.clear();
            list.Sent = true;
            statics->push_back(std::move(sd));
            continue;
        }
        if (!py::isinstance<py::tuple>(h)) throw py::type_error("sprite stacks: items must be tuples (or SpriteStackLists in a draw)");
        const py::tuple t = py::reinterpret_borrow<py::tuple>(h);
        if (t.size() < 6 || t.size() > 11) {
            throw py::value_error("sprite stacks: item = (stack, x, y, z, rotation, tint[, scale[, lean[, build[, sway[, damage]]]]])");
        }
        SpriteStackItem item;
        item.Stack = t[0].cast<const SpriteStack*>();
        item.X = t[1].cast<float>();
        item.Y = t[2].cast<float>();
        item.Z = t[3].cast<float>();
        item.Rotation = t[4].cast<float>();
        item.Tint = t[5].cast<Color>();
        if (t.size() >= 7) item.Scale = t[6].cast<float>();
        if (t.size() >= 8) item.Lean = t[7].cast<float>();
        if (t.size() >= 9) item.Build = t[8].cast<float>();
        if (t.size() >= 10) item.Sway = t[9].cast<float>();
        if (t.size() == 11) item.Damage = std::clamp(t[10].cast<float>(), 0.0f, 1.0f);
        out.push_back(item);
    }
}

void RegisterRaylibBindings(py::module_& m)
{
    EnsurePybindLocalInternals();

    // Types
    py::class_<Vector2>(m, "Vector2")
        .def(py::init<float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f)
        .def_readwrite("x", &Vector2::x)
        .def_readwrite("y", &Vector2::y)
        .def("__repr__", [](const Vector2& v) {
            return "<Vector2 (" + std::to_string(v.x) + ", " + std::to_string(v.y) + ")>";
        });

    py::class_<Vector3>(m, "Vector3")
        .def(py::init<float, float, float>(), py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("z") = 0.0f)
        .def_readwrite("x", &Vector3::x)
        .def_readwrite("y", &Vector3::y)
        .def_readwrite("z", &Vector3::z)
        .def("__repr__", [](const Vector3& v) {
            return "<Vector3 (" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")>";
        });

    py::class_<Rectangle>(m, "Rectangle")
        .def(py::init<float, float, float, float>(),
             py::arg("x") = 0.0f, py::arg("y") = 0.0f, py::arg("width") = 0.0f, py::arg("height") = 0.0f)
        .def_readwrite("x", &Rectangle::x)
        .def_readwrite("y", &Rectangle::y)
        .def_readwrite("width", &Rectangle::width)
        .def_readwrite("height", &Rectangle::height)
        .def("__repr__", [](const Rectangle& r) {
            return "<Rectangle (" + std::to_string(r.x) + ", " + std::to_string(r.y) + ", " +
                   std::to_string(r.width) + ", " + std::to_string(r.height) + ")>";
        });

    py::class_<Color>(m, "Color")
        .def(py::init<unsigned char, unsigned char, unsigned char, unsigned char>(),
             py::arg("r") = 0, py::arg("g") = 0, py::arg("b") = 0, py::arg("a") = 255)
        .def_readwrite("r", &Color::r)
        .def_readwrite("g", &Color::g)
        .def_readwrite("b", &Color::b)
        .def_readwrite("a", &Color::a)
        .def("__repr__", [](const Color& c) {
            return "<Color (" + std::to_string(static_cast<int>(c.r)) + ", " +
                   std::to_string(static_cast<int>(c.g)) + ", " +
                   std::to_string(static_cast<int>(c.b)) + ", " +
                   std::to_string(static_cast<int>(c.a)) + ")>";
        });

    py::class_<Camera2D>(m, "Camera2D")
        .def(py::init<Vector2, Vector2, float, float>(),
             py::arg("offset") = Vector2{ 0.0f, 0.0f },
             py::arg("target") = Vector2{ 0.0f, 0.0f },
             py::arg("rotation") = 0.0f,
             py::arg("zoom") = 1.0f)
        .def_readwrite("offset", &Camera2D::offset)
        .def_readwrite("target", &Camera2D::target)
        .def_readwrite("rotation", &Camera2D::rotation)
        .def_readwrite("zoom", &Camera2D::zoom)
        .def("__repr__", [](const Camera2D& c) {
            return "<Camera2D offset=(" + std::to_string(c.offset.x) + ", " + std::to_string(c.offset.y) +
                   ") target=(" + std::to_string(c.target.x) + ", " + std::to_string(c.target.y) +
                   ") rotation=" + std::to_string(c.rotation) + " zoom=" + std::to_string(c.zoom) + ">";
        });

    // A 3D camera for set_camera_3d, in world space with z = height up (Application.h).
    // projection: CAMERA_PERSPECTIVE (fovy in degrees) or CAMERA_ORTHOGRAPHIC (fovy = view height in world units).
    py::class_<Camera3D>(m, "Camera3D")
        .def(py::init([](Vector3 position, Vector3 target, Vector3 up, float fovy, int projection) {
            return Camera3D{ position, target, up, fovy, projection };
        }), py::arg("position"), py::arg("target"), py::arg("up") = Vector3{ 0.0f, 0.0f, 1.0f },
            py::arg("fovy") = 45.0f, py::arg("projection") = static_cast<int>(CAMERA_PERSPECTIVE))
        .def_readwrite("position", &Camera3D::position)
        .def_readwrite("target", &Camera3D::target)
        .def_readwrite("up", &Camera3D::up)
        .def_readwrite("fovy", &Camera3D::fovy)
        .def_readwrite("projection", &Camera3D::projection);
    m.attr("CAMERA_PERSPECTIVE") = static_cast<int>(CAMERA_PERSPECTIVE);
    m.attr("CAMERA_ORTHOGRAPHIC") = static_cast<int>(CAMERA_ORTHOGRAPHIC);

    // Cell terrain (CellTerrain.h): corners = 4 heights per cell (top-left, top-right, bottom-right,
    // bottom-left), cells row by row; water (optional) = one flag per cell, the same order: a wall
    // between two water cells is a waterfall. Built once per map; draw() in a world pass with a 3D camera.
    // Shared with the recorded commands that draw it and deleted on the render side (RenderOwned): its
    // meshes are GL objects, built by its first draw. Immutable after construction, so height() reads
    // it on the game side while the render side draws.
    py::class_<CellTerrain, std::shared_ptr<CellTerrain>>(m, "CellTerrain")
        .def(py::init(&Api::MakeCellTerrain), py::arg("columns"), py::arg("rows"), py::arg("cell"), py::arg("origin"),
            py::arg("corners"), py::arg("water") = std::vector<bool>{})
        .def("height", [](const CellTerrain& t, float x, float y) { return CellTerrainHeight(t, x, y); }, py::arg("x"), py::arg("y"))
        .def("draw", &Api::DrawCellTerrain, py::arg("texture"), py::arg("area"), py::arg("wall"), py::arg("upside_down") = true);

    py::class_<Texture2D>(m, "Texture2D")
        .def_readonly("width", &Texture2D::width)
        .def_readonly("height", &Texture2D::height)
        .def_readonly("id", &Texture2D::id)
        .def("set_smooth", [](Texture2D& t, bool smooth) {   // bilinear (a ground picture seen close) or point (pixel art)
            RecordSetup([tex = &t, smooth]() { SetTextureFilter(*tex, smooth ? TEXTURE_FILTER_BILINEAR : TEXTURE_FILTER_POINT); });
        }, py::arg("smooth") = true)
        .def("__repr__", [](const Texture2D& t) {
            return "<Texture2D id=" + std::to_string(t.id) + " width=" + std::to_string(t.width) +
                   " height=" + std::to_string(t.height) + ">";
        });

    py::class_<Font>(m, "Font")
        .def_property_readonly("base_size", [](const Font& f) { return f.baseSize; })
        .def("has_glyph", [](const Font& f, int cp) -> bool {
            int length = 0;
            const char* utf8 = CodepointToUTF8(cp, &length);
            const Font& view = *CurrentApp().GetAssets().FontForText(f, std::string(utf8, length));
            for (int i = 0; i < view.glyphCount; ++i) {
                if (view.glyphs[i].value == cp && (view.glyphs[i].advanceX > 0 || view.recs[i].width > 0)) return true;
            }
            return false;
        }, py::arg("codepoint"))
        .def("__repr__", [](const Font& f) {
            return "<Font base_size=" + std::to_string(f.baseSize) + ">";
        });

    py::class_<Sound>(m, "Sound")
        .def("__repr__", [](const Sound&) { return "<Sound>"; });

    py::class_<Music>(m, "Music")
        .def_readwrite("looping", &Music::looping)
        .def_property_readonly("time_length", [](const Music& mus) { return GetMusicTimeLength(mus); })
        .def("__repr__", [](const Music&) { return "<Music>"; });

    py::class_<Shader>(m, "Shader")
        .def_readonly("id", &Shader::id)
        .def("__repr__", [](const Shader& s) { return "<Shader id=" + std::to_string(s.id) + ">"; });

    py::class_<RenderTexture2D>(m, "RenderTexture2D")
        .def_readonly("id", &RenderTexture2D::id)
        .def_property_readonly("texture", [](const RenderTexture2D& t) -> const Texture2D& { return t.texture; },
                               py::return_value_policy::reference_internal)
        .def("__repr__", [](const RenderTexture2D& t) {
            return "<RenderTexture2D id=" + std::to_string(t.id) + " width=" + std::to_string(t.texture.width) +
                   " height=" + std::to_string(t.texture.height) + ">";
        });

    // A sprite stack from engine.load_sprite_stack: its layers packed into one engine-owned texture.
    py::class_<SpriteStack, std::unique_ptr<SpriteStack, py::nodelete>>(m, "SpriteStack")
        .def_readonly("texture", &SpriteStack::Texture)
        .def_readonly("layers", &SpriteStack::Layers)
        .def_readonly("width", &SpriteStack::Width)
        .def_readonly("height", &SpriteStack::Height)
        .def_readwrite("origin", &SpriteStack::Origin)
        .def_readwrite("layer_step", &SpriteStack::LayerStep)
        .def("__repr__", [](const SpriteStack& s) {
            return "<SpriteStack layers=" + std::to_string(s.Layers) + " width=" + std::to_string(s.Width) +
                   " height=" + std::to_string(s.Height) + ">";
        });

    // Named colors
    m.attr("WHITE")     = WHITE;
    m.attr("BLACK")     = BLACK;
    m.attr("GRAY")      = GRAY;
    m.attr("DARKGRAY")  = DARKGRAY;
    m.attr("LIGHTGRAY") = LIGHTGRAY;
    m.attr("RED")       = RED;
    m.attr("MAROON")    = MAROON;
    m.attr("GREEN")     = GREEN;
    m.attr("LIME")      = LIME;
    m.attr("DARKGREEN") = DARKGREEN;
    m.attr("BLUE")      = BLUE;
    m.attr("DARKBLUE")  = DARKBLUE;
    m.attr("SKYBLUE")   = SKYBLUE;
    m.attr("PURPLE")    = PURPLE;
    m.attr("VIOLET")    = VIOLET;
    m.attr("PINK")      = PINK;
    m.attr("YELLOW")    = YELLOW;
    m.attr("GOLD")      = GOLD;
    m.attr("ORANGE")    = ORANGE;
    m.attr("BROWN")     = BROWN;
    m.attr("BEIGE")     = BEIGE;
    m.attr("MAGENTA")   = MAGENTA;
    m.attr("BLANK")     = BLANK;
    m.attr("RAYWHITE")  = RAYWHITE;

    // Key constants
    m.attr("KEY_A") = static_cast<int>(KEY_A);
    m.attr("KEY_B") = static_cast<int>(KEY_B);
    m.attr("KEY_C") = static_cast<int>(KEY_C);
    m.attr("KEY_D") = static_cast<int>(KEY_D);
    m.attr("KEY_E") = static_cast<int>(KEY_E);
    m.attr("KEY_F") = static_cast<int>(KEY_F);
    m.attr("KEY_G") = static_cast<int>(KEY_G);
    m.attr("KEY_H") = static_cast<int>(KEY_H);
    m.attr("KEY_I") = static_cast<int>(KEY_I);
    m.attr("KEY_J") = static_cast<int>(KEY_J);
    m.attr("KEY_K") = static_cast<int>(KEY_K);
    m.attr("KEY_L") = static_cast<int>(KEY_L);
    m.attr("KEY_M") = static_cast<int>(KEY_M);
    m.attr("KEY_N") = static_cast<int>(KEY_N);
    m.attr("KEY_O") = static_cast<int>(KEY_O);
    m.attr("KEY_P") = static_cast<int>(KEY_P);
    m.attr("KEY_Q") = static_cast<int>(KEY_Q);
    m.attr("KEY_R") = static_cast<int>(KEY_R);
    m.attr("KEY_S") = static_cast<int>(KEY_S);
    m.attr("KEY_T") = static_cast<int>(KEY_T);
    m.attr("KEY_U") = static_cast<int>(KEY_U);
    m.attr("KEY_V") = static_cast<int>(KEY_V);
    m.attr("KEY_W") = static_cast<int>(KEY_W);
    m.attr("KEY_X") = static_cast<int>(KEY_X);
    m.attr("KEY_Y") = static_cast<int>(KEY_Y);
    m.attr("KEY_Z") = static_cast<int>(KEY_Z);

    m.attr("KEY_ZERO")  = static_cast<int>(KEY_ZERO);
    m.attr("KEY_ONE")   = static_cast<int>(KEY_ONE);
    m.attr("KEY_TWO")   = static_cast<int>(KEY_TWO);
    m.attr("KEY_THREE") = static_cast<int>(KEY_THREE);
    m.attr("KEY_FOUR")  = static_cast<int>(KEY_FOUR);
    m.attr("KEY_FIVE")  = static_cast<int>(KEY_FIVE);
    m.attr("KEY_SIX")   = static_cast<int>(KEY_SIX);
    m.attr("KEY_SEVEN") = static_cast<int>(KEY_SEVEN);
    m.attr("KEY_EIGHT") = static_cast<int>(KEY_EIGHT);
    m.attr("KEY_NINE")  = static_cast<int>(KEY_NINE);

    m.attr("KEY_SPACE")         = static_cast<int>(KEY_SPACE);
    m.attr("KEY_ENTER")         = static_cast<int>(KEY_ENTER);
    m.attr("KEY_ESCAPE")        = static_cast<int>(KEY_ESCAPE);
    m.attr("KEY_TAB")           = static_cast<int>(KEY_TAB);
    m.attr("KEY_BACKSPACE")     = static_cast<int>(KEY_BACKSPACE);
    m.attr("KEY_DELETE")        = static_cast<int>(KEY_DELETE);
    m.attr("KEY_LEFT")          = static_cast<int>(KEY_LEFT);
    m.attr("KEY_RIGHT")         = static_cast<int>(KEY_RIGHT);
    m.attr("KEY_UP")            = static_cast<int>(KEY_UP);
    m.attr("KEY_DOWN")          = static_cast<int>(KEY_DOWN);
    m.attr("KEY_LEFT_SHIFT")    = static_cast<int>(KEY_LEFT_SHIFT);
    m.attr("KEY_RIGHT_SHIFT")   = static_cast<int>(KEY_RIGHT_SHIFT);
    m.attr("KEY_LEFT_CONTROL")  = static_cast<int>(KEY_LEFT_CONTROL);
    m.attr("KEY_RIGHT_CONTROL") = static_cast<int>(KEY_RIGHT_CONTROL);
    m.attr("KEY_LEFT_ALT")      = static_cast<int>(KEY_LEFT_ALT);

    m.attr("KEY_F1")  = static_cast<int>(KEY_F1);
    m.attr("KEY_F2")  = static_cast<int>(KEY_F2);
    m.attr("KEY_F3")  = static_cast<int>(KEY_F3);
    m.attr("KEY_F4")  = static_cast<int>(KEY_F4);
    m.attr("KEY_F5")  = static_cast<int>(KEY_F5);
    m.attr("KEY_F6")  = static_cast<int>(KEY_F6);
    m.attr("KEY_F7")  = static_cast<int>(KEY_F7);
    m.attr("KEY_F8")  = static_cast<int>(KEY_F8);
    m.attr("KEY_F9")  = static_cast<int>(KEY_F9);
    m.attr("KEY_F10") = static_cast<int>(KEY_F10);
    m.attr("KEY_F11") = static_cast<int>(KEY_F11);
    m.attr("KEY_F12") = static_cast<int>(KEY_F12);

    m.attr("KEY_MINUS")       = static_cast<int>(KEY_MINUS);
    m.attr("KEY_EQUAL")       = static_cast<int>(KEY_EQUAL);
    m.attr("KEY_KP_ADD")      = static_cast<int>(KEY_KP_ADD);
    m.attr("KEY_KP_SUBTRACT") = static_cast<int>(KEY_KP_SUBTRACT);

    // Mouse constants
    m.attr("MOUSE_BUTTON_LEFT")   = static_cast<int>(MOUSE_BUTTON_LEFT);
    m.attr("MOUSE_BUTTON_RIGHT")  = static_cast<int>(MOUSE_BUTTON_RIGHT);
    m.attr("MOUSE_BUTTON_MIDDLE") = static_cast<int>(MOUSE_BUTTON_MIDDLE);

    // Gamepad constants
    m.attr("GAMEPAD_BUTTON_LEFT_FACE_UP")     = static_cast<int>(GAMEPAD_BUTTON_LEFT_FACE_UP);
    m.attr("GAMEPAD_BUTTON_LEFT_FACE_DOWN")   = static_cast<int>(GAMEPAD_BUTTON_LEFT_FACE_DOWN);
    m.attr("GAMEPAD_BUTTON_LEFT_FACE_LEFT")   = static_cast<int>(GAMEPAD_BUTTON_LEFT_FACE_LEFT);
    m.attr("GAMEPAD_BUTTON_LEFT_FACE_RIGHT")  = static_cast<int>(GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
    m.attr("GAMEPAD_BUTTON_RIGHT_FACE_UP")    = static_cast<int>(GAMEPAD_BUTTON_RIGHT_FACE_UP);
    m.attr("GAMEPAD_BUTTON_RIGHT_FACE_DOWN")  = static_cast<int>(GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
    m.attr("GAMEPAD_BUTTON_RIGHT_FACE_LEFT")  = static_cast<int>(GAMEPAD_BUTTON_RIGHT_FACE_LEFT);
    m.attr("GAMEPAD_BUTTON_RIGHT_FACE_RIGHT") = static_cast<int>(GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);

    m.attr("GAMEPAD_AXIS_LEFT_X")  = static_cast<int>(GAMEPAD_AXIS_LEFT_X);
    m.attr("GAMEPAD_AXIS_LEFT_Y")  = static_cast<int>(GAMEPAD_AXIS_LEFT_Y);
    m.attr("GAMEPAD_AXIS_RIGHT_X") = static_cast<int>(GAMEPAD_AXIS_RIGHT_X);
    m.attr("GAMEPAD_AXIS_RIGHT_Y") = static_cast<int>(GAMEPAD_AXIS_RIGHT_Y);

    // Thin forwards - Drawing
    // Thin forwards (Api.h holds each one's implementation, shared with native games). Python's
    // pixel coordinates are floats; raylib's int forms take them truncated, as before.
    m.def("draw_texture", [](const Texture2D& texture, float x, float y, Color tint) {
        Api::DrawTexture(texture, static_cast<int>(x), static_cast<int>(y), tint);
    }, py::arg("texture"), py::arg("x"), py::arg("y"), py::arg("tint") = WHITE);
    m.def("draw_texture_v", &Api::DrawTextureV, py::arg("texture"), py::arg("position"), py::arg("tint") = WHITE);
    m.def("draw_texture_rec", &Api::DrawTextureRec, py::arg("texture"), py::arg("source"), py::arg("position"), py::arg("tint") = WHITE);
    m.def("draw_texture_pro", &Api::DrawTexturePro, py::arg("texture"), py::arg("source"), py::arg("dest"), py::arg("origin"),
          py::arg("rotation"), py::arg("tint") = WHITE);

    // Sprite stacks, a whole list per call (SpriteStack.h): items are (stack, x, y, z, rotation, tint)
    // tuples, optionally followed by scale and lean (degrees added to view_rotation for that stack),
    // and SpriteStackLists; z is the pivot's height above the ground in world units. Layers
    // climb toward screen-up, so view_rotation is the Camera2D rotation and height_scale the tilt
    // (cot of the pitch). Stacks off the render target are culled, the rest depth sorted back to
    // front. The tuples are read once per call into a reused buffer; nothing is kept between calls.
    // A SpriteStackList is a list of item tuples read once (SpriteStack.h): static stacks are passed
    // to draw_sprite_stacks inside the items list as one entry and copied in, not converted again.
    py::class_<SpriteStackList>(m, "SpriteStackList")
        .def(py::init([](const py::list& items) {
            SpriteStackList list;
            ReadSpriteStackItems(items, list.Items, false);
            return list;
        }), py::arg("items"))
        .def("__len__", [](const SpriteStackList& l) { return l.Items.size(); })
        // Replaces one item (a building that changed): the list is not read again. Draws copy the list
        // when called, so the change shows from the next draw; a static set (StackLighting) needs a new key.
        .def("set", [](SpriteStackList& l, size_t index, const py::tuple& item) {
            if (index >= l.Items.size()) throw py::index_error("SpriteStackList.set: index out of range");
            std::vector<SpriteStackItem> one;
            py::list wrapped;
            wrapped.append(item);
            ReadSpriteStackItems(wrapped, one, false);
            l.Set(index, one[0]);
        }, py::arg("index"), py::arg("item"));

    m.def("draw_sprite_stacks", [](const py::list& items, float view_rotation, float height_scale, float layer_height,
                                   Vector2 offset, std::optional<Color> tint, float max_z, bool sort,
                                   bool ground_in_normal, float ground_max_z) -> void {
        std::vector<SpriteStackItem> read;          // read here; DrawSpriteStacks' cull, sort, emit at replay
        {
            ProfileScope scope("sprite_stacks_read");
            ReadSpriteStackItems(items, read, true);
        }
        SpriteStackPass pass;
        pass.ViewRotation = view_rotation;
        pass.HeightScale = height_scale;
        pass.LayerHeight = layer_height;
        pass.Offset = offset;
        pass.OverrideTint = tint.has_value();
        pass.Tint = tint.value_or(WHITE);
        pass.MaxZ = max_z;
        pass.Sort = sort;
        pass.GroundInNormal = ground_in_normal;
        pass.GroundMaxZ = ground_max_z;
        Api::DrawSpriteStacks(std::move(read), pass);
    }, py::arg("items"), py::arg("view_rotation") = 0.0f, py::arg("height_scale") = 1.0f, py::arg("layer_height") = 1.0f,
       py::arg("offset") = Vector2{ 0.0f, 0.0f }, py::arg("tint") = py::none(), py::arg("max_z") = INFINITY,
       py::arg("sort") = true, py::arg("ground_in_normal") = false, py::arg("ground_max_z") = INFINITY);

    // Lighting for sprite stacks (StackLighting.h): the sprite_stack library's fx in C++. Built once
    // (fx.load) with the light sprites by name and the shaders; render() each Update with the
    // moving stacks, the frame's lights and, optionally, the static set as (key, items, ids, lights):
    // items a SpriteStackList or tuple list, ids ints (one per item, or None), lights like the frame's.
    // The static set is read only when its key changes, so passing it every frame costs nothing.
    // A light is (name, x, y, size, rotation, power[, (r, g, b)]); ambient a level or an (r, g, b)
    // colour, 0..1. The look properties (shadow_tint, clouds, height_shade, wind) apply from then on.
    py::class_<StackLightingHandle>(m, "StackLighting")
        .def(py::init([](const py::dict& light_sprites, float ground_half, float shadow_scale, float contact_scale,
                         float light_half, float light_scale, float contact_strength, const Shader* shadow_fs,
                         const Shader* contact_fs, const Shader* light_fs, const Shader* sprite, const Shader* blur,
                         const Shader* wet, const Shader* glow, int sun_map, const Shader* sun_depth, const Shader* terrain,
                         int contact_map, int light_map, int mark_map, const Shader* marks) {
            StackLightingSettings settings;
            settings.GroundHalf = ground_half;
            settings.ShadowScale = shadow_scale;
            settings.ContactScale = contact_scale;
            settings.LightHalf = light_half;
            settings.LightScale = light_scale;
            settings.ContactStrength = contact_strength;
            settings.SunMapSize = sun_map;
            settings.ContactMapSize = contact_map;
            settings.LightMapSize = light_map;
            settings.MarkMapSize = mark_map;
            std::unordered_map<std::string, const SpriteStack*> sprites;
            for (const auto& [name, stack] : light_sprites) sprites[name.cast<std::string>()] = stack.cast<const SpriteStack*>();
            return std::make_unique<StackLightingHandle>(settings,
                StackLightingShaders{ shadow_fs, contact_fs, light_fs, sprite, blur, wet, glow, sun_depth, terrain, marks },
                std::move(sprites));
        }), py::arg("light_sprites"), py::arg("ground_half"), py::arg("shadow_scale"), py::arg("contact_scale"),
            py::arg("light_half"), py::arg("light_scale"), py::arg("contact_strength"), py::arg("shadow_fs"),
            py::arg("contact_fs"), py::arg("light_fs"), py::arg("sprite"), py::arg("blur"), py::arg("wet") = py::none(),
            py::arg("glow") = py::none(), py::arg("sun_map") = 0, py::arg("sun_depth") = py::none(),
            py::arg("terrain") = py::none(), py::arg("contact_map") = 0, py::arg("light_map") = 0,
            py::arg("mark_map") = 0, py::arg("marks") = py::none())
        // Ground marks: (kind, x, y, length, width, rotation, strength[, ...]) tuples; replace or add to them.
        .def("set_marks", [](StackLightingHandle& h, const py::list& marks, bool replace) {
            std::vector<StackMark> list;
            list.reserve(marks.size());
            for (const py::handle& item : marks) {
                const py::sequence m = item.cast<py::sequence>();
                if (m.size() < 7) throw py::value_error("StackLighting.set_marks: mark = (kind, x, y, length, width, rotation, strength)");
                list.push_back(StackMark{ m[0].cast<int>(), m[1].cast<float>(), m[2].cast<float>(), m[3].cast<float>(),
                                          m[4].cast<float>(), m[5].cast<float>(), m[6].cast<float>() });
            }
            h.SetMarks(std::move(list), replace);
        }, py::arg("marks"), py::arg("replace"))
        .def("render", [](StackLightingHandle& h, const py::list& items, float layer_height, const py::tuple& sun,
                          const py::object& ambient, float lamps, const py::list& lights, const py::object& stat,
                          std::shared_ptr<CellTerrain> ground, std::optional<Rectangle> sun_area) {
            auto readLights = [&h](const py::list& list, std::vector<StackLight>& out) {
                out.clear();
                out.reserve(list.size());
                for (py::handle item : list) {
                    const py::tuple t = py::reinterpret_borrow<py::tuple>(item);
                    if (t.size() != 6 && t.size() != 7) {
                        throw py::value_error("StackLighting: light = (name, x, y, size, rotation, power[, (r, g, b)])");
                    }
                    auto sprite = h.LightSprites.find(t[0].cast<std::string>());
                    if (sprite == h.LightSprites.end()) throw py::value_error("StackLighting: unknown light sprite");
                    StackLight light{ sprite->second, t[1].cast<float>(), t[2].cast<float>(), t[3].cast<float>(),
                                      t[4].cast<float>(), t[5].cast<float>() };
                    if (t.size() == 7) {
                        const py::tuple c = t[6].cast<py::tuple>();
                        light.Tint = Color{ c[0].cast<unsigned char>(), c[1].cast<unsigned char>(), c[2].cast<unsigned char>(), 255 };
                    }
                    out.push_back(light);
                }
            };
            // Everything the replay needs is read here, on the game side, into the recorded command.
            std::vector<SpriteStackItem> moving;
            ReadSpriteStackItems(items, moving, true);
            std::vector<StackLight> frameLights;
            readLights(lights, frameLights);
            std::shared_ptr<StaticSetData> set;
            if (!stat.is_none()) {
                set = std::make_shared<StaticSetData>();
                const py::tuple t = stat.cast<py::tuple>();
                if (t.size() != 4) throw py::value_error("StackLighting: static = (key, items, ids, lights)");
                set->Key = t[0].cast<long long>();
                if (h.StaticChanged(set->Key)) {             // changed: read it, once
                    set->Changed = true;
                    if (py::isinstance<SpriteStackList>(t[1])) {
                        set->Items = t[1].cast<const SpriteStackList&>().Items;
                    } else {
                        ReadSpriteStackItems(t[1].cast<py::list>(), set->Items, false);
                    }
                    if (!t[2].is_none()) {
                        set->Ids = t[2].cast<std::vector<long long>>();
                        if (set->Ids.size() != set->Items.size()) throw py::value_error("StackLighting: one static id per item");
                        set->HasIds = true;
                    }
                    readLights(t[3].cast<py::list>(), set->Lights);
                }
            }
            if (sun.size() != 3) throw py::value_error("StackLighting: sun = (direction, length, strength)");
            Vector3 amb;
            if (py::isinstance<py::tuple>(ambient) || py::isinstance<py::list>(ambient)) {
                const py::sequence c = ambient.cast<py::sequence>();
                if (c.size() != 3) throw py::value_error("StackLighting: ambient = level or (r, g, b), 0..1");
                amb = Vector3{ c[0].cast<float>(), c[1].cast<float>(), c[2].cast<float>() };
            } else {
                const float a = ambient.cast<float>();
                amb = Vector3{ a, a, a };
            }
            const Vector3 sunValue{ sun[0].cast<float>(), sun[1].cast<float>(), sun[2].cast<float>() };
            h.Render(std::move(moving), layer_height, sunValue, amb, lamps, std::move(frameLights), set, ground,
                     sun_area.value_or(Rectangle{}));
        }, py::arg("items"), py::arg("layer_height"), py::arg("sun"), py::arg("ambient"), py::arg("lamps"),
           py::arg("lights"), py::arg("static") = py::none(), py::arg("ground") = py::none(), py::arg("sun_area") = py::none())
        .def("draw_terrain", [](StackLightingHandle& h, std::shared_ptr<CellTerrain> terrain, const Texture2D& texture,
                                Rectangle area, Color wall, const py::object& water, std::optional<Rectangle> water_area) {
            if (!water.is_none() && !water_area) throw py::value_error("StackLighting.draw_terrain: water needs water_area");
            h.DrawTerrain(terrain, texture, area, wall, water.is_none() ? nullptr : &water.cast<const Texture2D&>(),
                          water_area.value_or(Rectangle{}));
        }, py::arg("terrain"), py::arg("texture"), py::arg("area"), py::arg("wall"), py::arg("water") = py::none(),
           py::arg("water_area") = py::none())
        .def("draw_ground", &StackLightingHandle::DrawGround)
        .def("draw_ground_light", &StackLightingHandle::DrawGroundLight)
        .def("draw_glow", &StackLightingHandle::DrawGlow, py::arg("strength"))
        .def("draw_wet", [](StackLightingHandle& h, const py::list& patches, float wetness, float view_rotation, Color sky,
                             const py::object& eye) {
            std::vector<WetPatch> wet;
            wet.reserve(patches.size());
            for (py::handle item : patches) {
                const py::tuple t = item.cast<py::tuple>();
                if (t.size() != 3) throw py::value_error("StackLighting.draw_wet: patch = (texture, source, dest)");
                wet.push_back(WetPatch{ &t[0].cast<const Texture2D&>(), t[1].cast<Rectangle>(), t[2].cast<Rectangle>() });
            }
            h.DrawWet(std::move(wet), wetness, view_rotation, sky,
                      eye.is_none() ? std::nullopt : std::optional<Vector3>(eye.cast<Vector3>()));
        }, py::arg("patches"), py::arg("wetness"), py::arg("view_rotation"), py::arg("sky"), py::arg("eye") = py::none())
        .def_property("fog",
            [](const StackLightingHandle& h) { return py::make_tuple(h.Look.Fog, h.Look.FogStart, h.Look.FogEnd); },
            [](StackLightingHandle& h, const py::tuple& t) {
                if (t.size() != 3) throw py::value_error("StackLighting.fog = (colour, start, end)");
                h.Look.Fog = t[0].cast<Color>();
                h.Look.FogStart = t[1].cast<float>();
                h.Look.FogEnd = t[2].cast<float>();
            })
        .def_property("shadow_tint", [](const StackLightingHandle& h) { return h.Look.ShadowTint; },
                      [](StackLightingHandle& h, Color c) { h.Look.ShadowTint = c; })
        .def_property("clouds",
            [](const StackLightingHandle& h) {
                const StackClouds& c = h.Look.Clouds;
                return py::make_tuple(c.Cover, c.Strength, c.Offset.x, c.Offset.y, c.Scale);
            },
            [](StackLightingHandle& h, const py::tuple& t) {
                if (t.size() != 5) throw py::value_error("StackLighting.clouds = (cover, strength, offset_x, offset_y, scale)");
                h.SetClouds(t[0].cast<float>(), t[1].cast<float>(), Vector2{ t[2].cast<float>(), t[3].cast<float>() }, t[4].cast<float>());
            })
        .def_property("height_shade",
            [](const StackLightingHandle& h) { return py::make_tuple(h.Look.BaseShade, h.Look.ShadeReach); },
            [](StackLightingHandle& h, const py::tuple& t) {
                if (t.size() != 2) throw py::value_error("StackLighting.height_shade = (strength, reach)");
                h.SetHeightShade(t[0].cast<float>(), t[1].cast<float>());
            })
        .def_property("wind",
            [](const StackLightingHandle& h) {
                return py::make_tuple(h.Look.WindTime, h.WindDegrees(), h.Look.WindStrength);
            },
            [](StackLightingHandle& h, const py::tuple& t) {
                if (t.size() != 3) throw py::value_error("StackLighting.wind = (time, direction_degrees, strength)");
                h.SetWind(t[0].cast<float>(), t[1].cast<float>(), t[2].cast<float>());
            })
        .def("draw_silhouettes", [](StackLightingHandle& h, const py::list& items, float view_rotation,
                                    float layer_height, float opacity, std::shared_ptr<CellTerrain> ground) {
            std::vector<SpriteStackItem> read;
            ReadSpriteStackItems(items, read, true);
            h.DrawSilhouettes(std::move(read), view_rotation, layer_height, opacity, ground);
        }, py::arg("items"), py::arg("view_rotation"), py::arg("layer_height"), py::arg("opacity"),
           py::arg("ground") = py::none())
        // Rotor downwash: up to 4 (x, y, radius, strength) points; swaying instances lean away from them.
        .def_property("wash",
            [](const StackLightingHandle& h) {
                py::list out;
                for (const Vector4& w : h.Wash()) out.append(py::make_tuple(w.x, w.y, w.z, w.w));
                return out;
            },
            [](StackLightingHandle& h, const py::list& points) {
                if (points.size() > 4) throw py::value_error("StackLighting.wash: at most 4 (x, y, radius, strength) points");
                std::vector<Vector4> read;
                for (py::handle item : points) {
                    const py::sequence p = item.cast<py::sequence>();
                    if (p.size() != 4) throw py::value_error("StackLighting.wash: point = (x, y, radius, strength)");
                    read.push_back(Vector4{ p[0].cast<float>(), p[1].cast<float>(), p[2].cast<float>(), p[3].cast<float>() });
                }
                h.SetWash(read);
            })
        .def("draw_stacks", [](StackLightingHandle& h, const py::list& items, float view_rotation, float height_scale,
                               float layer_height, bool z_up, std::shared_ptr<CellTerrain> ground) {
            std::vector<SpriteStackItem> read;
            std::vector<StaticStacksDraw> statics;
            {
                ProfileScope scope("sprite_stacks_read");
                ReadSpriteStackItems(items, read, true, &statics);
            }
            h.DrawStacks(std::move(read), std::move(statics), view_rotation, height_scale, layer_height, z_up, ground);
        }, py::arg("items"), py::arg("view_rotation"), py::arg("height_scale"), py::arg("layer_height"),
           py::arg("z_up") = false, py::arg("ground") = py::none())
        .def_property("static_sun",
            [](const StackLightingHandle& h) {
                const StaticSunMode mode = h.Look.SunMode;
                return std::string(mode == StaticSunMode::Smooth ? "smooth" : mode == StaticSunMode::Step ? "step" : "exact");
            },
            [](StackLightingHandle& h, const std::string& mode) {
                if (mode == "smooth") h.Look.SunMode = StaticSunMode::Smooth;
                else if (mode == "step") h.Look.SunMode = StaticSunMode::Step;
                else if (mode == "exact") h.Look.SunMode = StaticSunMode::Exact;
                else throw py::value_error("StackLighting.static_sun: smooth, step or exact");
            });

    // Particles (Particles.h): soft puffs in the world. emit one at a time from Python, update and draw
    // once a frame (draw in the world pass, with the camera's rotation and cot(pitch) like the stacks).
    py::class_<ParticlePool>(m, "Particles")
        .def(py::init([](int capacity) {
            return std::make_unique<ParticlePool>(static_cast<size_t>(std::max(0, capacity)));
        }), py::arg("capacity") = 4096)
        .def("emit", [](ParticlePool& pool, float x, float y, float z, float vx, float vy, float vz, float life,
                        float size, float size_end, Color tint, float drag, float lift) {
            Particle p;
            p.Position = Vector3{ x, y, z };
            p.Velocity = Vector3{ vx, vy, vz };
            p.Life = life;
            p.Size0 = size;
            p.Size1 = size_end;
            p.Tint = tint;
            p.Drag = drag;
            p.Lift = lift;
            EmitParticle(pool, p);
        }, py::arg("x"), py::arg("y"), py::arg("z"), py::arg("vx"), py::arg("vy"), py::arg("vz"), py::arg("life"),
           py::arg("size"), py::arg("size_end"), py::arg("tint"), py::arg("drag") = 0.0f, py::arg("lift") = 0.0f)
        .def("update", [](ParticlePool& pool, float dt) { UpdateParticles(pool, dt); }, py::arg("dt"))
        .def("draw", &Api::DrawParticles, py::arg("view_rotation") = 0.0f, py::arg("height_scale") = 0.0f, py::arg("z_up") = false,
             py::arg("ground") = py::none())
        .def("clear", &ParticlePool::Clear)
        .def("__len__", [](const ParticlePool& pool) { return pool.Live.size(); });

    // Culling inside draw_sprite_stacks is always on; switching it off is only for measuring it.
    m.def("set_sprite_stack_culling", &Api::SetSpriteStackCulling, py::arg("enabled"));
    m.def("set_sprite_stack_instancing", &Api::SetSpriteStackInstancing, py::arg("enabled"));
    m.def("clear_background", &Api::ClearBackground, py::arg("color"));
    m.def("begin_shader_mode", &Api::BeginShaderMode, py::arg("shader"));
    m.def("end_shader_mode", &Api::EndShaderMode);
    m.def("set_shader_value", [](const Shader& shader, const std::string& name, const std::vector<float>& value) -> void {
        if (value.empty() || value.size() > 4) throw py::value_error("set_shader_value: value takes 1 to 4 floats");
        Api::SetShaderValue(shader, name, value);
    }, py::arg("shader"), py::arg("name"), py::arg("value"));
    m.def("set_shader_texture", &Api::SetShaderTexture, py::arg("shader"), py::arg("name"), py::arg("texture"));

    m.def("draw_rectangle", [](float x, float y, float width, float height, Color color) {
        Api::DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width), static_cast<int>(height), color);
    }, py::arg("x"), py::arg("y"), py::arg("width"), py::arg("height"), py::arg("color"));
    m.def("draw_rectangle_rec", &Api::DrawRectangleRec, py::arg("rec"), py::arg("color"));
    m.def("draw_rectangle_lines", [](float x, float y, float width, float height, Color color) {
        Api::DrawRectangleLines(static_cast<int>(x), static_cast<int>(y), static_cast<int>(width), static_cast<int>(height), color);
    }, py::arg("x"), py::arg("y"), py::arg("width"), py::arg("height"), py::arg("color"));
    m.def("draw_rectangle_lines_ex", &Api::DrawRectangleLinesEx, py::arg("rec"), py::arg("thickness"), py::arg("color"));
    m.def("draw_circle", [](float center_x, float center_y, float radius, Color color) {
        Api::DrawCircle(static_cast<int>(center_x), static_cast<int>(center_y), radius, color);
    }, py::arg("center_x"), py::arg("center_y"), py::arg("radius"), py::arg("color"));
    m.def("draw_sphere", &Api::DrawSphere, py::arg("center"), py::arg("radius"), py::arg("color"));
    m.def("draw_billboards", &Api::DrawBillboards, py::arg("texture"), py::arg("camera"), py::arg("centers"), py::arg("size"),
          py::arg("tint") = WHITE);
    m.def("draw_circle_v", &Api::DrawCircleV, py::arg("center"), py::arg("radius"), py::arg("color"));
    m.def("draw_circle_lines", [](float center_x, float center_y, float radius, Color color) {
        Api::DrawCircleLines(static_cast<int>(center_x), static_cast<int>(center_y), radius, color);
    }, py::arg("center_x"), py::arg("center_y"), py::arg("radius"), py::arg("color"));
    m.def("draw_line", [](float start_x, float start_y, float end_x, float end_y, Color color) {
        Api::DrawLine(static_cast<int>(start_x), static_cast<int>(start_y), static_cast<int>(end_x), static_cast<int>(end_y), color);
    }, py::arg("start_x"), py::arg("start_y"), py::arg("end_x"), py::arg("end_y"), py::arg("color"));
    m.def("draw_line_ex", &Api::DrawLineEx, py::arg("start"), py::arg("end"), py::arg("thickness"), py::arg("color"));

    // Shapes beyond rects/circles/lines. Point lists cross as list[Vector2] and are forwarded as-is.
    // raylib culls by winding: triangles (and the first triangle of a strip or fan) must be
    // counter-clockwise as seen on screen, or nothing is drawn.
    m.def("draw_rectangle_pro", &Api::DrawRectanglePro, py::arg("rec"), py::arg("origin"), py::arg("rotation"), py::arg("color"));
    m.def("draw_rectangle_rounded", &Api::DrawRectangleRounded, py::arg("rec"), py::arg("roundness"), py::arg("segments"), py::arg("color"));
    m.def("draw_rectangle_gradient_v", &Api::DrawRectangleGradientV, py::arg("rec"), py::arg("top"), py::arg("bottom"));
    m.def("draw_rectangle_gradient_h", &Api::DrawRectangleGradientH, py::arg("rec"), py::arg("left"), py::arg("right"));
    m.def("draw_rectangle_gradient_ex", &Api::DrawRectangleGradientEx, py::arg("rec"), py::arg("top_left"), py::arg("bottom_left"),
          py::arg("bottom_right"), py::arg("top_right"));
    m.def("draw_circle_gradient", &Api::DrawCircleGradient, py::arg("center"), py::arg("radius"), py::arg("inner"), py::arg("outer"));
    m.def("draw_triangle", &Api::DrawTriangle, py::arg("v1"), py::arg("v2"), py::arg("v3"), py::arg("color"));
    m.def("draw_triangle_fan", &Api::DrawTriangleFan, py::arg("points"), py::arg("color"));
    m.def("draw_triangle_strip", &Api::DrawTriangleStrip, py::arg("points"), py::arg("color"));
    m.def("draw_poly", &Api::DrawPoly, py::arg("center"), py::arg("sides"), py::arg("radius"), py::arg("rotation"), py::arg("color"));
    m.def("draw_circle_sector", &Api::DrawCircleSector, py::arg("center"), py::arg("radius"), py::arg("start_angle"), py::arg("end_angle"),
          py::arg("segments"), py::arg("color"));
    m.def("draw_ring", &Api::DrawRing, py::arg("center"), py::arg("inner_radius"), py::arg("outer_radius"), py::arg("start_angle"),
          py::arg("end_angle"), py::arg("segments"), py::arg("color"));
    m.def("draw_line_strip", &Api::DrawLineStrip, py::arg("points"), py::arg("color"));
    // Mitered joints (raylib built with SUPPORT_SPLINE_MITERS, see CMakeLists.txt).
    // Points may be (x, y) pairs (tuple or list, e.g. a JSON path): building a Vector2 per point costs
    // ~1 us each in Python (a chart's 288).
    m.def("draw_spline_linear", [](const py::sequence& points, float thick, Color color) {
        std::vector<Vector2> pts;
        pts.reserve(points.size());
        for (py::handle p : points) {
            if (py::isinstance<Vector2>(p)) {
                pts.push_back(p.cast<Vector2>());
            } else {
                auto s = py::reinterpret_borrow<py::sequence>(p);
                pts.push_back({ s[0].cast<float>(), s[1].cast<float>() });
            }
        }
        Api::DrawSplineLinear(std::move(pts), thick, color);
    }, py::arg("points"), py::arg("thick"), py::arg("color"));

    m.def("draw_text", [](const std::string& text, float x, float y, int font_size, Color color) {
        Api::DrawText(text, static_cast<int>(x), static_cast<int>(y), font_size, color);
    }, py::arg("text"), py::arg("x"), py::arg("y"), py::arg("font_size"), py::arg("color"));
    m.def("draw_text_ex", &Api::DrawTextEx, py::arg("font"), py::arg("text"), py::arg("position"), py::arg("font_size"), py::arg("spacing"),
          py::arg("color"));
    m.def("measure_text", [](const std::string& text, int font_size) -> int {
        return MeasureText(text.c_str(), font_size);
    }, py::arg("text"), py::arg("font_size"));
    m.def("measure_text_ex", &Api::MeasureTextEx, py::arg("font"), py::arg("text"), py::arg("font_size"), py::arg("spacing"));

    // Input, answered from this frame's copy (FrameInput.h), never raylib's live state.
    m.def("is_key_down", &Api::IsKeyDown, py::arg("key"));
    m.def("is_key_up", &Api::IsKeyUp, py::arg("key"));
    m.def("is_key_pressed", &Api::IsKeyPressed, py::arg("key"));
    m.def("is_key_released", &Api::IsKeyReleased, py::arg("key"));
    m.def("get_key_pressed", &Api::GetKeyPressed);
    m.def("is_ime_composing", &Api::IsImeComposing);
    m.def("is_mouse_button_down", &Api::IsMouseButtonDown, py::arg("button"));
    m.def("is_mouse_button_pressed", &Api::IsMouseButtonPressed, py::arg("button"));
    m.def("is_mouse_button_released", &Api::IsMouseButtonReleased, py::arg("button"));
    m.def("get_mouse_position", &Api::GetMousePosition);
    m.def("get_mouse_delta", &Api::GetMouseDelta);
    m.def("get_mouse_wheel_move", &Api::GetMouseWheelMove);
    m.def("get_mouse_wheel_move_v", &Api::GetMouseWheelMoveV);
    m.def("get_pinch_zoom", &Api::GetPinchZoom);
    m.def("is_gamepad_available", &Api::IsGamepadAvailable, py::arg("gamepad"));
    m.def("is_gamepad_button_down", &Api::IsGamepadButtonDown, py::arg("gamepad"), py::arg("button"));
    m.def("is_gamepad_button_pressed", &Api::IsGamepadButtonPressed, py::arg("gamepad"), py::arg("button"));
    m.def("get_gamepad_axis_movement", &Api::GetGamepadAxisMovement, py::arg("gamepad"), py::arg("axis"));

    // Thin forwards - Collision
    m.def("check_collision_recs", [](Rectangle rec1, Rectangle rec2) -> bool {
        return CheckCollisionRecs(rec1, rec2);
    }, py::arg("rec1"), py::arg("rec2"));

    m.def("check_collision_circles", [](Vector2 c1, float r1, Vector2 c2, float r2) -> bool {
        return CheckCollisionCircles(c1, r1, c2, r2);
    }, py::arg("c1"), py::arg("r1"), py::arg("c2"), py::arg("r2"));

    m.def("check_collision_circle_rec", [](Vector2 center, float radius, Rectangle rec) -> bool {
        return CheckCollisionCircleRec(center, radius, rec);
    }, py::arg("center"), py::arg("radius"), py::arg("rec"));

    m.def("check_collision_point_rec", [](Vector2 point, Rectangle rec) -> bool {
        return CheckCollisionPointRec(point, rec);
    }, py::arg("point"), py::arg("rec"));

    m.def("check_collision_point_circle", [](Vector2 point, Vector2 center, float radius) -> bool {
        return CheckCollisionPointCircle(point, center, radius);
    }, py::arg("point"), py::arg("center"), py::arg("radius"));

    m.def("get_collision_rec", [](Rectangle rec1, Rectangle rec2) -> Rectangle {
        return GetCollisionRec(rec1, rec2);
    }, py::arg("rec1"), py::arg("rec2"));

    // Time / screen / camera math. The window's state as of this frame (FrameInput.h); changes are
    // platform requests the render side runs before it draws this frame, so they show next frame.
    m.def("get_frame_time", &Api::GetFrameTime);
    m.def("set_clipboard_text", &Api::SetClipboardText, py::arg("text"));
    m.def("get_clipboard_text", &Api::GetClipboardText);
    m.def("get_time", []() -> float { return static_cast<float>(IYI::GameInput().Time); });   // this frame's clock (--fixed-dt: game time)
    m.def("get_fps", &Api::GetFPS);
    m.def("get_screen_width", &Api::GetScreenWidth);
    m.def("get_screen_height", &Api::GetScreenHeight);
    m.def("set_window_size", &Api::SetWindowSize, py::arg("width"), py::arg("height"));
    m.def("set_display_cutout", &Api::SetDisplayCutout, py::arg("use"));
    m.def("set_window_title", &Api::SetWindowTitle, py::arg("title"));
    m.def("get_monitor_resolutions", &Api::GetMonitorResolutions);
    m.def("get_world_to_screen_2d", [](Vector2 position, Camera2D camera, float vertical_scale) -> Vector2 {
        if (!std::isfinite(vertical_scale) || vertical_scale <= 0.0f) throw py::value_error("vertical_scale must be a finite number > 0");
        return Api::GetWorldToScreen2D(position, camera, vertical_scale);
    }, py::arg("position"), py::arg("camera"), py::arg("vertical_scale") = 1.0f);
    // The ray under a screen point for a set_camera_3d camera, in world space (z up): (origin, direction).
    m.def("get_screen_to_world_ray", [](Vector2 position, const Camera3D& camera) -> std::pair<Vector3, Vector3> {
        const Ray ray = Api::GetScreenToWorldRay(position, camera);
        return { ray.position, ray.direction };
    }, py::arg("position"), py::arg("camera"));
    m.def("get_world_to_screen", &Api::GetWorldToScreen, py::arg("position"), py::arg("camera"));
    m.def("begin_mode_2d", &Api::BeginMode2D, py::arg("camera"));
    m.def("end_mode_2d", &Api::EndMode2D);
    m.def("get_screen_to_world_2d", [](Vector2 position, Camera2D camera, float vertical_scale) -> Vector2 {
        if (!std::isfinite(vertical_scale) || vertical_scale <= 0.0f) throw py::value_error("vertical_scale must be a finite number > 0");
        return Api::GetScreenToWorld2D(position, camera, vertical_scale);
    }, py::arg("position"), py::arg("camera"), py::arg("vertical_scale") = 1.0f);
}

} // namespace IYI
