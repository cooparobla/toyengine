#version 450

// Raymarch of the LOCAL volumes (fog pockets, wind ribbons, drifting haze -- see
// gfx/volumetrics.glsl for the three kinds), at REDUCED resolution.
//
// First half of VolumetricsPass. This shader only integrates the medium: it writes
// rgb = in-scattered light, a = transmittance, into a low-resolution target, and
// volumetrics_composite.frag upsamples that over the full-resolution scene (and
// applies the merged global fog term). The march is by far the most expensive
// fullscreen work in the frame, and a low-frequency medium loses nothing visible at
// half resolution once the composite's depth-aware upsample keeps silhouettes sharp.
//
// Per-step cost is kept down by noting that the sample point is LINEAR in t:
// p(t) = A + view_dir * t. Every affine projection of it -- a volume's local space, a
// shadow cascade's clip space, the spot light's clip space -- is therefore also
// linear in t, so each is computed once per pixel as origin + direction and the loop
// does a multiply-add instead of a mat4 product per volume / cascade / light.
//
// Vertex stage is the shared fullscreen triangle (fullscreen.vert).

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_scatter_t;   // rgb = in-scatter, a = transmittance

// Set 0: G-buffer normal/position (nearest -- linear filtering would blend world
// positions across silhouette edges and give a wrong ray-termination distance).
// volumetrics_composite.frag reads these same texels at this target's texel centres
// to recover the depth each low-res sample was marched to.
layout(set = 0, binding = 0) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 1) uniform sampler2D g_position_roughness;

#include <gfx/volumetrics_ubo.glsl>

// Set 2: shadow maps for the in-scatter terms. Compare-enabled samplers
// (util::Sampler::shadow(), VK_COMPARE_OP_GREATER), so one texture() call is a
// hardware-filtered depth compare returning 1 = in shadow -- the same convention
// gfx/shadow_sampling.glsl's *Shadow family documents.
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
// The local-light (point/spot) shadow atlas -- see gfx/local_shadow.glsl.
layout(set = 2, binding = 1) uniform sampler2DShadow local_shadow_atlas;
// The cloud layer's shadow map (cloud_shadow.glsl): light shafts through the gaps in the clouds.
layout(set = 2, binding = 4) uniform sampler2D cloud_shadow_map;
#define GFX_LOCAL_SHADOWS u_vol.local_shadows
#include <gfx/local_shadow.glsl>

#include <gfx/volumetrics.glsl>
#include <gfx/fog.glsl>        // gfx_fog_hg + the box/sphere containment weights
#include <gfx/spot_light.glsl> // gfx_spot_cone for the scatter-light loop
#include "cloud_shadow.glsl"     // the clouds' shadow on the sun's in-scatter

#define VOL_MAX_VOLUMES  8
#define VOL_MAX_CASCADES 4
#define VOL_MAX_LIGHTS   4

#include <gfx/volumetrics_lighting.glsl> // vol_shadow_proj, vol_sun_cascade_vis, vol_scatter_light

