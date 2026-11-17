#version 450

// The cloud layer's temporal reconstruction (SkyCloudPass), at the cloud region's resolution:
// this frame's march (sky_clouds.frag) traced one pixel of every 2x2 block; every pixel here
// reprojects the accumulated history to where its clouds were last frame -- along its own ray,
// at the clouds' distance, shifted by the wind's drift -- and
//   * every pixel blends that history toward this frame's estimate -- the fresh samples
//     around it, gaussian-weighted -- after clamping it to the range of those samples (so a
//     cloud that moved or changed cannot leave a ghost),
//   * a pixel with no usable history (first frame, off-screen last frame, uncovered by
//     geometry) takes the estimate alone.
// The result is a smooth, converged layer at the full region resolution for a quarter of the
// rays per frame: rgb = in-scattered light, a = packed transmittance + cloud distance + whether
// the pixel's full-resolution block holds sky (sky_cloud_common.glsl). The lighting pass's
// upsample reads it at sky pixels (sky_physical.glsl), cloud_composite.frag over geometry.

#define CLOUD_UBO_SET 0
#define CLOUD_UBO_BINDING 3
#include "sky_cloud_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_trace;     // nearest: this frame's march
layout(set = 0, binding = 1) uniform sampler2D u_history;   // nearest: last frame's result
layout(set = 0, binding = 2) uniform sampler2D g_normal;    // nearest

const float FAR_DEPTH = 60000.0;
const float FRESH_WEIGHT = 0.05;  // this frame's estimate vs the history, for every pixel
const float MOTION_CAP_PX = 2.0;  // the on-screen motion past which the anti-flicker cap stops widening

