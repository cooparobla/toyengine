#ifndef GFX_SURFACE_SNOW_HEIGHT_GLSL
#define GFX_SURFACE_SNOW_HEIGHT_GLSL

// gfx/surface/snow_height.glsl -- the deep-snow height field: how far the lying snow raises a
// `snow`-shaded surface at a world point. Shared by every stage of the `snow` surface shader
// (vertex / evaluation displacement, fragment normals) and mirrored on the CPU by
// toy::world::deep_snow_depth() (world/snow_field.h), so gameplay sinks into the snow drawn.
//
//   height(p) = max(0, depth * cover * open_sky(p) * patch(p) - trench(p))
//
// patch(p) is 1 under the soft style; under `snow_patch_style: hard` it is the round-patch mask
// (gfx/surface/snow_patches.glsl) with a short ramp, so the snow lies in rounded mounds.
//
// A function of the world POSITION only -- never of the normal or the vertex -- so two
// triangles sharing an edge displace it identically and no crack can open. Needs world.glsl.

#include <gfx/surface/snow_patches.glsl>

/// The settled snow surface above p before anything pressed in (m).
float gfx_snow_lying_height(vec3 p) {
    float cover = gfx_world.snow.x;
    float depth = gfx_world.snow.y;
    if (cover <= 0.0 || depth <= 0.0) return 0.0;
    float open = gfx_world_open_sky(p + vec3(0.0, 0.0, 0.35 + 0.35 * gfx_world.occl.z), 0.5);
    float patch_mask = 1.0;
    if (gfx_world.snow_style.x > 0.5 && !gfx_snow_patch_saturated(cover, open)) {
        patch_mask = gfx_snow_patch_ramp(gfx_snow_patch_value(p.xy, cover, open, gfx_world.snow_style.y));
    }
    return depth * cover * open * patch_mask;
}

/// The snow surface above p, trenches pressed in (m).
float gfx_snow_height(vec3 p) {
    float h = gfx_snow_lying_height(p);
    return h > 0.0 ? max(0.0, h - gfx_world_trench(p.xy)) : 0.0;
}

#endif // GFX_SURFACE_SNOW_HEIGHT_GLSL
