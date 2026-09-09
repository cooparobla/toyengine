#ifndef TOY_PIXEL_FORWARD_SHADING_GLSL
#define TOY_PIXEL_FORWARD_SHADING_GLSL

// pixel_forward_shading.glsl -- the forward-shading body originally written
// (and still used) by transparent.frag, extracted so sdf_forward.frag can
// share it byte-for-byte instead of hand-copying the formula the way
// transparent_capture.frag's band()/shade_light() duplicate documents doing
// (see that file's "KEEP IN SYNC" warning) -- with this extraction, a BLEND
// mesh and a BLEND SDF now share ONE implementation, so they can never
// silently diverge the way transparent.frag and transparent_capture.frag
// already have to work around.
//
// A "body" file in the ssr_trace_body.glsl sense: the includer must, BEFORE
// including this file, already have declared (with these exact names,
// whatever their own set/binding indices are):
//   - `lights`            -- the LightUBO block (dir_direction/dir_color/
//                             dir_shadow_params/dir_light_space_matrix/
//                             light_counts/point_lights[16]).
//   - `dir_shadow_map`    -- sampler2DShadow.
//   - `point_shadow_map`  -- samplerCubeShadow.
//   - #include <gfx/brdf.glsl>, <gfx/shadow_sampling.glsl>,
//     <gfx/indirect_specular.glsl>, <gfx/sky.glsl>, <gfx/ssr_common.glsl>,
//     "indirect_hooks.glsl", <gfx/ssr_trace_body.glsl> (which itself needs
//     `g_normal_metallic`/`g_position_roughness`/`u_hiz_map`/`u_scene_color`
//     declared first -- see transparent.frag's own include order for the
//     canonical sequence this file assumes was already followed).

/// Per-fragment material inputs to gfx_pixel_forward_shade() -- the subset
/// of PBRMaterial a forward-shaded surface (mesh or SDF) needs, regardless
/// of where it came from (a push constant for a BLEND mesh, an SSBO record
/// for a BLEND SdfRenderer).
struct GfxForwardMaterial {
    vec3  albedo;
    float alpha;
    float metallic;
    float roughness;
    float ao;
};

/// Per-frame lighting/indirect/SSR tuning -- byte-for-byte the same fields
/// ForwardGlobals carries (toyengine's forward_globals.h), wherever the
/// includer sources them from (a ForwardGlobals UBO for the mesh forward
/// pass, an SdfData UBO for the SDF forward pass).
struct GfxForwardLightingParams {
    float light_bands;
    float spec_threshold;
    float soft_lighting;
    float rim_strength;
    float ambient_intensity;
    float sky_intensity;
    float ssr_enabled;
    float ssgi_intensity;
    float ssgi_distance;
    float ssr_max_distance;
    float ssr_bias_texels;
    float ssr_thickness_min;
    float ssr_thickness_scale;
    float ssr_roughness_cutoff;
    int   ssr_max_iterations;
    int   ssr_max_hiz_mip;
    int   ssr_start_mip;
    int   ssr_min_mip0_steps;
    int   ssr_max_color_mip;
};

// Directional shadow: single hard compare -- same kernel as pixel_lighting.frag's calc_dir_shadow.
float gfx_forward_calc_dir_shadow(vec4 light_space_pos, vec3 N, vec3 L) {
    if (lights.dir_shadow_params.z < 0.5) return 0.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    if (proj_coords.z > 1.0 || proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0) {
        return 0.0;
    }

    float bias = max(lights.dir_shadow_params.x * (1.0 - max(dot(N, L), 0.0)), 0.0002);
    return gfx_shadow_dir_hard(dir_shadow_map, proj_coords, bias);
}

float gfx_forward_calc_point_shadow(vec3 frag_to_light, float range) {
    vec3 light_to_surface = -frag_to_light;
    vec3 dir = normalize(light_to_surface);
    float current_dist = length(frag_to_light) / range;
    float bias = 0.05 / range;
    return gfx_shadow_cube_hard(point_shadow_map, dir, current_dist, bias);
}

