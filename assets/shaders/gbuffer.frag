#version 450

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in mat3 frag_TBN;

// Push constants: Material only (32 bytes) -- model/normal_matrix moved to
// the per-instance vertex stream (see gbuffer.vert), and this block is now
// shared once per instanced draw batch rather than pushed per object.
layout(push_constant) uniform PushConstants {
    vec4  albedo;     // xyz = albedo, w = alpha
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff; // 0.0 disables the alpha test below
} material;

// G-Buffer Render Targets
layout(location = 0) out vec4 out_albedo_ao;          // RGB = Albedo, A = AO
layout(location = 1) out vec4 out_normal_metallic;    // RGB = World Normal, A = Metallic
layout(location = 2) out vec4 out_position_roughness; // RGB = World Pos, A = Roughness

void main() {
    // MASK materials: alpha_cutoff > 0 arms the test; albedo.a carries the alpha. A constant
    // per-material alpha makes this all-or-nothing today; it becomes a real silhouette test
    // once a sampled texture alpha multiplies into material.albedo.a.
    if (material.alpha_cutoff > 0.0 && material.albedo.a < material.alpha_cutoff) discard;

    vec3 albedo   = material.albedo.rgb;
    float metallic  = material.metallic;
    float roughness = max(material.roughness, 0.045);
    float ao        = material.ao;

    vec3 N = normalize(frag_world_normal);

    out_albedo_ao          = vec4(albedo, ao);
    out_normal_metallic    = vec4(N, metallic);
    out_position_roughness = vec4(frag_world_pos, roughness);
}
