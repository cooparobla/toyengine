#version 450

#include <gfx/ssr_common.glsl>

// Temporal resolve for the raw SSAO output, in the shape of Unreal's GTAO temporal filter:
// the raw pass jitters its sample pattern every frame, and each pixel here keeps a SHORT
// running average of those draws (frame N blends in at 1/(N+1) until the count reaches
// pc.max_accum -- 8 at the High tier, i.e. Unreal's ~0.1 history blend -- then at that fixed
// rate). History is reprojected with the G-buffer's per-object motion vectors (G4, written by
// gfx/surface/gbuffer_fs.glsl), so it follows moving objects as exactly as it follows the
// camera; a pixel whose history belongs to a different surface is detected by the depth the
// history pixel stored against the depth this surface HAD last frame, and accepted history is
// variance-clipped against the current 3x3 neighbourhood so a stale value (the occlusion an
// object cast before it moved away) can only survive within the noise band of what the
// estimator currently sees there. Together those three keep AO from trailing or sticking
// behind moving objects, which a camera-only reprojection with a deep history could not.
//
// Once the caller reports that the camera AND the scene have been still long enough
// (pc.frozen), accepted-history pixels are held verbatim, which is what makes a resting image
// byte-static without converging onto a single noisy draw. A frame in which anything moved
// never freezes (PixelRenderPipeline's scene_moved_), so a still camera watching a moving
// object keeps resolving every frame, the way Unreal does.

layout(location = 0) in  vec2 in_uv;
// R = resolved AO. G = this pixel's linear view depth, carried so that next frame's resolve
// can tell whether the history it reprojects onto belongs to the same surface. B = the
// accumulation count described above. The render target and history image are RGBA16F for
// these channels (see SsaoPass::kResolveFormat). The blur pass reads .r and .b.
layout(location = 0) out vec4 out_ao;

layout(set = 0, binding = 0) uniform sampler2D u_ao_current;          // raw AO, NEAREST
layout(set = 0, binding = 1) uniform sampler2D u_ao_history;          // LINEAR (reprojected UV)
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;  // NEAREST -- only textureSize()
layout(set = 0, binding = 3) uniform sampler2D g_normal_metallic;     // NEAREST -- background test
// The rasterized depth (the AO pyramid's level 0): the camera-only fallback reprojection
// (pc.use_velocity == 0) reconstructs this pixel's clip position from it, never from the
// RGBA16F G-buffer position, whose quantisation at world coordinates of a few hundred units
// is a multi-pixel UV error at close range.
layout(set = 0, binding = 4) uniform sampler2D u_depth;
// G4, the velocity attachment (NEAREST): xy = this surface's screen motion since last frame in
// UV units (unjittered-to-unjittered), z = its linear view depth LAST frame (-1 when it was
// behind the eye), w = its linear view depth this frame. Bound to a placeholder when the
// caller has no velocity image (pc.use_velocity == 0); only .w is read then, which the
// placeholder does not provide -- see the depth reconstruction in that path.
layout(set = 0, binding = 5) uniform sampler2D g_velocity;

/// Floor on the variance clip's half-width (AO units, ~5 levels of an R8 target): a flat raw
/// neighbourhood has sigma 0, and without the floor any sub-level history/current difference
/// there would count as a clip and keep the converged average from ever being accepted whole.
const float kSigmaFloor = 0.02;

/// Depth agreement required to accept history, as a fraction of the depth itself: fp16
/// storage of the depth channel plus the LINEAR history tap blending two depths together both
/// scale with depth. Tight enough that a silhouette-crossing tap is rejected, which is exactly
/// where blended AO would be wrong too.
const float kDepthTolerance = 0.05;

// Diagnostic build flag: when defined, every path overrides the resolved AO channel with
// this pixel's accumulation count (b / max_accum), so debug_view: ssao shows the count
// field instead of AO. The count dynamics stay faithful under the override -- .b evolution
// never reads .r. Capture-only; never ship with this defined.
// #define COUNT_DEBUG

layout(push_constant) uniform PushConstants {
    // Camera-only fallback (pc.use_velocity == 0): maps this frame's clip space to the
    // PREVIOUS frame's -- prev jittered (proj * view) times the inverse of the current
    // jittered (proj * view), composed in double precision on the CPU (the same scheme
    // TaaPass's reprojection uses) so no large world coordinate and no catastrophic float
    // cancellation ever enters the shader math.
    mat4  reproject;
    // Flattened vec2, matching the house rule in ssao.frag's SsaoPushConstants.
    float resolution_x;
    float resolution_y;
    // Accumulation-count cap -- see the file doc. 0 degenerates this pass into a passthrough
    // of the current frame (how ssao_temporal_enabled == false is implemented).
    float max_accum;
    int   history_valid;    // 0 until both a history image and a previous matrix exist
    // Nonzero once the camera and the scene have been still long enough for the average to
    // top up: hold accepted history verbatim so the resting image is byte-static.
    int   frozen;
    // 1 = same resolution as the G-buffer; 2 = half resolution (ssao_half_res): fragment h
    // reads G-buffer texel 2h + 1, as ssao.frag does. The AO images themselves (current,
    // history) are this pass's own resolution, so their reads are unchanged.
    int   gbuffer_scale;
    int   use_velocity;     // 1: g_velocity is the G-buffer's velocity attachment (see above)
    float variance_gamma;   // history clip half-width, std devs of the 3x3 raw neighbourhood
} pc;

