"""RTS selection: click and drag-box picking over things the game puts on screen, additive selection
and control groups. Plain functions over a dict the caller keeps (actor.state); no game rules here.

    st = {}                                         one selection state per player
    hit = select.drag(st, mouse, pressed, down, released)
          -> None, ("click", (x, y)) or ("box", (x0, y0, x1, y1)) on the release
    items = select.project(camera, entries)         entries (id, x, y, z, tier, boxable) -> screen items
    select.pick(items, (x, y), reach)               the id under the cursor: the lowest tier wins, then the nearest
    select.in_box(items, box)                       the boxable ids inside a screen box
    select.apply(st, ids, add)                      replace st["selected"], or toggle ids into it (add)
    select.group(st, n, assign, now)                control group n: store the selection, or recall it
                                                    (True on a quick second recall: centre the camera on it)

Tiers say what wins where things overlap (a game's own units over civilians over buildings); the
game decides them and what may be boxed. Selected ids are strings. World to screen is
engine.get_world_to_screen, the ground under the cursor engine.SightGrid.ray.

The logic is C++ (libraries/rts_select/src/Select.h, shared with native games); these functions
forward to it. Its checks are in the engine tests.
"""

from engine import rts_select as _c

drag, project, pick, in_box, group = _c.drag, _c.project, _c.pick, _c.in_box, _c.group


def apply(st, ids, add=False):
    _c.apply(st, list(ids), add)   # list: ids may be any iterable (a set, a generator)
