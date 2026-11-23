#version 330
// The terrain (engine CellTerrain) with the ground picture laid over it, shaded where the sun's
// depth map says something stands between it and the sun, and under clouds: the darker of the two
// moves it toward the shadow colour, as the flat sun composite (soft_shadow.fs) does for 2D.
// Contact occlusion is drawn here too, only where the terrain is about as high as the caster's
// ground (StackLighting.h, terrain contact), so a halo stops at a cliff; instead a wall takes the
// same shade at its own foot (wallFoot) and the ground below it is shaded from the terrain's height
// map (cliffShade). By night the cliff walls take the light map on their lower side (the tops
// already have it in the ground picture). A wall between two water cells (CellTerrain draws it with
// uv.x = 1 + its height) is a waterfall where the stream crosses it: toon streaks of water running
// down from a light lip, chunky foam where it lands (fall), churning foam in the pool below. With a
// water map (uUseWater) the tops draw the rivers per pixel as toon bands below an earth bank (water),
// and the walls carry the beach and bank on down where a shore meets them. With uDetail (fx.set_ground_detail)
// the tops get fine grain finer than the ground picture's texels, and its land-use edges wobble.

in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragWorld;
in vec3 fragNormal;
uniform sampler2D texture0;
uniform vec4 colDiffuse;    // the walls' colour (CellTerrain's mesh draw; white for the tops)
uniform sampler2DShadow uSunMap;
uniform mat4 uSunVP;        // world -> the sun map's clip space
uniform vec3 uSunOffset;    // world offset toward the sun before the lookup (keeps surfaces off themselves)
uniform vec2 uSunSoft;      // the filter taps' reach in the map's 0..1, up the map only (StackLighting.cpp kSunSoft):
                            // fills the gaps between a low sun's layer strips
uniform float uSunBias;     // depth bias, in the map's 0..1 depth
uniform float uUseSunMap;
uniform vec4 uFog;          // distance haze: rgb, the most it covers (0: none)
uniform vec2 uFogRange;     // world units from the eye where it begins and is full
uniform vec3 uEye;
uniform vec4 uClouds;       // as soft_shadow.fs
uniform float uCloudScale;
uniform float uSun;         // the sun's shadow strength
uniform vec3 uShadowTint;
uniform sampler2D uLightMap;     // as lit_sprite.fs
uniform vec4 uLightArea;         // the world rectangle it covers: x, y, width, height
uniform float uUseLight;
uniform float uLightTops;         // 1: the tops too (a ground picture without the light map in it: fx.load light_tops)
uniform sampler2D uContact;      // blurred contact: r = occlusion, g = occlusion * caster ground / CONTACT_TOP
uniform vec4 uContactArea;       // the world rectangle it covers: x, y, width, height
uniform float uContactStrength;  // 0: no terrain contact
uniform float uTime;             // seconds, the wind's clock: water runs with it
uniform sampler2D uWater;        // sprite_stack.water's map: one texel per cell, rg flow, b shore distance
uniform vec4 uWaterArea;         // the world rectangle (x, y, w, h) it covers
uniform float uUseWater;
uniform sampler2D uMarks;           // ground marks (marks.fs), premultiplied: rgb painted over, a covered
uniform vec4 uMarkArea;             // the world rectangle it covers
uniform float uUseMarks;
uniform sampler2D uTerrainHeights;  // CellTerrain: per cell its corner heights (top-left, top-right, bottom-right, bottom-left)
uniform vec4 uTerrainGrid;          // its origin x, y and cell size; w is 1 when set and this chunk has a cliff (CellTerrain.cpp)
uniform vec4 uTerrainUv;            // terrain.vs: a top's world x, y -> the ground picture's uv (scale, offset)
uniform vec2 uDetail;               // x: the grain's strength (0 = off), y: metres the picture's edges wobble
out vec4 finalColor;

const float CONTACT_TOP = 255.0;  // StackLighting.cpp kContactTop

