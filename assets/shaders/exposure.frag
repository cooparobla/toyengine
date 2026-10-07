#version 450

// Auto-exposure (eye adaptation) metering, into a 1x1 R16F target.
//
// One fragment covers the whole output, so the "grid" below is 256 taps in a
// single invocation -- cheaper than any mip-chain reduction would be at this
// scale, and with no chain to build or barrier.
//
// Metered in LOG luminance, not linear: the eye's response is logarithmic, and
// a linear mean is dominated by the few brightest pixels (one specular
// highlight would darken the whole frame). The geometric mean this produces is
// the standard "average scene luminance" a key-value exposure divides into.
//
// Adaptation is an exponential approach to the target with SEPARATE up/down
// rates, framed in 1 - exp(-dt * speed) so the rate is frame-rate independent:
// the eye darkens quickly walking into sunlight and brightens slowly walking
// into a cave, and a single symmetric rate reads as wrong in both directions.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_exposure;

// Set 0: the HDR image being metered (binding 0) and the previous frame's own
// 1x1 result (binding 1) -- see ExposurePass's ping-pong, which alternates which
// of its two targets is which rather than copying between them.
layout(set = 0, binding = 0) uniform sampler2D u_hdr;
layout(set = 0, binding = 1) uniform sampler2D u_prev_exposure;

layout(push_constant) uniform ExposurePush {
    float dt;            // seconds since the last frame
    float speed_up;      // adaptation rate (1/sec) when the scene gets BRIGHTER
    float speed_down;    // adaptation rate (1/sec) when the scene gets DARKER
    float compensation;  // multiplier on the metered exposure (HDRP's Exposure Compensation)
    float min_exposure;  // clamp on the result, as an exposure multiplier
    float max_exposure;
    int   history_valid; // 0 on the first frame: adopt the target outright, no adaptation ramp
} pc;

// 48x48 = 2304 taps. Dense on purpose: this whole shader runs for ONE pixel, so the
// tap count is nearly free, while the estimator's variance falls as 1/N -- and that
// variance is what decides whether the adaptation can ever come to rest. At 16x16 the
// sub-pixel TAA jitter moved enough scene edges across a sparse grid to swing the
// metered average by several percent every frame, so the EMA chased it forever.
const int   GRID       = 48;
const float KEY_VALUE  = 0.18;  // middle grey: the luminance exposure maps to 0.18
// Floor for the log. NOT a mere divide-by-zero guard: log() is unbounded below, so a
// tap landing on a near-black pixel drags the geometric mean far more than a bright
// one lifts it, and the floor is what bounds that leverage. 1e-3 is ~7.5 stops under
// middle grey -- dark enough to preserve real shadow detail in the average, high
// enough that a single black texel cannot dominate its variance (1e-4 could).
const float LUMA_FLOOR = 1e-3;

void main() {
    float log_sum = 0.0;
    float weight_sum = 0.0;

    for (int y = 0; y < GRID; ++y) {
        for (int x = 0; x < GRID; ++x) {
            vec2 uv = (vec2(x, y) + 0.5) / float(GRID);

            // Centre weighting, as every real metering mode does: the subject is
            // near the middle of frame, and a bright sky band along the top edge
            // should not stop down the whole shot.
            vec2  d = uv - 0.5;
            float w = exp(-dot(d, d) * 4.0);

            vec3 hdr = texture(u_hdr, uv).rgb;
            float luma = dot(hdr, vec3(0.2126, 0.7152, 0.0722));
            log_sum    += log(max(luma, LUMA_FLOOR)) * w;
            weight_sum += w;
        }
    }

    float average  = exp(log_sum / max(weight_sum, 1e-4));
    float target   = clamp(KEY_VALUE / max(average, 1e-4) * pc.compensation,
                           pc.min_exposure, pc.max_exposure);

    if (pc.history_valid == 0) {
        out_exposure = vec4(target, 0.0, 0.0, 1.0);
        return;
    }

    float prev = texture(u_prev_exposure, vec2(0.5)).r;
    // A stale/uninitialised history reads as 0, which would leave the image
    // black until the ramp climbed out; treat it as "no history" instead.
    if (!(prev > 0.0)) {
        out_exposure = vec4(target, 0.0, 0.0, 1.0);
        return;
    }

    float speed = (target > prev) ? pc.speed_up : pc.speed_down;
    float rate  = 1.0 - exp(-max(pc.dt, 0.0) * max(speed, 0.0));

    // Hysteresis on the RELATIVE error, so the adaptation can actually come to rest.
    // The image being metered is the pre-AA frame, so it still carries the per-frame
    // TAA jitter: its log-average luminance never goes exactly constant even with the
    // camera stopped, and an ungated EMA chases that forever -- the whole frame then
    // keeps breathing by a fraction of a level, which is both visible as a slow shimmer
    // and enough to fail the engine's "image settles when the camera stops" contract.
    // Below ~1% error the target is indistinguishable from sensor noise and the rate
    // goes to zero; above ~5% it is a real lighting change and adapts at full speed.
    // A real eye does not chase 1% fluctuations either.
    float rel = abs(target - prev) / max(prev, 1e-4);
    rate *= smoothstep(0.02, 0.10, rel);

    out_exposure = vec4(mix(prev, target, rate), 0.0, 0.0, 1.0);
}
