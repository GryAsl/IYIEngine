#version 330
// Contact occlusion: the buffer holds each grounded object's flattened footprint, blurred already
// (blur.fs) into a dark halo that hugs the base, which is what reads as "touching the ground"; r =
// occlusion. Tint = the shadow colour, its alpha the strength; output that colour + alpha, like
// soft_shadow.fs.

in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
out vec4 finalColor;

void main()
{
    finalColor = vec4(fragColor.rgb, texture(texture0, fragTexCoord).r * fragColor.a);
}
