#version 450

#include "ssr_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_ao;

// Set 0: Camera UBO (reused from the rest of the deferred pipeline, see camera_ubo.h)
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: G-Buffer normal/position + noise texture + hemisphere kernel
layout(set = 1, binding = 0) uniform sampler2D g_normal_metallic;
layout(set = 1, binding = 1) uniform sampler2D g_position_roughness;
layout(set = 1, binding = 2) uniform sampler2D u_noise;
layout(set = 1, binding = 3) uniform KernelUBO {
    vec4 samples[32];
} kernel;

layout(push_constant) uniform SsaoPushConstants {
    float radius;
    float bias;
    float power;
    int   kernel_size;
    // screen_size / 4.0 as two floats, not vec2 -- keeps this block plain scalars/ints like
    // ssr.frag's SsrPushConstants, so there is no vec2 base-alignment (8-byte) padding for the
    // C++ struct in ssao_pass.h to silently get wrong.
    float noise_scale_x;
    float noise_scale_y;
    // 0 when temporal resolve is disabled (or absent). When it's on, the caller advances this
    // every frame so the noise-tile rotation decorrelates consecutive frames' raw AO estimate --
    // otherwise the screen-locked 4x4 noise tile makes the raw pass nearly frame-invariant, and
    // ssao_resolve.frag's temporal accumulation has almost nothing left to average.
    int   noise_rotation;
} pc;

void main() {
    ivec2 gsize = textureSize(g_position_roughness, 0);
    ivec2 px    = clamp(ivec2(in_uv * vec2(gsize)), ivec2(0), gsize - 1);

    // Background pixels write N = vec3(0) (gbuffer.frag never touches unwritten texels beyond
    // the clear value) -- must check before normalize() to avoid propagating a NaN, the same
    // guard ssr.frag uses at its own early-out.
    vec3 N = texelFetch(g_normal_metallic, px, 0).rgb;
    if (dot(N, N) < 0.001) {
        out_ao = 1.0;
        return;
    }
    N = normalize(N);
    vec3 P = texelFetch(g_position_roughness, px, 0).rgb;

    vec2 noise_scale = vec2(pc.noise_scale_x, pc.noise_scale_y);
    vec2 n2 = texture(u_noise, in_uv * noise_scale).rg * 2.0 - 1.0;

    // Rotate by a multiple of the golden angle -- pc.noise_rotation is 0 (identity rotation)
    // whenever temporal resolve is off, so non-temporal configs (smaa/fxaa/off, or
    // ssao_temporal_enabled: false) see byte-identical behaviour to before this was added.
    if (pc.noise_rotation != 0) {
        float ang = float(pc.noise_rotation) * 2.39996323;
        float cs  = cos(ang);
        float sn  = sin(ang);
        n2 = vec2(n2.x * cs - n2.y * sn, n2.x * sn + n2.y * cs);
    }

    vec3 rand_vec = normalize(vec3(n2, 0.0));
    vec3 T = normalize(rand_vec - N * dot(rand_vec, N));
    vec3 B = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float occlusion = 0.0;
    for (int i = 0; i < pc.kernel_size; ++i) {
        vec3 sample_pos = P + (TBN * kernel.samples[i].xyz) * pc.radius;

        vec4 clip = camera.proj * camera.view * vec4(sample_pos, 1.0);
        if (clip.w <= 0.0) continue;
        vec2 uv = ssr_ndc_to_uv(clip.xy / clip.w);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) continue;

        // texelFetch, not a filtered texture() lookup -- g_position_roughness is bound with a
        // linear sampler elsewhere in the pipeline (see DeferredLightingPass::set_gbuffer_images),
        // and bilinearly blending world-space positions across a depth discontinuity produces a
        // garbage in-between point. Same reasoning as the explicit-texel snap in ssr.frag:46-51.
        ivec2 scene_px = clamp(ivec2(uv * vec2(gsize)), ivec2(0), gsize - 1);
        vec3 scene_P   = texelFetch(g_position_roughness, scene_px, 0).rgb;

        float sample_vz = (camera.view * vec4(sample_pos, 1.0)).z;
        float scene_vz  = (camera.view * vec4(scene_P,    1.0)).z;

        // Both view_z values are negative (RH, camera looks down -Z), so "scene surface is in
        // front of the sample point" means scene_vz > sample_vz (less negative == closer).
        float range_check = smoothstep(0.0, 1.0, pc.radius / max(abs(sample_vz - scene_vz), 1e-4));
        occlusion += (scene_vz >= sample_vz + pc.bias ? 1.0 : 0.0) * range_check;
    }

    float ao = 1.0 - occlusion / float(pc.kernel_size);
    out_ao = pow(clamp(ao, 0.0, 1.0), pc.power);
}
