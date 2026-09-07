#version 450

// Directional light depth map rendering. Depth is written automatically by
// the fixed-function rasterizer -- no BLEND alpha-dither discard, unlike
// blendy's version, since toyengine's forward transparent pass has no depth
// caster of its own (BLEND materials cast a shadow only at full opacity --
// see record_directional_shadow_'s doc). The alpha-mask discard below is
// CUTOUT (AlphaMode::Mask) support: alpha_cutoff > 0.0 arms it, so it's a
// no-op for every OPAQUE/BLEND caster and for a Mask caster with no texture
// (u_alpha_mask is then bound to a 1x1 white fallback -- see
// MaterialTextureCache).

layout(location = 0) in vec2 frag_uv;

layout(set = 0, binding = 0) uniform sampler2D u_alpha_mask;

layout(push_constant) uniform ShadowPC {
    mat4 light_space_matrix;
    float alpha;
    float alpha_cutoff;
} pc;

void main() {
    if (pc.alpha_cutoff > 0.0 && texture(u_alpha_mask, frag_uv).a < pc.alpha_cutoff) discard;
}
