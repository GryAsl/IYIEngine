#version 330
// The sun's depth map (StackLighting.cpp, SunMapPass): only depth is written; clear texels of a
// stack layer are cut out, so a layer casts the shape of what is drawn on it. Also the terrain
// contact footprints (ContactPass): each item flat in its own colour, which carries its height.

in vec2 fragTexCoord;
in vec4 fragColor;
in float fragDamage;
in vec4 fragCell;
uniform sampler2D texture0;
out vec4 finalColor;

#include "crumble.glsl"

void main()
{
    if (texture(texture0, fragTexCoord).a < 0.5 || crumbled()) discard;
    finalColor = fragColor;
}
