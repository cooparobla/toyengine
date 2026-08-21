#version 450

// Banded-PBR deferred lighting for the pixel-art pipeline. Derived from
// blendy/assets/shaders/deferred_lighting.frag with the GI and SSAO blocks
// removed entirely -- toyengine has neither -- and N.L quantized into
// discrete steps so shading reads as flat cel bands instead of a smooth PBR
// gradient. Shadows are a single hard comparison, no PCF -- soft shadow
// edges would just get dithered into mush by the palette pass later, so
// crisp edges are the point here, not a limitation.

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
    vec4 dir_shadow_params; // x=bias, y=pcf_samples, z=shadow_enabled

    uvec4 light_counts; // x=num_dir, y=num_point
    PointLight point_lights[16];
} lights;

// Set 2: Shadow maps
layout(set = 2, binding = 0) uniform sampler2D dir_shadow_map;
layout(set = 2, binding = 1) uniform samplerCube point_shadow_map;

// Set 3: G-Buffer textures
layout(set = 3, binding = 0) uniform sampler2D g_albedo_ao;          // RGB = Albedo, A = AO
layout(set = 3, binding = 1) uniform sampler2D g_normal_metallic;    // RGB = World Normal, A = Metallic
layout(set = 3, binding = 2) uniform sampler2D g_position_roughness; // RGB = World Pos, A = Roughness

layout(push_constant) uniform PixelParams {
    float light_bands;    // discrete N.L shading steps, e.g. 4.0
    float spec_threshold; // hard specular highlight cutoff
    float rim_strength;   // 0 disables the rim term
    float _pad;
} params;

layout(location = 0) out vec4 out_color;

const float PI = 3.14159265359;

float distribution_ggx(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return a2 / max(denom, 0.000001);
}

float geometry_smith(vec3 N, vec3 V, vec3 L, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx_v = NdotV / max(NdotV * (1.0 - k) + k, 0.000001);
    float ggx_l = NdotL / max(NdotL * (1.0 - k) + k, 0.000001);
    return ggx_v * ggx_l;
}

vec3 fresnel_schlick(float cos_theta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cos_theta, 0.0, 1.0), 5.0);
}

// Quantizes N.L into `params.light_bands` discrete steps -- the core of the
// cel-shaded look. bands <= 1 disables banding (smooth falloff), matching a
// config value of 0 or 1 being a sensible "off" default.
float band(float ndl) {
    if (params.light_bands <= 1.0) return ndl;
    return floor(ndl * params.light_bands) / params.light_bands;
}

// Single hard comparison against the directional shadow map -- no PCF, no
// Poisson disk. Returns 1.0 (fully shadowed) or 0.0.
float calc_dir_shadow(vec4 light_space_pos) {
    if (lights.dir_shadow_params.z < 0.5) return 0.0;

    vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
    proj_coords.xy = proj_coords.xy * 0.5 + 0.5;

    if (proj_coords.z > 1.0 || proj_coords.x < 0.0 || proj_coords.x > 1.0 ||
        proj_coords.y < 0.0 || proj_coords.y > 1.0) {
        return 0.0;
    }

    float closest_depth = texture(dir_shadow_map, proj_coords.xy).r;
    return (proj_coords.z - lights.dir_shadow_params.x > closest_depth) ? 1.0 : 0.0;
}

// Single hard comparison against the point shadow cubemap. Only lights.point_lights[0]
// can be a real shadow caster -- gfxcoopa's ShadowMapTarget holds exactly one
// point-light cube map at a time (see shadow_map_target.h), matching blendy's
// own "first shadow-casting point light only" behavior.
float calc_point_shadow(vec3 frag_to_light, float range) {
    float current_dist = length(frag_to_light) / range;
    float bias = 0.05 / range;
    float sampled_depth = texture(point_shadow_map, -frag_to_light).r;
    return (current_dist - bias > sampled_depth) ? 1.0 : 0.0;
}

// Direct lighting for one light: banded diffuse + hard-thresholded specular,
// multiplied by (1 - shadow).
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
    return (kD * albedo / PI + specular) * radiance * ndl * (1.0 - shadow);
}

void main() {
    vec4 g0 = texture(g_albedo_ao, in_uv);
    vec4 g1 = texture(g_normal_metallic, in_uv);
    vec4 g2 = texture(g_position_roughness, in_uv);

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
        float shadow = calc_dir_shadow(light_space_pos);

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
        float factor = clamp(dist / range, 0.0, 1.0);
        float falloff = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
        falloff *= falloff;
        float attenuation = (1.0 / (4.0 * PI * (dist * dist + 1.0))) * falloff;
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

    vec3 ambient = lights.dir_ambient.rgb * albedo * ao;
    out_color = vec4(ambient + Lo, 1.0);
}
