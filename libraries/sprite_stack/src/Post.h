// The sprite_stack library's world postprocess: colour grade, mist, tilt-shift, rain streaks, vignette,
// grain and heat haze in one shader (world_post.fs) set as the engine's postprocess shader with
// world_only, so the HUD is drawn after it untouched. A game makes one Post, calls Apply(look) each
// Update and Post::Unload() when leaving. Night bloom is not here (Fx's Lighting->DrawGlow).
// One implementation for native games and Python (scripts/sprite_stack/post.py, src/LibraryBindings.cpp).

#pragma once

#include <array>
#include "raylib.h"

namespace IYI {

// Any look not set stays neutral. Daylight::GradeAt gives the grade for the time of day.
struct PostLook
{
    float Exposure = 1.0f, Contrast = 1.0f, Saturation = 1.0f;     // 1 = unchanged
    std::array<float, 3> Lift{ 0.0f, 0.0f, 0.0f }, Gain{ 1.0f, 1.0f, 1.0f };   // added to the darks, multiplied into the lights
    float Mist = 0.0f;                                              // haze 0..1, thicker toward the top of the screen
    std::array<float, 3> MistColour{ 0.82f, 0.85f, 0.88f };
    float Tilt = 0.0f, TiltCentre = 0.5f, TiltBand = 0.16f;         // tilt-shift 0..1; sharp band centre and half height
    float Rain = 0.0f;                                              // rain streaks 0..1
    float Heat = 0.0f;                                              // heat haze 0..1: the image shimmers up to ~1.5 px
    float Vignette = 0.0f, Grain = 0.0f;
};

class Post
{
public:
    Post();                              // loads world_post.fs and makes it the postprocess shader (world_only)
    void Apply(const PostLook& look);    // this frame's look (Update)
    static void Unload();                // removes the postprocess shader

private:
    Shader* Shader_ = nullptr;
};

} // namespace IYI
