#version 450
// `heightmap_terrain` G-buffer tessellation control stage: the stock body (gfx/surface/
// tess_control.glsl) with edges sized -- and patches culled -- by where the control points really
// are once placed on the height grid, not by the flat patch's own positions. Two chunks sharing a
// border place its endpoints identically, so they agree on how finely to split it.

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    mat4 prev_view;
    mat4 prev_proj;
    vec4 jitter_ndc;
} camera;

layout(push_constant) uniform PushConstants {
    vec4  albedo;
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
    vec4  emissive;
    vec4  gfx_time;
    vec4  gfx_params;
    uvec4 surface_ext;
} material;

#define GFX_WORLD_SET 2
#include <gfx/surface/world.glsl>
layout(set = 1, binding = 4) uniform sampler2D u_displacement;
#include "heightmap_terrain_common.glsl"

vec3 ht_control_point_(vec3 pos_os, mat4 model) {
    int slot = ht_slot(material.gfx_params.x);
    return ht_world(slot, ht_grid_coord(slot, pos_os, model));
}

#define GFX_TESS_PACKED material.surface_ext.xy
#define GFX_TESS_VIEW gfx_world.tess_view
#define GFX_TESS_CLIP (camera.proj * camera.view)
#define GFX_TESS_EXTRA_MARGIN gfx_world.snow.y
// The grid between a patch's corners can rise above them: a few metres of slack.
#define GFX_TESS_CULL_MARGIN 4.0
#define GFX_TESS_POSITION_WS(i) ht_control_point_(tv_position_os[i], tv_model[i])
#include <gfx/surface/tess_control.glsl>
