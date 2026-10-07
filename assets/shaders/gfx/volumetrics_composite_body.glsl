#ifndef GFX_VOLUMETRICS_COMPOSITE_BODY_GLSL
#define GFX_VOLUMETRICS_COMPOSITE_BODY_GLSL

// gfx/volumetrics_composite_body.glsl -- shared by volumetrics_composite.frag (raymarch
// mode: joint-bilateral upsample of the low-res march) and volumetrics_froxel_apply.frag
// (froxel mode, VOL_FROXEL_APPLY: lookup into the integrated froxel grid). Everything else --
// merged global fog, max-opacity, debug view, the final colour * T + scatter -- is one body.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_color;

// Set 0: scene colour (linear), G-buffer normal/position (nearest -- see the march),
// and the march result (rgb = in-scatter, a = transmittance; read with texelFetch).
layout(set = 0, binding = 0) uniform sampler2D scene_color;
layout(set = 0, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;
layout(set = 0, binding = 3) uniform sampler2D march_result;

#include <gfx/volumetrics_ubo.glsl>

// Set 2: the GLOBAL fog description (FogUBO's layout, field for field -- see fog.frag),
// read only on the merged path (counts.w). Always declared and always bound, so the
// pipeline layout never depends on the runtime flag.
layout(set = 2, binding = 0) uniform FogUBO {
    mat4 inv_view_proj;
    vec4 camera_pos;
    vec4 fog_color;
    vec4 sun_direction;
    vec4 sun_color;
    vec4 mode_density;
    vec4 height_params;
    vec4 misc_params;
    vec4 sky_zenith;
    vec4 sky_horizon;
    vec4 sky_ground;
} u_fog;

#include <gfx/fog.glsl>

// Distance a march ray at `uv` was clipped to. Clamped to the march's max distance:
// past it every ray stops at the same t, so sky and far geometry carry the same march
// result and should blend freely rather than be rejected as a depth edge.
float vol_ray_depth(vec2 uv, vec3 cam_pos) {
    vec3  N      = texture(g_normal_metallic, uv).rgb;
    float d      = (dot(N, N) < 0.001) ? 1e6
                 : distance(cam_pos, texture(g_position_roughness, uv).rgb);
    return min(d, max(u_vol.march_params.y, 0.0));
}

#ifdef VOL_FROXEL_APPLY
#include <gfx/volumetrics_froxel.glsl>

// Integrated grid at fractional depth: atlas entry k holds (scatter, T) from the camera to
// the FAR edge of slice k, so boundary b (0..D) is (0,1) at b = 0 and entry b-1 otherwise;
// lerp between the two boundaries around this distance.
//
// froxel_params2.y > 0 jitters the lookup per pixel and per frame by up to +-jitter/2 froxel
// in xy and slice in depth (Unreal's r.VolumetricFog.UpsampleJitterMultiplier). Plain
// trilinear filtering spreads each froxel's error over a whole 8-px cell, which shows as a
// grid while the history is still converging; the jitter turns that into per-pixel noise
// for TAA to resolve. The pipeline sends 0 when TAA is off. The depth jitter is symmetric,
// so it adds no bias, though it can read up to half a slice past an opaque surface.
//
// IGN with the same R2 frame shift as gfx_volume_ign (gfx/volumetrics.glsl), copied rather
// than included so the composite does not pull in the whole density-field library.
float vol_lookup_ign(vec2 px, int frame) {
    px += 5.588238 * float(frame & 63);
    return fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))));
}

