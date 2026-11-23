// Sun shadow 0..1 and distance haze (#include; needs the uSun* and uFog* uniforms, uFogRange, uEye).
// sunShadow: five filtered depth compares (hardware PCF) at and round the point, so edges are soft
// over about two map texels.
float sunShadow(vec3 world)
{
    if (uUseSunMap < 0.5) return 0.0;
    vec4 p = uSunVP * vec4(world + uSunOffset, 1.0);
    vec3 q = p.xyz / p.w * 0.5 + 0.5;
    if (q.x < 0.0 || q.y < 0.0 || q.x > 1.0 || q.y > 1.0 || q.z > 1.0) return 0.0;
    vec2 t = max(0.75 / vec2(textureSize(uSunMap, 0)), uSunSoft);
    float lit = texture(uSunMap, vec3(q.xy, q.z - uSunBias))
              + texture(uSunMap, vec3(q.xy + vec2(-t.x, -t.y), q.z - uSunBias))
              + texture(uSunMap, vec3(q.xy + vec2(t.x, -t.y), q.z - uSunBias))
              + texture(uSunMap, vec3(q.xy + vec2(-t.x, t.y), q.z - uSunBias))
              + texture(uSunMap, vec3(q.xy + vec2(t.x, t.y), q.z - uSunBias));
    // Faded out toward the map's edge, where its area (the shadow distance) ends.
    float edge = min(min(q.x, 1.0 - q.x), min(q.y, 1.0 - q.y));
    return (1.0 - lit * 0.2) * smoothstep(0.0, 0.04, edge);
}

vec3 fogged(vec3 rgb, vec3 world)
{
    if (uFog.a <= 0.0) return rgb;
    return mix(rgb, uFog.rgb, smoothstep(uFogRange.x, uFogRange.y, distance(world, uEye)) * uFog.a);
}
