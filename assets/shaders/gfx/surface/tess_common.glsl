#ifndef GFX_SURFACE_TESS_COMMON_GLSL
#define GFX_SURFACE_TESS_COMMON_GLSL

// gfx/surface/tess_common.glsl -- what every tessellation stage shares: the per-draw parameters
// (packed into the push block's tess_a / tess_b, see GBufferPipeline::PushConstants and the
// shadow push blocks) and the edge factor.
//
// Edge factors are a function of the edge's two WORLD-SPACE endpoints only -- order-independent
// (midpoint and length) -- so the two triangles sharing an edge always agree on how finely to
// split it and no crack can open between them. The camera is the MAIN camera (the world set's
// tess_view) in every pass, so the shadow passes tessellate a caster exactly as the camera sees
// it and its displaced shadow matches its displaced surface.

struct GfxTessParams {
    float edge_pixels;   // target edge length in render pixels
    float max_factor;    // subdivision cap per edge
    float max_distance;  // beyond: factor 1 (the mesh as authored)
    float displacement;  // displacement-map metres at height 1
};

GfxTessParams gfx_unpack_tess(uint a, uint b) {
    vec2 ab = unpackHalf2x16(a);
    vec2 cd = unpackHalf2x16(b);
    GfxTessParams p;
    p.edge_pixels  = max(ab.x, 0.5);
    p.max_factor   = clamp(ab.y, 1.0, 64.0);
    p.max_distance = cd.x;
    p.displacement = cd.y;
    return p;
}

/// How many segments an edge from p0 to p1 is split into: its projected length at its midpoint's
/// distance, over the target pixel length; 1 beyond max_distance.
float gfx_tess_edge_factor(vec3 p0, vec3 p1, vec3 eye, float px_scale, GfxTessParams t) {
    vec3 mid = (p0 + p1) * 0.5;
    float d = max(distance(mid, eye), 0.05);
    if (d > t.max_distance) return 1.0;
    float projected = distance(p0, p1) * px_scale / d;
    // Fade the factor toward 1 over the last 20% of max_distance, so the hand-over to the plain
    // mesh is a gradient rather than a ring where detail pops.
    float fade = 1.0 - smoothstep(t.max_distance * 0.8, t.max_distance, d);
    return clamp(1.0 + (projected / t.edge_pixels - 1.0) * fade, 1.0, t.max_factor);
}

#endif // GFX_SURFACE_TESS_COMMON_GLSL
