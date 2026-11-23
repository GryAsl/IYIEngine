#version 330
// The world pass's finishing touches, as the engine's postprocess shader with world_only (the HUD is
// drawn after it, untouched): tilt-shift blur, colour grade, mist, rain streaks, vignette, grain.
// One pass, every effect off at its neutral value (post.py DEFAULTS). Screen up = fragTexCoord.y 1.
// With a 3D camera the engine gives the world pass's depth (uDepth, uView): mist then thickens with
// distance and tilt-shift blurs what is nearer or further than the camera's target. Without one
// (a 2D camera) both go by screen height instead.

in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 uResolution;   // engine: window pixels
uniform float uTime;        // engine: seconds
uniform float uExposure, uContrast, uSaturation;
uniform vec3 uLift, uGain;  // added to the darks, multiplied into the lights
uniform float uMist;        // 0..1 haze, thicker further away (2D: toward the top of the screen)
uniform vec3 uMistColour;
uniform float uTilt;        // 0..1 tilt-shift: blur above and below a sharp band
uniform float uTiltCentre;  // 2D: where the sharp band is, 0 bottom .. 1 top (3D: the camera's target)
uniform float uTiltBand;    // its half height, in screen heights (3D: in target distances)
uniform float uRain;        // 0..1 rain streaks
uniform float uVignette, uGrain;
uniform float uHeat;        // 0..1 heat haze: the image shimmers, up to HEAT_PIXELS
uniform sampler2D uDepth;   // engine: the world pass's depth (3D camera)
uniform vec4 uView;         // engine: near, far, 1 orthographic, distance to the target; 0s: no depth
out vec4 finalColor;

const float TILT_RADIUS = 4.0;   // pixels at 1080 lines, at the screen's edges
const float HEAT_PIXELS = 1.5;   // largest offset of the haze at uHeat 1

#include "noise.glsl"

// How far from the camera's target this pixel lies, in target distances (0 at the target), with
// its sign: < 0 nearer, > 0 further. 2D: screen heights from the sharp band, up = further.
float away(vec2 uv)
{
    if (uView.x <= 0.0) return uv.y - uTiltCentre;
    float d = texture(uDepth, uv).r;
    float n = uView.x, f = uView.y;
    float z = uView.z > 0.5 ? mix(n, f, d) : 2.0 * n * f / (f + n - (d * 2.0 - 1.0) * (f - n));
    return z / uView.w - 1.0;
}

vec3 tiltShift(vec2 uv)
{
    vec3 sharp = texture(texture0, uv).rgb;
    float blur = uTilt * smoothstep(uTiltBand, uTiltBand + 0.3, abs(away(uv)));
    if (blur < 0.02) return sharp;
    vec2 radius = blur * TILT_RADIUS * (uResolution.y / 1080.0) / uResolution;
    vec3 sum = sharp;
    for (int i = 1; i <= 12; i++) {                        // a golden-angle disc of taps
        float r = sqrt(float(i) / 12.0);
        float a = float(i) * 2.39996;
        sum += texture(texture0, uv + vec2(cos(a), sin(a)) * r * radius).rgb;
    }
    return sum / 13.0;
}

vec3 grade(vec3 c)
{
    c *= uExposure;
    c = (c - 0.5) * uContrast + 0.5;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, uSaturation);
    return max(c * uGain + uLift * (1.0 - c), 0.0);
}

float rain(vec2 px)
{
    float sum = 0.0;
    for (int k = 0; k < 3; k++) {                          // three depths: further = smaller, slower, fainter
        float scale = 1.0 + float(k) * 0.7;
        vec2 p = px / scale;
        p.x += p.y * 0.2;                                  // slanted by the wind
        float column = floor(p.x / 9.0);
        float r = hash(vec2(column, float(k) * 17.0));
        if (r > uRain * 0.8) continue;                     // heavier rain: more columns falling
        float y = p.y + uTime * (700.0 + 400.0 * r) / scale + r * 977.0;
        float along = mod(y, 160.0);
        float len = 18.0 + 12.0 * r;
        float streak = along < len ? along / len : 0.0;    // bright at the leading end
        float across = abs(fract(p.x / 9.0) - 0.5) * 9.0;  // pixels from the column's centre
        sum += streak * clamp(1.0 - across / 0.7, 0.0, 1.0) * (0.5 - float(k) * 0.13);
    }
    return sum;
}

void main()
{
    vec2 uv = fragTexCoord;
    if (uHeat > 0.0) {                                     // slow rippling bands that drift upward
        vec2 p = gl_FragCoord.xy / 40.0;
        vec2 wave = vec2(sin(p.y * 1.7 - uTime * 2.3 + sin(p.x * 0.8 + uTime)), sin(p.x * 1.3 + uTime * 1.9 + sin(p.y * 1.1 - uTime * 0.7)));
        uv += wave * uHeat * HEAT_PIXELS / uResolution;
    }
    vec3 c = grade(tiltShift(uv));
    float far = uView.x > 0.0 ? clamp(away(uv) + 0.5, 0.0, 1.0) : uv.y;   // 3D: half a target distance nearer .. as far again
    c = mix(c, uMistColour, uMist * (0.45 + 0.55 * far));
    if (uRain > 0.0) c += vec3(0.75, 0.8, 0.85) * rain(gl_FragCoord.xy) * 0.35 * min(1.0, uRain * 2.0);
    vec2 centred = (uv - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    c *= 1.0 - uVignette * smoothstep(0.35, 1.0, length(centred) * 1.2);
    c += (hash(gl_FragCoord.xy + fract(uTime) * 517.0) - 0.5) * uGrain;
    finalColor = vec4(c, 1.0);
}
