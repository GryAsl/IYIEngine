"""sprite_stack: shared engine library for sprite-stacked games (engine docs/spec/project.md §2, "libraries").

    fx        sun shadows, contact shadows, light map, per-texel lit stacks (engine.draw_sprite_stacks)
    daylight  time of day -> sun, ambient and lamp levels for fx
    water     flowing water map and beach for fx.draw_terrain
    post      world-pass finishing touches (grade, mist, tilt-shift, rain, vignette, grain, heat haze)
"""
