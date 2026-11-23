#version 330
// Lights sprite stacks per texel, at that texel's own ground point (from lit_sprite.vs): from the
// world-space light map by night, and in cloud shade by day. Why not sample the light map at the
// screen pixel: a raised layer covers ground that lies BEHIND the object, so a light pool on the far
// side of a house would light its roof. Stacks with no ground under them (airborne) get the ambient
// colour only and no cloud shade.

in vec2 fragTexCoord;
in vec4 fragColor;
in vec2 fragGround;
in float fragGrounded;
in vec3 fragWorld;
in float fragDamage;
in vec4 fragCell;
uniform sampler2D texture0;
uniform sampler2D uLightMap;
uniform vec4 uLightArea;    // the world rectangle the light map covers: x, y, width, height
uniform vec3 uAmbient;      // light for stacks with no ground under them
uniform float uUseLight;    // 1: the light map holds this frame's light (night); 0: daylight
uniform vec4 uClouds;       // as soft_shadow.fs
uniform float uCloudScale;
uniform float uSun;         // the sun's shadow strength: cloud shade scales with it
uniform vec3 uShadowTint;   // what shade blends toward, as on the ground
uniform sampler2DShadow uSunMap;   // the sun's depth map (3D camera), as terrain.fs
uniform mat4 uSunVP;
uniform vec3 uSunOffset;
uniform vec2 uSunSoft;     // the filter taps' reach in the map's 0..1, up the map only (StackLighting.cpp kSunSoft):
                            // fills the gaps between a low sun's layer strips
uniform float uSunBias;
uniform float uUseSunMap;
uniform vec4 uFog;          // distance haze: rgb, the most it covers (0: none)
uniform vec2 uFogRange;     // world units from the eye where it begins and is full
uniform vec3 uEye;
uniform float uSilhouette;
out vec4 finalColor;

#include "noise.glsl"
#include "cloud.glsl"
#include "crumble.glsl"
#include "sun_fog.glsl"

void main()
{
    vec4 own = texture(texture0, fragTexCoord);
    if (own.a < 0.5 || crumbled()) discard;   // clear texels must not write depth (a 3D camera's depth test)
    if (uSilhouette > 0.0) {                     // DrawStackSilhouettes: flat, its tint
        finalColor = vec4(fragColor.rgb, uSilhouette);
        return;
    }
    vec4 texel = own * fragColor;
    texel.rgb *= 1.0 - 0.55 * fragDamage * (0.5 + 0.5 * crumbleHash(vec3(floor(fragCell.xy * 0.25), 7.0)));   // soot
    vec3 light = vec3(1.0);
    if (uUseLight > 0.5) {
        vec2 uv = (fragGround - uLightArea.xy) / uLightArea.zw;
        vec3 ground = texture(uLightMap, vec2(uv.x, 1.0 - uv.y)).rgb;          // render textures are upside down
        light = mix(uAmbient, ground, fragGrounded);
    }
    vec3 rgb = texel.rgb * light;
    rgb = mix(rgb, uShadowTint, max(cloud(fragGround) * fragGrounded, sunShadow(fragWorld)) * uSun);
    finalColor = vec4(fogged(rgb, fragWorld), texel.a);
}