// The water's toon palette: few flat colours, no glints.
const vec3 DEEP = vec3(0.10, 0.40, 0.66);
const vec3 MID = vec3(0.17, 0.56, 0.79);
const vec3 SHALLOW = vec3(0.33, 0.73, 0.86);
const vec3 WAVE = vec3(0.62, 0.88, 0.95);     // the thin lines drifting with the flow
const vec3 FOAM = vec3(0.90, 0.96, 0.97);
const vec3 SAND = vec3(0.78, 0.73, 0.55);     // water.fs: keep equal
const vec3 BANK = vec3(0.56, 0.44, 0.30);     // the earth bank between the beach and the waterline
const float LAND = 2.0, DEEP_CELLS = 4.0;     // water.py LAND, DEEP
const float CALM = 0.7;                       // water.py: faster than this is foam
const float CYCLE = 3.0;    // seconds one flow phase drifts
const float REACH = 3.0;    // cells a phase drifts at full speed
const float WATERLINE = 0.15;   // cells from the shore to the waterline: the bank shows between
const float BEACH = -0.22;      // cells past the shore the sand reaches (water.fs: keep equal)

#include "noise.glsl"

// One octave of ground grain, features `size` metres across, fading out before they shrink under
// two pixels (fp: metres per pixel), so a far view does not shimmer.
float grain(vec2 world, float size, float fp)
{
    if (size <= fp) return 0.0;                              // the smoothstep is 0 there: skip the noise
    return (noise(world / size) - 0.5) * smoothstep(fp, 3.0 * fp, size);
}

#include "cloud.glsl"
#include "sun_fog.glsl"

// Render textures are stored upside down.
vec4 worldTexel(sampler2D map, vec4 area, vec2 world)
{
    vec2 uv = (world - area.xy) / area.zw;
    return texture(map, vec2(uv.x, 1.0 - uv.y));
}

float contact(vec3 world)
{
    if (uContactStrength <= 0.0) return 0.0;
    vec2 uv = (world.xy - uContactArea.xy) / uContactArea.zw;
    float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));   // faded out toward its edge
    if (edge <= 0.0) return 0.0;
    vec2 c = texture(uContact, vec2(uv.x, 1.0 - uv.y)).rg * smoothstep(0.0, 0.03, edge);
    if (c.r < 0.004) return 0.0;
    float caster = c.g / c.r * CONTACT_TOP;
    return c.r * (1.0 - smoothstep(3.0, 8.0, abs(world.z - caster))) * uContactStrength;
}

// A wall's foot: the same shade the contact halo puts at a stack's foot, fading up within 8 world
// units of the ground the wall meets. Its low height arrives in fragTexCoord.y (CellTerrain draws
// walls with uv (0, low)); fragWorld.z above it is how far up the wall the fragment is.
float wallFoot()
{
    return 1.0 - smoothstep(3.0, 8.0, fragWorld.z - fragTexCoord.y);
}

// Water falling down a wall: noise stretched tall into streaks sliding down, cut into flat toon
// colours, a light lip where the stream tips over the top (its height is uv.x - 1 above the low
// edge) and a chunky foam foot with a bubbling top where it lands.
vec3 fall()
{
    float along = fragWorld.x + fragWorld.y;               // walls run along x or y
    float s = noise(vec2(along * 0.4, (fragWorld.z + uTime * 30.0) * 0.05)) * 0.7
            + noise(vec2(along * 1.1 + 5.0, (fragWorld.z + uTime * 45.0) * 0.09)) * 0.3;
    float as = fwidth(s);
    vec3 col = mix(MID, SHALLOW, smoothstep(0.45 - as, 0.45 + as, s));
    col = mix(col, WAVE, smoothstep(0.6 - as, 0.6 + as, s));
    col = mix(col, FOAM, smoothstep(0.72 - as, 0.72 + as, s));
    float h = fragWorld.z - fragTexCoord.y, top = fragTexCoord.x - 1.0 - h;
    float lip = 1.2 + 0.8 * noise(vec2(along * 0.5, uTime * 2.0));
    col = mix(col, WAVE, 1.0 - smoothstep(lip - 0.3, lip + 0.3, top));
    float foot = 2.5 + 3.0 * noise(vec2(along * 0.5, uTime * 3.0));
    return mix(col, FOAM, 1.0 - smoothstep(foot - 0.4, foot + 0.4, h));
}

