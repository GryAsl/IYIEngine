#version 330
// raylib's default vertex shader without the colour, plus the world position of each vertex (drawn in
// the world pass, a vertex position is a world point), for wet_ground.fs and water.fs.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
uniform mat4 mvp;
out vec2 fragTexCoord;
out vec2 fragWorld;

void main()
{
    fragTexCoord = vertexTexCoord;
    fragWorld = vertexPosition.xy;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