// Everything below averages, clamps and blends in a tonemapped space (luminance Reinhard, scaled
// by the light's own brightness): a bright, thin silver-lining fringe whose samples swing from
// frame to frame then counts as much as any other pixel instead of dominating the average --
// the classic cause of shimmering edges in temporal resolves (Karis 2014).
float tm_k;
vec4 to_tm(vec4 v) { return vec4(v.rgb / (1.0 + tm_k * dot(v.rgb, vec3(0.2126, 0.7152, 0.0722))), v.a); }
vec4 from_tm(vec4 v) { return vec4(v.rgb / max(1.0 - tm_k * dot(v.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-3), v.a); }

void main() {
    tm_k = 4.0 / max(dot(cf.light_color.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
    ivec2 hp = ivec2(gl_FragCoord.xy);
    vec2 region = cf.trace.zw;
    ivec2 iregion = ivec2(region);
    vec2 uv = (vec2(hp) + 0.5) / region;

    // Whether the full-resolution block holds sky: the sky's upsample only reads such texels
    // (one marched to a surface would show its clipped clouds along every silhouette).
    vec4 nx = textureGather(g_normal, uv, 0);
    vec4 ny = textureGather(g_normal, uv, 1);
    vec4 nz = textureGather(g_normal, uv, 2);
    vec4 n2 = nx * nx + ny * ny + nz * nz;
    bool sky_block = any(lessThan(n2, vec4(0.001)));

    // The fresh samples around this pixel: 3x3 trace texels (2 region pixels apart).
    ivec2 o = ivec2(cf.trace.xy);
    ivec2 tregion = (iregion + 1) / 2;
    ivec2 qc = ivec2(floor(vec2(hp - o) * 0.5 + 0.5));
    bool traced = all(equal((hp - o) & 1, ivec2(0)));
    vec4 m1 = vec4(0.0), m2 = vec4(0.0), sum = vec4(0.0);
    float n = 0.0;
    float wsum = 0.0, best_w = 0.0, depth = FAR_DEPTH;
    // The distance the reprojection below travels: the opacity-weighted MEDIAN of the taps'
    // distances, not the nearest tap and not their mean. The nearest tap alternates with the
    // trace's 2x2 order, so a distance read off one steps every frame; and where the pixel sees
    // two layers at once -- thin haze just over the camera in front of a far bank, the usual
    // state inside a wrapping deck -- a mean lands between them, a distance neither layer is at,
    // and the reprojection then over- or under-applies the camera's translation to both: the
    // layer visibly overshoots while the camera moves and springs back once it stops. The
    // weighted median follows whichever layer dominates the pixel's opacity, which is also the
    // layer dominating its colour. Kept in the log space the channel is encoded in
    // (sky_cloud_common.glsl); a clear tap carries no opacity and so no vote.
    float dlog[9]; float dwt[9]; float dwsum = 0.0; int dn = 0;
    vec4 fresh = vec4(-1.0);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            ivec2 qq = qc + ivec2(dx, dy);
            if (any(lessThan(qq, ivec2(0))) || any(greaterThanEqual(qq, tregion))) continue;
            vec4 s = texelFetch(u_trace, qq, 0);
            if (s.a < 0.0) continue;
            vec4 v = to_tm(vec4(s.rgb, cloud_trans(s.a)));
            m1 += v;
            m2 += v * v;
            n += 1.0;
            vec2 off = vec2(qq * 2 + o - hp);
            float w = exp(-dot(off, off) * 0.35);
            sum += v * w;
            wsum += w;
            if (w > best_w) { best_w = w; depth = cloud_depth(s.a); }
            float dw = w * (1.0 - v.a);
            if (dw > 1e-5) {
                float l = log2(max(cloud_depth(s.a), 50.0));
                int j = dn++;
                for (; j > 0 && dlog[j - 1] > l; --j) { dlog[j] = dlog[j - 1]; dwt[j] = dwt[j - 1]; }
                dlog[j] = l;
                dwt[j] = dw;
                dwsum += dw;
            }
            if (dx == 0 && dy == 0 && traced) fresh = v;
        }
    }
    // With no opacity in any tap every one of them carries FAR_DEPTH, and the nearest tap's own
    // distance serves.
    if (dwsum > 1e-4) {
        float acc = 0.0;
        for (int j = 0; j < dn; ++j) {
            acc += dwt[j];
            if (acc >= 0.5 * dwsum) { depth = exp2(dlog[j]); break; }
        }
    }

    // Reproject: this pixel's ray at the clouds' distance (cloud space -> world), where the
    // clouds were last frame.
    // cf's matrices are camera-relative (SkyCloudPass): rays start at the origin and
    // cf.world_frame.xyz holds the centre relative to the camera, so the large-coordinate
    // cancellations that staircase at float precision never reach this chain.
    vec3 ndc = vec3(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0);
    vec4 wp = cf.inv_view_proj * vec4(ndc, 1.0);
    vec3 ro = vec3(0.0);
    vec3 rd = normalize(wp.xyz / wp.w);
    vec3 p = ro + rd * (depth * cf.look.x);
    // Where that point's cloud was LAST frame. A flat world's deck slid across the world, so
    // the history lies one drift vector away; a planet's deck TURNED, so it lies one spin step
    // back around the planet instead. Getting this wrong does not merely ghost: a
    // mis-reprojection raises `motion` below, which loosens the anti-flicker cap and lets the
    // raw per-frame march noise through -- the very sparkle the deck exists to avoid.
    if (cf.world_frame.w > 0.0) {
        vec3 d = p - cf.world_frame.xyz;
        float a = -cf.look.w;                      // this frame's spin step, backwards
        float c = cos(a), sn = sin(a);
        vec3  k = normalize(cf.spin.xyz);
        p = cf.world_frame.xyz + d * c + cross(k, d) * sn + k * (dot(k, d) * (1.0 - c));
    } else {
        p += vec3(cf.history.xy, 0.0);
    }
    vec4 pc = cf.prev_view_proj * vec4(p, 1.0);
    bool hv = cf.history.z > 0.5 && pc.w > 1e-4;
    vec4 hist = vec4(0.0);
    float motion = 0.0;   // region pixels the clouds moved on screen since last frame
    if (hv) {
        vec2 puv = vec2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
        motion = length((puv - uv) * region);
        // Just off-screen last frame (a pixel on the border, the clouds drifting in): the
        // border texel is a far better history than none -- dropping it would snap the pixel to
        // one frame's noisy estimate. Only a camera cut moves the clouds this far.
        vec2 lim = 4.0 / region;
        hv = all(greaterThanEqual(puv, -lim)) && all(lessThanEqual(puv, 1.0 + lim));
        puv = clamp(puv, 0.5 / region, 1.0 - 0.5 / region);
        if (hv) {
            // Bilinear over the history texels that held sky.
            vec2 pp = puv * region - 0.5;
            ivec2 i0 = ivec2(floor(pp));
            vec2 f = pp - vec2(i0);
            float hw = 0.0;
            for (int k = 0; k < 4; ++k) {
                ivec2 d = ivec2(k & 1, k >> 1);
                ivec2 qq = clamp(i0 + d, ivec2(0), iregion - 1);
                vec4 s = texelFetch(u_history, qq, 0);
                if (s.a < 0.0) continue;
                float w = (d.x == 1 ? f.x : 1.0 - f.x) * (d.y == 1 ? f.y : 1.0 - f.y);
                hist += to_tm(vec4(s.rgb, cloud_trans(s.a))) * w;
                hw += w;
            }
            hv = hw > 0.05;
            if (hv) hist /= hw;
        }
    }

    // This frame's estimate: the fresh samples around the pixel, gaussian-weighted (its own
    // sample, when it was traced, weighs most). Every pixel blends toward it by the same amount,
    // traced this frame or not -- blending only the traced pixel would flicker in a 2x2 pattern
    // with a four-frame period wherever no TAA follows to hide it.
    vec4 current = wsum > 0.0 ? sum / wsum : fresh;
    vec4 result;
    if (hv && wsum > 0.0) {
        // Variance clipping (Salvi 2016): the history is held within 4 standard deviations of
        // the fresh samples' mean -- wide, because the reprojection (the clouds' own depth and
        // drift) is accurate and nine sparse, noisy samples make a box that jitters frame to
        // frame: a tight one snaps the history around with it, popping along every edge.
        vec4 mean = m1 / n;
        vec4 sigma = sqrt(max(m2 / n - mean * mean, vec4(0.0)));
        vec4 box = sigma * 4.0 + vec4(vec3(2e-3), 0.01);
        // Moving, every frame's bilinear history read softens the layer a little more: lean on
        // the fresh estimate as the motion grows (TAA's usual trade, blur against noise).
        float w = mix(FRESH_WEIGHT, 0.3, clamp(motion / 6.0, 0.0, 1.0));
        // Camera translation near the layer changes what a pixel legitimately shows -- the
        // paths light takes through the deck, what occludes what -- faster than a far deck
        // ever changes, and that change is invisible to `motion` (the reprojection follows
        // the geometry; it cannot follow the appearance). Lean on the fresh estimate in
        // proportion to the parallax rate at this pixel's OWN cloud distance: the camera's
        // translation per frame (day_fill.w, world m) over the distance the reprojection just
        // travelled, as region px via the pixel's angular size (history.w). Clouds a few dozen
        // metres overhead re-render in a handful of frames instead of dragging a stale image
        // that springs back when the camera stops; the same deck out at the horizon -- several
        // times farther -- keeps its smoothing instead of dropping to a two-frame history,
        // which has only a quarter-rate checkerboard of fresh rays to rebuild from and would
        // show their four-frame-periodic noise as shimmer whenever the camera moves.
        float plx = cf.day_fill.w / max(depth * cf.look.x, 25.0) / max(cf.history.w, 1e-6);
        w = max(w, mix(FRESH_WEIGHT, 0.45, clamp(plx / 0.8, 0.0, 1.0)));
        result = mix(clamp(hist, mean - box, mean + box), current, w);
        // Anti-flicker: while the clouds hold still on screen, a pixel may change by at most
        // ~0.4% of the (tonemapped) range per frame. The slow changes of a still, distant deck
        // -- the sun moving, the weather fading in, the clouds evolving -- all fit under that
        // (the wind's drift is already followed by the reprojection), so on such a deck the cap
        // only ever bites on noise: the sampling noise of a thin, sunlit fringe can then never
        // show as a flicker, whatever the quality tier. On-screen motion raises the cap with it,
        // and so does a sudden change of the light itself (lightning, the sun handing over to
        // the moon) or of the layer's settings (an edit in the editor).
        //
        // Motion widens the cap only up to MOTION_CAP_PX, because past that the march's own
        // per-frame noise is what the extra room admits: fast motion is meant to cost sharpness,
        // which the fresh weight above already trades away, not stability. The two cases that do
        // need the history replaced outright never reach here -- a reprojection landing off-screen
        // takes `current` uncapped below, and the first traced frame has no history at all.
        // The same parallax rate keeps the cap from pinning what the blend above lets move.
        // The parallax contribution is bounded: past one region px per frame the fresh weight
        // above is already saturated, and an unbounded term would push the cap past the whole
        // tonemapped range (1 / tm_k) -- a cap that large is no cap at all, and the march's
        // per-frame noise is exactly what the extra room admits.
        float rate = 0.004 * (1.0 + min(motion, MOTION_CAP_PX) * 4.0) + cf.camera.w * 1.5 + min(plx, 1.0) * 0.15;
        vec4 cap = vec4(vec3(rate / tm_k), rate);
        // A deck close to the camera breaks the slow-change premise above: ordinary camera
        // translation legitimately occludes and reveals its clouds in a handful of frames, and
        // pixels whose history is valid but no longer true (clear sky where a cloud now is, and
        // the reverse) would crawl across that gap at the capped rate -- a band of stale sky
        // hanging off every silhouette while the camera moves. Noise and real change separate
        // cleanly here: noise swings the fresh taps apart (sigma grows with it, keeping the
        // history inside the box), while a real change moves their agreed mean away from the
        // history. So however far the history sits OUTSIDE the taps' own plausible box is
        // evidence of real change, and the cap widens by that much, scaled to cross the
        // remaining gap in a few frames. On a far deck the history stays inside the box and
        // this term is zero.
        cap += max(abs(hist - mean) - box, vec4(0.0)) * 0.35;
        result = hist + clamp(result - hist, -cap, cap);
    } else if (hv) {
        result = hist;
    } else if (wsum > 0.0) {
        result = current;
    } else {
        result = vec4(0.0, 0.0, 0.0, 1.0);
    }
    result = from_tm(result);
    out_color = vec4(max(result.rgb, vec3(0.0)), cloud_pack_sky(depth, result.a, sky_block));
}
