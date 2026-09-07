#version 450

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in mat3 frag_TBN;

// Push constants: Material only (48 bytes) -- model/normal_matrix moved to
// the per-instance vertex stream (see gbuffer.vert), and this block is now
// shared once per instanced draw batch rather than pushed per object.
layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff; // 0.0 disables the alpha test below
    vec4  emissive;     // xyz = pre-multiplied emissive radiance, w reserved
} material;

// Set 1: material texture(s) -- currently just the CUTOUT alpha mask. Bound to a 1x1 white
// fallback for every non-masked material (see MaterialTextureCache), so texture(...).a == 1.0
// and the test below collapses back to the plain constant-alpha MASK test it replaces.
layout(set = 1, binding = 0) uniform sampler2D u_alpha_mask;

// G-Buffer Render Targets
layout(location = 0) out vec4 out_albedo_ao;          // RGB = Albedo, A = AO
layout(location = 1) out vec4 out_normal_metallic;    // RGB = World Normal, A = Metallic
layout(location = 2) out vec4 out_position_roughness; // RGB = World Pos, A = Roughness
layout(location = 3) out vec4 out_emissive;           // RGB = emissive radiance (HDR), A = unused

void main() {
    // CUTOUT/MASK materials: alpha_cutoff > 0 arms the test. albedo.a (the constant per-material
    // alpha) multiplied by the sampled mask's alpha gives a real per-texel silhouette test when a
    // texture_alpha_mask is authored, and collapses to the old constant-only test otherwise.
    float alpha = material.albedo.a * texture(u_alpha_mask, frag_uv).a;
    if (material.alpha_cutoff > 0.0 && alpha < material.alpha_cutoff) discard;

    vec3 albedo   = material.albedo.rgb;
    float metallic  = material.metallic;
    float roughness = max(material.roughness, 0.045);
    float ao        = material.ao;

    vec3 N = normalize(frag_world_normal);

    out_albedo_ao          = vec4(albedo, ao);
    out_normal_metallic    = vec4(N, metallic);
    out_position_roughness = vec4(frag_world_pos, roughness);
    out_emissive            = vec4(material.emissive.rgb, 0.0);
}
