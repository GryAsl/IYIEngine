#version 330
// raylib's default vertex shader plus the ground point engine.draw_sprite_stacks(ground_in_normal=True)
// puts in each vertex normal: normalize(x, y, 1024), so ground = normal.xy / normal.z * 1024
// (SpriteStack.h, kGroundNormalScale). It is decoded here, per vertex, so the fragment stage gets a
// linearly interpolated ground point. normal.z < 0 marks a stack with no ground under it (airborne).
//
// uInstanced 1 (DrawSpriteStacksInstanced): the vertex is a stack's layer corner in its own space (x, y
// texels round the pivot, z the layer index) and the instance record places it, doing on the GPU what
// DrawSpriteStacks does per quad on the CPU (SpriteStack.cpp: Place, LayerHeight, LayerPoint, Tilt,
// BuildLayer, the height shade). Keep the two the same. The sun map's depth pass uses this shader too.

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
layout(location = 10) in vec4 instancePlace;    // x, y, ground height, z above the ground
layout(location = 11) in vec4 instanceTurn;     // cos(rotation) * scale, sin(rotation) * scale, world units per layer, layers
layout(location = 12) in vec4 instanceMotion;   // lean (degrees), sway (degrees), build 0..1, pivot layer
layout(location = 13) in vec4 instanceTint;
layout(location = 14) in float instanceDamage;  // 0..1 (lit_sprite.fs, sun_depth.fs crumble)
uniform mat4 mvp;
uniform float uInstanced;
uniform float uViewRotation;    // degrees: the camera's yaw (a lean tilts toward screen-left)
uniform vec4 uSway;             // time, strength 0..1, wind direction x, y
uniform vec2 uShade;            // height shade: strength, reach (world units)
uniform float uGroundMaxZ;      // instances higher than this have no ground under them
uniform vec4 uWash[4];          // rotor downwash: x, y, radius, strength (0: none); swaying instances lean away
uniform float uSilhouette;      // > 0: DrawStackSilhouettes, pulled SILHOUETTE_PULL toward the eye (depth GREATER)
uniform vec3 uEye;
const float SILHOUETTE_PULL = 3.0;   // world units: what stands closer than this in front hides nothing (StackLighting.h)
out vec2 fragTexCoord;
out vec4 fragColor;
out vec2 fragGround;
out float fragGrounded;
out vec3 fragWorld;         // with a 3D camera (z_up) the world point, for the sun map
out float fragDamage;
out vec4 fragCell;          // instanced: texel x, y round the pivot, layer, height 0..1 (the crumble's noise)

const float GROUND_NORMAL_SCALE = 1024.0;
const float BUILD_FLIGHT = 4.0;     // SpriteStack.h kBuildFlight, kBuildDrop
const float BUILD_DROP = 3.0;

vec3 pulled(vec3 world)
{
    return uSilhouette > 0.0 ? world + normalize(uEye - world) * SILHOUETTE_PULL : world;
}

void main()
{
    fragTexCoord = vertexTexCoord;
    fragDamage = 0.0;
    fragCell = vec4(0.0);
    if (uInstanced < 0.5) {
        fragColor = vertexColor;
        fragGrounded = vertexNormal.z > 0.0 ? 1.0 : 0.0;
        fragGround = fragGrounded > 0.0 ? vertexNormal.xy / vertexNormal.z * GROUND_NORMAL_SCALE : vec2(0.0);
        fragWorld = vertexPosition;
        gl_Position = mvp * vec4(pulled(vertexPosition), 1.0);
        return;
    }
    float layer = vertexPosition.z, layers = instanceTurn.w, build = instanceMotion.z;
    float alpha = 1.0, drop = 0.0;
    if (build < 1.0) {   // BuildLayer
        float t = clamp((max(build, 0.0) * (layers + BUILD_FLIGHT) - layer) / BUILD_FLIGHT, 0.0, 1.0);
        alpha = min(1.0, t * 2.5);
        drop = (1.0 - t) * (1.0 - t) * BUILD_DROP;
    }
    if (alpha <= 0.0) {          // not drawn: the whole quad collapses
        gl_Position = vec4(0.0);
        return;
    }
    float above = instancePlace.w + (layer - instanceMotion.w + drop) * instanceTurn.z;
    // Tilt: the lean toward screen-left, the wind's sway downwind with a flutter across.
    float r = radians(uViewRotation), lean = tan(radians(instanceMotion.x));
    vec2 tilt = vec2(-cos(r), sin(r)) * lean;
    if (instanceMotion.y != 0.0 && uSway.y > 0.0) {
        vec2 dir = uSway.zw, at = instancePlace.xy;
        float gust = 0.55 + 0.45 * sin(uSway.x * 0.8 - dot(at, dir) * 0.008);
        float flutter = sin(uSway.x * 2.1 + at.x * 0.37 + at.y * 0.61);
        float k = radians(instanceMotion.y * uSway.y * gust);
        tilt += dir * (0.8 * k) + vec2(-dir.y, dir.x) * (0.35 * flutter * k);
    }
    if (instanceMotion.y != 0.0) {
        for (int i = 0; i < 4; i++) {
            vec2 away = instancePlace.xy - uWash[i].xy;
            float d = length(away);
            float k = uWash[i].w * (1.0 - smoothstep(0.0, max(uWash[i].z, 0.01), d));
            if (k > 0.0 && d > 0.01) {
                float beat = 0.75 + 0.25 * sin(uSway.x * 11.0 + d * 0.9);   // the rotor's gusts
                tilt += away / d * (0.6 * k * beat);
            }
        }
    }
    vec2 corner = vec2(instanceTurn.x * vertexPosition.x - instanceTurn.y * vertexPosition.y,
                       instanceTurn.y * vertexPosition.x + instanceTurn.x * vertexPosition.y);
    vec2 base = instancePlace.xy + corner;
    float below = max(0.0, 1.0 - above / uShade.y);
    fragColor = vec4(instanceTint.rgb * (1.0 - uShade.x * below * below), instanceTint.a * alpha);
    fragGrounded = instancePlace.w <= uGroundMaxZ ? 1.0 : 0.0;
    fragGround = fragGrounded > 0.0 ? base : vec2(0.0);
    fragWorld = vec3(base + tilt * above, instancePlace.z + above);
    fragDamage = instanceDamage;
    fragCell = vec4(vertexPosition.xy + floor(instancePlace.xy), layer, layer / max(layers - 1.0, 1.0));
    gl_Position = mvp * vec4(pulled(fragWorld), 1.0);
}
