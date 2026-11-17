//!HOOK LUMA
//!HOOK CHROMA
//!BIND HOOKED
//!DESC Conservative Remote Play deblocking

vec4 repairBoundary(vec4 center, vec2 axis, int coordinate)
{
    // HEVC transform boundaries align to at least four samples. The same
    // grid here also covers larger boundaries on subsampled chroma planes.
    int phase = coordinate % 4;
    if (phase != 0 && phase != 3)
        return vec4(0.0);
    vec2 across = phase == 0 ? -axis : axis;
    vec4 other = HOOKED_texOff(across);
    vec4 nearby = HOOKED_texOff(-across);
    vec4 farther = HOOKED_texOff(across * 2.0);
    vec4 delta = other - center;
    vec4 jump = abs(delta);
    vec4 local_gradient = max(abs(nearby - center), abs(farther - other));
    // A small step between two smooth regions is more likely a block edge.
    vec4 strength = smoothstep(0.004, 0.013, jump)
                  * (1.0 - smoothstep(0.028, 0.058, jump))
                  * (1.0 - smoothstep(0.003, 0.016, local_gradient));
    return 0.24 * delta * strength;
}

vec4 hook()
{
    ivec2 pixel = ivec2(floor(HOOKED_pos * HOOKED_size));
    vec4 center = HOOKED_tex(HOOKED_pos);
    return center + repairBoundary(center, vec2(1.0, 0.0), pixel.x)
                  + repairBoundary(center, vec2(0.0, 1.0), pixel.y);
}
