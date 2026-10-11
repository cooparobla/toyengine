#version 450
// `heightmap_terrain` depth shadow tessellation control stage: edges sized by the MAIN camera
// (like shadow_depth.tesc) from the control points placed on the height grid -- the
// same factors the G-buffer pass computes, so the shadow is cast by the surface the camera sees.

layout(push_constant) uniform ShadowPC {
    mat4  light_space_matrix;
    float alpha;
    float alpha_cutoff;
    uint  tess_a;
    uint  tess_b;
    vec4  gfx_time;
    vec4  gfx_params;
} pc;

#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>
layout(set = 0, binding = 4) uniform sampler2D u_displacement;
#include "heightmap_terrain_common.glsl"

vec3 ht_control_point_(vec3 pos_os, mat4 model) {
    int slot = ht_slot(pc.gfx_params.x);
    return ht_world(slot, ht_grid_coord(slot, pos_os, model));
}

#define GFX_TESS_PACKED uvec2(pc.tess_a, pc.tess_b)
#define GFX_TESS_VIEW gfx_world.tess_view
#define GFX_TESS_POSITION_WS(i) ht_control_point_(tv_position_os[i], tv_model[i])
#include <gfx/surface/tess_control.glsl>
