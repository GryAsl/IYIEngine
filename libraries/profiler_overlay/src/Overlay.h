// Profiler overlay: the engine's frame profiler (ProfileResults) as a table on screen. An opt-in
// development view for any game (engine rule 9: the engine itself draws nothing). F3 cycles the view
// (0 off, 1 one window with the two threads and their main stages, 2 one window per thread, every
// row), F4 switches GPU timing. Rows are the last one-second window; the cheapest leaves are left out
// first when the screen cannot fit them all. One implementation: the C++ actor below for native games,
// and scripts/profiler_overlay/actor.py for Python games (engine.profiler_overlay, LibraryBindings.cpp).
// Checks: tests/engine_tests.cpp TestProfilerOverlay.

#pragma once

#include <string>
#include <utility>
#include <vector>
#include "IYI/GameActor.h"
#include "IYI/Profiler.h"

namespace IYI::ProfilerOverlay {

struct Row { std::string Label; int Depth; float Avg, Worst, Calls; };

std::vector<ProfileSection> Summary(const std::vector<ProfileSection>& profile);   // thread roots and their direct children
// (game rows, render rows): the render window also gets the "gpu/render/..." rows after its CPU rows.
std::pair<std::vector<ProfileSection>, std::vector<ProfileSection>> Split(const std::vector<ProfileSection>& profile);
// The shown rows and how many were left out to fit. A label is the name past its nearest listed parent
// ("update/scripts/actors/world.py/sim" reads "sim" under "actors/world.py"; "scripts/" is dropped; a
// thread root reads "game thread" / "render thread" and is kept whatever its time).
std::pair<std::vector<Row>, int> Rows(const std::vector<ProfileSection>& profile, float minMs, int maxRows);

// Update: F3 / F4 (mode 0..2, gpu: GPU timing on). Draw: the table at (x, y), screen space.
void Update(int& mode, bool& gpu);
void Draw(int mode, int x, int y, bool gpu);

// The native game's actor: spawn it screen space, after the HUD, so it draws on top.
struct Actor : GameActor
{
    int Mode = 0, X = 12, Y = 12;
    bool Gpu = false;
    void Update(float) override { ProfilerOverlay::Update(Mode, Gpu); }
    void Drawing() override { Draw(Mode, X, Y, Gpu); }
};

} // namespace IYI::ProfilerOverlay
