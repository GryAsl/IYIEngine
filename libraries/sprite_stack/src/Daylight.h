// Time of day -> sun shadow direction/length/strength, ambient level and colour, artificial light level,
// shadow colour, morning mist, distance haze and a colour grade: the inputs of the lighting pass
// (StackLighting) and of the world postprocess (Post.h). Pure functions of the hour (0..24).
// One implementation for native games and Python (scripts/sprite_stack/daylight.py is a thin wrapper
// over engine.sprite_stack, src/LibraryBindings.cpp). Checks: tests/engine_tests.cpp.

#pragma once

#include <array>

namespace IYI::Daylight {

using Rgb = std::array<double, 3>;

constexpr double NIGHT_AMBIENT = 0.32;     // light-map value with no artificial light at night
constexpr double SHADOW_STRENGTH = 0.55;   // darkest a sun shadow gets, at full daylight
constexpr double HAZE = 0.55;              // the most of a far view the haze covers; morning mist raises it to 0.85

struct Sun { double Direction, Length, Strength; };
struct Grade { double Exposure, Contrast, Saturation; Rgb Lift, Gain; };   // 1 / 0 = unchanged

double Elevation(double hour);                  // 1 at noon, 0 at 06:00/18:00, negative at night
// Direction: a view_rotation for the shadow pass, away from the sun (90 = west at dawn, 0 = north at
// noon, -90 = east at dusk); Length scales layer height into ground distance; Strength the shadow.
Sun SunAt(double hour);
double Ambient(double hour);                    // light-map clear value: 1 by day, NIGHT_AMBIENT at night
double Lamps(double hour);                      // artificial lights: 0 by day, 1 at night
Rgb AmbientColour(double hour);                // 0..1: white by day, warm at twilight, moon blue at night
std::array<int, 3> ShadowTint(double hour);    // 0..255 that shadows blend toward
double Mist(double hour);                       // morning mist 0..1
std::array<int, 4> Haze(double hour);          // 0..255 distance haze; a is the most it covers
Grade GradeAt(double hour);                    // a gentle colour grade for the hour

} // namespace IYI::Daylight
