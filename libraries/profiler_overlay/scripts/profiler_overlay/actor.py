"""Profiler overlay: the engine's frame profiler (engine.get_profile) as a table on screen.

Opt-in development view for any game (engine rule 9: the engine itself draws nothing). Add the
library to project.json "libraries" and spawn it after the HUD (screen-space actors draw in spawn
order, so it stays on top), optionally placing it with state x, y (screen pixels):
    p = engine.spawn_actor("Profiler", "scripts/profiler_overlay/actor.py", screen_space=True)
    p.state.update(x=12, y=270)
F3 cycles the view: one window with the two threads and their main stages ("game thread", "render thread"
and their direct children), then two windows (game left, render right, every row), then off (state "mode" 0 off,
1 summary, 2 two windows). F4 switches the engine's GPU timing on / off (engine.set_gpu_profiling: a
"gpu/..." row for every section, the GPU time of what it drew). Rows are the last one-second window: engine stages (update, draw, present...),
each actor script under the stage that ran it, the game's own profile_begin sections and the
engine's C++ sections under whatever called them (sprite_stacks: read, cull, sort, emit); as many
as the screen fits, the cheapest leaves left out first when there are more. The bar
is the average, the thin tick the worst frame, against the frame budget of the measured fps.
A "self" row is CPU time in its parent outside the named child sections.

The table itself is C++ (libraries/profiler_overlay/src/Overlay.h, shared with native games, which
spawn its ProfilerOverlay::Actor instead); this actor keeps the state and forwards.
"""

from engine import profiler_overlay as _c


def Start(actor):
    actor.state.setdefault("mode", 0)
    actor.state.setdefault("x", 12)
    actor.state.setdefault("y", 12)
    actor.state.setdefault("gpu", False)


def Update(actor, dt):
    actor.state["mode"], actor.state["gpu"] = _c.update(actor.state["mode"], actor.state["gpu"])


def Drawing(actor):
    _c.draw(actor.state["mode"], actor.state["x"], actor.state["y"], actor.state["gpu"])
