#version 450

// Deferred lighting for the pixel-art pipeline. Direct lighting has two
// looks, picked by soft_lighting -- see shade_light():
//   - Ramped (soft_lighting == 0, this engine's default): N.L is quantized
//     into discrete steps (band(), gated by light_bands) and specular is a
//     hard step()-masked highlight (gated by spec_threshold), for the
//     cel-shaded look.
//   - Soft (soft_lighting != 0): N.L falls off continuously (band() is
//     bypassed) and specular is a standard Cook-Torrance
//     NDF*G*F/(4*NdotV*NdotL) term, same shape as blendy's smooth PBR.
// Everything else is independently toggleable on top of whichever direct-
// lighting look is active:
//   - g_ssao: always bound (to SsaoPass::output_view() or its neutral
//     fully-unoccluded texture, decided once at pipeline construction from
//     ssao_enabled -- see PixelRenderPipeline), so ambient occlusion is
//     just another multiplier here regardless of the toggle.
//   - indirect ambient: sky-gradient-based diffuse + specular (see
//     gfx/sky.glsl, gfx/brdf.glsl) rather than a flat ambient constant, so
//     ssr.frag/ssr_composite.frag have a well-defined indirect specular
//     term to swap reflections into when ssr_enabled is set -- and a
//     nicer-looking ambient fallback when it isn't. The indirect-specular
//     expression itself is shared with the composite via
//     gfx/indirect_specular.glsl (indirect_hooks.glsl provides this
//     engine's hooks) rather than duplicated, so the two structurally
//     cannot drift apart.
//
// rim_strength stays independent of soft_lighting in both modes (0 disables
// it either way) -- it's a common accent in both cel-shaded and painterly
// soft-shaded styles, not exclusively a cel-shading technique.

#include <gfx/sky.glsl>
#include <gfx/brdf.glsl>
#include <gfx/shadow_sampling.glsl>
#include <gfx/indirect_specular.glsl>
#include <gfx/ao_composite.glsl>
#include <gfx/spot_light.glsl>
// Pure-function header (declares no uniforms or samplers), for the contact-shadow
// march below: ssr_texel_world_size() sizes its bias to a screen texel, and
// ssr_ndc_to_uv() is the same NDC->G-buffer mapping the SSR trace uses.
#include <gfx/ssr_common.glsl>

layout(location = 0) in vec2 in_uv;

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: Light UBO -- identical layout to gfxcoopa's LightUBO (light_data.h)
struct PointLight {
    vec4 position_range;  // xyz = pos, w = range
    vec4 color_intensity; // xyz = color, w = intensity
    vec4 attenuation;     // x=const, y=lin, z=quad, w=cast_shadows (1 or 0)
};

layout(set = 1, binding = 0) uniform LightUBO {
    vec4 dir_direction;
    vec4 dir_color;
    vec4 dir_shadow_extra; // x=shadow_intensity, y=point_pcf_radius, z=pcf_samples, w=frame_offset -- see LightUBO's C++ doc (light_data.h)
    mat4 dir_light_space_matrix;
    vec4 dir_shadow_params; // x=bias, y=pcf_radius_texels (0=hard), z=shadow_enabled, w=normal_bias

    uvec4 light_counts; // x=num_dir, y=num_point
    PointLight point_lights[16];

    // Configurable sky/ambient colour (see IndirectParams in render_features.h).
    // Trailing so no field above moves -- std140 only requires a matching prefix.
    vec4 sky_zenith;
    vec4 sky_horizon;
    vec4 sky_ground;

    // Spot Lights -- appended after sky_ground; see light_data.h's LightUBO doc on why
    // nothing above this line may move.
    mat4 spot_light_space_matrix;
    vec4 spot_shadow_params; // x=bias, y=penumbra scale K, texels*distance (0=hard; see calc_spot_shadow), z=shadow_enabled, w=normal_bias
    SpotLight spot_lights[8];

    // Appended after spot_lights per light_data.h's append-only rule.
    vec4 pcss_params;    // x=enabled, y=penumbra texels per unit depth gap,
                         // z=blocker search radius texels (see calc_dir_shadow)
    vec4 contact_params; // x=strength (0 disables), y=length m, z=thickness m,
                         // w=steps -- read only by pixel_lighting.frag's contact march
    vec4 contact_soft_params; // x=cone half-angle tangent (sun angular size, soft_shadows
                         // on) -- 0 = hard single-ray march. y/z/w reserved.
} lights;

