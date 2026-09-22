#version 450

#include <gfx/ssr_common.glsl>

// Temporal resolve for the raw SSAO output: continuous stochastic accumulation (the scheme
// Unity HDRP's GTAO and Intel's XeGTAO use). The raw pass jitters its sample pattern every
// frame the accumulator is running, and each pixel here maintains a running AVERAGE of those
// draws together with a count of how many frames it holds: frame N blends in at 1/(N+1) until
// the count reaches pc.max_accum, then at that fixed rate. Reprojection is exact for camera
// motion over static geometry, so the average survives movement at full quality -- the moving
// image and the resting image are the SAME many-frame estimate, and stopping the camera
// cannot reveal a different-looking AO. Once the caller reports the camera has been still
// long enough (pc.frozen), accepted-history pixels are held verbatim, which is what makes a
// resting image byte-static without ever converging onto a single noisy draw.

layout(location = 0) in  vec2 in_uv;
// R = resolved AO. G = this pixel's distance to the eye, carried so that next frame's resolve
// can tell whether the history it reprojects onto belongs to the same surface. B = the
// accumulation count described above. The render target and history image are RGBA16F for
// these channels (see SsaoPass::kResolveFormat). The blur pass reads .r and .b.
layout(location = 0) out vec4 out_ao;

layout(set = 0, binding = 0) uniform sampler2D u_ao_current;          // raw AO, NEAREST
layout(set = 0, binding = 1) uniform sampler2D u_ao_history;          // LINEAR (reprojected UV)
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;  // NEAREST
layout(set = 0, binding = 3) uniform sampler2D g_normal_metallic;     // NEAREST -- background test

/// Slack around the current 3x3 neighbourhood range inside which history is trusted as-is.
/// With exact reprojection and the distance-channel rejection below, deeply accumulated
/// history is almost always legitimate; this bound only exists to cap how far a ghost can
/// survive the cases the rejection misses (e.g. its LINEAR distance blending straddling two
/// same-distance surfaces with different occlusion).
const float kClampSlack = 0.25;

layout(push_constant) uniform PushConstants {
    mat4  prev_view_proj;   // previous frame's JITTERED proj * view
    // Flattened vec2, matching the house rule in ssao.frag's SsaoPushConstants.
    float resolution_x;
    float resolution_y;
    // Accumulation-count cap -- see the file doc. 0 degenerates this pass into a passthrough
    // of the current frame (how ssao_temporal_enabled == false is implemented).
    float max_accum;
    int   history_valid;    // 0 until both a history image and a previous matrix exist
    // Flattened vec3s, same rule: the eye position that produced THIS frame's G-buffer, and
    // the one that produced the history frame -- both for the distance channel below.
    float camera_pos_x;
    float camera_pos_y;
    float camera_pos_z;
    float prev_camera_pos_x;
    float prev_camera_pos_y;
    float prev_camera_pos_z;
    // Nonzero once the camera has been still long enough for the average to top up: hold
    // accepted history verbatim so the resting image is byte-static.
    int   frozen;
} pc;

/// Catmull-Rom resample of the history AO value (9 bilinear taps). A plain bilinear history
/// read acts as a low-pass filter applied once per frame, and under sustained camera motion
/// that repeated subpixel resampling erodes the pixel-scale AO detail the accumulator exists
/// to preserve -- most visibly at vista distances where a crease is one or two pixels wide.
/// Negative-lobe overshoot is bounded by the neighbourhood clamp in main().
float sample_history_catmull_rom(vec2 uv, vec2 res) {
    vec2 sample_pos = uv * res;
    vec2 tex_pos1   = floor(sample_pos - 0.5) + 0.5;
    vec2 f  = sample_pos - tex_pos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;
    vec2 p0  = (tex_pos1 - 1.0) / res;
    vec2 p3  = (tex_pos1 + 2.0) / res;
    vec2 p12 = (tex_pos1 + offset12) / res;
    return texture(u_ao_history, vec2(p0.x,  p0.y )).r * (w0.x  * w0.y )
         + texture(u_ao_history, vec2(p12.x, p0.y )).r * (w12.x * w0.y )
         + texture(u_ao_history, vec2(p3.x,  p0.y )).r * (w3.x  * w0.y )
         + texture(u_ao_history, vec2(p0.x,  p12.y)).r * (w0.x  * w12.y)
         + texture(u_ao_history, vec2(p12.x, p12.y)).r * (w12.x * w12.y)
         + texture(u_ao_history, vec2(p3.x,  p12.y)).r * (w3.x  * w12.y)
         + texture(u_ao_history, vec2(p0.x,  p3.y )).r * (w0.x  * w3.y )
         + texture(u_ao_history, vec2(p12.x, p3.y )).r * (w12.x * w3.y )
         + texture(u_ao_history, vec2(p3.x,  p3.y )).r * (w3.x  * w3.y );
}

