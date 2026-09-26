#version 450

#include <gfx/ssr_common.glsl>

// Horizon-based ambient occlusion (GTAO -- Jimenez et al., "Practical Realtime Strategies
// for Accurate Indirect Occlusion", 2016) over a dedicated prefiltered depth pyramid
// (ao_depth_downsample.frag: depth-aware weighted average per level, not the SSR chain's
// min() reduction).
//
// Per pixel, a small set of screen-space SLICES (half-planes through the view vector) each
// march the depth pyramid away from the pixel in both directions and track the highest
// horizon angle they see; the slice's occlusion is then the closed-form integral of
// cosine-weighted visibility between the two horizons, and the pixel's AO is the average
// over slices. The estimate is a continuous function of the depth field along each march:
// when the camera turns slightly, horizon angles MOVE rather than flip, unlike a
// point-sample depth comparison, whose fetched surface changes discontinuously wherever a
// sample's projection crosses a silhouette. On high-depth-complexity geometry (this
// engine's terraced terrain) that discontinuity was the dominant AO instability under
// camera motion -- whole faces spiking darker as one patch -- and no temporal resolve can
// tell such a coherent spike from legitimate content. Marching coarser Hi-Z mips at larger
// distances also prefilters far occluders, which both stabilises them further and keeps the
// per-pixel cost flat in radius.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_ao;

