#version 330
// Wet ground (fx.draw_wet): where the patch texture has alpha (the paved ground), darken it and
// reflect the light map. On wet asphalt a light's reflection stretches toward the viewer, which on
// screen is straight down under the light: so the light above this point on screen (uUp, the world
// direction shown as screen-up) is gathered in a fading line. A perspective camera (uEye) sees each
// point along its own ray, so there the line runs from the eye's ground point through this one.
// By day a little sky is reflected.
// Premultiplied output (blend: dst * (1 - a) + rgb): a = how much darker, rgb = the reflection.

in vec2 fragTexCoord;
in vec2 fragWorld;
uniform sampler2D texture0;
uniform sampler2D uLightMap;
uniform vec4 uLightArea;
uniform vec3 uAmbient;      // the light map's colour where no light reaches
uniform float uUseLight;    // 1 at night: the light map is this frame's
uniform vec2 uUp;
uniform vec3 uEye;          // a perspective camera's world position; z < 0: none (use uUp)
uniform float uWet;         // 0 dry .. 1 soaked
uniform vec3 uSky;          // what a wet surface reflects by day
out vec4 finalColor;

const int STEPS = 10;
const float STEP = 5.0;     // world units between taps along the streak

vec3 lightAt(vec2 world)
{
    vec2 uv = (world - uLightArea.xy) / uLightArea.zw;
    return max(texture(uLightMap, vec2(uv.x, 1.0 - uv.y)).rgb - uAmbient, 0.0);
}

void main()
{
    float paved = texture(texture0, fragTexCoord).a;
    vec3 reflection = uSky * 0.06 * (1.0 - uUseLight);
    if (uUseLight > 0.5) {
        vec2 away = fragWorld - uEye.xy;
        vec2 dir = uEye.z >= 0.0 && dot(away, away) > 1.0 ? normalize(away) : uUp;
        vec3 sum = vec3(0.0);
        float total = 0.0;
        for (int i = 0; i < STEPS; i++) {
            float w = 1.0 - float(i) / float(STEPS);
            sum += w * lightAt(fragWorld + dir * (float(i) * STEP));
            total += w;
        }
        reflection += sum / total * 0.55;
    }
    float a = 0.22 * uWet * paved;
    finalColor = vec4(reflection * uWet * paved, a);
}
