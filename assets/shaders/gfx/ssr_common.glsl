#ifndef GFX_SSR_COMMON_GLSL
#define GFX_SSR_COMMON_GLSL

// gfx/ssr_common.glsl -- geometry helpers shared by the SSR raymarch,
// SSAO, and composite passes.
//
// Declares no uniforms or samplers: includers bind the camera UBO at
// different set indices, so every input here is a function parameter (same
// rule as ibl.glsl).

/// World-space size of one full-resolution screen texel at a given view-space depth.
///
/// A perspective frustum is 2*|view_z| / proj[1][1] world units tall at depth view_z, spread
/// over screen_height texels. SSR distance constants are expressed as multiples of this
/// rather than as hand-tuned world-space numbers, so the tuning survives the camera dollying in or
/// a scene being authored at a different scale.
float ssr_texel_world_size(float view_z, float p11, float screen_height) {
    return 2.0 * abs(view_z) / (max(abs(p11), 1e-6) * max(screen_height, 1.0));
}

/// NDC -> G-buffer UV. The SSR chain overrides OffscreenTarget::begin()'s negative-height
/// viewport with a positive one (ssr_pass.h), while the G-buffer and the deferred-lit offscreen
/// target are rendered with the negative one, so the Y flip has to be folded in here.
vec2 ssr_ndc_to_uv(vec2 ndc_xy) {
    return vec2(ndc_xy.x * 0.5 + 0.5, -ndc_xy.y * 0.5 + 0.5);
}

/// Tangent of the GGX specular lobe's half-angle, used as the tracing cone's aperture.
///
/// Converts the GGX alpha to its Blinn-Phong-equivalent specular power, then fits the cone that
/// contains the bulk of the lobe's energy (Uludag, "Hi-Z Screen-Space Cone-Traced Reflections",
/// GPU Pro 5). The 0.244 is the energy fraction the fit is taken at. Evaluated once per pixel,
/// not per march step, so the pow() is not a concern.
float ssr_ggx_cone_tan(float roughness) {
    float alpha      = max(roughness * roughness, 1e-3);
    float spec_power = 2.0 / (alpha * alpha) - 2.0;
    float cos_theta  = pow(0.244, 1.0 / (spec_power + 1.0));
    return sqrt(max(1.0 - cos_theta * cos_theta, 0.0)) / max(cos_theta, 1e-4);
}

/// Interleaved gradient noise (Jimenez, "Next Generation Post Processing in Call of Duty:
/// Advanced Warfare") -- one scalar in [0, 1) from a pixel coordinate. Same method as
/// gfx/shadow_sampling.glsl's gfx_ign_angle(), duplicated locally rather than shared because
/// that one already multiplies by TAU for an angle and this needs the raw scalar.
float ssr_ign(vec2 px) {
    return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

/// Per-frame offset for ssr_ign()-based sampling, via the R2 low-discrepancy sequence
/// (Roberts, "The Unreasonable Effectiveness of Quasirandom Sequences", 2018):
/// fract(frame * (1/phi2, 1/phi2^2)) for the plastic number phi2. Scaled up to move the
/// sample by tens of pixels per frame -- a sub-texel shift would fall inside IGN's own
/// dithering and barely change which cell is sampled.
vec2 ssr_frame_shift(int frame) {
    return fract(vec2(0.7548776662, 0.5698402909) * float(frame & 0xFF)) * 97.0;
}

/// Two DECORRELATED values in [0, 1) for stochastic SSR ray sampling, re-seeded every frame.
///
/// The obvious "call ssr_ign() twice with transposed constants" pairing is equal on the
/// px.x == px.y diagonal and correlated everywhere else (the two dot products differ only in
/// which axis carries the larger weight) -- structured, not noisy. This instead evaluates
/// ssr_ign() at two pixel coordinates separated by a fixed, non-axis-aligned offset large
/// enough that the second falls in an unrelated IGN cell. Both coordinates carry the SAME
/// per-frame shift (ssr_frame_shift), so consecutive frames scatter the pair together over the
/// 2-D neighbourhood instead of sliding along one axis, which is what a shared scalar*frame
/// shift on a single coordinate would do.
vec2 ssr_ign2(vec2 px, int frame) {
    vec2 shifted = px + ssr_frame_shift(frame);
    return vec2(ssr_ign(shifted), ssr_ign(shifted + vec2(37.0, 17.0)));
}

/// GGX visible-normal sample (Heitz 2018, "Sampling the GGX Distribution of Visible Normals"),
/// in the tangent frame where +Z is the surface normal. `Ve` is the unit direction TOWARD the
/// viewer in that frame, `ax`/`ay` the GGX alphas, `u` two uniform numbers in [0,1). Returns the
/// sampled microfacet normal; reflecting the view about it gives a ray distributed like the
/// specular lobe the viewer actually sees (no backfacing or masked microfacets).
vec3 ssr_sample_ggx_vndf(vec3 Ve, float ax, float ay, vec2 u) {
    vec3 Vh = normalize(vec3(ax * Ve.x, ay * Ve.y, Ve.z));
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = lensq > 0.0 ? vec3(-Vh.y, Vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(Vh, T1);
    float r   = sqrt(u.x);
    float phi = 6.28318530718 * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s  = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(max(1.0 - t1 * t1, 0.0)) + s * t2;
    vec3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    return normalize(vec3(ax * Nh.x, ay * Nh.y, max(0.0, Nh.z)));
}

#endif // GFX_SSR_COMMON_GLSL