// Set 0: Camera UBO (reused from the rest of the deferred pipeline, see camera_ubo.h)
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Set 1: G-Buffer normal/position + the AO depth pyramid (ao_depth_downsample.frag's
// weighted-average reduction; every mip reachable -- HiZPass's sampler sets maxLod to the
// full chain).
layout(set = 1, binding = 0) uniform sampler2D g_normal_metallic;
layout(set = 1, binding = 1) uniform sampler2D g_position_roughness;
layout(set = 1, binding = 2) uniform sampler2D u_hiz_map;

layout(push_constant) uniform SsaoPushConstants {
    float radius;           // world-space gather radius
    float bias;             // world-space offset of the shading point along its normal --
                            // lifts the tangent plane so depth quantisation on a flat
                            // surface cannot register as a horizon (same units and intent
                            // as the old hemisphere formulation's occlusion-test bias)
    float power;
    int   slices;           // horizon slices per pixel (2-3; rotation covers the rest)
    int   steps;            // march steps per slice DIRECTION (two directions per slice)
    // Flattened vec2, house rule: full render-target size in texels.
    float resolution_x;
    float resolution_y;
    // 0 when temporal resolve is disabled (or absent). When it's on, the caller advances
    // this EVERY frame the accumulator is running -- the resolve keeps a per-pixel running
    // average over the rotating draws, so still and moving views show the same estimate --
    // and holds it only once the camera has been still long enough for the resolve to
    // freeze the accumulated image (PixelRenderPipeline's ssao_frozen_).
    int   noise_rotation;
    int   max_mip;          // top usable Hi-Z mip for the march
    float max_radius_px;    // upper clamp on the march extent in render-target pixels
                            // (Unity HDRP's "Maximum Radius in Pixels")
} pc;

const float PI      = 3.14159265359;
const float HALF_PI = 1.57079632679;

/// Fraction of a sampled pyramid cell's world footprint tolerated as in-plane depth
/// variation before it can raise a horizon. The pyramid stores each cell's weighted-average
/// depth but the march reconstructs it at the sampled UV, so a sloped surface viewed at a
/// grazing angle still reads back a phantom bump of up to the cell footprint times the
/// surface slope (the average centers the error where min() biased it fully near, but does
/// not remove it) -- without this the open ground plane accumulates a distance-growing
/// false-occlusion gradient. Scaling the tolerance with the footprint cancels the bump at
/// exactly the scale it occurs, while a real wall -- whose horizon rise dwarfs one cell's
/// footprint -- is barely dented.
const float kMinDepthSlack = 0.75;

/// How strongly an already-passed horizon decays when later, farther samples see open space
/// behind it (per sample, as a mix factor). This is the GTAO thickness heuristic: a real
/// wall keeps re-asserting its horizon at every subsequent step, while a thin silhouette
/// edge -- one step wide, sky behind it -- gets its horizon bled back down instead of
/// occluding everything behind it forever. Thin-feature over-occlusion is view-dependent
/// (WHICH pixels sit just behind a silhouette changes every camera move), so without this
/// the silhouette's one-step horizon reads as darkening that crawls with the camera.
const float kThinOccluderBleed = 0.10;

void main() {
    ivec2 gsize = textureSize(g_position_roughness, 0);
    ivec2 px    = clamp(ivec2(in_uv * vec2(gsize)), ivec2(0), gsize - 1);

    // Background pixels write N = vec3(0) (gbuffer.frag never touches unwritten texels
    // beyond the clear value) -- must check before normalize() to avoid propagating a NaN,
    // the same guard ssr.frag uses at its own early-out.
    vec3 N_w = texelFetch(g_normal_metallic, px, 0).rgb;
    if (dot(N_w, N_w) < 0.001) {
        out_ao = 1.0;
        return;
    }

    // Closed-form inverse of camera.proj for every (ndc.xy, depth) -> view-space
    // reconstruction in this shader -- the shading point below and each marched sample.
    // camera.proj is the JITTERED matrix (the camera UBO intentionally carries it), and the
    // jitter terms live in different cells per projection kind -- proj[2][0/1] for
    // perspectiveRH_ZO (proj[2][3] == -1), proj[3][0/1] for orthographic -- matching the
    // pipeline's apply_taa_jitter_. The scalar form replaces a full mat4 multiply plus vec4
    // perspective divide per reconstruction.
    bool  proj_persp = camera.proj[2][3] != 0.0;
    float inv_p00 = 1.0 / camera.proj[0][0];
    float inv_p11 = 1.0 / camera.proj[1][1];
    float c22 = camera.proj[2][2];
    float c32 = camera.proj[3][2];
    float jx  = camera.proj[2][0];
    float jy  = camera.proj[2][1];
    float tx  = camera.proj[3][0];
    float ty  = camera.proj[3][1];

    // Shading point from the pixel's own NDC and the rasterized depth (pyramid mip 0 is an
    // exact texelFetch copy of the depth buffer, see ao_depth_downsample.frag) -- NOT from
    // the G-buffer position: G2 stores world position in RGBA16F, whose quantization step at
    // world coordinates of a few hundred units is 0.06-0.25 wu -- larger than pc.bias -- so
    // a G2-derived shading point oscillates against the depth-reconstructed marched samples
    // as surfaces cross the half-float lattice, striping the AO along the lattice's
    // iso-lines. Reconstructing both ends of the horizon test through the same inverse
    // removes that offset entirely.
    float depth0 = texelFetch(u_hiz_map, px, 0).r;
    vec2  ndc0   = vec2(in_uv.x * 2.0 - 1.0, -(in_uv.y * 2.0 - 1.0));
    vec3  N_v    = normalize(mat3(camera.view) * normalize(N_w));
    vec3  P_v;
    if (proj_persp) {
        // clip = (p00 x + jx z, p11 y + jy z, c22 z + c32, -z), inverted.
        float z_v = -c32 / (depth0 + c22);
        P_v = vec3(-z_v * (ndc0.x + jx) * inv_p00,
                   -z_v * (ndc0.y + jy) * inv_p11,
                   z_v);
    } else {
        // clip = (p00 x + tx, p11 y + ty, c22 z + c32, 1), inverted.
        P_v = vec3((ndc0.x - tx) * inv_p00,
                   (ndc0.y - ty) * inv_p11,
                   (depth0 - c32) / c22);
    }
    // The tangent-plane lift described at pc.bias.
    P_v += N_v * pc.bias;
    vec3 V = normalize(-P_v);

    // World units per full-res texel at this depth -- converts the world-space radius into
    // a march length in pixels. The clamp keeps a near-camera pixel from marching the whole
    // screen and a far one from collapsing below one step per texel.
    float texel_ws  = ssr_texel_world_size(P_v.z, camera.proj[1][1], pc.resolution_y);
    // The 4 px floor keeps the far-field march from degenerating: below ~4 px the
    // estimate collapses onto a couple of texels, its per-frame jitter redraws dominate, and
    // distant AO dissolves under camera motion. The cost is far creases rendering slightly
    // wider than their exact projection, which reads as legibility rather than error.
    float radius_px = clamp(pc.radius / max(texel_ws, 1e-6), 4.0, pc.max_radius_px);

    // Per-pixel slice rotation: same fract(sin()) hash as the rest of the pipeline's
    // per-pixel angles (inlined per ssr_common.glsl's deliberate-duplication precedent),
    // advanced across frames by the golden angle so consecutive frames decorrelate for the
    // temporal resolve. Full per-pixel entropy shatters any residual estimator bias into
    // single-pixel events, which the bilateral blur and the resolve absorb.
    float hash = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float slice_rot = hash * PI + float(pc.noise_rotation) * 2.39996323;
    // Independent per-pixel/per-frame fraction jittering the first march step, so step
    // banding (all pixels sampling the same ring of distances) never forms.
    float step_jitter = ssr_ign2(gl_FragCoord.xy, pc.noise_rotation).x;

    float p00 = camera.proj[0][0];
    float p11 = camera.proj[1][1];
    vec2  texel_uv = 1.0 / vec2(pc.resolution_x, pc.resolution_y);

    float visibility = 0.0;
    for (int s = 0; s < pc.slices; ++s) {
        float phi = slice_rot + PI * float(s) / float(pc.slices);
        vec2 dir_px = vec2(cos(phi), sin(phi));

        // The slice's view-space in-plane direction matching that screen direction. UV +y
        // is NDC -y (ssr_ndc_to_uv's flip), and NDC maps to the view-space image plane
        // through the projection's diagonal.
        vec3 d_v = normalize(vec3(dir_px.x / p00, -dir_px.y / p11, 0.0));

        // Slice plane basis: B is the plane normal, T the in-plane axis orthogonal to V on
        // the +dir side. The normal projected into the plane gives the arc integral its
        // orientation (gamma) and magnitude (n_len).
        vec3 B = normalize(cross(d_v, V));
        // cross(V, B), not cross(B, V): T must point toward the +dir_px march side, or gamma
        // comes out mirrored and the hemisphere clamps below cut the wrong side of the arc --
        // exact on symmetric geometry, but on a tilted surface it under-integrates visibility
        // in proportion to the tilt, which reads as a grazing-angle darkening gradient across
        // every large flat surface.
        vec3 T = cross(V, B);
        vec3 N_proj = N_v - B * dot(N_v, B);
        float n_len = length(N_proj);
        if (n_len < 1e-4) continue;
        N_proj /= n_len;
        float gamma = atan(dot(N_proj, T), dot(N_proj, V));

        // March both directions, tracking each side's highest horizon as cos(angle from V).
        vec2 horizon_cos = vec2(-1.0);
        for (int side = 0; side < 2; ++side) {
            float sgn = (side == 0) ? 1.0 : -1.0;
            float h = -1.0;
            for (int j = 0; j < pc.steps; ++j) {
                float t = (float(j) + step_jitter) / float(pc.steps);
                // Squared distribution -- half the steps land in the nearest quarter of the
                // radius, where contact occlusion lives -- with a one-texel-per-step linear
                // floor so consecutive early steps never collapse onto the same texel, and a
                // break once past the radius instead of wasted out-of-range taps.
                float d_px = max(t * t * radius_px, float(j) + step_jitter + 1.0);
                if (d_px > radius_px) break;
                vec2 uv_s = in_uv + sgn * dir_px * d_px * texel_uv;
                if (any(lessThan(uv_s, vec2(0.0))) || any(greaterThan(uv_s, vec2(1.0)))) break;

                // Coarser pyramid mips with distance: the fetched value is then a
                // prefiltered occluder over the step's whole footprint (near-surface-
                // weighted average, see ao_depth_downsample.frag) instead of whichever
                // single texel the step happened to land on -- which both stabilises far
                // occluders under camera motion and keeps the taps cache-resident at any
                // radius. The average reduction leaves no per-cell min() bias, so the march
                // may use every level the pyramid built (pc.max_mip); the residual
                // slope-reconstruction error is covered by kMinDepthSlack at every level.
                float mip = clamp(floor(log2(d_px)) - 2.0, 0.0, float(pc.max_mip));
                float depth = textureLod(u_hiz_map, uv_s, mip).r;

                vec2 ndc = vec2(uv_s.x * 2.0 - 1.0, -(uv_s.y * 2.0 - 1.0));
                vec3 S_v;
                if (proj_persp) {
                    // clip = (p00 x + jx z, p11 y + jy z, c22 z + c32, -z), inverted.
                    float z_v = -c32 / (depth + c22);
                    S_v = vec3(-z_v * (ndc.x + jx) * inv_p00,
                               -z_v * (ndc.y + jy) * inv_p11,
                               z_v);
                } else {
                    // clip = (p00 x + tx, p11 y + ty, c22 z + c32, 1), inverted.
                    S_v = vec3((ndc.x - tx) * inv_p00,
                               (ndc.y - ty) * inv_p11,
                               (depth - c32) / c22);
                }

                // The kMinDepthSlack tolerance: lower the candidate along the normal by
                // a fraction of the sampled cell's world footprint before measuring its
                // horizon angle.
                vec3 ds = S_v - P_v - N_v * (kMinDepthSlack * texel_ws * exp2(mip));
                float dist = length(ds);
                if (dist < 1e-4) continue;
                float cos_h = dot(ds, V) / dist;

                // Distance falloff: a candidate's ability to RAISE the horizon fades to
                // zero at the radius, so geometry beyond it never contributes -- the
                // view-independence guarantee the old formulation needed a bounded
                // occluder-thickness term for. When the candidate is BELOW the current
                // horizon, it instead bleeds the horizon down (thin-occluder heuristic,
                // see kThinOccluderBleed).
                float w = clamp(1.0 - dist / pc.radius, 0.0, 1.0);
                h = (cos_h > h) ? mix(h, cos_h, w)
                                : mix(h, cos_h, kThinOccluderBleed);
            }
            horizon_cos[side] = h;
        }

        // Horizon angles about V, clamped to the hemisphere around the projected normal,
        // then the closed-form cosine-weighted visibility of the remaining arc
        // (Jimenez et al. 2016, eq. 3).
        float t1 = gamma + max(-acos(clamp(horizon_cos.y, -1.0, 1.0)) - gamma, -HALF_PI);
        float t2 = gamma + min( acos(clamp(horizon_cos.x, -1.0, 1.0)) - gamma,  HALF_PI);
        float arc = 0.25 * ((-cos(2.0 * t1 - gamma) + cos(gamma) + 2.0 * t1 * sin(gamma))
                          + (-cos(2.0 * t2 - gamma) + cos(gamma) + 2.0 * t2 * sin(gamma)));
        visibility += n_len * arc;
    }

    visibility /= float(pc.slices);
    out_ao = pow(clamp(visibility, 0.0, 1.0), pc.power);
}
