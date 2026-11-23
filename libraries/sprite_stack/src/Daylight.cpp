// Daylight.h: the curves, as tuned in the Python original (doubles, same constants and order of operations,
// so the Python wrapper returns what it did).

#include "Daylight.h"

#include <algorithm>
#include <cmath>

namespace IYI::Daylight {

static constexpr Rgb MOON{ 0.80, 0.92, 1.30 };      // night's ambient colour per unit of level: moonlight is blue
static constexpr Rgb DUSK{ 1.15, 0.92, 0.85 };      // ... at twilight, warm toward the horizon's glow
static constexpr Rgb WHITE3{ 1.0, 1.0, 1.0 };
static constexpr Rgb SHADOW_NOON{ 34, 44, 82 };        // what shadows blend toward under a high sun: lit by the blue sky only
static constexpr Rgb SHADOW_LOW{ 58, 38, 84 };         // ... under a low sun: violet against the golden light
static constexpr Rgb SHADOW_NIGHT{ 12, 14, 26 };       // contact occlusion at night (there are no sun shadows)
static constexpr Rgb HAZE_DAY{ 196, 208, 222 };        // distance haze by day: pale sky
static constexpr Rgb HAZE_LOW{ 226, 190, 160 };        // ... with a low sun: warm
static constexpr Rgb HAZE_NIGHT{ 22, 28, 44 };         // ... at night: dark blue

static double Smooth(double edge0, double edge1, double x)
{
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

static Rgb Lerp(const Rgb& a, const Rgb& b, double t)
{
    return { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t };
}

static double Night(double hour)   // 0 by day .. 1 at full night, following the ambient level
{
    return 1.0 - (Ambient(hour) - NIGHT_AMBIENT) / (1.0 - NIGHT_AMBIENT);
}

static int Round(double v) { return static_cast<int>(std::nearbyint(v)); }   // Python's round: half to even

double Elevation(double hour) { return std::sin(3.141592653589793 * (hour - 6.0) / 12.0); }

Sun SunAt(double hour)
{
    const double e = Elevation(hour);
    return { 90.0 - 180.0 * (hour - 6.0) / 12.0, 0.6 + 2.4 * (1.0 - std::max(0.0, e)), SHADOW_STRENGTH * Smooth(0.0, 0.12, e) };
}

double Ambient(double hour) { return NIGHT_AMBIENT + (1.0 - NIGHT_AMBIENT) * Smooth(-0.15, 0.25, Elevation(hour)); }

double Lamps(double hour) { return 1.0 - Smooth(0.15, 0.85, (Ambient(hour) - NIGHT_AMBIENT) / (1.0 - NIGHT_AMBIENT)); }

Rgb AmbientColour(double hour)
{
    const double n = Night(hour);
    const Rgb tint = Lerp(WHITE3, MOON, n), dusk = Lerp(WHITE3, DUSK, 4.0 * n * (1.0 - n));
    const double level = Ambient(hour);
    return { std::min(1.0, level * tint[0] * dusk[0]), std::min(1.0, level * tint[1] * dusk[1]), std::min(1.0, level * tint[2] * dusk[2]) };
}

std::array<int, 3> ShadowTint(double hour)
{
    const double low = 1.0 - Smooth(0.05, 0.6, Elevation(hour));
    const Rgb c = Lerp(Lerp(SHADOW_NOON, SHADOW_LOW, low), SHADOW_NIGHT, Smooth(0.4, 0.9, Night(hour)));
    return { Round(c[0]), Round(c[1]), Round(c[2]) };
}

double Mist(double hour) { return Smooth(4.0, 5.5, hour) * (1.0 - Smooth(7.0, 9.5, hour)); }

std::array<int, 4> Haze(double hour)
{
    const double low = 1.0 - Smooth(0.05, 0.6, Elevation(hour));
    const Rgb c = Lerp(Lerp(HAZE_DAY, HAZE_LOW, low), HAZE_NIGHT, Smooth(0.4, 0.9, Night(hour)));
    return { Round(c[0]), Round(c[1]), Round(c[2]), Round(255 * (HAZE + 0.3 * Mist(hour))) };
}

Grade GradeAt(double hour)
{
    const double n = Night(hour), e = Elevation(hour);
    const double golden = Smooth(0.0, 0.12, e) * (1.0 - Smooth(0.12, 0.45, e));
    return { 1.0 + 0.06 * n, 1.0 + 0.08 * n, 1.0 + 0.05 * golden - 0.1 * n,
             Rgb{ 0.006 * n, 0.012 * n, 0.028 * n },
             Lerp(Lerp(WHITE3, { 1.05, 1.0, 0.93 }, golden), { 0.97, 0.99, 1.05 }, n) };
}

} // namespace IYI::Daylight
