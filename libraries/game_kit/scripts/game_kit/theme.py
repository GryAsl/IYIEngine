"""UI skins and style helpers for management games.

A skin is a palette plus a shape (corner radius, line width, hard shadow offset). `apply` copies the
skin's colours into this module's names (INK, GOOD, ...), so pages read them as `theme.INK` at build
time, never `from game_kit.theme import INK`.
  office:   light grey surfaces, green window title bands (default; HR's Modern UI is the same family)
  retro_os: pastel desktop OS, 3 px ink outlines, solid offset shadows

Text sizes are roles of one type scale (TYPE), not numbers: `style(..., role="body")`. The engine sizes them
(theme.type_scale x canvas.text_scale), so a text-size setting is `canvas.text_scale = x`, no rebuild.
"""

import engine

C = engine.Color
CLEAR = C(0, 0, 0, 0)

SKINS = {
    "office": dict(
        BG=C(232, 233, 232, 255), SURFACE=C(214, 216, 214, 255), CARD=C(245, 245, 243, 255),
        INSET=C(255, 255, 255, 255), LINE=C(168, 170, 168, 255), ACCENT=C(72, 150, 72, 255),
        ACCENT_SOFT=C(72, 150, 72, 50), INK=C(25, 25, 25, 255), MUTED=C(95, 98, 95, 255),
        GOOD=C(40, 140, 60, 255), WARN=C(180, 110, 0, 255), BAD=C(200, 50, 50, 255),
        TITLE=C(72, 150, 72, 255), TITLE_INK=C(255, 255, 255, 255), BTN=C(226, 228, 226, 255),
        BTN_HOVER=C(242, 244, 242, 255), BTN_DOWN=C(205, 208, 205, 255), ROW=C(236, 237, 236, 255),
        ROW_ON=C(208, 234, 208, 255), SHADOW=C(0, 0, 0, 45), DIM=C(0, 0, 0, 110),
        RADIUS=2.0, LINE_W=1.0, SHADOW_OFF=3.0),
    "retro_os": dict(
        BG=C(246, 226, 244, 255), SURFACE=C(232, 214, 248, 255), CARD=C(255, 246, 252, 255),
        INSET=C(255, 255, 255, 255), LINE=C(128, 96, 210, 255), ACCENT=C(140, 96, 232, 255),
        ACCENT_SOFT=C(140, 96, 232, 50), INK=C(64, 36, 110, 255), MUTED=C(130, 100, 168, 255),
        GOOD=C(30, 140, 100, 255), WARN=C(200, 110, 20, 255), BAD=C(215, 50, 105, 255),
        TITLE=C(150, 110, 235, 255), TITLE_INK=C(255, 255, 255, 255), BTN=C(240, 228, 255, 255),
        BTN_HOVER=C(252, 242, 255, 255), BTN_DOWN=C(218, 200, 252, 255), ROW=C(250, 238, 252, 255),
        ROW_ON=C(226, 210, 255, 255), SHADOW=C(128, 96, 210, 255), DIM=C(80, 40, 120, 110),
        RADIUS=0.0, LINE_W=3.0, SHADOW_OFF=6.0),
}
globals().update(SKINS["office"])
# the type scale (DejaVu sizes at text scale 1); a pixel font's pixel_em scales them all
TYPE = {"tiny": 14, "small": 16, "label": 18, "body": 19, "heading": 22, "title": 28, "display": 34}
SHADOW_ALPHA = 105   # a hard shadow is its border colour at this alpha


def style(theme, name, bg=None, text=None, border=None, width=0.0, radius=None, size=None, pad=None, role=None,
          base=None):
    """Sets a style class's fields. role: a TYPE size name. base: start as a copy of that class (theme.extend)."""
    s = theme.extend(name, base) if base else theme.get(name)
    if role is not None:
        s.font_role = role
    if bg is not None:
        s.background_color = bg
    if text is not None:
        s.text_color = text
    s.border_width = width
    if border is not None:
        s.border_color = border
    if radius is not None:
        s.corner_radius = radius
    if size is not None:
        s.font_size = size
    if pad is not None:
        s.padding_left = s.padding_right = pad[0]
        s.padding_top = s.padding_bottom = pad[1]
    return s


def state(s, which, **fields):
    """Sets fields of a hover / pressed / disabled state."""
    st = getattr(s, which)
    for k, v in fields.items():
        setattr(st, k, v)
    setattr(s, which, st)


def flat(s):
    """No shadow: only cards and modals float."""
    s.shadow_color = CLEAR
    return s


