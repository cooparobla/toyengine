#version 450

// Froxel volumetrics, stage 1 of 3 (FroxelVolumetricsPass): one fragment per froxel of the
// camera-aligned grid (see gfx/volumetrics_froxel.glsl for the layout). Evaluates the local
// volumes' density and in-scatter ONCE per froxel -- the work the raymarch does per pixel
// per step -- at a point jittered inside the froxel, then blends with last frame's grid
// reprojected to this froxel (temporal reprojection, as Unreal's and HDRP's volumetric fog
// do), so a single jittered sample per frame integrates to a well-sampled froxel over time.
// Froxels with no history (froxel_params2.x samples) are supersampled instead.
//
// Output is PRE-INTEGRATED over the slice's length dt -- exactly the raymarch's per-step
// update, so stage 2 integrates with nothing more than scatter += T * rgb; T *= a:
//   rgb = in-scattered light the slice adds   = (emit / sigma) * (1 - exp(-sigma * dt))
//   a   = transmittance through the slice     = exp(-sigma_ext * dt)
// Emission and extinction stay decoupled exactly as in the march (occlusion-weighted
// extinction; see volumetrics_march.frag).

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_froxel;

// Set 0: LAST frame's injected grid (the other half of the ping-pong pair), linear sampler.
layout(set = 0, binding = 0) uniform sampler2D history_grid;

#include <gfx/volumetrics_ubo.glsl>

// Set 2: shadow maps for the in-scatter terms (compare samplers, as the march).
layout(set = 2, binding = 0) uniform sampler2DShadow dir_shadow_map;
// The local-light (point/spot) shadow atlas -- see gfx/local_shadow.glsl.
layout(set = 2, binding = 1) uniform sampler2DShadow local_shadow_atlas;
#define GFX_LOCAL_SHADOWS u_vol.local_shadows
#include <gfx/local_shadow.glsl>

#include <gfx/volumetrics.glsl>
#include <gfx/fog.glsl>        // gfx_fog_hg + the box/sphere containment weights
#include <gfx/spot_light.glsl> // gfx_spot_cone
#include <gfx/volumetrics_lighting.glsl>
#include <gfx/volumetrics_froxel.glsl>

#define VOL_MAX_VOLUMES  8
#define VOL_MAX_CASCADES 4
#define VOL_MAX_LIGHTS   4

/// Last frame's value for world point p, or a negative alpha when p falls outside last
/// frame's grid (no history there).
vec4 vol_froxel_history(vec3 p) {
    vec4 clip = u_vol.prev_view_proj * vec4(p, 1.0);
    if (clip.w <= 0.0) return vec4(-1.0);
    vec2 ndc = clip.xy / clip.w;
    // Same NDC.y convention as the ray reconstruction below (inverted relative to uv).
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec4(-1.0);

    // Froxel values describe whole slices, so sample at slice CENTRES: centre k sits at
    // boundary coordinate k + 0.5.
    float D  = u_vol.froxel_grid.z;
    float fs = vol_froxel_slice_of(distance(u_vol.prev_camera_pos.xyz, p)) - 0.5;
    if (fs < -0.5 || fs > D - 0.5) return vec4(-1.0);
    fs = clamp(fs, 0.0, D - 1.0);
    int   s0 = int(floor(fs));
    int   s1 = min(s0 + 1, int(D) - 1);
    return mix(vol_froxel_sample(history_grid, uv, s0), vol_froxel_sample(history_grid, uv, s1), fs - float(s0));
}

/// Pre-integrated (in-scatter, transmittance) of the slice [d0, d1] evaluated at the single
/// point p on view_dir -- one raymarch step's worth of work.
vec4 vol_froxel_evaluate(vec3 p, vec3 view_dir, float dt) {
    // --- Density: the same per-volume accumulation as one raymarch step ---
    int   volume_count = min(int(u_vol.counts.x), VOL_MAX_VOLUMES);
    float sigma    = 0.0;
    vec3  emit     = vec3(0.0);
    float light_w  = 0.0;
    float occl_sum = 0.0;
    for (int v = 0; v < VOL_MAX_VOLUMES; ++v) {
        if (v >= volume_count) break;
        float density = u_vol.volumes[v].shape_params.x;
        if (density <= 0.0) continue;
        vec3  lp   = (u_vol.volumes[v].inv_world * vec4(p, 1.0)).xyz;
        vec3  ext  = u_vol.volumes[v].extent_shape.xyz;
        float soft = u_vol.volumes[v].mode_params.z;
        float vw   = (u_vol.volumes[v].extent_shape.w > 0.5)
                   ? gfx_fog_sphere_point_weight(lp, ext.x, soft)
                   : gfx_fog_box_edge_weight(lp, ext, soft);
        if (vw <= 1e-4) continue;
        float f = gfx_volume_field(p, u_vol.time_params.x, int(u_vol.volumes[v].mode_params.x),
                                   u_vol.volumes[v].direction_speed, u_vol.volumes[v].field_params,
                                   u_vol.volumes[v].shape_params, u_vol.volumes[v].flow_params);
        float s = f * vw * density;
        if (s <= 1e-5) continue;
        sigma    += s;
        emit     += s * u_vol.volumes[v].color_occlusion.rgb;
        light_w  += s * u_vol.volumes[v].mode_params.y;
        occl_sum += s * u_vol.volumes[v].color_occlusion.w;
    }

    if (sigma <= 1e-4) return vec4(0.0, 0.0, 0.0, 1.0);   // empty air: adds nothing, transmits everything

    // --- Lighting: the shared body the raymarch uses (gfx/volumetrics_lighting.glsl) ---
    if (light_w > 0.0) {
        float phase    = gfx_fog_hg(dot(view_dir, -u_vol.sun_direction.xyz), u_vol.march_params.w);
        float sun_vis  = 1.0;
        if (u_vol.shadow_params.x > 0.5) {
            int cascades = min(int(u_vol.dir_cascade_info.x), VOL_MAX_CASCADES);
            for (int c = 0; c < VOL_MAX_CASCADES; ++c) {
                if (c >= cascades) break;
                float vis;   // undefined on a false return -- never write sun_vis directly
                if (vol_sun_cascade_vis(c, u_vol.dir_cascade_matrix[c] * vec4(p, 1.0), vis)) {
                    sun_vis = vis;
                    break;
                }
            }
        }
        vec3 in_scatter = u_vol.sun_color.rgb * phase * sun_vis;

        float light_strength = u_vol.counts.z;
        int   light_count    = (light_strength > 0.0) ? min(int(u_vol.counts.y), VOL_MAX_LIGHTS) : 0;
        for (int li = 0; li < VOL_MAX_LIGHTS; ++li) {
            if (li >= light_count) break;
            in_scatter += vol_scatter_light(li, p, view_dir, light_strength);
        }
        emit += light_w * in_scatter;
    }

    float a   = 1.0 - exp(-sigma * dt);
    float ext = 1.0 - exp(-sigma * dt * (occl_sum / sigma));
    return vec4((emit / sigma) * a, 1.0 - ext);
}

