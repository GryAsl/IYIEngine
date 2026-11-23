// RTS selection: click and drag-box picking over things the game puts on screen, additive selection
// and control groups, over a state the caller keeps (an actor member; Python: the state dict). No game
// rules: the game decides the tiers (what wins where things overlap) and what may be boxed. One
// implementation for native games and Python (scripts/rts_select/select.py wraps it through
// engine.rts_select, src/LibraryBindings.cpp). Checks: tests/engine_tests.cpp TestRtsSelect.

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "raylib.h"

namespace IYI::Select {

constexpr float DRAG = 6.0f;      // screen pixels the cursor moves with the button down before a click becomes a box
constexpr double DOUBLE = 0.35;   // seconds between two recalls of the same group that centre the camera on it

using Box = std::array<float, 4>;   // x0, y0, x1, y1 on screen

struct State
{
    std::vector<std::string> Selected;
    std::map<int, std::vector<std::string>> Groups;
    std::optional<Vector2> DragFrom;   // where the button went down
    std::optional<Box> DragBox;        // the box being dragged, while it is one
    std::optional<int> Recalled;       // the group last recalled, and when
    double RecalledAt = -1e9;
};

struct Hit { bool IsBox; Vector2 At; Box Area; };   // a click at At, or a box over Area
struct Item { float X, Y; int Tier; bool Boxable; };   // a thing on screen; the game keeps its id by index

// The mouse this frame -> a click or a box on the release (nullopt otherwise).
std::optional<Hit> Drag(State& st, Vector2 mouse, bool pressed, bool down, bool released);
// World points (z up) through a 3D camera to screen items; index[i] says which entry items[i] is (the
// ones behind the camera are left out).
std::vector<Item> Project(const Camera3D& camera, const std::vector<Vector3>& points, const std::vector<int>& tiers,
                          const std::vector<bool>& boxable, std::vector<int>& index);
int Pick(const std::vector<Item>& items, Vector2 at, float reach);   // the index under the cursor (lowest tier, then nearest), -1: none
std::vector<int> InBox(const std::vector<Item>& items, Box box);    // indices of the boxable items inside
void Apply(State& st, const std::vector<std::string>& ids, bool add = false);   // replace, or toggle ids into it
// Control group n: store the selection (assign), or recall it; true on a quick second recall.
bool Group(State& st, int n, bool assign, double now);

} // namespace IYI::Select
