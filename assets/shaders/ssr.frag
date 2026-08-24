#version 450

// Hi-Z screen-space reflection raymarch. Unmodified port of
// gfxcoopa/assets/shaders/ssr.frag -- this shader only reads the camera UBO,
// G-buffer, Hi-Z depth pyramid and prefiltered scene-colour mip chain, none
// of which differ between blendy's GI-enabled pipeline and toyengine's
// GI-free one, so nothing here needed to change. Only the composite
// (ssr_composite.frag) drops the reflection-probe/GI descriptor set.

#include "ssr_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_ssr_color;

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: G-Buffer
layout(set = 1, binding = 0) uniform sampler2D g_albedo_ao;
layout(set = 1, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 1, binding = 2) uniform sampler2D g_position_roughness;

// Set 2: Hi-Z map
layout(set = 2, binding = 0) uniform sampler2D u_hiz_map;

// Set 3: Deferred Lit Scene Color (prefiltered mip chain as of Phase 4)
layout(set = 3, binding = 0) uniform sampler2D u_scene_color;

layout(push_constant) uniform SsrPushConstants {
    mat4  inv_proj;
    float max_distance;      // world-space ray length (ssr_max_distance)
    float bias_texels;       // normal bias, in full-res screen texels (ssr_bias_texels)
    float thickness_min;     // world-space floor for the thickness test (ssr_thickness)
    float thickness_scale;   // thickness as a fraction of |view z| (ssr_thickness_scale)
    float roughness_cutoff;
    int   max_iterations;
    int   max_hiz_mip;
    int   start_mip;         // Hi-Z mip the march starts at (ssr_start_mip)
    int   min_mip0_steps;    // self-reflection gate (ssr_min_mip0_steps)
    int   max_color_mip;     // top mip of the prefiltered scene-colour chain
} u_ssr;

float get_view_z(float depth_ndc, mat4 inv_proj) {
    vec4 clip = inv_proj * vec4(0.0, 0.0, depth_ndc, 1.0);
    return clip.z / clip.w;
}