/// Catmull-Rom resample of the history AO value (9 bilinear taps). A plain bilinear history
/// read acts as a low-pass filter applied once per frame, and under sustained camera motion
/// that repeated subpixel resampling erodes the pixel-scale AO detail the accumulator exists
/// to preserve -- most visibly at vista distances where a crease is one or two pixels wide.
/// Negative-lobe overshoot is bounded by the variance clip in main().
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

void emit(float ao, float depth, float count) {
    out_ao = vec4(ao, depth, count, 0.0);
#ifdef COUNT_DEBUG
    out_ao.r = count / max(pc.max_accum, 1.0);
#endif
}

void main() {
    float current = texture(u_ao_current, in_uv).r;

    // The G-buffer texel this fragment stands for, and its UV (identical to in_uv at scale 1).
    ivec2 gsize = textureSize(g_position_roughness, 0);
    ivec2 gpx   = clamp(ivec2(gl_FragCoord.xy) * pc.gbuffer_scale + (pc.gbuffer_scale - 1), ivec2(0), gsize - 1);
    vec2  uv0   = (pc.gbuffer_scale <= 1) ? in_uv : (vec2(gpx) + 0.5) / vec2(gsize);

    // Background: the raw pass already wrote 1.0 here and there is no surface to reproject.
    // Depth 0 in .g: any real surface that later reprojects onto this pixel fails the depth
    // test below outright, which is correct -- there is no history for it here.
    vec3 N = texelFetch(g_normal_metallic, gpx, 0).rgb;
    if (dot(N, N) < 0.001) {
        emit(current, 0.0, 0.0);
        return;
    }

    // This pixel's own clip position from the rasterized depth: the fallback reprojection's
    // input, and the depth channel's source when no velocity image carries it.
    float depth = texelFetch(u_depth, gpx, 0).r;
    vec2  ndc   = vec2(uv0.x * 2.0 - 1.0, -(uv0.y * 2.0 - 1.0));

    // Where this surface was last frame, how deep it was then, and how deep it is now.
    vec2  prev_uv;
    float prev_depth;
    float cur_depth;
    if (pc.use_velocity != 0) {
        vec4 vel = texelFetch(g_velocity, gpx, 0);
        // Sub-0.05px motion is indistinguishable from the fp32 noise of re-projecting an
        // interpolated world position; treating it as real would resample the converged
        // history at a wandering sub-pixel offset every frame (same dead-zone as taa.frag).
        vec2 v = vel.xy;
        if (length(v * vec2(pc.resolution_x, pc.resolution_y) * float(pc.gbuffer_scale)) < 0.05) v = vec2(0.0);
        prev_uv    = uv0 - v;
        prev_depth = vel.z;     // exact for a moving object: its own pose last frame
        cur_depth  = vel.w;
    } else {
        vec4 prev_clip = pc.reproject * vec4(ndc, depth, 1.0);
        prev_uv    = ssr_ndc_to_uv(prev_clip.xy / prev_clip.w);
        prev_depth = prev_clip.w;   // == previous linear view depth for a perspective camera
        // This pass binds no camera UBO, so the fallback has no exact current depth to store;
        // the previous one stands in. Next frame's test compares it against ITS prev_clip.w
        // for this surface, and the two differ by one frame of camera motion -- far inside
        // kDepthTolerance for the camera-only fallback's purpose (toyengine itself always
        // binds the velocity image; see SsaoPass::update_descriptors()).
        cur_depth  = prev_depth;
    }

    // The CURRENT frame's raw neighbourhood. The raw estimate is per-pixel white noise by
    // design (ssao.frag's fully per-pixel slice rotation and step jitter), so statistics taken
    // straight from it describe that noise, not the occlusion field: a clip box built from
    // nine raw draws is as wide as the estimator's variance and bounds nothing. Unreal's GTAO
    // runs its spatial filter BEFORE its temporal filter for exactly this reason. The
    // equivalent here, without reordering the passes: read the 5x5 window once, form the nine
    // overlapping 3x3 means inside it (each a 9-draw average, noise cut ~3x), and take the
    // clip box's centre and width from THOSE -- the spread of the smoothed field across the
    // window, i.e. real local occlusion structure plus a third of the noise.
    //
    // The centre 3x3 mean is also every no-history path's fallback: a pixel with no usable
    // history -- fresh disocclusions along exactly the creases AO darkens -- would otherwise
    // show one raw estimate at the estimator's full variance, and the bilateral blur still
    // runs after this.
    vec2 texel_size = 1.0 / vec2(pc.resolution_x, pc.resolution_y);
    float raw[25];
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            raw[(y + 2) * 5 + (x + 2)] = texture(u_ao_current, in_uv + vec2(float(x), float(y)) * texel_size).r;
        }
    }
    float m1 = 0.0;
    float m2 = 0.0;
    float mean = 0.0;   // the centre 3x3 mean
    for (int cy = 1; cy <= 3; ++cy) {
        for (int cx = 1; cx <= 3; ++cx) {
            float sub = 0.0;
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) sub += raw[(cy + y) * 5 + (cx + x)];
            }
            sub /= 9.0;
            if (cx == 2 && cy == 2) mean = sub;
            m1 += sub;
            m2 += sub * sub;
        }
    }
    float smooth_mean  = m1 / 9.0;
    float smooth_sigma = sqrt(max(m2 / 9.0 - smooth_mean * smooth_mean, 0.0));

    // Temporal resolve disabled: the passthrough the file doc promises, with no neighbourhood
    // blending at all so a single draw is reproducible frame to frame.
    if (pc.max_accum <= 0.0) {
        emit(current, cur_depth, 1.0);
        return;
    }
    if (pc.history_valid == 0) {
        emit(mean, cur_depth, 1.0);
        return;
    }

    // Disocclusion. Behind the previous eye, off-screen last frame, or history.g (the depth the
    // history pixel's surface had) disagreeing with the depth THIS surface had last frame: the
    // history belongs to a different surface (this point was occluded, the occluder moved away,
    // an object moved in front, or the reprojection landed across a silhouette -- the history
    // sampler is LINEAR, so its depth channel blends across depth edges and trips this test
    // there, which is exactly where blended AO would be wrong too). A rejected pixel restarts
    // its accumulation at count 1.
    bool reject = prev_depth <= 0.0
               || any(lessThan(prev_uv, vec2(0.0))) || any(greaterThan(prev_uv, vec2(1.0)));
    vec4 history_sample = texture(u_ao_history, prev_uv);
    reject = reject || abs(history_sample.g - prev_depth) > kDepthTolerance * prev_depth;
    if (reject) {
        emit(mean, cur_depth, 1.0);
        return;
    }

    // Camera and scene still, average topped up: hold the accumulated value verbatim.
    // texelFetch, not the bilinear tap above: a bilinear read an epsilon off the texel centre
    // plus the RGBA16F round-trip oscillates the held value by one ulp per frame -- sub-level
    // dither that never lets the image reach byte-static. The exact fetch copies the texel
    // bit-for-bit, so the held image is the many-frame AVERAGE that was on screen while
    // moving, frozen in place.
    if (pc.frozen != 0) {
        vec4 held = texelFetch(u_ao_history, ivec2(gl_FragCoord.xy), 0);
        emit(held.r, cur_depth, held.b);
        return;
    }

    // Accumulate: detail-preserving resample of the history value, variance-clipped against
    // the smoothed current neighbourhood (mean +- gamma sigma of the nine 3x3 means, the
    // TAA-style box ssr_resolve and taa.frag use) rather than bounded by the raw min/max with
    // a slack wider than the AO range itself. A ghost -- occlusion that is no longer there,
    // e.g. the contact darkening a ball cast on the ground before it moved away -- lands well
    // outside the box and is pulled to its edge in one frame. What remains is the box's own
    // half-width, and a history that far outside has demonstrably stopped describing this
    // surface: its accumulation count is cut in proportion to how far outside it was (to
    // zero at a full box-width), so the next frames weight the fresh estimate heavily (the
    // 0.25 ceiling below) instead of fading the remainder out at 1/(cap+1) per frame -- the
    // same role taa.frag's clip-driven age reduction plays. Converged history on a static
    // surface sits inside the box (its centre is a 25-draw average) and keeps its count.
    float hist_ao = sample_history_catmull_rom(prev_uv, vec2(pc.resolution_x, pc.resolution_y));
    float ext     = pc.variance_gamma * smooth_sigma + kSigmaFloor;
    float clipped = clamp(hist_ao, smooth_mean - ext, smooth_mean + ext);
    float outside = abs(hist_ao - clipped);
    hist_ao = clipped;

    float n = min(history_sample.b, pc.max_accum);
    n *= clamp(1.0 - outside / ext, 0.0, 1.0);
    // The 0.25 ceiling caps how much one frame can inject into barely-accumulated pixels
    // (counts 1-3, i.e. the fresh bands a panning camera opens every frame): their noise
    // amplitude halves in exchange for AO fading in over ~4 frames instead of ~2 -- a soft
    // lag that reads far quieter than sparkle. Pixels at count 4+ are unaffected.
    float w = min(1.0 / (n + 1.0), 0.25);
    emit(mix(hist_ao, current, w), cur_depth, n + 1.0);
}
