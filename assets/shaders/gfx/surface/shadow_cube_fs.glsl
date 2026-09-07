#ifndef GFX_SURFACE_SHADOW_CUBE_FS_GLSL
#define GFX_SURFACE_SHADOW_CUBE_FS_GLSL

// gfx/surface/shadow_cube_fs.glsl -- point-light cubemap-face shadow-depth
// fragment backbone. No displacement hook here (see shadow_cube_vs.glsl) --
// this stage alpha-tests against the CUTOUT mask and writes linear depth.
// Push block must stay byte-identical to shadow_cube_vs.glsl's.

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec2 frag_uv;

layout(set = 0, binding = 0) uniform sampler2D u_alpha_mask;

layout(push_constant) uniform CubeShadowPC {
    mat4 light_space_matrix;
    vec4 light_pos_range; // xyz = light pos, w = range
    float alpha;
    float alpha_cutoff;
    vec4  gfx_time;
    vec4  gfx_params;
} pc;

void main() {
    if (pc.alpha_cutoff > 0.0 && texture(u_alpha_mask, frag_uv).a < pc.alpha_cutoff) discard;
    float light_dist = length(frag_world_pos - pc.light_pos_range.xyz);
    gl_FragDepth = clamp(light_dist / pc.light_pos_range.w, 0.0, 1.0);
}

#endif // GFX_SURFACE_SHADOW_CUBE_FS_GLSL