void main() {
    int  volume_count = int(u_vol.counts.x);
    if (volume_count <= 0) {
        out_scatter_t = vec4(0.0, 0.0, 0.0, 1.0);   // nothing placed -- the composite passes colour through
        return;
    }

    vec3 N       = texture(g_normal_metallic, in_uv).rgb;
    vec3 A       = u_vol.camera_pos.xyz;
    bool is_sky  = dot(N, N) < 0.001;   // G1 normal is zero on sky, same test fog.frag uses

    // Reconstruct the view ray from inv_view_proj for EVERY pixel rather than by
    // subtracting the G2 world position on geometry pixels -- fog.frag's comment
    // explains the precision reasoning. NDC.y is negated because this fullscreen
    // triangle uses a positive-height viewport while inv_view_proj follows the
    // Y-up convention every other unprojection here shares.
    vec3 ndc      = vec3(in_uv.x * 2.0 - 1.0, 1.0 - in_uv.y * 2.0, 1.0);
    vec4 world    = u_vol.inv_view_proj * vec4(ndc, 1.0);
    vec3 view_dir = normalize(world.xyz / world.w - A);

    float d_geo   = is_sky ? 1e6 : distance(A, texture(g_position_roughness, in_uv).rgb);
    float ray_end = min(d_geo, max(u_vol.march_params.y, 0.0));

    int steps = int(u_vol.march_params.x);
    if (ray_end <= 0.0 || steps <= 0) {
        out_scatter_t = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Clip the march to the UNION of the volumes' bounds, and keep each volume's
    // local-space ray (inv_world is rotation + translation only -- volumes assume unit
    // scale -- so t stays in world units on both sides of the transform). A volume the
    // ray misses entirely is dropped from the per-step loop via `live`.
    vec3  lro[VOL_MAX_VOLUMES];
    vec3  lrd[VOL_MAX_VOLUMES];
    bool  live[VOL_MAX_VOLUMES];
    float t_near = 1e30;
    float t_far  = -1e30;
    for (int v = 0; v < VOL_MAX_VOLUMES; ++v) {
        live[v] = false;
        if (v >= volume_count || u_vol.volumes[v].shape_params.x <= 0.0) {
            continue;
        }
        lro[v] = (u_vol.volumes[v].inv_world * vec4(A, 1.0)).xyz;
        lrd[v] = (u_vol.volumes[v].inv_world * vec4(view_dir, 0.0)).xyz;
        vec3 ext = u_vol.volumes[v].extent_shape.xyz;
        vec2 hit = (u_vol.volumes[v].extent_shape.w > 0.5)
                 ? gfx_fog_sphere_intersect(lro[v], lrd[v], ext.x)
                 : gfx_fog_box_intersect(lro[v], lrd[v], ext);
        if (hit.y <= hit.x || hit.y <= 0.0 || hit.x >= ray_end) {
            continue;                     // miss, behind the camera, or behind the surface
        }
        live[v] = true;
        t_near = min(t_near, max(hit.x, 0.0));
        t_far  = max(t_far,  min(hit.y, ray_end));
    }

    if (t_far <= t_near) {
        out_scatter_t = vec4(0.0, 0.0, 0.0, 1.0);     // no volume in front of this pixel
        return;
    }

    float dt = (t_far - t_near) / float(steps);

    // Dither the START offset within one step. Without it a low step count lays
    // visible concentric bands over every feature. The per-frame R2 shift means a
    // temporal accumulator downstream averages a DIFFERENT pattern each frame,
    // resolving the banding instead of re-resolving one frozen one.
    float t = t_near + dt * gfx_volume_ign(gl_FragCoord.xy, int(u_vol.time_params.z));

    // Hoisted: the phase function depends only on view_dir and the sun, neither of
    // which varies along the ray or between volumes. Negated sun_direction because
    // that field stores the direction light TRAVELS, not the direction to the sun.
    float phase    = gfx_fog_hg(dot(view_dir, -u_vol.sun_direction.xyz), u_vol.march_params.w);
    vec3  sun_base = u_vol.sun_color.rgb * phase;

    // Sun shadow: each cascade's clip-space ray, so a step's projection is one madd.
    // Selection is the same first-containing-cascade rule gfx_csm_select() applies
    // (cascade 0 is the smallest box, so first hit is the sharpest tile covering the
    // point), without its transition dither: there is no temporal filter tuned for it
    // here, and a hard cascade switch is invisible in fog, where a shadow tap only
    // modulates in-scatter.
    bool  sun_shadowed  = u_vol.shadow_params.x > 0.5;
    int   cascade_count = sun_shadowed ? min(int(u_vol.dir_cascade_info.x), VOL_MAX_CASCADES) : 0;
    vec4  cso[VOL_MAX_CASCADES];
    vec4  csd[VOL_MAX_CASCADES];
    for (int c = 0; c < VOL_MAX_CASCADES; ++c) {
        if (c >= cascade_count) break;
        cso[c] = u_vol.dir_cascade_matrix[c] * vec4(A, 1.0);
        csd[c] = u_vol.dir_cascade_matrix[c] * vec4(view_dir, 0.0);
    }

    // Scatter lights: drop any whose range sphere the march segment never enters, so
    // the per-step loop only visits lights that can contribute somewhere on this ray.
    float light_strength = u_vol.counts.z;
    int   light_count    = (light_strength > 0.0) ? min(int(u_vol.counts.y), VOL_MAX_LIGHTS) : 0;
    bool  light_live[VOL_MAX_LIGHTS];
    for (int li = 0; li < VOL_MAX_LIGHTS; ++li) {
        light_live[li] = false;
        if (li >= light_count) continue;
        vec3  c   = u_vol.scatter_lights[li].position_range.xyz;
        float r   = u_vol.scatter_lights[li].position_range.w;
        float tc  = clamp(dot(c - A, view_dir), t_near, t_far);
        vec3  q   = A + view_dir * tc - c;
        light_live[li] = dot(q, q) < r * r;
    }

    float T        = 1.0;
    vec3  scatter  = vec3(0.0);
    float coverage = 0.0;

    for (int i = 0; i < steps; ++i) {
        vec3 p = A + view_dir * t;

        float sigma     = 0.0;
        vec3  emit      = vec3(0.0);
        float light_w   = 0.0;   // density-weighted in-scatter response (mode_params.y)
        float occl_sum  = 0.0;

        for (int v = 0; v < VOL_MAX_VOLUMES; ++v) {
            if (v >= volume_count) break;
            if (!live[v]) continue;

            // Containment first: it is far cheaper than the field, and outside the
            // bounds the field is irrelevant. gfx_fog_box_edge_weight is already a
            // POINT-based soft containment test (0 outside), so it drops straight in.
            vec3  lp   = lro[v] + lrd[v] * t;
            vec3  ext  = u_vol.volumes[v].extent_shape.xyz;
            float soft = u_vol.volumes[v].mode_params.z;
            float vw   = (u_vol.volumes[v].extent_shape.w > 0.5)
                       ? gfx_fog_sphere_point_weight(lp, ext.x, soft)
                       : gfx_fog_box_edge_weight(lp, ext, soft);
            if (vw <= 1e-4) {
                continue;
            }

            float f = gfx_volume_field(p, u_vol.time_params.x,
                                       int(u_vol.volumes[v].mode_params.x),
                                       u_vol.volumes[v].direction_speed,
                                       u_vol.volumes[v].field_params,
                                       u_vol.volumes[v].shape_params,
                                       u_vol.volumes[v].flow_params);
            float s = f * vw * u_vol.volumes[v].shape_params.x;
            if (s <= 1e-5) {
                continue;
            }

            sigma    += s;
            emit     += s * u_vol.volumes[v].color_occlusion.rgb;
            light_w  += s * u_vol.volumes[v].mode_params.y;
            occl_sum += s * u_vol.volumes[v].color_occlusion.w;
        }

        // Light in-scatter, evaluated once per STEP (not per volume: it depends
        // only on the sample point) and only where density responded to light at
        // all -- shadow taps and the light loop cost nothing over empty air.
        // Each volume's own response is its density-weighted mode_params.y
        // (light_w), so sun and local lights share one per-volume dial.
        if (light_w > 0.0) {
            float sun_vis = 1.0;
            for (int c = 0; c < VOL_MAX_CASCADES; ++c) {
                if (c >= cascade_count) break;
                float v;   // an out param is undefined on a false return -- never pass sun_vis itself
                if (vol_sun_cascade_vis(c, cso[c] + csd[c] * t, v)) { sun_vis = v; break; }
            }
            vec2  cloud_uv;
            float cloud_w;
            cloud_shadow_lookup(u_vol.cloud_shadow, u_vol.cloud_shadow_layer, -u_vol.sun_direction.xyz, p, cloud_uv, cloud_w);
            if (cloud_w > 0.0) sun_vis *= mix(1.0, textureLod(cloud_shadow_map, cloud_uv, 0.0).r, cloud_w);
            vec3 in_scatter = sun_base * sun_vis;

            for (int li = 0; li < VOL_MAX_LIGHTS; ++li) {
                if (li >= light_count) break;
                if (!light_live[li]) continue;
                in_scatter += vol_scatter_light(li, p, view_dir, light_strength);
            }

            emit += light_w * in_scatter;
        }

        if (sigma > 1e-4) {
            // Emission and extinction are DECOUPLED. With one coefficient driving
            // both, a feature bright enough to see necessarily also darkens what is
            // behind it -- which is what makes thin bright strands read as grey
            // smudges rather than as light. The occlusion scale is density-weighted
            // across the volumes contributing here, so overlapping volumes composite
            // as one medium instead of double-darkening.
            float a   = 1.0 - exp(-sigma * dt);
            scatter  += T * a * (emit / sigma);
            coverage += T * a;

            float ext = 1.0 - exp(-sigma * dt * (occl_sum / sigma));
            T        *= (1.0 - ext);
            if (T < 0.01) {
                break;   // saturated -- no remaining step can contribute visibly
            }
        }
        t += dt;
    }

    T = clamp(T, 1.0 - clamp(u_vol.march_params.z, 0.0, 1.0), 1.0);

    // Debug view: accumulated emission coverage alone, carried in .r for the composite
    // to show with scene colour suppressed. Coverage, not 1 - T: the occlusion scale is
    // independent, so transmittance is near 1 by design and says almost nothing about
    // the field's shape.
    if (u_vol.camera_pos.w > 0.5) {
        out_scatter_t = vec4(clamp(coverage, 0.0, 1.0), 0.0, 0.0, 1.0);
        return;
    }

    out_scatter_t = vec4(scatter, T);
}
