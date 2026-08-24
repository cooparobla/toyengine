#version 450

// Deferred lighting for the pixel-art pipeline. Direct lighting keeps its
// original banded-cel formula unchanged: N.L is quantized into discrete
// steps (band(), gated by light_bands) and specular is a hard step()-masked
// highlight (gated by spec_threshold) rather than a smooth Cook-Torrance
// falloff -- see shade_light(). Everything else is independently
// toggleable on top of that fixed direct-lighting look:
//   - soft_shadows: rotated-PCF shadow edges instead of a single hard
//     compare (ported from blendy/assets/shaders/deferred_lighting.frag).
//   - g_ssao: always bound (to SsaoPass::output_view() or its neutral
//     fully-unoccluded texture, decided once at pipeline construction from
//     ssao_enabled -- see PixelRenderPipeline), so ambient occlusion is
//     just another multiplier here regardless of the toggle.
//   - indirect ambient: sky-gradient-based diffuse + specular (see
//     sky.glsl, brdf.glsl) rather than a flat ambient constant, so ssr.frag/
//     ssr_composite.frag have a well-defined indirect specular term to swap
//     reflections into when ssr_enabled is set -- and a nicer-looking
//     ambient fallback when it isn't.
//
// There is deliberately no "smooth PBR" mode for direct lighting -- the
// banded/masked-specular formula is this engine's one direct-lighting look;
// softness comes from shadows/SSAO/SSR, not from replacing that formula.

#include "sky.glsl"
#include "brdf.glsl"

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
    vec4 dir_ambient;
    mat4 dir_light_space_matrix;
    vec4 dir_shadow_params; // x=bias, y=pcf_samples, z=shadow_enabled, w=normal_bias

    uvec4 light_counts; // x=num_dir, y=num_point
    PointLight point_lights[16];
} lights;

// Set 2: Shadow maps -- toyengine has exactly one directional map and one
// point cube map (see shadow_map_target.h), unlike blendy's four cube slots.
layout(set = 2, binding = 0) uniform sampler2D dir_shadow_map;
layout(set = 2, binding = 1) uniform samplerCube point_shadow_map;

// Set 3: G-Buffer textures + screen-space AO
layout(set = 3, binding = 0) uniform sampler2D g_albedo_ao;          // RGB = Albedo, A = AO
layout(set = 3, binding = 1) uniform sampler2D g_normal_metallic;    // RGB = World Normal, A = Metallic
layout(set = 3, binding = 2) uniform sampler2D g_position_roughness; // RGB = World Pos, A = Roughness
layout(set = 3, binding = 3) uniform sampler2D g_ssao;                // R = SsaoPass output (or its neutral 1.0 texture)

layout(push_constant) uniform PixelParams {
    float light_bands;       // discrete N.L shading steps, e.g. 4.0; <= 1 disables banding
    float spec_threshold;    // hard specular highlight cutoff
    float rim_strength;      // 0 disables the rim term
    float soft_shadows;      // != 0 -> rotated-PCF shadows; 0 -> single hard compare
    float ambient_intensity; // scales sky_gradient(N) indirect diffuse
    float sky_intensity;     // scales sky_gradient(reflect(-V,N)) indirect specular base
} params;

layout(location = 0) out vec4 out_color;

// Quantizes N.L into `params.light_bands` discrete steps -- the core of the
// cel-shaded look. bands <= 1 disables banding (smooth falloff), matching a
// config value of 0 or 1 being a sensible "off" default.
float band(float ndl) {
    if (params.light_bands <= 1.0) return ndl;
    return floor(ndl * params.light_bands) / params.light_bands;
}

// --- Soft shadows: rotated-tap PCF, ported from
// blendy/assets/shaders/deferred_lighting.frag. Unrotated fixed taps only
// ever produce a handful of discrete shadow values, which quantizes the
// penumbra into visible concentric contour bands -- rotating the pattern per
// pixel/per shadow-point turns that banding into noise, which reads as soft.

const vec2 POISSON_DISK_16[16] = vec2[](
    vec2(-0.94201624, -0.39906216),
    vec2( 0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870),
    vec2( 0.34495938,  0.29387760),
    vec2(-0.91588581,  0.45771432),
    vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543,  0.27676845),
    vec2( 0.97484398,  0.75648370),
    vec2( 0.44323325, -0.97511554),
    vec2( 0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023),
    vec2( 0.79197514,  0.19090160),
    vec2(-0.24188840,  0.99706507),
    vec2(-0.81409955,  0.91437590),
    vec2( 0.19984126,  0.78641367),
    vec2( 0.14383161, -0.14100790)
);

float random_angle(vec2 seed) {
    return fract(sin(dot(seed, vec2(12.9898, 78.233))) * 43758.5453) * 6.28318530718;
}

const vec3 POINT_PCF_OFFSETS[8] = vec3[](
    vec3( 1,  1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1,  1,  1),
    vec3( 1,  1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1,  1, -1)
);

float random_angle_3d(vec3 p) {
    return fract(sin(dot(p, vec3(12.9898, 78.233, 37.719))) * 43758.5453) * 6.28318530718;
}

