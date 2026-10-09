#ifndef GFX_SURFACE_TESS_CONTROL_GLSL
#define GFX_SURFACE_TESS_CONTROL_GLSL

// gfx/surface/tess_control.glsl -- the tessellation control stage body every pass shares. The
// entry file (gbuffer.tesc, shadow_depth.tesc, shadow_cube.tesc, transparent.tesc) first
// declares its pass's push block and the world set, then defines:
//
//   GFX_TESS_PACKED        an expression giving uvec2(tess_a, tess_b) from its push block
//   GFX_TESS_VIEW          vec4(camera position, pixel scale) -- the world set's tess_view, or
//                          the camera UBO's (jitter_ndc.z) in the transparent pass
//   GFX_TESS_CLIP          (optional) a mat4 view-projection: patches wholly outside its frustum
//                          (by GFX_TESS_CULL_MARGIN plus the displacement scale) get factor 0
//   GFX_TESS_CULL_MARGIN   (optional, metres; default 1) how far the shader's own displacement may
//                          push a vertex -- water's waves want more
//
// Outer factor i is the edge OPPOSITE vertex i (Vulkan's triangle domain), computed from that
// edge's two endpoints only (gfx_tess_edge_factor); the inner factor is the largest outer.

#include <gfx/surface/tess_common.glsl>

#ifndef GFX_TESS_CULL_MARGIN
#define GFX_TESS_CULL_MARGIN 1.0
#endif
// Displacement the frame adds on top (the G-buffer passes the deep-snow depth).
#ifndef GFX_TESS_EXTRA_MARGIN
#define GFX_TESS_EXTRA_MARGIN 0.0
#endif

layout(vertices = 3) out;

layout(location = 0) in vec3 tv_position_os[];
layout(location = 1) in vec3 tv_normal_os[];
layout(location = 2) in vec2 tv_uv[];
layout(location = 3) in vec4 tv_tangent_os[];
layout(location = 4) in vec3 tv_position_ws[];
layout(location = 5) in mat4 tv_model[];
layout(location = 9) in mat4 tv_prev_model[];
layout(location = 13) in mat4 tv_snow_anchor[];

layout(location = 0) out vec3 tc_position_os[];
layout(location = 1) out vec3 tc_normal_os[];
layout(location = 2) out vec2 tc_uv[];
layout(location = 3) out vec4 tc_tangent_os[];
layout(location = 4) patch out mat4 tc_model;        // 4-7
layout(location = 8) patch out mat4 tc_prev_model;   // 8-11
layout(location = 12) patch out mat4 tc_snow_anchor; // 12-15

#ifdef GFX_TESS_CLIP
// True if the sphere (c, r) lies wholly outside one of the view-projection's frustum planes
// (Gribb-Hartmann extraction; Vulkan's z range 0..w).
bool gfx_tess_outside_(mat4 m, vec3 c, float r) {
    mat4 t = transpose(m);
    vec4 planes[5] = vec4[](t[3] + t[0], t[3] - t[0], t[3] + t[1], t[3] - t[1], t[2]);
    for (int i = 0; i < 5; ++i) {
        float len = length(planes[i].xyz);
        if (dot(planes[i].xyz, c) + planes[i].w < -r * len) return true;
    }
    return false;
}
#endif

void main() {
    tc_position_os[gl_InvocationID] = tv_position_os[gl_InvocationID];
    tc_normal_os[gl_InvocationID]   = tv_normal_os[gl_InvocationID];
    tc_uv[gl_InvocationID]          = tv_uv[gl_InvocationID];
    tc_tangent_os[gl_InvocationID]  = tv_tangent_os[gl_InvocationID];
    if (gl_InvocationID == 0) {
        tc_model      = tv_model[0];
        tc_prev_model = tv_prev_model[0];
        tc_snow_anchor = tv_snow_anchor[0];

        uvec2 packed_params = GFX_TESS_PACKED;
        GfxTessParams t = gfx_unpack_tess(packed_params.x, packed_params.y);
        vec4 view = GFX_TESS_VIEW;
        vec3 p0 = tv_position_ws[0], p1 = tv_position_ws[1], p2 = tv_position_ws[2];

        bool culled = false;
#ifdef GFX_TESS_CLIP
        vec3 c = (p0 + p1 + p2) / 3.0;
        float r = max(max(distance(c, p0), distance(c, p1)), distance(c, p2)) +
                  GFX_TESS_CULL_MARGIN + abs(t.displacement) + GFX_TESS_EXTRA_MARGIN;
        culled = gfx_tess_outside_(GFX_TESS_CLIP, c, r);
#endif
        if (culled) {
            gl_TessLevelOuter[0] = 0.0;
            gl_TessLevelOuter[1] = 0.0;
            gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
        } else {
            float e0 = gfx_tess_edge_factor(p1, p2, view.xyz, view.w, t);
            float e1 = gfx_tess_edge_factor(p2, p0, view.xyz, view.w, t);
            float e2 = gfx_tess_edge_factor(p0, p1, view.xyz, view.w, t);
            gl_TessLevelOuter[0] = e0;
            gl_TessLevelOuter[1] = e1;
            gl_TessLevelOuter[2] = e2;
            gl_TessLevelInner[0] = max(e0, max(e1, e2));
        }
    }
}

#endif // GFX_SURFACE_TESS_CONTROL_GLSL
