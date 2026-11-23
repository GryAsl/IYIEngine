"""Tiny IYIUI helpers: place elements by rect, lay cards on a grid, make text and buttons in one call.

Layout rule for every page: the page is a grid of cards (`cell`) with GAP between them and no
outer margin (the shell already insets the page); inside a card, content starts PAD from the edges
and below the card title (`card` returns the card and its body panel).

Let the engine do the arithmetic (docs/SPEC-IYIUI.md "Toolbox"): `stack` lines children up (vertical,
horizontal or wrapping rows), `fit=True` sizes a text, button or container to its content, `window`
is a movable card with a close button. Hand-placed y = i * row_h lists and text-length height guesses
are what these replace.
"""

import engine

V = engine.Vector2
GAP = 20        # between cards
PAD = 20        # inside a card
TITLE_H = 52    # card title row


def place(el, x, y, w, h):
    """Fixed logical rect from the parent's top-left."""
    el.anchor_min = el.anchor_max = el.pivot = V(0, 0)
    el.anchored_position, el.size_delta = V(x, y), V(w, h)
    return el


def frac(el, x, y, w, h):
    """Rect as fractions of the parent (0..1): follows the parent's size."""
    el.anchor_min, el.anchor_max = V(x, y), V(x + w, y + h)
    el.pivot, el.anchored_position, el.size_delta = V(0, 0), V(0, 0), V(0, 0)
    return el


def fill(el, left=0, top=0, right=0, bottom=0):
    """Stretch over the parent with insets."""
    el.anchor_min, el.anchor_max, el.pivot = V(0, 0), V(1, 1), V(0, 0)
    el.anchored_position, el.size_delta = V(left, top), V(-left - right, -top - bottom)
    return el


def cell(el, x0, y0, x1, y1, gap=GAP):
    """Rect as fractions of the parent with `gap` on every inner edge (none on the outer ones),
    so neighbouring cells line up exactly: cell(a, 0, 0, 0.5, 1) and cell(b, 0.5, 0, 1, 1)."""
    left, top = (gap / 2 if x0 > 0 else 0), (gap / 2 if y0 > 0 else 0)
    right, bottom = (gap / 2 if x1 < 1 else 0), (gap / 2 if y1 < 1 else 0)
    el.anchor_min, el.anchor_max, el.pivot = V(x0, y0), V(x1, y1), V(0, 0)
    el.anchored_position, el.size_delta = V(left, top), V(-left - right, -top - bottom)
    return el


def band(el, top, height, left=PAD, right=PAD):
    """A full-width row of the parent (inset left/right) at `top`, `height` tall."""
    el.anchor_min, el.anchor_max, el.pivot = V(0, 0), V(1, 0), V(0, 0)
    el.anchored_position, el.size_delta = V(left, top), V(-left - right, height)
    return el


def foot(el, bottom, height, left=0, right=0):
    """A full-width row measured from the parent's bottom edge."""
    el.anchor_min, el.anchor_max, el.pivot = V(0, 1), V(1, 1), V(0, 1)
    el.anchored_position, el.size_delta = V(left, -bottom), V(-left - right, height)
    return el


def corner(el, w, h, right=0, bottom=0):
    """A fixed-size element pinned to the parent's bottom-right corner."""
    el.anchor_min = el.anchor_max = el.pivot = V(1, 1)
    el.anchored_position, el.size_delta = V(-right, -bottom), V(w, h)
    return el


def stack(canvas, name, parent, axis="vertical", gap=8, pad=0, fit=False, style="panel"):
    """A layout container: children line up by themselves, so they only need a size (place(el, 0, 0, w, h)).
    axis: "vertical" (full width, stacked), "horizontal" (a row), "wrap" (a row that flows onto new rows).
    fit: its height follows the children."""
    p = panel(canvas, name, parent, style)
    p.layout_mode = "vertical" if axis == "vertical" else "horizontal"
    p.wrap_children = axis == "wrap"
    p.spacing, p.padding, p.fit_height = gap, pad, fit
    return p


