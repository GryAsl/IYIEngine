#version 330
// One direction of the lighting buffers' 5-tap Gaussian, weights exp(-i*i / uFalloff); run along x
// and then along y it blurs each buffer texel once (StackLighting.cpp, BlurBuffer). uInvert 1 reads a
// raw buffer (white = lit) as occlusion 1 - r; the output is occlusion in r, opaque. g is blurred the
// same way (terrain contact keeps occlusion * the caster's ground height there: StackLighting.h).

in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 uStep;         // UV between taps
uniform float uFalloff;
uniform float uInvert;
out vec4 finalColor;

void main()
{
    vec2 sum = vec2(0.0);
    float total = 0.0;
    for (int i = -2; i <= 2; i++) {
        float w = exp(-float(i * i) / uFalloff);
        vec2 rg = texture(texture0, fragTexCoord + uStep * float(i)).rg;
        sum += w * mix(rg, 1.0 - rg, uInvert);
        total += w;
    }
    vec2 o = sum / total;
    finalColor = vec4(o, o.x, 1.0);
}
