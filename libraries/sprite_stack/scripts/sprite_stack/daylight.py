"""Time of day -> sun shadow direction/length/strength, ambient level and colour, artificial light
level, shadow colour, morning mist, distance haze and a colour grade.

The inputs of sprite_stack.fx.render (and of a screen grade, such as the screen_fx library's).
Pure functions of the hour (0..24). The curves are C++ (libraries/sprite_stack/src/Daylight.h, shared
with native games); this module only forwards to them. Their checks are in the engine tests.
"""

from engine import sprite_stack as _c


def elevation(hour):
    """Sun height: 1 at noon, 0 at 06:00/18:00, negative at night."""
    return _c.daylight_elevation(hour)


def sun(hour):
    """(direction, length, strength) for the shadow pass.

    direction is a view_rotation for draw_sprite_stacks: the world direction layers are pushed,
    i.e. away from the sun. 90 = west at dawn, 0 = north at noon, -90 = east at dusk.
    length scales layer height into ground distance: long at dawn/dusk, short at noon.
    """
    return _c.daylight_sun(hour)


def ambient(hour):
    """Light-map clear value: 1 by day, 0.32 at night, smooth through twilight."""
    return _c.daylight_ambient(hour)


def lamps(hour):
    """How far artificial lights are switched on: 0 by day, 1 at night."""
    return _c.daylight_lamps(hour)


def ambient_colour(hour):
    """ambient() as an (r, g, b) light-map colour, 0..1: white by day (no light map), warm through
    twilight, moonlight blue at night, at the same overall level."""
    return _c.daylight_ambient_colour(hour)


def shadow_tint(hour):
    """(r, g, b) 0..255 shadows blend toward: sky blue at noon, violet with a low sun, near black at night."""
    return _c.daylight_shadow_tint(hour)


def mist(hour):
    """Morning mist 0..1: rises before dawn, gone by mid-morning."""
    return _c.daylight_mist(hour)


def haze(hour):
    """(r, g, b, a) 0..255 distance haze (fx.set_fog): pale sky by day, warm with a low sun, dark blue
    at night; a is the most it covers, thicker in the morning mist."""
    return _c.daylight_haze(hour)


def grade(hour):
    """A gentle colour grade for the hour: day is left as it is, the golden hours warm a little,
    night gets cooler shadows, a touch more contrast and less saturation away from the lights.
    Keys: exposure, contrast, saturation (1 = unchanged), lift and gain (r, g, b): added to the
    darks, multiplied into the lights."""
    return _c.daylight_grade(hour)
