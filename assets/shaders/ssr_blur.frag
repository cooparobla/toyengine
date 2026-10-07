#version 450

// SSR bilateral blur -- spatial denoise for the temporally-resolved SSR buffer, an
// edge-aware blur in the style of ssao_blur.frag (normal/plane-distance weighting sourced
// from the G-buffer), adapted to blur a vec4
// (premultiplied color + confidence, GfxSsrHit's own convention -- see gfx/ssr_trace_body.glsl)
// instead of a scalar AO value.
//
// Why this exists: SsrPass's raymarch (ssr.frag) produces a single sample per pixel per frame;
// temporal resolve blends that against history, but the reflection's confidence fades
// (roughness_fade/grazing_fade/dist_fade, plus the underlying Hi-Z hit/miss test itself) create
// a genuinely noisy hit/miss BOUNDARY near silhouettes and grazing angles that temporal
// accumulation alone never fully converges on -- especially under continuous camera motion,
// where the boundary itself keeps moving frame to frame. SsaoPass solved the analogous problem
// (single-sample raw AO noise) with exactly this kind of spatial blur; this is SSR's
// equivalent stage.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_ssr;

layout(set = 0, binding = 0) uniform sampler2D u_ssr;                 // resolved SSR (post temporal)
layout(set = 0, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;

layout(push_constant) uniform BlurPushConstants {
    float radius;  // ssr_blur_radius -- sigma for the plane-distance weight scales with it
    int   flags;   // bit 0: 3x3 footprint instead of 5x5; bit 1: write 0 where every tap is 0;
                   // bit 2: roughness-aware (specular chain)
} pc;

#define BLUR_FLAG_LIGHT           1
#define BLUR_FLAG_ZERO_SKIP       2
// Specular reflections are only as blurry as the surface is rough: a mirror's reflection must
// stay pixel-sharp, and a box blur there is pure loss. With this flag the blurred result is
// blended in by the centre's roughness (none below 0.05, full from 0.4), taps of a different
// roughness are down-weighted, and a tap whose hit/miss state differs from the centre's is
// skipped so a sharp reflection's edge is not smeared into a neighbouring miss.
#define BLUR_FLAG_ROUGHNESS_AWARE 4
const float kRoughnessSigma = 0.1;

void main() {
    // u_ssr is sampled at ITS OWN native resolution (texelFetch, pixel-exact -- this is the
    // buffer actually being blurred). g_normal_metallic/g_position_roughness are sampled by
    // NORMALIZED UV instead, not texelFetch: SsrPass's trace/resolve/blur chain can run at
    // HALF the G-buffer's resolution (SsrPass's half_res ctor param, driven by config
    // ssr_half_res). UV sampling scales proportionally regardless of the two textures'
    // relative sizes; a NEAREST sampler (bound by the caller, matching every other in-shader
    // G-buffer point-lookup in this engine -- see ssr.frag's own nearest_sampler_ doc) keeps
    // this exactly as point-sampled as texelFetch would be when the resolutions match.
    ivec2 size = textureSize(u_ssr, 0);
    ivec2 center_px = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);
    vec2  texel_uv = 1.0 / vec2(size);

    vec3 Nc = texture(g_normal_metallic, in_uv).rgb;
    vec4 center = texelFetch(u_ssr, center_px, 0);

    // Background: nothing to weight against (G2 holds no real surface here) -- pass through,
    // matching ssao_blur.frag's own early-out for the same case.
    if (dot(Nc, Nc) < 0.001) {
        out_ssr = center;
        return;
    }
    Nc = normalize(Nc);

    // Footprint half-width: 5x5 (the original) or, with BLUR_FLAG_LIGHT, 3x3 -- 9 taps with
    // 3 reads each instead of 25, the same plane-distance sigma, so the denoise just reaches
    // one texel less. The weight maths below is unchanged either way.
    int r = (pc.flags & BLUR_FLAG_LIGHT) != 0 ? 1 : 2;

    // Zero skip (SSR only, exact): where the resolved buffer is exactly zero across the whole
    // footprint -- rough dielectrics the trace skipped, pixels past the fades -- the weighted
    // sum below is 0 / wsum = 0 whatever the weights, since the centre tap alone contributes
    // wn * wd = 1 and keeps wsum above the 1e-5 floor. Every channel must be tested: the
    // temporal resolve clips colour and confidence independently, so alpha alone can be zero
    // while rgb is not. These fetches hit the same texels the loop would, from one texture,
    // against 2 scattered G-buffer reads per tap in the loop.
    if ((pc.flags & BLUR_FLAG_ZERO_SKIP) != 0) {
        bool all_zero = true;
        for (int y = -r; y <= r; ++y) {
            for (int x = -r; x <= r; ++x) {
                ivec2 tap_px = clamp(center_px + ivec2(x, y), ivec2(0), size - 1);
                if (any(notEqual(texelFetch(u_ssr, tap_px, 0), vec4(0.0)))) all_zero = false;
            }
        }
        if (all_zero) {
            out_ssr = vec4(0.0);
            return;
        }
    }

    vec4 pr_c = texture(g_position_roughness, in_uv);
    vec3 Pc   = pr_c.rgb;
    bool rough_aware = (pc.flags & BLUR_FLAG_ROUGHNESS_AWARE) != 0;
    float blur_amount = rough_aware ? smoothstep(0.05, 0.4, pr_c.a) : 1.0;
    if (blur_amount <= 0.0) {
        out_ssr = center;
        return;
    }
    bool center_hit = center.a > 0.0;

    // Plane-distance sigma scales with the blur radius -- same scale-relative philosophy as
    // ssao_blur.frag / ssr_common.glsl (world-space tuning survives the scene/camera being
    // authored at a different scale), rather than a fixed-in-world-units constant.
    float sigma = max(pc.radius * 0.5, 1e-4);

    // Symmetric footprint, identical to ssao_blur.frag's own -- see that file's doc for why
    // symmetric (not the old asymmetric box-blur footprint) is correct once the source is
    // already temporally denoised before this runs.
    vec4  sum  = vec4(0.0);
    float wsum = 0.0;
    for (int y = -r; y <= r; ++y) {
        for (int x = -r; x <= r; ++x) {
            vec2  tap_uv = clamp(in_uv + vec2(x, y) * texel_uv, vec2(0.0), vec2(1.0));
            ivec2 tap_px = clamp(center_px + ivec2(x, y), ivec2(0), size - 1);

            vec3 Nt_raw = texture(g_normal_metallic, tap_uv).rgb;
            bool tap_is_background = dot(Nt_raw, Nt_raw) < 0.001;

            // A background tap votes for the CENTER's own value instead of being excluded from
            // sum/wsum outright. Falling back to Nt=Nc/Pt=Pc makes wn=wd=1 (full weight) --
            // effectively "this tap agrees with the center" -- rather than the previous
            // continue-based skip, which changed the SET of contributing taps in discrete,
            // integer steps as an object's silhouette swept the 5x5 footprint during camera
            // rotation, and jumped the output every time a tap crossed that boundary. The
            // fallback keeps wsum's shape constant regardless of how many taps are background;
            // the only thing that varies continuously across a moving silhouette is each real
            // tap's own Pt (via wd), which is already smooth.
            vec3 Nt = tap_is_background ? Nc : normalize(Nt_raw);
            vec4 pr_t = tap_is_background ? pr_c : texture(g_position_roughness, tap_uv);
            vec3 Pt = pr_t.rgb;
            vec4 tap_val = tap_is_background ? center : texelFetch(u_ssr, tap_px, 0);

            float wn = pow(max(dot(Nc, Nt), 0.0), 16.0);
            float d  = dot(Nc, Pt - Pc);
            float wd = exp(-(d * d) / (2.0 * sigma * sigma));
            float w  = wn * wd;
            if (rough_aware) {
                float dr = pr_t.a - pr_c.a;
                w *= exp(-(dr * dr) / (2.0 * kRoughnessSigma * kRoughnessSigma));
                if ((tap_val.a > 0.0) != center_hit) w = 0.0;
            }

            sum  += tap_val * w;
            wsum += w;
        }
    }

    vec4 blurred = (wsum > 1e-5) ? (sum / wsum) : center;
    out_ssr = mix(center, blurred, blur_amount);
}
