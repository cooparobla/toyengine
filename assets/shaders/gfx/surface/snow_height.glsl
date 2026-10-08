#ifndef GFX_SURFACE_SNOW_HEIGHT_GLSL
#define GFX_SURFACE_SNOW_HEIGHT_GLSL

// gfx/surface/snow_height.glsl -- the deep-snow height field: how far the lying snow raises a
// `snow`-shaded surface at a world point. Shared by every stage of the `snow` surface shader
// (vertex / evaluation displacement, fragment normals) and mirrored on the CPU by
// toy::world::deep_snow_depth() (world/snow_field.h), so gameplay sinks into the snow drawn.
//
//   height(p) = max(0, depth * cover * open_sky(p) - trench(p))
//
// A function of the world POSITION only -- never of the normal or the vertex -- so two
// triangles sharing an edge displace it identically and no crack can open. Needs world.glsl.

/// The settled snow surface above p before anything pressed in (m).
float gfx_snow_lying_height(vec3 p) {
    float cover = gfx_world.snow.x;
    float depth = gfx_world.snow.y;
    if (cover <= 0.0 || depth <= 0.0) return 0.0;
    float open = gfx_world_open_sky(p + vec3(0.0, 0.0, 0.35 + 0.35 * gfx_world.occl.z), 0.5);
    return depth * cover * open;
}

/// The snow surface above p, trenches pressed in (m).
float gfx_snow_height(vec3 p) {
    float h = gfx_snow_lying_height(p);
    return h > 0.0 ? max(0.0, h - gfx_world_trench(p.xy)) : 0.0;
}

#endif // GFX_SURFACE_SNOW_HEIGHT_GLSL
