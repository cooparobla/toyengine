#ifndef GFX_SURFACE_SNOW_PATCHES_GLSL
#define GFX_SURFACE_SNOW_PATCHES_GLSL

// gfx/surface/snow_patches.glsl -- the HARD-edged (stylized) snow patch pattern, the weather's
// `snow_patch_style: hard` (gfx_world.snow_style.x). Round patches that appear as the cover
// rises, grow, and merge into one another until the ground is white.
//
// The pattern is a metaball field: each grid cell (`size` metres) holds one circle with a hashed
// centre and radius, and the field sums a smooth bump per circle -- so where two circles near
// each other their bumps add and the outline bulges into a rounded bridge, the way toon snow
// clumps. A second, finer, light layer keeps the outlines from looking machine-made. A patch is wherever the field
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

/// One layer of circles, `size` metres per cell: the summed bumps at world xy, and their
/// world-space gradient -- (F, dF/dx, dF/dy). One pass gives both, so a fragment that needs a
/// normal or an edge width evaluates the pattern once, not once per finite-difference tap.
vec3 gfx_snow_blob_layer_(vec2 xy, float size) {
    vec2 g = xy / size;
    vec2 c = floor(g);
    vec3 sum = vec3(0.0);
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            vec2 cell = c + vec2(float(i), float(j));
            vec3 h = gfx_snow_hash3_(cell);
            vec2 centre = cell + 0.15 + 0.7 * h.xy;
            float r = mix(0.35, 1.0, h.z);
            vec2 dg = g - centre;
            float q = 1.0 - dot(dg, dg) / (r * r);
            if (q > 0.0) {
                // d(q^2)/dg = 2q * dq/dg, dq/dg = -2 dg / r^2; dg/dxy = 1 / size.
                sum += vec3(q * q, -4.0 * q * dg / (r * r * size));
            }
        }
    }
    return sum;
}

/// The metaball field and its gradient: broad patches plus a finer, lighter layer.
vec3 gfx_snow_blobs_grad(vec2 xy, float size) {
    return gfx_snow_blob_layer_(xy, size) + 0.3 * gfx_snow_blob_layer_(xy + vec2(17.3, 41.9), size * 0.45);
}
float gfx_snow_blobs(vec2 xy, float size) {
    return gfx_snow_blobs_grad(xy, size).x;
}

/// The field level a patch starts at, for a cover 0..1. Fitted to the field's distribution so
/// the patched fraction of the ground roughly tracks the cover: nothing at 0 (above the field's
/// peak), the first small patches soon after, all of it at 1 (far enough below the field's floor
/// that even the ramped deep-snow mask is full).
float gfx_snow_patch_threshold(float cover) {
    return 1.25 - 1.7 * cover + 1.4 * pow(1.0 - cover, 12.0);
}

/// > 0 inside a patch, < 0 outside. `receptive` (0..1, up-facing x open to the sky) raises the
/// threshold rather than fading the result, so patches shrink crisply toward roof edges.
float gfx_snow_patch_value_from_field(float field, float cover, float receptive) {
    return field - gfx_snow_patch_threshold(cover) - (1.0 - receptive) * 1.5;
}
float gfx_snow_patch_value(vec2 xy, float cover, float receptive, float size) {
    return gfx_snow_patch_value_from_field(gfx_snow_blobs(xy, size), cover, receptive);
}

/// True when every point is deep inside a patch -- the threshold sits so far below the field's
/// floor (0) that even the ramp is saturated -- so the pattern need not be evaluated at all. The
/// common case at full cover in the open: a fully snowed scene pays nothing for the style.
bool gfx_snow_patch_saturated(float cover, float receptive) {
    return gfx_snow_patch_value_from_field(0.0, cover, receptive) >= 0.35;
}

/// The mask with a ramp (vertex / tessellation stages, the CPU mirror): 0..1 over the first 0.35
/// of the field above the threshold -- wide enough that the mound's wall is a slope the
/// tessellated grid resolves, not a step it aliases into teeth.
float gfx_snow_patch_ramp(float v) {
    return smoothstep(0.0, 0.35, v);
}
/// d/dv of gfx_snow_patch_ramp().
float gfx_snow_patch_ramp_slope(float v) {
    float t = clamp(v / 0.35, 0.0, 1.0);
    return 6.0 * t * (1.0 - t) / 0.35;
}

/// The crisp (fragment) patch edge: a pixel-wide antialiased step at v = 0. `grad` is v's world
/// gradient (the field's), `dpdx` / `dpdy` the fragment's world-xy screen derivatives -- taken
/// by the caller outside any per-pixel branch -- so the edge width needs no fwidth() here.
float gfx_snow_patch_edge(float v, vec2 grad, vec2 dpdx, vec2 dpdy) {
    float w = abs(dot(grad, dpdx)) + abs(dot(grad, dpdy));
    return clamp(v / max(w, 1e-4) + 0.5, 0.0, 1.0);
}

#endif // GFX_SURFACE_SNOW_PATCHES_GLSL
