// ssr_common.glsl -- geometry helpers shared by the SSR raymarch and composite passes.
//
// Declares no uniforms or samplers: ssr.frag and ssr_composite.frag bind the camera UBO at
// different set indices, so every input here is a function parameter (same rule as ibl.glsl).

/// World-space size of one full-resolution screen texel at a given view-space depth.
///
/// A perspective frustum is 2*|view_z| / proj[1][1] world units tall at depth view_z, spread
/// over screen_height texels. Every SSR constant that used to be a hand-tuned world-space
/// number is expressed as a multiple of this, so the tuning survives the camera dollying in or
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