def card(canvas, name, parent, title_key="", **args):
    """A card with an optional title; returns (card, body). Body is the padded content area."""
    c = panel(canvas, name, parent, "card")
    if title_key:   # window title band across the card's top edge
        bar = band(panel(canvas, f"{name}_bar", c, "title_bar"), 0, TITLE_H - 12, 0, 0)
        band(text(canvas, f"{name}_title", bar, title_key, "title_bar_text", **args), 4, TITLE_H - 20, 14, 14)
    body = fill(panel(canvas, f"{name}_body", c), PAD, TITLE_H + 4 if title_key else PAD, PAD, PAD)
    return c, body


def window(canvas, name, parent, title_key="", close=True, drag=True, **args):
    """A desktop-style window: a card with a title band; returns (window, body). close: a button
    `<name>_close` in the band (the caller decides what its click does: hide, close a modal). drag: holding
    the band moves the window; call update_windows(canvas) once a frame."""
    win, body = card(canvas, name, parent, title_key, **args)
    band_el = canvas.find(f"{name}_bar")
    if drag:
        band_el = fill(button(canvas, f"{name}_drag", band_el, style="hit"))
        canvas.state.setdefault("game_kit_windows", []).append(name)   # update_windows moves these
    if close:   # inside the drag area: a child button never counts as overlapping its parent
        set_text(corner(button(canvas, f"{name}_close", band_el, style="window_close"), TITLE_H - 20, TITLE_H - 20, 4, 4), raw="×")
    return win, body


def update_windows(canvas):
    """Moves each draggable window whose title band is held by the mouse's movement (window(drag=True))."""
    names = canvas.state.get("game_kit_windows", [])
    alive = [n for n in names if canvas.find(n)]
    names[:] = alive
    d = engine.get_mouse_delta()
    if not (d.x or d.y):
        return
    for n in alive:
        if canvas.find(f"{n}_drag").pressed:
            win = canvas.find(n)
            win.anchored_position = V(win.anchored_position.x + d.x / canvas.scale_factor,
                                      win.anchored_position.y + d.y / canvas.scale_factor)


def icon(canvas, name, parent, path, size):
    img = canvas.add_image(name, parent)
    img.texture = engine.load_texture(path)
    img.preserve_aspect = True
    return place(img, 0, 0, size, size)


def panel(canvas, name, parent, style="panel"):
    p = canvas.add_panel(name, parent)
    p.style_class = style
    return p


def text(canvas, name, parent, key="", style="text", align="left", fit=False, **args):
    """fit: wraps, and its height follows the text (inside a stack, or place() it with any height)."""
    t = canvas.add_text(name, parent)
    t.style_class = style
    t.horizontal_alignment = align
    t.fit_height = fit
    set_text(t, key, **args)
    return t


def set_text(el, key="", raw=None, **args):
    """A locale key with args, or raw text (site words, numbers)."""
    if raw is not None:
        if el.loc_key:
            el.loc_key = ""
        if el.text != raw:
            el.text = raw
        return
    args = {k: str(v) for k, v in args.items()}
    if el.loc_key != key:
        el.loc_key = key
    if dict(el.loc_args) != args:
        el.loc_args = args


def button(canvas, name, parent, key="", style="button", **args):
    b = canvas.add_button(name, parent)
    b.style_class = style
    set_text(b, key, **args)
    return b


def loc(loc_key, **args) -> str:
    """Resolves a key whose args may themselves be keys ("where.top_left")."""
    resolved = {k: (engine.loc.text(v) if isinstance(v, str) and "." in v and engine.loc.has(v) else str(v))
                for k, v in args.items()}
    return engine.loc.text(loc_key, **resolved)


def money(value) -> str:
    v = int(round(value))
    return ("-" if v < 0 else "") + "$" + f"{abs(v):,}"