vec3 rotate_around_axis(vec3 v, vec3 axis, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

// Directional shadow: single hard compare, or a 16-tap rotated Poisson disk
// (params.soft_shadows != 0) with a deliberately widened (>1 texel) penumbra.
float calc_dir_shadow(vec4 light_space_pos, vec3 N, vec3 L) {
    if (lights.dir_shadow_params.z < 0.5) return 0.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    if (proj_coords.z > 1.0 || proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0) {
        return 0.0;
    }

    float current_depth = proj_coords.z;
    float bias = max(lights.dir_shadow_params.x * (1.0 - max(dot(N, L), 0.0)), 0.0002);

    if (params.soft_shadows == 0.0) {
        float closest_depth = texture(dir_shadow_map, proj_coords.xy).r;
        return (current_depth - bias > closest_depth) ? 1.0 : 0.0;
    }

    float angle = random_angle(gl_FragCoord.xy);
    float cos_a = cos(angle);
    float sin_a = sin(angle);
    mat2 rot = mat2(cos_a, -sin_a, sin_a, cos_a);
    vec2 texel_size = 1.75 / textureSize(dir_shadow_map, 0);

    float shadow = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rot * POISSON_DISK_16[i] * texel_size;
        float pcf_depth = texture(dir_shadow_map, proj_coords.xy + offset).r;
        shadow += (current_depth - bias > pcf_depth) ? 1.0 : 0.0;
    }
    return shadow / 16.0;
}

// Point shadow: single hard compare, or an 8-tap Rodrigues-rotated cube-corner
// PCF. Only lights.point_lights[0] can be a real shadow caster -- toyengine's
// ShadowMapTarget holds exactly one cube map at a time. `frag_to_light` here
// is surface-to-light (negated at the point of sampling, since the cube map
// was rendered looking outward FROM the light -- see shadow_cube.vert/frag)
// -- flipped once up front so the rotated-tap loop below can otherwise
// mirror blendy/assets/shaders/deferred_lighting.frag's
// sample_point_shadow_map verbatim (rotating the fixed corner offsets around
// the light-to-surface axis, not negating the already-rotated sum, which
// would not commute).
float calc_point_shadow(vec3 frag_to_light, float range) {
    vec3 light_to_surface = -frag_to_light;
    vec3 dir = normalize(light_to_surface);
    float current_dist = length(frag_to_light) / range;
    float bias = 0.05 / range;

    if (params.soft_shadows == 0.0) {
        float sampled_depth = texture(point_shadow_map, light_to_surface).r;
        return (current_dist - bias > sampled_depth) ? 1.0 : 0.0;
    }

    const float disk_radius = 0.03;
    float angle = random_angle_3d(frag_to_light);

    float shadow = 0.0;
    for (int i = 0; i < 8; ++i) {
        vec3 offset = rotate_around_axis(POINT_PCF_OFFSETS[i], dir, angle);
        vec3 sample_dir = dir + offset * disk_radius;
        float sampled_depth = texture(point_shadow_map, sample_dir).r;
        shadow += (current_dist - bias > sampled_depth) ? 1.0 : 0.0;
    }
    return shadow / 8.0;
}

// Direct lighting for one light: banded diffuse + hard-thresholded specular,
// multiplied by (1 - shadow). Unchanged from this engine's original formula
// -- see the file doc for why this stays fixed regardless of the other
// toggles.
vec3 shade_light(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic,
                 float roughness, vec3 F0, float shadow) {
    vec3 H = normalize(V + L);
    float ndl_raw = max(dot(N, L), 0.0);
    if (ndl_raw <= 0.0) return vec3(0.0);
    float ndl = band(ndl_raw);

    float NDF = distribution_ggx(N, H, roughness);
    float spec_mask = step(params.spec_threshold, NDF * geometry_smith(N, V, L, roughness));
    vec3 F = fresnel_schlick(max(dot(H, V), 0.0), F0);
    vec3 specular = F * spec_mask;

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

    // Rim light: brightens the silhouette edge, a common cel-shading accent.
    if (params.rim_strength > 0.0) {
        float rim = 1.0 - max(dot(N, V), 0.0);
        rim = pow(rim, 3.0) * params.rim_strength;
        Lo += albedo * rim;
    }

    // Indirect lighting: no baked GI probes and no reflection probes (this
    // engine deliberately has neither), so the sky gradient is always both
    // the diffuse irradiance and the specular base. ssr_composite.frag
    // reproduces ind_spec's exact expression so its confidence-weighted
    // subtraction cancels what this pass added when ssr_enabled is set.
    float NdotV = max(dot(N, V), 0.0);
    vec3  F_ind    = fresnel_schlick_roughness(NdotV, F0, roughness);
    vec2  brdf_ind = env_brdf_approx(NdotV, roughness);
    vec3  ind_diff = sky_gradient(N) * params.ambient_intensity;
    vec3  ind_spec = sky_gradient(reflect(-V, N)) * (F_ind * brdf_ind.x + brdf_ind.y) * params.sky_intensity;
    vec3  kD_ind   = (vec3(1.0) - F_ind) * (1.0 - metallic);
    vec3  ambient  = (kD_ind * albedo * ind_diff + ind_spec) * ao * ssao;

    out_color = vec4(ambient + Lo, 1.0);
}