// Set 2: Shadow maps -- toyengine has exactly one directional map, one point
// cube map, and one spot map (see shadow_map_target.h), unlike blendy's four
// cube slots.
// *Shadow sampler types: the bound VkSampler has hardware compareEnable
// (util::Sampler::shadow()), so the GPU compares depth before it filters --
// see gfx/shadow_sampling.glsl's *Shadow-family doc for why that matters.
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
layout(set = 2, binding = 1) uniform samplerCubeShadow point_shadow_map;
layout(set = 2, binding = 2) uniform sampler2DShadow spot_shadow_map;
// The directional map AGAIN, through a plain nearest sampler: PCSS's blocker
// search needs stored depths, which a compare sampler cannot return.
layout(set = 2, binding = 3) uniform sampler2D dir_shadow_map_raw;

// Set 3: G-Buffer textures + screen-space AO
layout(set = 3, binding = 0) uniform sampler2D g_albedo_ao;          // RGB = Albedo, A = AO
layout(set = 3, binding = 1) uniform sampler2D g_normal_metallic;    // RGB = World Normal, A = Metallic
layout(set = 3, binding = 2) uniform sampler2D g_position_roughness; // RGB = World Pos, A = Roughness
layout(set = 3, binding = 3) uniform sampler2D g_ssao;                // R = SsaoPass output (or its neutral 1.0 texture)
layout(set = 3, binding = 4) uniform sampler2D g_emissive;            // RGB = emissive radiance (HDR)

layout(push_constant) uniform PixelParams {
    float light_bands;       // discrete N.L shading steps, e.g. 4.0; used when soft_lighting is off
    float spec_threshold;    // hard specular highlight cutoff; used when soft_lighting is off
    float rim_strength;      // 0 disables the rim term
    float ambient_intensity; // scales sky_gradient(N) indirect diffuse
    float sky_intensity;     // scales sky_gradient(reflect(-V,N)) indirect specular base
    float soft_lighting;     // != 0 -> smooth Cook-Torrance direct lighting; 0 -> banded/ramped cel look (default)
    float ssao_direct_strength; // how much occlusion darkens DIRECT lighting (HDRP's
                                // Direct Lighting Strength): 0 = indirect only
} params;

layout(location = 0) out vec4 out_color;

#include "indirect_hooks.glsl"
#include "pixel_shadow_body.glsl"

// Quantizes N.L into `params.light_bands` discrete steps -- the core of the
// cel-shaded look. Bypassed entirely when soft_lighting is on (smooth N.L
// falloff), or when bands <= 1 (matching a config value of 0 or 1 being a
// sensible "off" default even in ramped mode).
float band(float ndl) {
    if (params.soft_lighting != 0.0) return ndl;
    if (params.light_bands <= 1.0) return ndl;
    return floor(ndl * params.light_bands) / params.light_bands;
}

// Direct lighting for one light, multiplied by (1 - shadow). Two looks --
// see the file doc and band() above: ramped (default) bands the diffuse
// N.L and hard-masks the specular into a toon highlight; soft
// (soft_lighting != 0) uses standard continuous Cook-Torrance for both.
vec3 shade_light(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic,
                 float roughness, vec3 F0, float shadow) {
    vec3 H = normalize(V + L);
    float ndl_raw = max(dot(N, L), 0.0);
    if (ndl_raw <= 0.0) return vec3(0.0);
    float ndl = band(ndl_raw);

    float NDF = distribution_ggx(N, H, roughness);
    float G   = geometry_smith(N, V, L, roughness);
    vec3  F   = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 specular;
    if (params.soft_lighting != 0.0) {
        float denom = 4.0 * max(dot(N, V), 0.0) * ndl_raw + 0.0001;
        specular = (NDF * G * F) / denom;
    } else {
        float spec_mask = step(params.spec_threshold, NDF * G);
        specular = F * spec_mask;
    }

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / BRDF_PI + specular) * radiance * ndl * (1.0 - shadow);
}

