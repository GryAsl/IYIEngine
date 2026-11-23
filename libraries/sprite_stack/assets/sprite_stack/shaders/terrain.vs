#version 330
// raylib's default vertex shader plus the world position (the sun map, the light map and contact)
// and the normal (up on the tops, toward the lower side on the cliff walls), for terrain.fs. The
// tops' coordinates are world x, y, mapped onto the ground picture by uTerrainUv (CellTerrain.cpp;
// walls get (1, 1, 0, 0)).

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform vec4 uTerrainUv;    // xy scale, zw offset
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragWorld;
out vec3 fragNormal;

void main()
{
    fragTexCoord = vertexTexCoord * uTerrainUv.xy + uTerrainUv.zw;
    fragColor = vertexColor;
    fragWorld = vertexPosition;
    fragNormal = vertexNormal;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