void main() {
    // Snap to a definite full-res G-buffer texel. At full-res tracing in_uv is already texel-
    // centred and this is the identity; under half-res tracing (Phase 5) in_uv lands on a
    // full-res texel *corner*, where a NEAREST sampler picks one of two texels by float
    // rounding. Deriving an explicit integer coordinate keeps one code path for both.
    ivec2 gsize     = textureSize(g_position_roughness, 0);
    ivec2 origin_px = clamp(ivec2(in_uv * vec2(gsize)), ivec2(0), gsize - 1);

    vec4 norm_met  = texelFetch(g_normal_metallic,    origin_px, 0);
    vec4 pos_rough = texelFetch(g_position_roughness, origin_px, 0);
    vec3 N = norm_met.rgb;
    vec3 P = pos_rough.rgb;
    float roughness = pos_rough.a;

    // Background or high roughness check. Must happen before normalize(N) below --
    // background pixels write N = vec3(0), and normalize(vec3(0)) is NaN, which would
    // silently defeat this early-out (comparisons against NaN are always false in GLSL).
    if (dot(N, N) < 0.001 || roughness >= u_ssr.roughness_cutoff) {
        out_ssr_color = vec4(0.0);
        return;
    }
    N = normalize(N);

    vec3 V = normalize(P - camera.camera_pos);
    vec3 R = reflect(V, N);

    if (dot(R, N) <= 0.0) {
        out_ssr_color = vec4(0.0);
        return;
    }

    // Depth-scaled bias. The self-reflection this exists to prevent is a screen-space
    // phenomenon -- the ray must clear roughly one texel's worth of the source surface -- so
    // the correct model is a bias constant in TEXELS, not in metres. The old constants
    // (0.015 / 0.020) were 3.4 / 4.6 texels at the tuned camera distance; hence the defaults.
    float view_z   = (camera.view * vec4(P, 1.0)).z;  // negative: RH + DEPTH_ZERO_TO_ONE
    float p11      = abs(camera.proj[1][1]);          // untouched by TAA jitter, which only
                                                       // perturbs proj[2][0] / proj[2][1]
    float px_world = ssr_texel_world_size(view_z, p11, float(gsize.y));

    // 0.002 floor: px_world -> 0 near the near plane, and a zero bias reintroduces the
    // silhouette ring outright. This is the one case where depth-scaling is strictly WORSE
    // than the old constant, so the floor is not optional.
    float normal_bias = max(u_ssr.bias_texels * px_world, 0.002);
    float ray_bias    = normal_bias * 1.35;
    float self_hit_r  = normal_bias * 1.0;    // relaxed from 1.35x in Phase 2 tuning

    // Ray start and end in world space, biased along N and R to prevent self-reflection.
    vec3 P0 = P + N * normal_bias + R * ray_bias;
    vec3 P1 = P + N * normal_bias + R * u_ssr.max_distance;

    vec4 clip0 = camera.proj * camera.view * vec4(P0, 1.0);
    vec4 clip1 = camera.proj * camera.view * vec4(P1, 1.0);

    if (clip0.w <= 0.0 || clip1.w <= 0.0) {
        out_ssr_color = vec4(0.0);
        return;
    }

    vec3 ndc0 = clip0.xyz / clip0.w;
    vec3 ndc1 = clip1.xyz / clip1.w;

    vec3 ray_start = vec3(ssr_ndc_to_uv(ndc0.xy), ndc0.z);
    vec3 ray_end   = vec3(ssr_ndc_to_uv(ndc1.xy), ndc1.z);
    vec3 ray_dir   = ray_end - ray_start;

    if (length(ray_dir.xy) < 0.0001) {
        out_ssr_color = vec4(0.0);
        return;
    }

    vec3 current_pos = ray_start;
    // Starting at a coarse mip is safe: cell_min_depth is a conservative MIN over the cell, so
    // "ray is in front of the cell" at a coarse mip provably means no hit anywhere in that
    // cell. The only cost is that a ray starting behind its own cell's min burns start_mip
    // iterations descending, which is why this is a knob rather than a fixed 3 or 4.
    int current_mip = clamp(u_ssr.start_mip, 0, u_ssr.max_hiz_mip);
    bool hit_found = false;
    vec2 hit_uv = vec2(0.0);
    vec3 hit_P = vec3(0.0);
    float hit_view_z = -1.0;
    int mip0_steps = 0;

    for (int i = 0; i < u_ssr.max_iterations; ++i) {
        if (current_pos.x < 0.0 || current_pos.x > 1.0 ||
            current_pos.y < 0.0 || current_pos.y > 1.0 ||
            current_pos.z < 0.0 || current_pos.z > 1.0) {
            break;
        }

        vec2 mip_size = vec2(textureSize(u_hiz_map, current_mip));
        vec2 cell_idx = floor(current_pos.xy * mip_size);

        float cell_min_depth = textureLod(u_hiz_map, (cell_idx + 0.5) / mip_size, float(current_mip)).r;

        if (current_pos.z < cell_min_depth) {
            // Ray is in front of surface geometry in cell -> step to cell boundary & step up mip
            vec2 cell_min = cell_idx / mip_size;
            vec2 cell_max = (cell_idx + 1.0) / mip_size;

            vec2 t_planes;
            t_planes.x = (ray_dir.x > 0.0) ? (cell_max.x - current_pos.x) / ray_dir.x : (cell_min.x - current_pos.x) / ray_dir.x;
            t_planes.y = (ray_dir.y > 0.0) ? (cell_max.y - current_pos.y) / ray_dir.y : (cell_min.y - current_pos.y) / ray_dir.y;

            float t_step = max(min(t_planes.x, t_planes.y), 0.0001) + 0.0001;
            current_pos += ray_dir * t_step;

            current_mip = min(current_mip + 1, u_ssr.max_hiz_mip);
        } else {
            // Ray penetrated cell surface
            if (current_mip == 0) {
                // Require at least a couple of mip-0 steps before honoring a hit. Right at a
                // silhouette (grazing angle, N nearly perpendicular to R), the normal bias barely
                // projects along the ray, so the very first texel(s) sampled after the bias can
                // still land back on the *same* reflecting surface just past the self-hit radius
                // -- a false self-reflection ring traced right along every silhouette edge.
                ivec2 hit_px = clamp(ivec2(current_pos.xy * vec2(gsize)), ivec2(0), gsize - 1);
                vec3 hit_world_pos = texelFetch(g_position_roughness, hit_px, 0).rgb;
                if (mip0_steps >= u_ssr.min_mip0_steps && length(hit_world_pos - P) >= self_hit_r) {
                    float ray_z  = get_view_z(current_pos.z, u_ssr.inv_proj);
                    float surf_z = get_view_z(cell_min_depth, u_ssr.inv_proj);
                    float depth_diff_m = surf_z - ray_z;

                    // Thickness as a fraction of view depth with a world-space floor. A flat
                    // 0.1 m tolerance is far too tight for distant geometry (one Hi-Z texel
                    // already spans more than that in depth) and needlessly loose up close.
                    // 0.01 * 11.7 = 0.117 at the tuned distance, i.e. the old constant.
                    float thickness = max(u_ssr.thickness_min, u_ssr.thickness_scale * abs(ray_z));

                    if (depth_diff_m >= 0.0 && depth_diff_m <= thickness) {
                        vec3 hit_normal = texelFetch(g_normal_metallic, hit_px, 0).rgb;
                        if (dot(hit_normal, R) < -0.05) {
                            hit_found  = true;
                            hit_uv     = current_pos.xy;
                            hit_P      = hit_world_pos;
                            hit_view_z = ray_z;
                            break;
                        }
                    }
                }
                // Advance exactly one mip-0 texel along the ray direction. ray_dir is the
                // full un-normalized span of the whole ray (P0 -> P1 in UV space), not a
                // unit vector, so it must be normalized here -- otherwise this step is
                // "1/1920 of however long the ray happens to be on screen", which is far
                // too small for short on-screen rays (stalling the iteration budget) and
                // far too large for long ones (skipping over thin geometry).
                vec2 mip0_size = vec2(textureSize(u_hiz_map, 0));
                float texel_size = 1.0 / max(mip0_size.x, mip0_size.y);
                current_pos += (ray_dir / max(length(ray_dir.xy), 1e-5)) * texel_size;
                mip0_steps++;
            } else {
                current_mip = current_mip - 1;
            }
        }
    }

    if (!hit_found) {
        out_ssr_color = vec4(0.0);
        return;
    }

    // Confidence / Fading factors
    vec2 edge = smoothstep(vec2(0.0), vec2(0.08), hit_uv) * smoothstep(vec2(1.0), vec2(0.92), hit_uv);
    float screen_fade = edge.x * edge.y;

    // Fade out reflections whose ray points back toward the camera -- these are the ones
    // most prone to grazing-angle stretching/parallax error near the viewer's own reflection.
    // dot(-V, R) approaches 1 as R points back at the camera, so this fade must go toward
    // 0 (not 1) as that dot product increases.
    float dir_fade = 1.0 - smoothstep(0.25, 0.85, dot(-V, R));
    // Widened band (was 0.2). With cone tracing there is no longer a sharp-to-nothing pop to
    // hide, so the fade's job changes: it now hands over to the probe/sky prefilter, which
    // above ~0.8 roughness is genuinely the better answer anyway -- a single screen-space ray
    // with a cone that wide is sampling a mip so coarse that it has stopped being a reflection
    // of anything local, and the screen-space blur ignores the depth discontinuities the probe
    // does not have.
    float roughness_fade = 1.0 - smoothstep(u_ssr.roughness_cutoff - 0.3, u_ssr.roughness_cutoff, roughness);

    // Fade out reflections originating right at a silhouette (view direction nearly tangent
    // to the surface, NdotV near 0). Hit data there is the least reliable: the normal bias
    // barely projects along the ray at grazing angles, so even with the minimum-step gate
    // above, occasional false self-hits still show up as a thin ring right on every silhouette.
    float NdotV_origin = max(dot(N, -V), 0.0);
    float grazing_fade = smoothstep(0.0, 0.05, NdotV_origin);

    // Distance fade. Rays that run to max_distance are the ones whose screen-space error is
    // largest, and without this they pop out abruptly as the camera moves.
    float travel    = length(hit_P - P);
    float dist_fade = 1.0 - smoothstep(u_ssr.max_distance * 0.7, u_ssr.max_distance, travel);

    float confidence = screen_fade * dir_fade * roughness_fade * grazing_fade * dist_fade;

    // Roughness-aware cone footprint -> mip level. The specular cone has half-angle theta at
    // the origin, so at the hit it has spread to a world-space radius of tan(theta) * travel.
    // Project that to full-res texels at the HIT's depth (not the origin's -- the footprint is
    // a feature of the reflected image, which lives at the hit); a footprint N texels across is
    // exactly mip log2(N).
    //
    // This is also what makes the composite's split-sum weight (F * brdf.x + brdf.y) CORRECT:
    // that factor assumes its radiance input is prefiltered over the GGX lobe. A LOD-0 mirror
    // tap violated that for every non-zero roughness. The prefiltered tap satisfies it, and
    // matches how ibl_specular_probe() already samples its prefiltered cubemap at
    // roughness * max_mip -- so the two indirect-specular terms the composite subtracts are now
    // prefiltered on the same footing.
    float px_world_hit  = ssr_texel_world_size(hit_view_z, p11, float(gsize.y));
    float cone_diameter = 2.0 * ssr_ggx_cone_tan(roughness) * travel;
    float lod = clamp(log2(max(cone_diameter / max(px_world_hit, 1e-6), 1.0)),
                      0.0, float(u_ssr.max_color_mip));

    vec3 hit_color = textureLod(u_scene_color, hit_uv, lod).rgb;

    // Premultiplied by confidence. Temporal accumulation and (Phase 5) bilateral upsampling
    // are both weighted averages, and averaging an unpremultiplied colour against a
    // confidence-0 sample whose rgb is vec4(0) darkens the accumulated reflection while
    // leaving the confidence high. Premultiplied radiance is the linearly-filterable quantity.
    out_ssr_color = vec4(hit_color * confidence, confidence);
}
