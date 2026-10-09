#ifndef GFX_SURFACE_SNOW_PATCHES_GLSL
#define GFX_SURFACE_SNOW_PATCHES_GLSL

// gfx/surface/snow_patches.glsl -- the HARD-edged (stylized) snow patch pattern, the weather's
// `snow_patch_style: hard` (gfx_world.snow_style.x). Round patches that appear as the cover
// rises, grow, and merge into one another until the ground is white.
//
// The pattern is a metaball field: each grid cell (`size` metres) holds one circle with a hashed
// centre and radius, and the field sums a smooth bump per circle -- so where two circles near
// each other their bumps add and the outline bulges into a rounded bridge, the way toon snow
// clumps. A second, finer layer breaks the outline up a little. A patch is wherever the field
// rises above a threshold that falls as the cover grows.
//
// Stage-agnostic (no fwidth here): gfx_snow_patch_value() is the signed distance-like value, and
// callers turn it into a mask -- the cover layer and the deep-snow albedo with a pixel-wide
// antialiased edge (fragment), the deep-snow displacement with a short world-space ramp so the
// mounds have a wall tessellation can resolve. Mirrored on the CPU by toy::world::snow_patch_value()
// (world/snow_field.h): keep the two identical.

/// Three hashes in [0,1) per integer cell.
vec3 gfx_snow_hash3_(vec2 c) {
    vec3 p = fract(vec3(c.x, c.y, c.x) * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return fract((p.xxy + p.yzz) * p.zyx);
}

/// One layer of circles, `size` metres per cell: the summed bumps at world xy.
float gfx_snow_blob_layer_(vec2 xy, float size) {
    vec2 g = xy / size;
    vec2 c = floor(g);
    float sum = 0.0;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 cell = c + vec2(float(i), float(j));
            vec3 h = gfx_snow_hash3_(cell);
            vec2 centre = cell + 0.15 + 0.7 * h.xy;
            float r = mix(0.35, 1.0, h.z);
            float d = length(g - centre) / r;
            float q = max(1.0 - d * d, 0.0);
            sum += q * q;
        }
    }
    return sum;
}

/// The metaball field: broad patches plus a finer, lighter layer.
float gfx_snow_blobs(vec2 xy, float size) {
    return gfx_snow_blob_layer_(xy, size) + 0.5 * gfx_snow_blob_layer_(xy + vec2(17.3, 41.9), size * 0.45);
}

/// > 0 inside a patch, < 0 outside. `receptive` (0..1, up-facing x open to the sky) raises the
/// threshold rather than fading the result, so patches shrink crisply toward roof edges.
float gfx_snow_patch_value(vec2 xy, float cover, float receptive, float size) {
    float threshold = 1.05 - 1.15 * cover;
    return gfx_snow_blobs(xy, size) - threshold - (1.0 - receptive) * 1.5;
}

/// The mask with a world-space ramp (vertex / tessellation stages, the CPU mirror): 0..1 over
/// the first 0.15 of the field above the threshold.
float gfx_snow_patch_ramp(float v) {
    return smoothstep(0.0, 0.15, v);
}

#endif // GFX_SURFACE_SNOW_PATCHES_GLSL
