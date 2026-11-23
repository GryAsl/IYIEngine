// Damage (#include; needs in float fragDamage, in vec4 fragCell): texels crumble away in chunks of two,
// more of them higher up (the top goes first). lit_sprite.fs and sun_depth.fs share it, so shadows
// and contact keep the holes.
float crumbleHash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

bool crumbled()
{
    if (fragDamage <= 0.0) return false;
    float h = crumbleHash(vec3(floor(fragCell.xy * 0.5), fragCell.z));
    return h < fragDamage * (0.4 + 0.9 * fragCell.w) - 0.15;
}
