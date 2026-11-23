#version 330
// Multiplies the ground by the light buffer (the ambient colour where no light reaches, a light's
// colour where it does; white = as lit as by day). Drawn over the flat ground only, before the
// stacks, with raylib's multiplied blend (dst * src); stacks are lit per texel by lit_sprite. A 3x3
// tent smooths the half-resolution buffer.

in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
out vec4 finalColor;

void main()
{
    vec2 texel = 1.0 / vec2(textureSize(texture0, 0));
    vec3 light = vec3(0.0);
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            light += (2.0 - abs(float(x))) * (2.0 - abs(float(y))) * texture(texture0, fragTexCoord + vec2(x, y) * texel).rgb;
        }
    }
    finalColor = vec4(light / 16.0, 1.0);
}
