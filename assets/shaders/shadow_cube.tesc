#version 450
// shadow_cube.tesc -- the cube shadow pass's tessellation control stage. Edge factors come from
// the MAIN camera (the world set's tess_view), never the light, so the shadow is cast by the same
// subdivided -- and displaced -- surface the camera sees. No frustum cull here: the light's
// frustum is a different volume, and patches outside it simply rasterize nothing.

layout(push_constant) uniform CubeShadowPC {
    mat4  light_space_matrix;
    vec4  light_pos_range;
    float alpha;
    float alpha_cutoff;
    uint  tess_a;
    uint  tess_b;
    vec4  gfx_time;
    vec4  gfx_params;
} pc;

#define GFX_WORLD_SET 1
#include <gfx/surface/world.glsl>

#define GFX_TESS_PACKED uvec2(pc.tess_a, pc.tess_b)
#define GFX_TESS_VIEW gfx_world.tess_view
#include <gfx/surface/tess_control.glsl>
