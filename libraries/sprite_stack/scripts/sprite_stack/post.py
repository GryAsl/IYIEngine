"""Finishing touches on the world pass: colour grade, mist, tilt-shift, rain streaks, vignette, grain, heat haze.

One shader (world_post.fs) as the engine's postprocess shader with world_only, so the HUD is drawn
after it untouched. No buffers of its own: night bloom is fx.draw_glow, from the light map.
    Start:   post.load(s)
    Update:  post.apply(s, look)        look: any of DEFAULTS' keys; the rest stay neutral
    Leaving: post.unload()
daylight.grade(hour) gives the grade keys for the time of day.
"""

from engine import sprite_stack as _c


def load(s):
    s["post"] = _c.Post()


def apply(s, look):
    """Set this frame's look (Update). Unknown keys are an error, so a typo does not pass silently.
    look: any of exposure, contrast, saturation (1 = unchanged), lift (0, 0, 0), gain (1, 1, 1) (added to
    the darks, multiplied into the lights), mist 0..1, mist_colour, tilt 0..1, tilt_centre, tilt_band,
    rain 0..1, heat 0..1 (the image shimmers up to ~1.5 px), vignette, grain; the defaults are in
    libraries/sprite_stack/src/Post.h (PostLook)."""
    post = s.get("post")
    if post is not None:
        post.apply(look)


def unload():
    _c.post_unload()
