// Cloud field 0..1 at a world point (#include after noise.glsl; needs uniform vec4 uClouds = cover 0..1,
// strength, drift x, y and uniform float uCloudScale). One code for roofs and ground, so they agree.
float cloud(vec2 world)
{
    if (uClouds.y <= 0.0) return 0.0;
    vec2 p = (world + uClouds.zw) / uCloudScale;
    float n = (0.5 * noise(p) + 0.25 * noise(p * 2.03 + 17.0) + 0.125 * noise(p * 4.01 + 31.0) + 0.0625 * noise(p * 8.07 + 7.0)) / 0.9375;
    float edge = 1.0 - uClouds.x;
    return smoothstep(edge - 0.1, edge + 0.1, n) * uClouds.y;
}
