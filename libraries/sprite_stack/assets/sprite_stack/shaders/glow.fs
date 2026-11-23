#version 330
// Night glow (fx.draw_glow): the light map's light above the ambient, blurred wide, added over the
// finished world pass, so lamps and headlights bloom without a screen pass of their own.
// Tint alpha = strength; drawn with additive blending (dst + rgb * alpha).

in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec3 uAmbient;      // the light map's colour where no light reaches
out vec4 finalColor;

void main()
{
    vec2 texel = 3.0 / vec2(textureSize(texture0, 0));     // taps 3 texels apart: a wide, soft halo
    vec3 light = vec3(0.0);
    float total = 0.0;
    for (int y = -2; y <= 2; y++) {
        for (int x = -2; x <= 2; x++) {
            float w = exp(-float(x * x + y * y) / 4.0);
            light += w * max(texture(texture0, fragTexCoord + vec2(x, y) * texel).rgb - uAmbient, 0.0);
            total += w;
        }
    }
    finalColor = vec4(light / total, fragColor.a);
}
