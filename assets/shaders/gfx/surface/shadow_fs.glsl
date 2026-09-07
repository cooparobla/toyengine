#ifndef GFX_SURFACE_SHADOW_FS_GLSL
#define GFX_SURFACE_SHADOW_FS_GLSL

// gfx/surface/shadow_fs.glsl -- directional-light shadow-depth fragment
// backbone. No displacement hook here (see shadow_vs.glsl) -- this stage
// only alpha-tests against the CUTOUT mask. Its push block must stay
// byte-identical to shadow_vs.glsl's (they share one VkPushConstantRange).

layout(location = 0) in vec2 frag_uv;

layout(set = 0, binding = 0) uniform sampler2D u_alpha_mask;

layout(push_constant) uniform ShadowPC {
    mat4 light_space_matrix;
    float alpha;
    float alpha_cutoff;
    vec4  gfx_time;
    vec4  gfx_params;
} pc;

void main() {
    if (pc.alpha_cutoff > 0.0 && texture(u_alpha_mask, frag_uv).a < pc.alpha_cutoff) discard;
}

#endif // GFX_SURFACE_SHADOW_FS_GLSL
