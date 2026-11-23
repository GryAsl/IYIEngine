#version 330
// Ground marks (StackLighting.h): one quad per mark painted into the marks buffer, drawn with
// premultiplied alpha blending, so later marks cover earlier ones. Tint: r = kind (0 crater,
// 1 tyre track, 2 rubble, 3 blood), g = a seed for the noise, a = strength. fragTexCoord runs 0..1
// over the mark's length (x) and width (y).

in vec2 fragTexCoord;
in vec4 fragColor;
out vec4 finalColor;

#include "noise.glsl"

// Premultiplied "over": paint colour c at coverage a on top of (rgb, alpha).
void over(inout vec4 dst, vec3 c, float a)
{
    dst = vec4(c * a, a) + dst * (1.0 - a);
}

void main()
{
    int kind = int(fragColor.r * 255.0 + 0.5);
    float seed = fragColor.g * 255.0;
    vec2 uv = fragTexCoord, p = uv * 2.0 - 1.0;
    vec4 m = vec4(0.0);
    if (kind == 0) {                                   // crater: scorched pit, a rim of thrown soil, ejecta
        float r = length(p) + (noise(p * 4.0 + seed) - 0.5) * 0.25;
        over(m, vec3(0.30, 0.25, 0.19), 0.45 * (1.0 - smoothstep(0.6, 1.0, r)) * step(0.55, noise(p * 9.0 + seed)));
        over(m, vec3(0.56, 0.46, 0.34), 0.55 * smoothstep(0.38, 0.5, r) * (1.0 - smoothstep(0.6, 0.8, r)));
        over(m, vec3(0.10, 0.08, 0.06), 0.85 * (1.0 - smoothstep(0.3, 0.48, r)));
    } else if (kind == 1) {                            // tyre track: two ruts, a tread, ends fading
        float side = abs(uv.y - 0.5);
        float rut = smoothstep(0.22, 0.27, side) * (1.0 - smoothstep(0.43, 0.48, side));
        float tread = 0.75 + 0.25 * noise(vec2(uv.x * 40.0, seed));
        float ends = smoothstep(0.0, 0.15, uv.x) * (1.0 - smoothstep(0.85, 1.0, uv.x));
        over(m, vec3(0.30, 0.24, 0.17), 0.4 * rut * tread * ends);
    } else if (kind == 2) {                            // rubble: dust underneath, chunks of masonry
        float r = length(p) + (noise(p * 3.0 + seed) - 0.5) * 0.3;
        float density = 1.0 - smoothstep(0.45, 1.0, r);
        vec2 cell = floor(uv * 26.0) + seed;
        over(m, vec3(0.62, 0.57, 0.50), 0.35 * density);
        over(m, mix(vec3(0.58, 0.55, 0.50), vec3(0.38, 0.34, 0.29), hash(cell + 7.0)),
             0.9 * step(1.0 - 0.75 * density, hash(cell)));
    } else {                                           // blood: a dark, uneven stain
        float r = length(p) + (noise(p * 3.0 + seed) - 0.5) * 0.5;
        over(m, vec3(0.27, 0.03, 0.03), 0.75 * (1.0 - smoothstep(0.45, 0.65, r)));
    }
    finalColor = m * fragColor.a;
}
