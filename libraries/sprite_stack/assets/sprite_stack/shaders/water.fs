#version 330
// The water's beach in the ground picture (water.py): flat SAND wherever the shore distance is above
// BEACH, under the water and just past its shore. terrain.fs draws the water itself per pixel and
// reads the picture there as its lights, overlays and tool previews (picture / SAND); the land keeps
// the sand as a beach. texture0 is the water map (water.py; the shore distance as terrain.fs's waterAt, a Catmull-Rom).

in vec2 fragWorld;
uniform sampler2D texture0;
uniform vec2 uOrigin;       // world point of cell (0, 0)'s top-left corner
uniform float uCell;        // world units across a cell
out vec4 finalColor;

const vec3 SAND = vec3(0.78, 0.73, 0.55);   // terrain.fs: keep equal
const float BEACH = -0.22;  // cells past the shore the sand reaches
const float LAND = 2.0, DEEP = 4.0;         // water.py

float shoreTexel(ivec2 c, ivec2 n)
{
    c = clamp(c, ivec2(0), n - 1);
    return texelFetch(texture0, ivec2(c.x, n.y - 1 - c.y), 0).b * (LAND + DEEP) - LAND;
}

float shoreAt(vec2 g, ivec2 n)
{
    vec2 p = g - 0.5, f = fract(p), f2 = f * f, f3 = f2 * f;
    ivec2 i = ivec2(floor(p));
    vec2 c[4] = vec2[4]((-f + 2.0 * f2 - f3) * 0.5, (2.0 - 5.0 * f2 + 3.0 * f3) * 0.5,
                        (f + 4.0 * f2 - 3.0 * f3) * 0.5, (f3 - f2) * 0.5);
    float d = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) d += shoreTexel(i + ivec2(x - 1, y - 1), n) * c[x].x * c[y].y;
    return d;
}

void main()
{
    ivec2 n = textureSize(texture0, 0);
    if (shoreAt((fragWorld - uOrigin) / uCell, n) < BEACH) discard;
    finalColor = vec4(SAND, 1.0);
}