vec4 vol_froxel_lookup(vec2 uv, float dist) {
    float D  = u_vol.froxel_grid.z;
    float fs = vol_froxel_slice_of(dist);          // 0..D, boundary coordinate
    float jitter = u_vol.froxel_params2.y;
    if (jitter > 0.0) {
        int  frame = int(u_vol.time_params.z);
        vec2 px    = gl_FragCoord.xy;
        vec3 n     = vec3(vol_lookup_ign(px, frame),
                          vol_lookup_ign(px + vec2(47.0, 17.0), frame + 21),
                          vol_lookup_ign(px + vec2(19.0, 83.0), frame + 42)) - 0.5;
        uv += n.xy * jitter / u_vol.froxel_grid.xy;
        fs  = clamp(fs + n.z * jitter, 0.0, D);
    }
    float b0 = clamp(floor(fs), 0.0, D);
    float b1 = min(b0 + 1.0, D);
    vec4 v0 = (b0 < 0.5) ? vec4(0.0, 0.0, 0.0, 1.0) : vol_froxel_sample(march_result, uv, int(b0) - 1);
    vec4 v1 = (b1 < 0.5) ? vec4(0.0, 0.0, 0.0, 1.0) : vol_froxel_sample(march_result, uv, int(b1) - 1);
    return mix(v0, v1, clamp(fs - b0, 0.0, 1.0));
}
#endif

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;

    int  volume_count = int(u_vol.counts.x);
    bool merged_fog   = u_vol.counts.w > 0.5;
    bool debug_view   = u_vol.camera_pos.w > 0.5;

    if (volume_count <= 0 && !merged_fog) {
        out_color = vec4(color, 1.0);   // nothing placed -- costs one fetch
        return;
    }

    vec3 cam_pos = u_vol.camera_pos.xyz;

    if (merged_fog) {
        vec3 N      = texture(g_normal_metallic, in_uv).rgb;
        bool is_sky = dot(N, N) < 0.001;
        vec3 ndc      = vec3(in_uv.x * 2.0 - 1.0, 1.0 - in_uv.y * 2.0, 1.0);
        vec4 world    = u_vol.inv_view_proj * vec4(ndc, 1.0);
        vec3 view_dir = normalize(world.xyz / world.w - cam_pos);
        float d_geo   = is_sky ? 1e6 : distance(cam_pos, texture(g_position_roughness, in_uv).rgb);
        color = gfx_fog_apply(color, cam_pos, view_dir, d_geo, is_sky,
                              u_fog.mode_density, u_fog.height_params, u_fog.misc_params,
                              u_fog.fog_color.rgb, u_fog.sun_direction.xyz, u_fog.sun_color.rgb,
                              u_fog.sky_zenith.rgb, u_fog.sky_horizon.rgb, u_fog.sky_ground.rgb);
    }

    if (volume_count <= 0) {
        out_color = vec4(color, 1.0);
        return;
    }

#ifdef VOL_FROXEL_APPLY
    // Froxel mode: one filtered lookup into the integrated grid at this pixel's ray distance
    // (the same clipped distance the march would have stopped at).
    vec4 vol = vol_froxel_lookup(in_uv, vol_ray_depth(in_uv, cam_pos));
    vol.a = clamp(vol.a, 1.0 - clamp(u_vol.march_params.z, 0.0, 1.0), 1.0);   // max opacity, as the march

    // Debug view: how much the medium covers -- 1 - transmittance -- scene colour suppressed.
    if (debug_view) {
        out_color = vec4(vec3(clamp(1.0 - vol.a + dot(vol.rgb, vec3(0.333)), 0.0, 1.0)), 1.0);
        return;
    }
    out_color = vec4(color * vol.a + vol.rgb, 1.0);
}
#else
    // Joint-bilateral upsample of the march.
    ivec2 lo_size = textureSize(march_result, 0);
    vec2  lo_pos  = in_uv * vec2(lo_size) - 0.5;
    ivec2 base    = ivec2(floor(lo_pos));
    vec2  f       = lo_pos - vec2(base);

    float d_full = vol_ray_depth(in_uv, cam_pos);
    // Relative tolerance: a few percent of the distance, so the edge test is as strict
    // up close as far away, plus a small floor for pixels right at the camera.
    float inv_tol = 1.0 / (0.05 * d_full + 0.05);

    vec4  acc      = vec4(0.0);
    float wsum     = 0.0;
    vec4  nearest  = vec4(0.0, 0.0, 0.0, 1.0);
    float best_err = 1e30;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            ivec2 tc = clamp(base + ivec2(i, j), ivec2(0), lo_size - 1);
            vec2  uv = (vec2(tc) + 0.5) / vec2(lo_size);
            vec4  s  = texelFetch(march_result, tc, 0);
            float err = abs(vol_ray_depth(uv, cam_pos) - d_full);
            float wb  = (i == 1 ? f.x : 1.0 - f.x) * (j == 1 ? f.y : 1.0 - f.y);
            float w   = wb * exp(-err * inv_tol);
            acc  += s * w;
            wsum += w;
            if (err < best_err) {
                best_err = err;
                nearest  = s;
            }
        }
    }
    // Every neighbour sits across a depth edge (a thin feature narrower than one
    // low-res texel): take the closest-depth one rather than a meaningless average.
    vec4 vol = (wsum > 1e-4) ? acc / wsum : nearest;

    // Debug view: march coverage alone (carried in .r), scene colour suppressed.
    if (debug_view) {
        out_color = vec4(vec3(vol.r), 1.0);
        return;
    }

    out_color = vec4(color * vol.a + vol.rgb, 1.0);
}
#endif // VOL_FROXEL_APPLY

#endif // GFX_VOLUMETRICS_COMPOSITE_BODY_GLSL