/// View direction through screen uv of the current (unjittered) camera.
vec3 vol_froxel_view_dir(vec2 uv) {
    vec3 ndc   = vec3(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0);
    vec4 world = u_vol.inv_view_proj * vec4(ndc, 1.0);
    return normalize(world.xyz / world.w - u_vol.camera_pos.xyz);
}

void main() {
    ivec2 W_H   = ivec2(u_vol.froxel_grid.xy);
    int   D     = int(u_vol.froxel_grid.z);
    int   cols  = int(u_vol.froxel_grid.w);
    ivec2 px    = ivec2(gl_FragCoord.xy);
    ivec2 tile  = px / W_H;
    int   slice = tile.y * cols + tile.x;
    ivec2 cell  = px - tile * W_H;
    if (slice >= D) {                      // atlas padding past the last slice
        out_froxel = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Per-frame jitter. xy walks an R2 sequence (like a TAA jitter), shifted by a fixed
    // per-froxel offset: each froxel still covers its footprint evenly over time, but
    // neighbours sample different sub-froxel positions in the same frame, so the per-frame
    // error is uncorrelated noise rather than a grid-wide shift that shows the cell structure.
    // Depth uses IGN, which already varies froxel to froxel and frame to frame.
    int   frame    = int(u_vol.time_params.z);
    vec2  seed     = vec2(cell) + vec2(float(slice) * 13.0, float(slice) * 7.0);
    vec2  cell_off = vec2(gfx_volume_ign(seed, 0), gfx_volume_ign(seed.yx + vec2(37.0, 61.0), 0));
    const vec2 R2  = vec2(0.7548776662, 0.5698402910);
    vec2  jxy      = fract(vec2(0.5) + cell_off + float(frame & 255) * R2);
    float jz       = gfx_volume_ign(seed, frame);

    vec3  A  = u_vol.camera_pos.xyz;
    float d0 = vol_froxel_boundary_dist(float(slice));
    float d1 = vol_froxel_boundary_dist(float(slice + 1));
    float dt = d1 - d0;

    vec3 view_dir = vol_froxel_view_dir((vec2(cell) + jxy) / vec2(W_H));
    vec3 p        = A + view_dir * mix(d0, d1, jz);

    // --- Temporal reprojection ---
    // History stores each froxel's running AVERAGE, so it is reprojected from the froxel's
    // centre (as Unreal does), not from this frame's jittered point. Reprojecting the jittered
    // point reads history up to half a froxel off-centre, i.e. a random shift-and-blur every
    // frame, and with per-froxel jitter that turns into spatial noise. For a still camera the
    // centre maps exactly onto the same history texel.
    float w       = clamp(u_vol.froxel_params.z, 0.0, 0.99);
    vec4  history = vec4(-1.0);
    if (w > 0.0 && u_vol.froxel_params.w > 0.5) {
        vec3 centre_dir = vol_froxel_view_dir((vec2(cell) + 0.5) / vec2(W_H));
        history = vol_froxel_history(A + centre_dir * vol_froxel_boundary_dist(float(slice) + 0.5));
    }

    vec4 current = vol_froxel_evaluate(p, view_dir, dt);
    if (history.a >= 0.0) {
        current = mix(current, history, w);
    } else {
        // History miss (newly revealed by camera motion, or no history at all): there is
        // nothing to average this frame's single sample with, so take extra jittered samples
        // instead -- Unreal's r.VolumetricFog.HistoryMissSupersampleCount. Without this every
        // revealed froxel starts as one raw sample, which reads as a noisy grid in motion.
        int extra = clamp(int(u_vol.froxel_params2.x), 1, 16) - 1;
        for (int k = 1; k <= 16; ++k) {
            if (k > extra) break;
            vec2  jxy_k = fract(jxy + float(k) * R2);
            float jz_k  = fract(jz + float(k) * 0.6180339887);
            vec3  dir_k = vol_froxel_view_dir((vec2(cell) + jxy_k) / vec2(W_H));
            current += vol_froxel_evaluate(A + dir_k * mix(d0, d1, jz_k), dir_k, dt);
        }
        current /= float(extra + 1);
    }

    out_froxel = current;
}