void main() {
    float current = texture(u_ao_current, in_uv).r;

    // Background: the raw pass already wrote 1.0 here and G2 holds no real surface to
    // reproject, so don't even attempt it. Distance 0 in .g: any real surface that later
    // reprojects onto this pixel fails the distance test below outright, which is correct --
    // there is no history for it here.
    vec3 N = texture(g_normal_metallic, in_uv).rgb;
    if (dot(N, N) < 0.001) {
        out_ao = vec4(current, 0.0, 0.0, 0.0);
        return;
    }

    vec3 P = texture(g_position_roughness, in_uv).rgb;
    float current_dist = length(P - vec3(pc.camera_pos_x, pc.camera_pos_y, pc.camera_pos_z));

    // 3x3 neighbourhood of the CURRENT frame's raw AO: the accepted-history path bounds
    // history against its min/max (kClampSlack), and every no-history path falls back to its
    // MEAN. The mean fallback matters most: a pixel with no usable history -- which under
    // camera motion means the fresh disocclusions that open up every frame, clustered along
    // exactly the creases AO darkens -- would otherwise show one raw estimate at the
    // estimator's full variance. Averaging nine estimates cuts that amplitude by ~3x for one
    // texture tap per neighbour, and the bilateral blur still runs after this.
    vec2 texel_size = 1.0 / vec2(pc.resolution_x, pc.resolution_y);
    float s0 = texture(u_ao_current, in_uv + vec2(-1.0,  1.0) * texel_size).r;
    float s1 = texture(u_ao_current, in_uv + vec2( 0.0,  1.0) * texel_size).r;
    float s2 = texture(u_ao_current, in_uv + vec2( 1.0,  1.0) * texel_size).r;
    float s3 = texture(u_ao_current, in_uv + vec2(-1.0,  0.0) * texel_size).r;
    float s4 = current;
    float s5 = texture(u_ao_current, in_uv + vec2( 1.0,  0.0) * texel_size).r;
    float s6 = texture(u_ao_current, in_uv + vec2(-1.0, -1.0) * texel_size).r;
    float s7 = texture(u_ao_current, in_uv + vec2( 0.0, -1.0) * texel_size).r;
    float s8 = texture(u_ao_current, in_uv + vec2( 1.0, -1.0) * texel_size).r;

    float aabb_min = min(s0, min(s1, min(s2, min(s3, min(s4, min(s5, min(s6, min(s7, s8))))))));
    float aabb_max = max(s0, max(s1, max(s2, max(s3, max(s4, max(s5, max(s6, max(s7, s8))))))));
    float spatial_fallback = (s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + s8) / 9.0;

    if (pc.history_valid == 0) {
        out_ao = vec4(spatial_fallback, current_dist, 1.0, 0.0);
        return;
    }

    vec4 prev_clip = pc.prev_view_proj * vec4(P, 1.0);

    // Behind the previous frame's eye: no history exists for this point at all.
    if (prev_clip.w <= 0.0) {
        out_ao = vec4(spatial_fallback, current_dist, 1.0, 0.0);
        return;
    }

    vec2 prev_uv = ssr_ndc_to_uv(prev_clip.xy / prev_clip.w);

    // Off-screen last frame -- the cheapest and most common disocclusion case.
    if (any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)))) {
        out_ao = vec4(spatial_fallback, current_dist, 1.0, 0.0);
        return;
    }

    // Disocclusion: history.g holds the sampled pixel's distance to the PREVIOUS eye. If this
    // point was what that pixel saw last frame, that distance matches this point's own distance
    // to the previous eye; a mismatch means the history belongs to a different surface (this
    // point was occluded, or the reprojection landed across a silhouette -- the history sampler
    // is LINEAR, so its distance channel blends across depth edges and trips this test there,
    // which is exactly where blended AO would be wrong too). A rejected pixel restarts its
    // accumulation at count 1.
    vec4 history_sample = texture(u_ao_history, prev_uv);
    float prev_dist = length(P - vec3(pc.prev_camera_pos_x, pc.prev_camera_pos_y, pc.prev_camera_pos_z));
    if (abs(history_sample.g - prev_dist) > 0.05 * prev_dist) {
        out_ao = vec4(spatial_fallback, current_dist, 1.0, 0.0);
        return;
    }

    // Still camera, average topped up: hold the accumulated value verbatim. texelFetch, not
    // the bilinear tap above: with a still camera the reprojected UV sits an epsilon off the
    // texel center, and a bilinear read there plus the RGBA16F round-trip oscillates the held
    // value by one ulp per frame -- sub-level dither that never lets the image reach
    // byte-static. The exact fetch copies the texel bit-for-bit, so the held image is the
    // many-frame AVERAGE that was on screen while moving, frozen in place.
    if (pc.frozen != 0) {
        vec4 held = texelFetch(u_ao_history, ivec2(gl_FragCoord.xy), 0);
        out_ao = vec4(held.r, current_dist, held.b, 0.0);
        return;
    }

    // Accumulate: detail-preserving resample of the history value, a loose safety bound
    // against the current neighbourhood, then a running average by stored count.
    float hist_ao = sample_history_catmull_rom(prev_uv, vec2(pc.resolution_x, pc.resolution_y));
    hist_ao = clamp(hist_ao, aabb_min - kClampSlack, aabb_max + kClampSlack);

    float n = min(history_sample.b, pc.max_accum);
    // The 0.25 ceiling caps how much one frame can inject into barely-accumulated pixels
    // (counts 1-3, i.e. the fresh bands a panning camera opens every frame): their noise
    // amplitude halves in exchange for AO fading in over ~4 frames instead of ~2 -- a soft
    // lag that reads far quieter than sparkle. Pixels at count 4+ are unaffected.
    float w = min(1.0 / (n + 1.0), 0.25);
    out_ao = vec4(mix(hist_ao, current, w), current_dist, n + 1.0, 0.0);
}