// One cell's texel of the water map: (flow x, flow y, shore distance in cells).
vec3 waterTexel(ivec2 c, ivec2 n)
{
    c = clamp(c, ivec2(0), n - 1);
    vec3 t = texelFetch(uWater, ivec2(c.x, n.y - 1 - c.y), 0).rgb;   // stored bottom row first
    return vec3(t.rg * 2.0 - 1.0, t.b * (LAND + DEEP_CELLS) - LAND);
}

// The map at grid point g from the 4x4 texels round it, no cell squares: the shore distance a
// Catmull-Rom curve through the texels (a 1-cell stream stays 1 cell wide; water.fs: keep equal), the
// flow a cubic B-spline over the wet texels only, so a narrow river keeps its full speed and foam.
vec3 waterAt(vec2 g, ivec2 n)
{
    vec2 p = g - 0.5, f = fract(p), f2 = f * f, f3 = f2 * f;
    ivec2 i = ivec2(floor(p));
    vec2 b[4] = vec2[4]((1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0,
                        (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, f3 / 6.0);
    vec2 c[4] = vec2[4]((-f + 2.0 * f2 - f3) * 0.5, (2.0 - 5.0 * f2 + 3.0 * f3) * 0.5,
                        (f + 4.0 * f2 - 3.0 * f3) * 0.5, (f3 - f2) * 0.5);
    vec3 flow = vec3(0.0);   // xy the weighted flow, z the wet weight
    float d = 0.0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            vec3 t = waterTexel(i + ivec2(x - 1, y - 1), n);
            flow += vec3(t.xy, 1.0) * (b[x].x * b[y].y * step(0.0, t.z));
            d += t.z * c[x].x * c[y].y;
        }
    return vec3(flow.xy / max(flow.z, 1e-4), d);
}

// A world point on the water map's grid.
vec2 waterGrid(vec2 world)
{
    return (world - uWaterArea.xy) / uWaterArea.zw * vec2(textureSize(uWater, 0));
}

// The shade on the ground at the foot of the neighbouring cells that stand higher (CellTerrain's
// height map): from each one's nearest point, by how far off and how much higher it is, a taller
// cliff reaching farther. Two walls meeting in a corner darken it more; past a cliff's outer corner
// the shade rounds. x: the shade; y: the same under a waterfall (where that higher point lies in
// the stream), which the water turns into foam instead.
vec2 cliffShade(vec3 p)
{
    if (uTerrainGrid.w < 0.5) return vec2(0.0);
    ivec2 n = textureSize(uTerrainHeights, 0), wn = textureSize(uWater, 0);
    vec2 g = (p.xy - uTerrainGrid.xy) / uTerrainGrid.z;
    ivec2 c = ivec2(floor(g));
    float side = 0.0, corner = 0.0, falls = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            ivec2 o = c + ivec2(x, y);
            if ((x == 0 && y == 0) || any(lessThan(o, ivec2(0))) || any(greaterThanEqual(o, n))) continue;
            vec2 q = clamp(g - vec2(o), 0.0, 1.0);               // its nearest point, in its cell
            vec4 k = texelFetch(uTerrainHeights, o, 0);
            float rise = mix(mix(k.x, k.y, q.x), mix(k.w, k.z, q.x), q.y) - p.z;
            float dist = length(g - vec2(o) - q) * uTerrainGrid.z;
            float s = smoothstep(1.0, 4.0, rise) * (1.0 - smoothstep(0.0, clamp(rise * 0.6, 3.0, 14.0), dist));
            if (s <= 0.0) continue;
            vec2 centre = uTerrainGrid.xy + (vec2(o) + 0.5) * uTerrainGrid.z;
            if (uUseWater > 0.5 && waterTexel(ivec2(floor(waterGrid(centre))), wn).z > 0.0) {   // a water cell
                float d = waterAt(waterGrid(uTerrainGrid.xy + (vec2(o) + q) * uTerrainGrid.z), wn).z;
                float wet = smoothstep(-0.05, 0.05, d - WATERLINE);
                falls = max(falls, s * wet);
                s *= 1.0 - wet;
            }
            if (x * y == 0) side = 1.0 - (1.0 - side) * (1.0 - s);
            else corner = max(corner, s);
        }
    return vec2(max(side, corner), falls);
}