/// Limb fade for the sub-texel silhouette band of this RAW, undenoised forward
/// path -- used by the SSR miss-fallback's weight and by refraction.glsl's
/// fresnel dimming (keep both on THIS one curve so their energy ledgers stay in
/// step; the trace's own hits are instead guarded by the asymmetric dark_trust
/// clamp in gfx_pixel_forward_shade(), which deliberately does NOT fade bright
/// hits).
///
/// gfx_ssr_trace()'s own grazing_fade spans NdotV [0, 0.05], but at this engine's
/// low internal resolution that band is far thinner than one texel of a curved
/// silhouette: NdotV rises like sqrt(texels-from-limb / radius), so a BLEND
/// sphere ~12 render-texels in radius is already past 0.05 well inside its
/// outermost texel. In that sub-texel band the trace's hit data is garbage (see
/// the miss-fallback comment in gfx_pixel_forward_shade() for the geometry) and
/// Schlick's pow5 fresnel spike zeroes refracted transmission with no rendered
/// reflection to compensate -- both of which used to render as near-black pixels
/// stippled along every BLEND silhouette. [0.05, 0.25] spans roughly the
/// outermost texel of a small sphere and nothing more: wide enough to catch the
/// sub-texel garbage, narrow enough that the look of everything past that first
/// texel is untouched.
float gfx_forward_silhouette_fade(float ndv) {
    return smoothstep(0.05, 0.25, ndv);
}

// Banded diffuse + hard-thresholded specular, or smooth Cook-Torrance -- see
// pixel_lighting.frag's identical formula/toggle for why this engine has
// exactly one direct-lighting look, shared by every forward-shaded surface.
float gfx_forward_band(float ndl, GfxForwardLightingParams p) {
    if (p.soft_lighting != 0.0) return ndl;
    if (p.light_bands <= 1.0) return ndl;
    return floor(ndl * p.light_bands) / p.light_bands;
}

vec3 gfx_forward_shade_light(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic,
                             float roughness, vec3 F0, float shadow, GfxForwardLightingParams p) {
    vec3 H = normalize(V + L);
    float ndl_raw = max(dot(N, L), 0.0);
    if (ndl_raw <= 0.0) return vec3(0.0);
    float ndl = gfx_forward_band(ndl_raw, p);

    float NDF = distribution_ggx(N, H, roughness);
    float G   = geometry_smith(N, V, L, roughness);
    vec3  F   = fresnel_schlick(max(dot(H, V), 0.0), F0);

    vec3 specular;
    if (p.soft_lighting != 0.0) {
        float denom = 4.0 * max(dot(N, V), 0.0) * ndl_raw + 0.0001;
        specular = (NDF * G * F) / denom;
    } else {
        float spec_mask = step(p.spec_threshold, NDF * G);
        specular = F * spec_mask;
    }

    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / BRDF_PI + specular) * radiance * ndl * (1.0 - shadow);
}

