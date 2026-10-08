#version 450
// gbuffer.tesc -- the G-buffer passes' tessellation control stage (gfx/surface/tess_control.glsl).

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

#define GFX_TESS_PACKED material.surface_ext.xy
#define GFX_TESS_VIEW gfx_world.tess_view
#define GFX_TESS_CLIP (camera.proj * camera.view)
#define GFX_TESS_EXTRA_MARGIN gfx_world.snow.y
#include <gfx/surface/tess_control.glsl>
