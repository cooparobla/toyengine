#version 450

// surface_tess.vert -- the vertex stage of every TESSELLATED surface pipeline (G-buffer, both
// shadow passes, forward transparent). It only forwards the authored vertex and its instance's
// model matrices: everything a non-tessellated backbone does per vertex (world transform, the
// displacement hook, the TBN) happens per GENERATED vertex in the evaluation stage instead
// (gfx/surface/gbuffer_tes.glsl and siblings). The world position goes to the control stage
// for the edge factors.

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent;
layout(location = 4) in mat4 in_model;       // per-instance (locations 4-7)
layout(location = 8) in mat4 in_prev_model;  // per-instance (locations 8-11)
layout(location = 12) in mat4 in_snow_anchor; // per-instance (locations 12-15)

layout(location = 0) out vec3 tv_position_os;
layout(location = 1) out vec3 tv_normal_os;
layout(location = 2) out vec2 tv_uv;
layout(location = 3) out vec4 tv_tangent_os;
layout(location = 4) out vec3 tv_position_ws;
layout(location = 5) out mat4 tv_model;       // 5-8
layout(location = 9) out mat4 tv_prev_model;  // 9-12
layout(location = 13) out mat4 tv_snow_anchor; // 13-16

void main() {
    tv_position_os = in_position;
    tv_normal_os   = in_normal;
    tv_uv          = in_uv;
    tv_tangent_os  = in_tangent;
    tv_position_ws = (in_model * vec4(in_position, 1.0)).xyz;
    tv_model       = in_model;
    tv_prev_model  = in_prev_model;
    tv_snow_anchor = in_snow_anchor;
    gl_Position    = vec4(tv_position_ws, 1.0);
}