void main() {
    vec4 g0 = texture(g_albedo_ao, in_uv);
    vec4 g1 = texture(g_normal_metallic, in_uv);
    vec4 g2 = texture(g_position_roughness, in_uv);
    // 1.0 (fully unoccluded) whenever SSAO is disabled -- PixelRenderPipeline binds
    // SsaoPass::neutral_view() in that case, so this read needs no separate flag.
    float ssao = texture(g_ssao, in_uv).r;
    vec3 emissive = texture(g_emissive, in_uv).rgb;

    vec3 N = g1.rgb;
    if (dot(N, N) < 0.001) {
        // Background pixel -- left blank for the skybox pass to fill in
        // afterward, in the same open render pass (see PixelRenderPipeline).
        discard;
    }
    N = normalize(N);

    vec3  albedo    = g0.rgb;
    float ao        = g0.a;
    float metallic  = g1.a;
    vec3  world_pos = g2.rgb;
    float roughness = g2.a;

    vec3 V = normalize(camera.camera_pos - world_pos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    if (lights.light_counts.x > 0) {
        vec3 L = normalize(-lights.dir_direction.xyz);
        vec3 radiance = lights.dir_color.rgb * lights.dir_direction.w;

        float normal_bias_scale = clamp(1.0 - dot(N, L), 0.0, 1.0);
        vec3 biased_pos = world_pos + N * (lights.dir_shadow_params.w * (0.5 + 0.5 * normal_bias_scale));
        vec4 light_space_pos = lights.dir_light_space_matrix * vec4(biased_pos, 1.0);
        float shadow = calc_dir_shadow(light_space_pos, N, L);

        // Screen-space contact shadows (HDRP's own feature of that name): a short
        // G-buffer march from the surface toward the light, catching the
        // small-scale occlusion the shadow map's normal-offset bias necessarily
        // recedes from (see PixelRenderConfig::shadow_normal_bias's ~2% trade).
        // max()-combined with the map's result so each technique only ever ADDS
        // occlusion the other missed; scaled by the same per-light darkness
        // calc_dir_shadow applies (dir_shadow_extra.x). Independent of the shadow
        // map: with shadows_enabled false calc_dir_shadow returns 0 and the march
        // becomes the only directional occlusion term, so shadows_enabled: false +
        // contact_shadows_enabled: true renders a contact-only view.
        //
        // The estimator is deliberately SMOOTH and COMB-INDEPENDENT: every fetched
        // texel's occlusion is an analytic function of that texel and the ray
        // geometry alone (evaluated over the whole ray, at the texel's nearest
        // approach to the ray), so WHERE the samples land only decides which texels
        // are visited -- with nothing downstream to average per-pixel estimator
        // disagreement (unlike the traced SSGI, which has its own denoise), any
        // sample-position dependence in the VALUE reads as salt-and-pepper grain
        // over every penumbra. The strongest texel wins, and soft fades take the
        // result to zero wherever the march is least trustworthy.
        if (lights.contact_params.x > 0.0 && shadow < lights.dir_shadow_extra.x) {
            float ndl = dot(N, L);
            // Grazing fade: as N.L -> 0 the ray travels nearly parallel to the surface
            // it started from, so every depth comparison is against that same surface.
            float graze = smoothstep(0.0, 0.15, ndl);
            if (graze > 0.0) {
                ivec2 gsize    = textureSize(g_position_roughness, 0);

                // Jitter-stable march origin. world_pos is the surface sampled at THIS
                // frame's TAA sub-pixel offset, and on a grazing receiver one screen
                // texel spans px_world/N.V world units -- so the raw origin slides a
                // large distance ALONG the surface every frame, translating the whole
                // march onto different occluder texels and flickering the contact line
                // (worst on close, grazing geometry, where the march spans the most
                // texels). The perspective jitter is recoverable from the projection
                // (apply_taa_jitter_ adds it to proj[2][0]/[2][1], where perspectiveRH_ZO's
                // own terms are zero; the NDC displacement is their negation), and the
                // screen-space derivatives of world_pos are the surface's world footprint
                // per pixel -- together they slide the origin back to the point under the
                // texel CENTRE, the same world point every frame on a planar receiver.
                // This is the origin half of "march in the unjittered frame"; proj_march
                // below is the projection half.
                float px_raw = ssr_texel_world_size(
                    (camera.view * vec4(world_pos, 1.0)).z, abs(camera.proj[1][1]),
                    float(gsize.y));
                vec3 origin = world_pos;
                if (camera.proj[2][3] != 0.0) {
                    vec3 dpdx = dFdx(world_pos);
                    vec3 dpdy = dFdy(world_pos);
                    vec2 jitter_ndc = vec2(-camera.proj[2][0], -camera.proj[2][1]);
                    // ndc-per-pixel: +x pixel = +2/W ndc x; +y pixel (down) =
                    // -2/H ndc y (ssr_ndc_to_uv's flip) -- hence the minus.
                    vec3 corr = dpdx * (0.5 * float(gsize.x) * jitter_ndc.x)
                              - dpdy * (0.5 * float(gsize.y) * jitter_ndc.y);
                    // In-plane only, and capped: where the derivative quad straddles a
                    // depth edge the "footprint" is garbage metres of cross-surface
                    // distance, not slope.
                    corr -= N * dot(N, corr);
                    float cap = px_raw * 4.0 / max(abs(dot(N, V)), 0.05);
                    if (dot(corr, corr) < cap * cap) origin += corr;
                }

                // px_world (hence bias/ray_len/depth_eps) from the CORRECTED origin's
                // view-z, so the march LENGTH is also stable frame to frame -- otherwise
                // ray_len = px_world*64 tracks the jittered depth and its far-distance
                // fade wobbles on close geometry.
                float view_z   = (camera.view * vec4(origin, 1.0)).z;
                float px_world = ssr_texel_world_size(view_z, abs(camera.proj[1][1]),
                                                      float(gsize.y));

                // Start offset sized to the receiver's own screen texel, so the first
                // sample always clears the surface it came from. Divided by N.L because a
                // grazing ray must travel further to gain the same height off its surface,
                // and floored for the near plane where px_world -> 0 (the same floor, for
                // the same reason, as gfx/ssr_trace_body.glsl's).
                float bias = max(px_world * 2.0 / max(ndl, 0.15), 0.002);

                // Clamp the march's SCREEN extent. A fixed world length spans hundreds of
                // texels up close (so the samples straddle them and step over thin
                // occluders) and less than one at range (so every sample lands in the same
                // texel and the whole march is wasted). 64 px is the same order as SSAO's
                // own ssao_max_radius_px cap.
                float ray_len = min(lights.contact_params.y, px_world * 64.0);

                int   steps      = int(max(lights.contact_params.w, 1.0));
                float thickness  = max(lights.contact_params.z, 1e-4);
                float depth_eps  = max(px_world, 0.002);  // depth-precision floor

                // The ray point is stepped in VIEW space for the projection (one mat4
                // multiply per sample) and in WORLD space for the occlusion test below,
                // which measures against the fetched surface's world-space plane.
                vec3 view_o = (camera.view * vec4(origin, 1.0)).xyz;

                // Project the march with the UNJITTERED projection so the sample
                // fetch path does not slide with TAA's sub-pixel offset every frame.
                // apply_taa_jitter_ adds the perspective jitter to proj[2][0]/[2][1],
                // where perspectiveRH_ZO's own terms are zero, so zeroing them
                // recovers the clean projection.
                mat4 proj_march = camera.proj;
                if (camera.proj[2][3] != 0.0) {
                    proj_march[2][0] = 0.0;
                    proj_march[2][1] = 0.0;
                }

                // Per-pixel, per-frame PHASE for the sample comb: interleaved gradient
                // noise, staggered spatially so neighbouring pixels visit different
                // texels (a G-buffer seam flip then never moves a whole coherent block
                // of TAA's variance box at once), and rotated each frame by the golden
                // ratio -- the same recipe as pixel_shadow_body.glsl's PCF rotation.
                // The rotation lets TAA integrate the sampling pattern to a smooth
                // fixed point rather than leaving a screen-locked comb; a still camera
                // still converges because the per-texel occlusion VALUE is comb-
                // independent (whole-ray, below) -- the comb only decides which texels
                // are visited, and TAA averages that choice out.
                float phase = fract(fract(52.9829189 *
                                          fract(0.06711056 * gl_FragCoord.x +
                                                0.00583715 * gl_FragCoord.y))
                                    + lights.dir_shadow_extra.w * 0.61803398875);

                // Soft contact shadows: AVERAGE occlusion over K rays spread across the
                // sun's angular cone (half-angle atan(cone_tan), cone_tan = the sun's
                // angular size, shared with the shadow map's PCSS growth dial). Each ray
                // is the same short march, in a slightly rotated direction; the cone
                // widens with distance, so a blocker touching the receiver stays sharp
                // (all rays hit) while one seen far along the ray blocks only part of the
                // cone (a penumbra that grows with blocker distance -- the PCSS relation).
                // Averaging IS what a soft shadow is, and it is far steadier frame to
                // frame than the single-ray max. cone_tan == 0 (soft_shadows off)
                // collapses to one tap with no offset: the hard march, unchanged.
                // The march is short (<=contact_shadow_length, ~0.5 m), so the sun's true
                // angular penumbra over it is sub-centimetre -- physically correct but
                // invisible. Contact shadows are a stylized near-field effect; scale the
                // effective cone so the outer edge reads as soft at the default sun size,
                // still proportional to shadow_pcss_light_size so the dial keeps working.
                const float kContactPenumbraScale = 6.0;
                float cone_tan  = lights.contact_soft_params.x * kContactPenumbraScale;
                int   cone_taps = cone_tan > 0.0 ? 4 : 1;

                // Orthonormal basis spanning the plane perpendicular to the sun.
                vec3 up     = abs(L.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
                vec3 cone_t = normalize(cross(up, L));
                vec3 cone_b = cross(L, cone_t);
                // Per-pixel STATIC rotation of the Vogel disk (no per-frame term: the cone
                // must add no new temporal variance; the along-ray step comb already
                // carries the per-frame stagger TAA integrates).
                float cone_rot = 6.28318530 * fract(52.9829189 *
                                    fract(0.11267 * gl_FragCoord.x +
                                          0.17383 * gl_FragCoord.y));

                float occ = 0.0;
                for (int c = 0; c < cone_taps; ++c) {
                    // Vogel-disk direction for this tap, rotated per pixel. u is the
                    // sqrt-spaced disk radius (equal-area), scaled by cone_tan into a
                    // small tilt of the sun direction.
                    float cu   = cone_tan > 0.0
                               ? sqrt((float(c) + 0.5) / float(cone_taps)) : 0.0;
                    float cang = cone_rot + float(c) * 2.39996323;
                    vec3  Lc   = normalize(L + (cone_t * (cu * cos(cang)) +
                                                cone_b * (cu * sin(cang))) * cone_tan);
                    vec3  view_Lc = mat3(camera.view) * Lc;

                    float tap = 0.0;
                    for (int s = 0; s < steps; ++s) {
                    // Quadratic spacing concentrates samples near the contact point --
                    // where this effect lives -- and spends least on the far end, where
                    // screen-space data is least reliable. The samples only decide
                    // WHICH texels the march visits: each visited texel's occlusion is
                    // evaluated analytically over the whole ray below, so the comb's
                    // phase does not enter the per-texel value.
                    float f = (float(s) + phase) / float(steps);
                    float t = ray_len * f * f;
                    vec3  p_view  = view_o + view_Lc * (t + bias);

                    vec4 clip = proj_march * vec4(p_view, 1.0);
                    if (clip.w <= 0.0) break;
                    vec2 uv = ssr_ndc_to_uv(clip.xy / clip.w);

                    // texelFetch, NOT texture(): this set binds the G-buffer through a
                    // LINEAR sampler (harmless for the lighting pass's own texel-centred
                    // reads, where linear == nearest), but a march samples at arbitrary
                    // UVs, and there bilinear filtering blends world positions ACROSS
                    // silhouette edges into positions that exist on no real surface.
                    // The G-buffer is discrete per-pixel data; fetch texels. (SsrPass
                    // keeps a whole separate nearest sampler for exactly this reason.)
                    //
                    // The occlusion RESULT, however, IS blended -- a normalized 3x3
                    // tent (radius 1.5 texels) around the sample point. Where two
                    // surfaces meet on screen (a wall-floor corner, a silhouette),
                    // seam texels' rasterized identity flips between the two every
                    // frame under TAA's sub-pixel jitter -- one plane registers a
                    // hit, the other a miss, and no per-plane test can stabilize
                    // that pop. Running the plane test PER TEXEL and blending the
                    // scalar hits spatially antialiases the estimate, and the tent's
                    // support caps any single flipping texel's weight well below 1,
                    // so a seam flip moves the estimate by a fraction instead of
                    // toggling it.
                    vec2  st = uv * vec2(gsize) - 0.5;
                    ivec2 p00 = ivec2(round(st));
                    float hit = 0.0;
                    float wsum = 0.0;
                    for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        ivec2 px = clamp(p00 + ivec2(i, j), ivec2(0), gsize - 1);
                        vec2  d  = vec2(p00 + ivec2(i, j)) - st;
                        float w = max(0.0, 1.5 - abs(d.x)) * max(0.0, 1.5 - abs(d.y));
                        if (w <= 0.0) continue;
                        vec3 vis_pos = texelFetch(g_position_roughness, px, 0).rgb;
                        if (dot(vis_pos, vis_pos) < 1e-6) continue;  // sky: no occluder there
                        vec3 vis_n = texelFetch(g_normal_metallic, px, 0).rgb;
                        if (dot(vis_n, vis_n) < 0.5) continue;  // unwritten texel: no plane to test

                        // Signed distance of the ray point BEHIND the visible surface's
                        // PLANE, in world units along its normal -- not a raw view-z
                        // difference. A z compare is blind to slope: where the camera
                        // grazes a surface, one screen texel of it spans px_world/N.V
                        // world units, and neighbouring texels' stored depths differ by
                        // more than any fixed tolerance, so the march false-hits the
                        // receiver's own surface. Against the plane, every texel of one
                        // flat surface measures the same ~0 for a ray point on that
                        // surface, whatever the view angle.
                        vec3  vis_nn = normalize(vis_n);

                        // Signed distance of the ray point BEHIND this texel's plane is
                        // LINEAR in t: ahead(t) = A0 - (t + bias) * k. That makes the
                        // best (most-occluding) point over the WHOLE ray analytic --
                        // clamp the acceptance window's centre into the ray's
                        // ahead-range and evaluate there. Whole-ray, not per sample
                        // interval: with interval bounds in this expression a texel's
                        // contribution depends on WHICH sample interval evaluates it,
                        // i.e. on the comb phase -- and near the contact point the
                        // comb's step spacing is coarser than the acceptance ramps, so
                        // the estimate swings with the per-pixel phase and the
                        // penumbra renders as checkerboard grain. Evaluated over
                        // [0, ray_len], the value is a function of the fetched texel
                        // and the ray geometry alone; the samples only decide which
                        // texels are visited, and revisiting one is idempotent under
                        // the max below.
                        float A0 = dot(vis_nn, vis_pos - origin);
                        float k  = dot(vis_nn, Lc);
                        float a_start = A0 - bias * k;
                        float a_end   = A0 - (ray_len + bias) * k;
                        float a_min = min(a_start, a_end);
                        float a_max = max(a_start, a_end);
                        float ahead = clamp(0.5 * (depth_eps + thickness * 1.5), a_min, a_max);

                        // Locality: `ahead` measures distance behind the texel's PLANE,
                        // and a plane is infinite -- on gridded terrain every distant
                        // wall's extended plane slices through open floor, and a ray
                        // whose screen path crosses that wall's texels would take a
                        // phantom soft hit meters away from any real occluder (the
                        // mid-floor blotch field). A real occluder's fetched surface
                        // POINT lies within the thickness scale of the RAY, so gate on
                        // the point's distance to the ray at its nearest approach.
                        // That nearest-approach parameter is analytic in the fetched
                        // point alone -- NOT a function of which sample interval is
                        // being evaluated. Evaluating this distance at a per-sample t
                        // ties the prox ramp to the sample comb, and the comb's step
                        // spacing near the contact point is coarser than the ramp
                        // itself -- the estimate then swings with the comb's phase,
                        // which renders as checkerboard grain across every penumbra.
                        float t_near = clamp(dot(vis_pos - origin, Lc), 0.0,
                                             ray_len + bias);
                        vec3  dvec = vis_pos - (origin + Lc * t_near);
                        float prox = 1.0 - smoothstep(2.0 * thickness, 3.0 * thickness,
                                                      length(dvec));

                        // Elevation gate: a blocker can only shade the receiver if it
                        // STANDS ABOVE the receiver's tangent plane -- the sun ray climbs
                        // away from that plane, so anything at or below it (the receiver's
                        // own micro-relief on a displaced terrain top, or a pit wall seen
                        // past a convex edge) can never be between the surface and the sun.
                        // The plane/prox pair is blind to this: bumpy same-surface texels
                        // and below-edge walls both pass it, and those marginal hits are
                        // per-pixel bistable -- they render as salt-and-pepper stipple over
                        // whole faces and as isolated full-occlusion (sky-ambient blue)
                        // speckles at convex corners. Ramp over [0.5, 1.5] x thickness of
                        // elevation so real occluders (walls, steps -- their fetched texels
                        // climb well above the receiver) keep a smooth penumbra while
                        // relief on the thickness scale itself stays inert.
                        float elev = dot(N, vis_pos - origin);
                        float standing = smoothstep(0.5 * thickness, 1.5 * thickness, elev);

                        // Soft acceptance: occlusion ramps in once the ray is meaningfully
                        // behind the visible surface, and back out again as the gap exceeds
                        // the assumed occluder thickness (beyond that the "occluder" is just
                        // distant background that happens to be in the way on screen). A hard
                        // in/out test here is the other half of what made this grainy.
                        wsum += w;
                        hit += w * prox * standing
                                 * smoothstep(depth_eps, depth_eps + thickness * 0.5, ahead)
                                 * (1.0 - smoothstep(thickness, thickness * 2.0, ahead));
                    }
                    if (wsum > 0.0) hit /= wsum;

                    // Fade with distance along the ray, and toward the screen edge where
                    // the sample may have left the G-buffer entirely -- the same edge ramp
                    // gfx/ssr_trace_body.glsl uses, so leaving the screen fades rather
                    // than cutting off at a hard seam.
                    vec2 e = smoothstep(vec2(0.0), vec2(0.08), uv)
                           * smoothstep(vec2(1.0), vec2(0.92), uv);
                    hit *= (1.0 - smoothstep(0.6, 1.0, f)) * e.x * e.y;

                    // MAX along this ray: one occluder must not darken more for being
                    // sampled several times as the march steps past it.
                    tap = max(tap, hit);
                    }

                    // AVERAGE across the cone: the fraction of the sun's disk this
                    // receiver point cannot see is the soft-shadow coverage.
                    occ += tap;
                }
                occ /= float(cone_taps);

                shadow = max(shadow,
                             occ * graze * lights.contact_params.x * lights.dir_shadow_extra.x);
            }
        }

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    uint num_points = min(lights.light_counts.y, 16u);
    for (uint i = 0u; i < num_points; ++i) {
        PointLight pl = lights.point_lights[i];
        vec3 frag_to_light = pl.position_range.xyz - world_pos;
        float dist = length(frag_to_light);
        float range = pl.position_range.w;
        if (dist > range || dist < 0.0001) continue;

        vec3 L = frag_to_light / dist;
        // pl.attenuation.x -- formerly an unused classical "constant attenuation" term -- is
        // repurposed as a per-light falloff sharpness exponent: ~1 gives a gradual, realistic
        // fade to the light's range; ~4-8 gives a crisper, more cel-shaded-style cutoff (4
        // reproduces this engine's original hardcoded curve exactly). Defaults to
        // PointLightComponent::attenuation_constant's own default (1.0, smooth) if a scene
        // doesn't set it.
        float sharpness = max(pl.attenuation.x, 0.1);
        // `factor` (dist normalized by range, 0 at the light itself, 1 at its boundary) drives
        // BOTH the hard cutoff window (falloff, unchanged) AND the inverse-square-shaped
        // softening below -- range is the light's actual visible-width control now, not just a
        // late hard clamp. Previously the softening used raw world-space dist^2, which is
        // range-independent: since a light's own intensity already decays it to
        // imperceptibility well before typical range values, changing range had almost no
        // visible effect except when set smaller than that natural falloff distance. Using
        // `factor` here instead makes the whole curve self-similar and scaled by range, so
        // growing/shrinking range visibly grows/shrinks the light's glow. The 4*PI divisor is
        // kept (rather than dropped) so peak brightness at the light's center is unchanged from
        // before -- only the curve's width changes, not its scale, so existing intensity tuning
        // still holds.
        float factor = clamp(dist / range, 0.0, 1.0);
        float falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
        falloff *= falloff;
        float attenuation = falloff / (4.0 * BRDF_PI * (factor * factor + 1.0));
        vec3 radiance = pl.color_intensity.rgb * (pl.color_intensity.w * 0.08) * attenuation;

        // Small fixed world-space normal offset for the shadow test only (direct
        // lighting above stays unbiased) -- point lights have no analog of
        // dir_shadow_params.w to derive this from, unlike calc_dir_shadow's caller.
        vec3 shadow_bias_pos = world_pos + N * 0.02;
        float shadow = (i == 0u && pl.attenuation.w > 0.5)
            ? calc_point_shadow(pl.position_range.xyz - shadow_bias_pos, range) : 0.0;

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    uint num_spots = min(lights.light_counts.z, 8u);
    for (uint i = 0u; i < num_spots; ++i) {
        SpotLight sl = lights.spot_lights[i];
        vec3 frag_to_light = sl.position_range.xyz - world_pos;
        float dist = length(frag_to_light);
        float range = sl.position_range.w;
        if (dist > range || dist < 0.0001) continue;

        vec3 L = frag_to_light / dist;
        float cone = gfx_spot_cone(L, sl.direction_cone.xyz, sl.direction_cone.w, sl.params.y);
        if (cone <= 0.0) continue;

        // Identical distance curve to the point loop directly above -- see
        // gfx/spot_light.glsl's file doc on why that curve isn't shared here.
        float sharpness = max(sl.params.x, 0.1);
        float factor = clamp(dist / range, 0.0, 1.0);
        float falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
        falloff *= falloff;
        float attenuation = falloff / (4.0 * BRDF_PI * (factor * factor + 1.0));
        vec3 radiance = sl.color_intensity.rgb * (sl.color_intensity.w * 0.08) * attenuation * cone;

        float shadow = 0.0;
        if (i == lights.light_counts.w && sl.params.z > 0.5) {
            float normal_bias_scale = clamp(1.0 - dot(N, L), 0.0, 1.0);
            vec3 biased_pos = world_pos + N * (lights.spot_shadow_params.w * (0.5 + 0.5 * normal_bias_scale));
            shadow = calc_spot_shadow(lights.spot_light_space_matrix * vec4(biased_pos, 1.0), N, L);
        }

        Lo += shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow);
    }

    // Rim light: brightens the silhouette edge, a common cel-shading accent.
    if (params.rim_strength > 0.0) {
        float rim = 1.0 - max(dot(N, V), 0.0);
        rim = pow(rim, 3.0) * params.rim_strength;
        Lo += albedo * rim;
    }

    // Indirect lighting: no baked GI probes and no reflection probes (this
    // engine deliberately has neither), so the sky gradient is always both
    // the diffuse irradiance and the specular base. The specular half is the
    // one call shared with ssr_composite.frag (gfx/indirect_specular.glsl,
    // indirect_hooks.glsl) so its confidence-weighted subtraction cancels
    // what this pass added when ssr_enabled is set.
    vec3 ind_diff = sky_gradient(N, lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb)
                  * params.ambient_intensity;
    GfxIndirectSpecular ind = gfx_indirect_specular(world_pos, N, V, F0, roughness, params.sky_intensity,
                                                    lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb);
    vec3 kD_ind = (vec3(1.0) - ind.F) * (1.0 - metallic);

    // Occlusion applied the way Unity HDRP applies its GTAO (gfx/ao_composite.glsl):
    //  * material AO and screen-space AO combine by min() -- they estimate the same
    //    quantity at different scales, so multiplying would double-darken overlaps;
    //  * indirect diffuse gets multi-bounce AO (tints creases toward albedo);
    //  * indirect specular gets its own NdotV/roughness occlusion cone, tinted by F0;
    //  * direct lighting is scaled by the multi-bounce factor faded in with
    //    params.ssao_direct_strength (0 = indirect only).
    // ssr_composite.frag subtracts the same ind.value term this pass adds, so its
    // occlusion factor must match this one exactly (see ssr_composite_body.glsl).
    float occlusion = min(ao, ssao);
    vec3 ao_diffuse = gfx_gtao_multi_bounce(occlusion, albedo);
    float spec_occ  = gfx_specular_occlusion(max(dot(N, V), 0.0), occlusion, roughness);
    vec3 ambient = kD_ind * albedo * ind_diff * ao_diffuse
                 + ind.value * gfx_gtao_multi_bounce(spec_occ, F0);
    Lo *= mix(vec3(1.0), ao_diffuse, params.ssao_direct_strength);

    // emissive is added last, after the occluded terms -- an emissive surface glows
    // even in a fully occluded/dark crevice, unlike the lit terms above it.
    out_color = vec4(ambient + Lo + emissive, 1.0);
}
