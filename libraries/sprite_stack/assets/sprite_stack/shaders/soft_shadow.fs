#version 330
// Composites the world-space sun-shadow buffer onto the ground, blurred already (blur.fs, r =
// shadow), so hard layer silhouettes have a soft penumbra. Cloud shade joins it:
// the darker of the two, so a shadow under a cloud is not darkened twice.
// Tint = the shadow colour, its alpha the sun's shadow strength. Output that colour + alpha: plain
// alpha blending moves the ground toward it.

in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform float uGroundHalf;  // the buffer covers [-uGroundHalf, uGroundHalf] on both axes
uniform vec4 uClouds;       // cover 0..1, strength (0 = none), drift offset x, y (world units)
uniform float uCloudScale;  // world units per noise cell
out vec4 finalColor;

#include "noise.glsl"
#include "cloud.glsl"

void main()
{
    // Drawn upside down (a render texture): v = 1 at world y = -half.
    vec2 world = vec2(2.0 * fragTexCoord.x - 1.0, 1.0 - 2.0 * fragTexCoord.y) * uGroundHalf;
    finalColor = vec4(fragColor.rgb, max(texture(texture0, fragTexCoord).r, cloud(world)) * fragColor.a);
}