// Noise smeared along the flow into ribbons, drifted downstream by phase (0..1).
float ribbons(vec2 g, vec2 flow, float phase, vec2 seed)
{
    vec2 p = g - flow * REACH * phase + seed, step = flow * 0.4;
    float sum = 0.0;
    for (int k = -2; k <= 2; k++) {
        vec2 q = p + step * float(k);
        sum += noise(q * 1.3) * 0.65 + noise(q * 2.9 + 11.0) * 0.35;
    }
    return sum / 5.0;
}

// `picture` with the earth bank over it from the shore (d = 0) on: two tones, darker past half way
// to the waterline, times `lit`.
vec3 banked(vec3 picture, vec3 lit, float d, float aa)
{
    vec3 bank = mix(BANK, BANK * 0.72, smoothstep(WATERLINE * 0.5 - aa, WATERLINE * 0.5 + aa, d)) * lit;
    return mix(picture, bank, smoothstep(-aa, aa, d));
}

// The water over a top at ground picture colour `picture` (SAND there times its lights, overlays and
// previews), or the picture: past the shore an earth bank down to the waterline, then toon bands by
// depth broken by ribbons riding the flow (two phases that fade into each other, each starting on a
// new pattern, so it moves without stretching or repeating), thin wave lines, a wobbling foam line
// at the waterline and a wave washing out from it, chunky foam on rapids and where a waterfall
// lands (`pool`, cliffShade's y).
vec3 water(vec3 picture, float pool)
{
    ivec2 n = textureSize(uWater, 0);
    vec2 g = waterGrid(fragWorld.xy);
    if (waterTexel(ivec2(floor(g)), n).z < 0.01 - LAND) return picture;   // far from any water
    vec3 m = waterAt(g, n);
    float d = m.z, aa = fwidth(d);
    if (d < -aa) return picture;                                          // the beach
    vec3 lit = min(picture / SAND, vec3(1.3)), bank = banked(picture, lit, d, aa);
    d -= WATERLINE;
    if (d < -aa) return bank;
    float speed = length(m.xy);
    vec2 flow = m.xy / CALM;
    float foam = smoothstep(0.8, 0.95, speed);
    float t = uTime / CYCLE, a = fract(t), k = abs(2.0 * a - 1.0);
    vec2 sa = fract(floor(t) * vec2(0.618, 0.414)) * 97.0, sb = fract(floor(t + 0.5) * vec2(0.382, 0.732)) * 89.0 + 7.0;
    float r = mix(ribbons(g, flow, a, sa), ribbons(g, flow, fract(t + 0.5), sb), k);
    r = 0.5 + (r - 0.5) * inversesqrt(k * k + (1.0 - k) * (1.0 - k));   // same contrast through the fade
    float ar = fwidth(r);
    float v = clamp(d * 0.45, 0.0, 1.0) + (r - 0.5) * 0.7;   // depth, broken by the ribbons
    float av = fwidth(v);
    vec3 col = mix(SHALLOW, MID, smoothstep(0.32 - av, 0.32 + av, v));
    col = mix(col, DEEP, smoothstep(0.75 - av, 0.75 + av, v));
    float wave = 1.0 - smoothstep(0.007, 0.007 + ar, abs(r - 0.65));
    col = mix(col, WAVE, wave * smoothstep(0.3, 0.6, d) * (1.0 - foam));
    float edge = 0.05 + 0.03 * noise(g * 1.5 + uTime * vec2(0.25, -0.2));
    float lap = fract(uTime * 0.22 + noise(g * 0.6) * 0.6);               // a wave washing out from the shore
    float ring = (1.0 - smoothstep(0.015, 0.015 + aa, abs(d - 0.15 - 0.4 * lap))) * (1.0 - lap) * 0.5;
    float chunks = foam * smoothstep(-ar, ar, r - mix(0.75, 0.58, foam));   // bubbles, not a sheet
    float churn = pool * (0.4 + noise(g * 5.0 - flow * uTime * 2.0)), ac = fwidth(churn);
    chunks = max(chunks, smoothstep(0.5 - ac, 0.5 + ac, churn));
    col = mix(col, FOAM, max(max(1.0 - smoothstep(edge - aa, edge + aa, d), ring), chunks));
    return mix(bank, col * lit, smoothstep(-aa, aa, d));
}