/// Full forward shading of one surface point: direct lighting (directional +
/// up to 16 point lights, with shadows), rim accent, base indirect term, and
/// -- when `p.ssr_enabled != 0` -- the same screen-space reflection +
/// SSGI-bounce delta ssr_composite_body.glsl applies to opaque geometry,
/// traced against the caller's already-bound Hi-Z/scene-colour chain.
///
/// @param world_pos    Shaded surface point, world space.
/// @param N            Shading normal, world space, already facing the viewer.
/// @param camera_pos   World-space camera position (V = normalize(camera_pos - world_pos)).
/// @param view, proj   Camera view/projection -- proj is inverted for the SSR trace's ray
///                     reconstruction, and view*proj position the SSGI bounce sample.
/// @param mat          Material inputs (see GfxForwardMaterial).
/// @param p            Per-frame lighting/indirect/SSR tuning (see GfxForwardLightingParams).
/// @return vec4(shaded RGB, mat.alpha).
vec4 gfx_pixel_forward_shade(vec3 world_pos, vec3 N, vec3 camera_pos, mat4 view, mat4 proj,
                             GfxForwardMaterial mat, GfxForwardLightingParams p) {
    vec3 albedo     = mat.albedo;
    float alpha     = clamp(mat.alpha, 0.0, 1.0);
    float metallic  = mat.metallic;
    float roughness = mat.roughness;
    float ao        = mat.ao;
    const float ssao = 1.0; // no screen-space AO for any forward-shaded surface

    vec3 V = normalize(camera_pos - world_pos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 Lo = vec3(0.0);

    if (lights.light_counts.x > 0) {
        vec3 L = normalize(-lights.dir_direction.xyz);
        vec3 radiance = lights.dir_color.rgb * lights.dir_direction.w;

        float normal_bias_scale = clamp(1.0 - dot(N, L), 0.0, 1.0);
        vec3 biased_pos = world_pos + N * (lights.dir_shadow_params.w * (0.5 + 0.5 * normal_bias_scale));
        vec4 light_space_pos = lights.dir_light_space_matrix * vec4(biased_pos, 1.0);
        float shadow = gfx_forward_calc_dir_shadow(light_space_pos, N, L);

        Lo += gfx_forward_shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow, p);
    }

    uint num_points = min(lights.light_counts.y, 16u);
    for (uint i = 0u; i < num_points; ++i) {
        PointLight pl = lights.point_lights[i];
        vec3 frag_to_light = pl.position_range.xyz - world_pos;
        float dist = length(frag_to_light);
        float range = pl.position_range.w;
        if (dist > range || dist < 0.0001) continue;

        vec3 L = frag_to_light / dist;
        float sharpness = max(pl.attenuation.x, 0.1);
        float factor = clamp(dist / range, 0.0, 1.0);
        float falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
        falloff *= falloff;
        float attenuation = falloff / (4.0 * BRDF_PI * (factor * factor + 1.0));
        vec3 radiance = pl.color_intensity.rgb * (pl.color_intensity.w * 0.08) * attenuation;

        vec3 shadow_bias_pos = world_pos + N * 0.02;
        float shadow = (i == 0u && pl.attenuation.w > 0.5)
            ? gfx_forward_calc_point_shadow(pl.position_range.xyz - shadow_bias_pos, range) : 0.0;

        Lo += gfx_forward_shade_light(N, V, L, radiance, albedo, metallic, roughness, F0, shadow, p);
    }

    if (p.rim_strength > 0.0) {
        float rim = 1.0 - max(dot(N, V), 0.0);
        rim = pow(rim, 3.0) * p.rim_strength;
        Lo += albedo * rim;
    }

    vec3 ind_diff = sky_gradient(N, lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb)
                  * p.ambient_intensity;
    GfxIndirectSpecular ind = gfx_indirect_specular(world_pos, N, V, F0, roughness, p.sky_intensity,
                                                    lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb);
    vec3 kD_ind = (vec3(1.0) - ind.F) * (1.0 - metallic);
    vec3 ambient = (kD_ind * albedo * ind_diff + ind.value) * ao * ssao;

    if (p.ssr_enabled != 0.0) {
        GfxSsrParams sp;
        sp.max_distance     = p.ssr_max_distance;
        sp.bias_texels      = p.ssr_bias_texels;
        sp.thickness_min    = p.ssr_thickness_min;
        sp.thickness_scale  = p.ssr_thickness_scale;
        sp.roughness_cutoff = p.ssr_roughness_cutoff;
        sp.max_iterations   = p.ssr_max_iterations;
        sp.max_hiz_mip      = p.ssr_max_hiz_mip;
        sp.start_mip        = p.ssr_start_mip;
        sp.min_mip0_steps   = p.ssr_min_mip0_steps;
        sp.max_color_mip    = p.ssr_max_color_mip;
        // Jitter is opaque-surface-only -- see GfxSsrParams' own doc.
        sp.jitter_strength  = 0.0;
        sp.frame_index      = 0;

        float ssr_ndv = max(dot(N, V), 0.0);
        float silhouette_fade = gfx_forward_silhouette_fade(ssr_ndv);

        vec3 ssr_R = reflect(-V, N);
        GfxSsrHit ssr_hit = (roughness < p.ssr_roughness_cutoff && dot(ssr_R, N) > 0.0)
            ? gfx_ssr_trace(world_pos, N, roughness, inverse(proj), sp)
            : GfxSsrHit(vec3(0.0), 0.0, 0.0, false);
        vec3 ssr_color   = ssr_hit.color;
        float confidence = ssr_hit.confidence;

        // Miss fallback for the grazing band. Where NdotV is low the reflected ray
        // is nearly the view ray's own continuation (dot(-V,R) = 2*NdotV^2 - 1 ~ -1):
        // it recedes into the depth buffer at almost its own pixel, so it MUST end on
        // the geometry visible right behind this surface -- a miss there is a marching
        // failure (the march barely moves in screen space, and hit-vs-miss flips on
        // sub-texel Hi-Z differences texel to texel), not a ray that cleared the scene.
        // On this undenoised path those failures used to read as a dark dotted ring
        // along every curved BLEND silhouette: one texel's hit carried the bright
        // floor reflection + SSGI bounce, its neighbour's miss fell back to the sky
        // term. Synthesizing the miss from the prefiltered scene colour at the
        // fragment's own UV (a couple of mips up -- the real hits land within texels
        // of it) makes miss texels agree with their hit neighbours instead; the
        // [0.45, 0.65] rolloff hands back to the plain sky fallback where rays point
        // far enough off-axis that missing the whole depth buffer is legitimate.
        if (!ssr_hit.hit) {
            float fallback_w = silhouette_fade * (1.0 - smoothstep(0.45, 0.65, ssr_ndv));
            // (silhouette_fade keeps the sub-texel limb band, where even the one-step
            // estimate below reads garbage geometry, at the plain sky fallback.)
            if (fallback_w > 0.0) {
                // One-step ray estimate, not the fragment's own UV: neighbouring texels'
                // REAL hits land a few texels along the projected ray (past e.g. the
                // sphere's own contact shadow, onto the lit floor beyond it), so sampling
                // in place would fill the miss with the wrong side of exactly the kind of
                // high-contrast boundary that made the dots visible in the first place.
                // The step length is a small fixed fraction of the trace's own reach so it
                // scales with the same knob that scales every real hit's travel.
                vec3 fb_point = world_pos + ssr_R * (p.ssr_max_distance * 0.03);
                vec4 fb_clip  = proj * view * vec4(fb_point, 1.0);
                if (fb_clip.w > 0.0) {
                    vec2 fb_uv = clamp(ssr_ndc_to_uv(fb_clip.xy / fb_clip.w), 0.0, 1.0);
                    float fb_lod = min(2.0, float(p.ssr_max_color_mip));
                    ssr_color  = textureLod(u_scene_color, fb_uv, fb_lod).rgb * fallback_w;
                    confidence = fallback_w;
                }
            }
        }

        vec3 ssr_specular = ssr_color * (ind.F * ind.brdf.x + ind.brdf.y);

        // Asymmetric silhouette guard: BRIGHTENING deltas pass at full strength
        // everywhere (they are what give a BLEND surface its lit reflection band, and
        // symmetric fading here provably changes the look), but a delta that would
        // DARKEN the pixel below its sky/indirect base is scaled down toward the limb.
        // Near the limb the trace is a per-texel coin flip on sub-texel Hi-Z detail
        // (see the miss-fallback comment above), and only its dark outcomes ever read
        // as artifacts: a darker-than-sky hit swaps the bright sky term for e.g. the
        // surface's own contact shadow, stippling near-black texels along the
        // silhouette between bright-sky neighbours. dark_trust reaches 1 by NdotV
        // 0.45 -- past the coin-flip ring -- so legitimately dark interior reflections
        // are untouched.
        vec3 delta = ssr_specular - confidence * ind.value;
        float dark_trust = smoothstep(0.05, 0.45, ssr_ndv);
        delta = mix(max(delta, vec3(0.0)), delta, dark_trust);
        ambient = max(ambient + delta * ao * ssao, 0.0);

        if (p.ssgi_intensity > 0.0) {
            vec4 bounce_clip = proj * view * vec4(world_pos + N * p.ssgi_distance, 1.0);
            if (bounce_clip.w > 0.0) {
                vec2 bounce_uv = ssr_ndc_to_uv(bounce_clip.xy / bounce_clip.w);
                vec2 edge = smoothstep(vec2(0.0), vec2(0.08), bounce_uv)
                          * smoothstep(vec2(1.0), vec2(0.92), bounce_uv);
                vec3 bounce = textureLod(u_scene_color, clamp(bounce_uv, 0.0, 1.0),
                                         float(p.ssr_max_color_mip)).rgb;
                // `confidence` here is the unified weight from above -- a real hit's
                // confidence, or the miss fallback's own weight in the grazing band.
                // Weighting by raw hit confidence alone (the way ssr_composite_body.glsl
                // does for the denoised opaque bounce) is what used to dot a dark broken
                // ring along curved BLEND silhouettes: on this RAW path it was a
                // per-texel binary, so one texel's ray hit (bounce added, visibly
                // brighter) while its neighbour's missed (no bounce). The fallback
                // filling misses in makes this weight smooth again.
                ambient += kD_ind * albedo * bounce * (edge.x * edge.y) * confidence
                         * p.ssgi_intensity * ao * ssao;
            }
        }
    }

    return vec4(ambient + Lo, alpha);
}

#endif // TOY_PIXEL_FORWARD_SHADING_GLSL