def hard_shadow(s):
    """Solid offset shadow (no blur): windows sit on the desk like paper. Its colour is the style's
    border at SHADOW_ALPHA (SHADOW without a border), so outline and shadow read as two things."""
    b = s.border_color
    s.shadow_color = C(b.r, b.g, b.b, SHADOW_ALPHA) if b.a and s.border_width > 0 else SHADOW
    s.shadow_size = 0.0
    s.shadow_offset = engine.Vector2(SHADOW_OFF, SHADOW_OFF)
    return s


def apply(canvas, text_scale: float = 1.0, font="assets/fonts/DejaVuSans.ttf", extra=None, skin="office",
          pixel_fonts=(), pixel_em=1.0) -> None:
    """pixel_fonts: [(path, size), ...], one pixel font's native sizes (engine.load_font pixel=True); text
    draws only at whole multiples of them, so it stays sharp. It replaces `font`. pixel_em: how much bigger
    the style sizes get, as the pixel font's caps fill less of its size than DejaVu's (0.73)."""
    globals().update(SKINS[skin])
    theme = engine.ui.modern_theme()
    if pixel_fonts:
        theme.default_font = engine.load_font(*pixel_fonts[0], pixel=True)
        for path, size in pixel_fonts[1:]:
            engine.add_pixel_font_size(theme.default_font, path, size)
        text_scale *= pixel_em   # TYPE is for DejaVu; extra() gets the same scale
    else:
        theme.default_font = engine.load_font(font, 32)
    theme.type_scale = dict(TYPE)
    r, w = RADIUS, LINE_W
    flat(style(theme, "app", bg=BG, radius=0.0))
    flat(style(theme, "panel", bg=CLEAR, radius=0.0, pad=(0, 0)))      # plain container
    flat(style(theme, "surface", bg=SURFACE, border=LINE, width=w, radius=0.0))
    hard_shadow(style(theme, "card", bg=CARD, border=LINE, width=w, radius=r, pad=(0, 0)))
    flat(style(theme, "title_bar", bg=TITLE, radius=r, pad=(0, 0)))   # card / window title band
    style(theme, "title_bar_text", bg=CLEAR, text=TITLE_INK, role="heading")
    flat(style(theme, "sunken", bg=INSET, border=LINE, width=w, radius=r))
    flat(style(theme, "row", bg=ROW, border=LINE, width=1.0, radius=r))
    row_on = flat(style(theme, "row_on", bg=ROW_ON, border=ACCENT, width=w, radius=r))
    row_on.padding_left = 0.0
    style(theme, "text", bg=CLEAR, text=INK, role="body")
    style(theme, "small", bg=CLEAR, text=MUTED, role="small")
    style(theme, "title", bg=CLEAR, text=INK, role="title")
    style(theme, "group_head", bg=CLEAR, text=ACCENT, role="label", radius=0.0, pad=(0, 0))
    style(theme, "modal_dim", bg=DIM, radius=0.0)
    for name in ("button", "button_primary"):
        primary = name == "button_primary"
        s = flat(style(theme, name, bg=ACCENT if primary else BTN, text=TITLE_INK if primary else INK,
                       border=LINE, role="label", pad=(12, 4), width=w, radius=r))
        state(s, "hover", background_color=TITLE if primary else BTN_HOVER, text_color=TITLE_INK if primary else INK,
              border_color=ACCENT)
        state(s, "pressed", background_color=ACCENT if primary else BTN_DOWN, border_color=ACCENT, scale_mul=0.98)
        state(s, "disabled", background_color=SURFACE, text_color=MUTED, border_color=LINE)
    style(theme, "window_close", base="button", width=w, pad=(0, 0))   # window()'s close box
    for name, on in (("rail", False), ("rail_on", True)):   # left icon rail tiles
        s = flat(style(theme, name, bg=ROW_ON if on else CLEAR, text=INK if on else MUTED,
                         border=ACCENT if on else CLEAR, width=w if on else 0.0, radius=r, role="small"))
        state(s, "hover", background_color=BTN_HOVER, text_color=INK)
        state(s, "pressed", scale_mul=0.97)
    style(theme, "progress", bg=INSET, border=LINE, width=w, radius=r)
    style(theme, "progress_fill", bg=ACCENT, radius=r)
    flat(style(theme, "scroll_view", bg=CLEAR, radius=0.0))
    style(theme, "text_input", bg=INSET, text=INK, border=LINE, role="body", pad=(8, 4), width=w, radius=r)
    hit = flat(style(theme, "hit", bg=CLEAR, radius=0.0))   # invisible click area
    for which in ("hover", "pressed"):
        state(hit, which, background_color=CLEAR, scale_mul=1.0)
    if extra is not None:
        extra(theme, text_scale)
    canvas.set_theme(theme)
    canvas.text_scale = text_scale