void main()
{
    bool wall = fragNormal.z < 0.5;
    vec2 uv = fragTexCoord;
    float detail = 0.0;
    if (!wall && uDetail.x > 0.0) {                          // grain over the picture, its edges wobbling
        vec2 w = fragWorld.xy;
        float fp = max(fwidth(w.x), fwidth(w.y));
        float wobble = 1.0 - smoothstep(uDetail.y, 2.0 * uDetail.y, fp);   // gone once it is under a pixel
        if (wobble > 0.0) uv += (vec2(noise(w * 0.8), noise(w * 0.8 + 19.0)) - 0.5) * 2.0 * uDetail.y * wobble * uTerrainUv.xy;
        detail = grain(w, 1.7, fp) * 0.7 + grain(w + 7.0, 0.45, fp) + grain(w + 13.0, 0.13, fp) * 0.8;
    }
    vec4 base = texture(texture0, uv) * fragColor * colDiffuse;
    base.rgb *= 1.0 + uDetail.x * detail;
    vec2 mu = (fragWorld.xy - uMarkArea.xy) / uMarkArea.zw;
    if (uUseMarks > 0.5 && !wall && all(greaterThan(mu, vec2(0.0))) && all(lessThan(mu, vec2(1.0)))) {
        vec4 mark = texture(uMarks, vec2(mu.x, 1.0 - mu.y));   // render textures are stored upside down
        base.rgb = base.rgb * (1.0 - mark.a) + mark.rgb;
    }
    vec2 cliff = wall ? vec2(0.0) : cliffShade(fragWorld);
    float stream = 0.0;                                       // how much of a waterfall wall the stream covers
    if (wall) {
        bool falls = fragTexCoord.x > 0.5;
        ivec2 n = textureSize(uWater, 0);
        vec2 g = waterGrid(fragWorld.xy);
        if (uUseWater > 0.5 && waterTexel(ivec2(floor(g)), n).z > 0.01 - LAND) {
            // The tops' beach, bank and stream run on down the wall, so a fall is only as wide as the
            // river and the shore bands it crosses carry on down its sides.
            float d = waterAt(g, n).z, aa = fwidth(d);
            base.rgb = banked(mix(base.rgb, SAND, smoothstep(BEACH - aa, BEACH + aa, d)), vec3(1.0), d, aa);
            if (falls) stream = smoothstep(-aa, aa, d - WATERLINE);
        }
        else if (falls) stream = 1.0;
        base.rgb = mix(base.rgb, fall(), stream);
    }
    else if (uUseWater > 0.5) base.rgb = water(base.rgb, cliff.y);
    if (uUseLight > 0.5 && (wall || uLightTops > 0.5)) {      // a wall: lit as the ground just below it
        base.rgb *= worldTexel(uLightMap, uLightArea, fragWorld.xy + (wall ? fragNormal.xy * 2.0 : vec2(0.0))).rgb;
    }
    float ao = contact(fragWorld);
    if (wall) ao = max(ao, wallFoot() * uContactStrength) * (1.0 - stream);   // its own foot in shade; falling water none
    else ao = max(ao, cliff.x * uContactStrength);           // the ground at a cliff's foot
    vec3 rgb = mix(base.rgb, uShadowTint, ao);
    float shade = max(sunShadow(fragWorld), cloud(fragWorld.xy)) * uSun;
    finalColor = vec4(fogged(mix(rgb, uShadowTint, shade), fragWorld), base.a);
}
