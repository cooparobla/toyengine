#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_ao;

layout(set = 0, binding = 0) uniform sampler2D u_ao;                  // resolved AO (post temporal)
layout(set = 0, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;

layout(push_constant) uniform BlurPushConstants {
    float radius;  // ssao_radius -- sigma for the plane-distance weight scales with it
} pc;

void main() {
    ivec2 size = textureSize(u_ao, 0);
    ivec2 center_px = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);

    vec3 Nc = texelFetch(g_normal_metallic, center_px, 0).rgb;
    float center = texelFetch(u_ao, center_px, 0).r;

    // Background: nothing to weight against (G2 holds no real surface here) -- pass through,
    // matching ssao.frag's own early-out for the same case.
    if (dot(Nc, Nc) < 0.001) {
        out_ao = center;
        return;
    }
    Nc = normalize(Nc);
    vec3 Pc = texelFetch(g_position_roughness, center_px, 0).rgb;

    // Plane-distance sigma scales with the AO sample radius -- consistent with the
    // scale-relative philosophy in ssr_common.glsl (world-space tuning survives the scene/camera
    // being authored at a different scale), rather than a fixed-in-world-units constant.
    float sigma = max(pc.radius * 0.5, 1e-4);

    // Symmetric 5x5 footprint. The old 4x4 box blur used an asymmetric -2..+1 footprint to
    // exactly cancel a screen-locked 4x4 noise tile; with the raw pass's noise now rotated per
    // frame (ssao.frag's noise_rotation) and denoised temporally before this runs
    // (ssao_resolve.frag), that reasoning no longer applies, and the asymmetry was introducing a
    // half-texel bias against silhouettes.
    float sum  = 0.0;
    float wsum = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            ivec2 tap_px = clamp(center_px + ivec2(x, y), ivec2(0), size - 1);

            vec3 Nt = texelFetch(g_normal_metallic, tap_px, 0).rgb;
            if (dot(Nt, Nt) < 0.001) continue; // background tap -- skip, don't drag AO toward 1.0

            Nt = normalize(Nt);
            vec3 Pt = texelFetch(g_position_roughness, tap_px, 0).rgb;

            float wn = pow(max(dot(Nc, Nt), 0.0), 16.0);
            float d  = dot(Nc, Pt - Pc);
            float wd = exp(-(d * d) / (2.0 * sigma * sigma));
            float w  = wn * wd;

            sum  += texelFetch(u_ao, tap_px, 0).r * w;
            wsum += w;
        }
    }

    out_ao = (wsum > 1e-5) ? (sum / wsum) : center;
}
