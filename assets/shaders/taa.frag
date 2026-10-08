#version 450

#include <gfx/ycocg.glsl>

// Temporal anti-aliasing resolve.
//
// Inputs are the post-processed LDR scene (sRGB-encoded bytes, rasterised with this frame's
// sub-pixel projection jitter), the accumulation history (rgb = accumulated colour, a = sample
// age), the scene depth buffer and, when the caller has one, a velocity image (the G-buffer's
// per-object motion vectors). The resolve reprojects the history through that per-pixel
// motion -- or, without a velocity image, through the camera's frame-to-frame motion alone,
// where dynamic objects are protected from ghosting only by the variance clip -- rejects
// invalid history with YCoCg variance clipping, and accumulates with a per-pixel age so a
// still camera converges to a true multi-jitter average rather than orbiting an exponential
// blend.
//
// The output target is RGBA16F: the age channel needs more than 8 bits of alpha, and the
// accumulated colour needs sub-LSB precision or the shrinking late-accumulation increments
// would round to zero and stall convergence.

layout(location = 0) in vec2 frag_uv;

layout(set = 0, binding = 0) uniform sampler2D tex_scene;    // LINEAR; this frame, jittered
layout(set = 0, binding = 1) uniform sampler2D tex_history;  // LINEAR; rgb = colour, a = age
layout(set = 0, binding = 2) uniform sampler2D tex_depth;    // NEAREST; scene depth, sky = 1
// NEAREST; the G-buffer's velocity attachment (gfx/surface/gbuffer_fs.glsl): xy = this
// surface's uv motion since last frame, unjittered-to-unjittered. A placeholder when
// pc.use_velocity is 0.
layout(set = 0, binding = 3) uniform sampler2D tex_velocity;
// LINEAR; the REACTIVE mask (r = 0..1): how much to trust this frame over history -- particles
// (rain, snow, sparks) write it, since nothing describes their motion to the reprojection. A
// placeholder when pc.use_reactive is 0.
layout(set = 0, binding = 4) uniform sampler2D tex_reactive;

layout(push_constant) uniform PushConstants {
    mat4  reproject;        // prev UNjittered view-proj * inverse(current JITTERED view-proj)
    vec2  resolution;       // render extent in pixels
    vec2  jitter_ndc;       // this frame's jitter as the NDC displacement it applied
    float feedback_still;   // history weight the accumulation converges to at zero velocity
    float feedback_motion;  // history weight floor under fast motion
    float velocity_scale;   // feedback reaches its motion floor at ~100/scale px of velocity
    float sharpness;        // motion-gated high-frequency restore, 0 disables
    float variance_gamma;   // clip box half-width in standard deviations, under motion
    int   history_valid;    // 0 until both a history image and a previous matrix exist
    int   use_velocity;     // 1: tex_velocity carries per-object motion vectors (see above)
    int   use_reactive;     // 1: tex_reactive holds this frame's reactive mask
} pc;

layout(location = 0) out vec4 out_color;

// Maximum stored sample age (exactly representable in the 16F alpha channel). feedback_still
// implies the effective cap; this is only the storage ceiling.
const float kMaxAge = 255.0;

// Extra age added per frame as velocity reaches zero. At rest the reprojected history is
// exactly valid, so weighting it up faster than the honest 1/(n+1) schedule trades nothing
// visible for a much shorter post-stop settle tail (the image_settles_after_camera_stops
// contract). Self-limiting under motion: the motion-side age cap still applies first.
const float kRestAgeBoost = 15.0;

// NDC xy <-> UV, matching ssr_common.glsl's convention (Y flips between the two spaces).
vec2 ndc_to_uv(vec2 ndc) { return vec2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5); }
vec2 uv_to_ndc(vec2 uv)  { return vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0); }

// Clips `h` toward the box centre along the segment centre->h (Salvi): unlike a per-axis
// clamp, the result stays on the line toward the history sample, so a rejected ghost slides
// toward plausible colours instead of snapping to a box corner.
// Also returns, via `units_out`, how far outside the box `h` was (1.0 = on the surface).
vec3 clip_to_aabb(vec3 centre, vec3 extent, vec3 h, out float units_out) {
    vec3 d = h - centre;
    vec3 units = abs(d) / max(extent, vec3(1e-5));
    float m = max(units.x, max(units.y, units.z));
    units_out = m;
    return m > 1.0 ? centre + d / m : h;
}

// 9-tap Catmull-Rom expressed as 5 bilinear fetches (Jimenez, SIGGRAPH 2016). A plain
// bilinear history tap low-passes the accumulation a little more every reprojected frame;
// Catmull-Rom keeps it sharp. At an exact texel centre the weights degenerate to the single
// centre tap, so a still camera resamples nothing.
vec4 sample_history_catmull_rom(vec2 uv) {
    vec2 res       = pc.resolution;
    vec2 sample_pos = uv * res;
    vec2 tex_pos1   = floor(sample_pos - 0.5) + 0.5;
    vec2 f = sample_pos - tex_pos1;

    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / w12;

    vec2 tex_pos0  = (tex_pos1 - 1.0) / res;
    vec2 tex_pos3  = (tex_pos1 + 2.0) / res;
    vec2 tex_pos12 = (tex_pos1 + offset12) / res;

    vec4 result = vec4(0.0);
    result += texture(tex_history, vec2(tex_pos0.x,  tex_pos0.y))  * w0.x  * w0.y;
    result += texture(tex_history, vec2(tex_pos12.x, tex_pos0.y))  * w12.x * w0.y;
    result += texture(tex_history, vec2(tex_pos3.x,  tex_pos0.y))  * w3.x  * w0.y;
    result += texture(tex_history, vec2(tex_pos0.x,  tex_pos12.y)) * w0.x  * w12.y;
    result += texture(tex_history, vec2(tex_pos12.x, tex_pos12.y)) * w12.x * w12.y;
    result += texture(tex_history, vec2(tex_pos3.x,  tex_pos12.y)) * w3.x  * w12.y;
    result += texture(tex_history, vec2(tex_pos0.x,  tex_pos3.y))  * w0.x  * w3.y;
    result += texture(tex_history, vec2(tex_pos12.x, tex_pos3.y))  * w12.x * w3.y;
    result += texture(tex_history, vec2(tex_pos3.x,  tex_pos3.y))  * w3.x  * w3.y;
    // The corner weights are slightly negative (that is what makes it sharpening); the sum of
    // all nine is 1, so no renormalisation is needed, but age can dip epsilon-negative.
    result.a = max(result.a, 0.0);
    return result;
}

void main() {
    vec2 texel_size = 1.0 / pc.resolution;

    // -----------------------------------------------------------------------
    // Current frame: centre + 3x3 neighbourhood, and its YCoCg moments.
    // -----------------------------------------------------------------------
    vec3 s0 = textureOffset(tex_scene, frag_uv, ivec2(-1,  1)).rgb;
    vec3 s1 = textureOffset(tex_scene, frag_uv, ivec2( 0,  1)).rgb;
    vec3 s2 = textureOffset(tex_scene, frag_uv, ivec2( 1,  1)).rgb;
    vec3 s3 = textureOffset(tex_scene, frag_uv, ivec2(-1,  0)).rgb;
    vec3 s4 = texture      (tex_scene, frag_uv).rgb;
    vec3 s5 = textureOffset(tex_scene, frag_uv, ivec2( 1,  0)).rgb;
    vec3 s6 = textureOffset(tex_scene, frag_uv, ivec2(-1, -1)).rgb;
    vec3 s7 = textureOffset(tex_scene, frag_uv, ivec2( 0, -1)).rgb;
    vec3 s8 = textureOffset(tex_scene, frag_uv, ivec2( 1, -1)).rgb;

    vec3 y0 = gfx_rgb_to_ycocg(s0), y1 = gfx_rgb_to_ycocg(s1), y2 = gfx_rgb_to_ycocg(s2);
    vec3 y3 = gfx_rgb_to_ycocg(s3), y4 = gfx_rgb_to_ycocg(s4), y5 = gfx_rgb_to_ycocg(s5);
    vec3 y6 = gfx_rgb_to_ycocg(s6), y7 = gfx_rgb_to_ycocg(s7), y8 = gfx_rgb_to_ycocg(s8);

    vec3 m1 = y0 + y1 + y2 + y3 + y4 + y5 + y6 + y7 + y8;
    vec3 m2 = y0*y0 + y1*y1 + y2*y2 + y3*y3 + y4*y4 + y5*y5 + y6*y6 + y7*y7 + y8*y8;
    vec3 mean  = m1 / 9.0;
    vec3 sigma = sqrt(max(m2 / 9.0 - mean * mean, vec3(0.0)));

    if (pc.history_valid == 0) {
        out_color = vec4(s4, 1.0);
        return;
    }

    // -----------------------------------------------------------------------
    // Reprojection, from the closest depth in the 3x3 (velocity dilation: at a
    // silhouette the FOREGROUND's motion is the one the eye tracks, and taking
    // the nearest neighbour extends its velocity one pixel past its edge).
    // -----------------------------------------------------------------------
    float d0 = textureOffset(tex_depth, frag_uv, ivec2(-1,  1)).r;
    float d1 = textureOffset(tex_depth, frag_uv, ivec2( 0,  1)).r;
    float d2 = textureOffset(tex_depth, frag_uv, ivec2( 1,  1)).r;
    float d3 = textureOffset(tex_depth, frag_uv, ivec2(-1,  0)).r;
    float d4 = texture      (tex_depth, frag_uv).r;
    float d5 = textureOffset(tex_depth, frag_uv, ivec2( 1,  0)).r;
    float d6 = textureOffset(tex_depth, frag_uv, ivec2(-1, -1)).r;
    float d7 = textureOffset(tex_depth, frag_uv, ivec2( 0, -1)).r;
    float d8 = textureOffset(tex_depth, frag_uv, ivec2( 1, -1)).r;

    float closest = d4; vec2 closest_off = vec2(0.0);
    if (d0 < closest) { closest = d0; closest_off = vec2(-1.0,  1.0); }
    if (d1 < closest) { closest = d1; closest_off = vec2( 0.0,  1.0); }
    if (d2 < closest) { closest = d2; closest_off = vec2( 1.0,  1.0); }
    if (d3 < closest) { closest = d3; closest_off = vec2(-1.0,  0.0); }
    if (d5 < closest) { closest = d5; closest_off = vec2( 1.0,  0.0); }
    if (d6 < closest) { closest = d6; closest_off = vec2(-1.0, -1.0); }
    if (d7 < closest) { closest = d7; closest_off = vec2( 0.0, -1.0); }
    if (d8 < closest) { closest = d8; closest_off = vec2( 1.0, -1.0); }

    vec2 motion_uv   = frag_uv + closest_off * texel_size;
    vec2 motion_ndc  = uv_to_ndc(motion_uv);

    // Previous frame's UNjittered screen position of the surface under motion_uv.
    vec4 prev_clip = pc.reproject * vec4(motion_ndc, closest, 1.0);

    // This frame's UNjittered position of the same surface: the jitter sits in the
    // projection's [2][0]/[2][1] (perspective) or [3][0]/[3][1] (ortho) terms, both of which
    // displace NDC by a depth-independent constant, so removing it is a subtraction.
    vec2 cur_unjit_uv = ndc_to_uv(motion_ndc - pc.jitter_ndc);

    // Behind the previous eye: nothing to reproject to.
    if (prev_clip.w <= 0.0) {
        out_color = vec4(s4, 1.0);
        return;
    }
    vec2 prev_uv = ndc_to_uv(prev_clip.xy / prev_clip.w);

    // Velocity is unjittered-to-unjittered, so a still camera measures exactly zero and the
    // history is fetched at frag_uv verbatim -- the jitter is meant to be INTEGRATED by the
    // accumulation below, not unrolled out of the fetch.
    vec2  velocity    = cur_unjit_uv - prev_uv;
    // Per-object motion vectors, when the caller provides them: the surface's own motion,
    // taken at the same closest-depth neighbour the dilation chose, so a moving object's
    // history is fetched from where that object WAS rather than from where a static point
    // would have been. The sky (depth 1, nothing written to the velocity attachment) keeps
    // the camera-only result above.
    if (pc.use_velocity != 0 && closest < 1.0) {
        velocity = texture(tex_velocity, motion_uv).xy;
    }
    float velocity_px = length(velocity * pc.resolution);

    // Sub-0.05px velocities are indistinguishable from the fp32 noise of the CPU-side matrix
    // inverse (measured ~0.04px at rest); treating them as real would re-sample the converged
    // history at a wandering sub-pixel offset every frame. Genuine drift this slow parks the
    // fetch on the exact texel instead and lets the variance clip absorb the creep.
    if (velocity_px < 0.05) {
        velocity    = vec2(0.0);
        velocity_px = 0.0;
    }
    vec2 history_uv = frag_uv - velocity;

    // Off-screen last frame: the cheapest disocclusion case.
    if (any(lessThan(history_uv, vec2(0.0))) || any(greaterThan(history_uv, vec2(1.0)))) {
        out_color = vec4(s4, 1.0);
        return;
    }

    // 0 at rest -> 1 under fast motion.
    float motion = clamp(velocity_px * pc.velocity_scale * 0.01, 0.0, 1.0);

    // -----------------------------------------------------------------------
    // History fetch, variance clip, and age-weighted accumulation.
    // -----------------------------------------------------------------------
    vec4 history = sample_history_catmull_rom(history_uv);

    // At rest the neighbourhood statistics themselves cycle with the jitter phase, and a
    // converged mean near a thin feature can fall outside one phase's tight box -- the classic
    // TAA flicker. Widening the box when nothing moves accepts the converged value in every
    // phase; motion narrows it back down to keep ghost rejection sharp.
    float gamma = pc.variance_gamma * mix(2.0, 1.0, motion);

    // The extent floor is load-bearing: a flat neighbourhood has sigma == 0, and without the
    // floor any 1-LSB history/current difference there would register as an enormous clip and
    // wipe the accumulation age every frame (measured: median age ~3 instead of the cap).
    // Two 8-bit LSB covers the quantisation band real convergence sits inside.
    float clip_units;
    vec3 extent        = gamma * sigma + vec3(2.0 / 255.0);
    vec3 hist_ycocg    = gfx_rgb_to_ycocg(history.rgb);
    vec3 clipped_ycocg = clip_to_aabb(mean, extent, hist_ycocg, clip_units);
    vec3 clipped_rgb   = gfx_ycocg_to_rgb(clipped_ycocg);

    // Age: how many frames this pixel's accumulation has been valid. Blending at
    // 1/(age+1) makes the still-camera path a TRUE running average -- its increments shrink
    // toward zero, so the periodic jitter integrates to a fixed point instead of the orbit an
    // exponential blend converges to. Motion caps the age so the effective blend never goes
    // below the feedback floor while the camera moves; a hard clip (history outside the
    // variance box) knocks the age back down, since whatever was accumulated no longer
    // describes this surface.
    float cap_still  = clamp(pc.feedback_still,  0.0, 0.996);
    float cap_motion = clamp(pc.feedback_motion, 0.0, 0.995);
    float age_cap = mix(cap_still / (1.0 - cap_still), cap_motion / (1.0 - cap_motion), motion);

    float age = min(history.a, age_cap);
    // A clip during MOTION is disocclusion evidence, so the accumulation restarts in
    // proportion to how far outside the box the history was. A clip at rest is just the
    // extreme phases of the jitter cycle exceeding the box near high-contrast features --
    // resetting the age there would restart accumulation every 8 frames forever (measured as
    // a recurring settle-breaking bump), so at rest the colour is clipped but the age kept.
    if (clip_units > 1.0) age /= mix(1.0, clip_units, motion);

    float alpha = 1.0 / (1.0 + age);

    // Reactive pixels (a particle here now): take the current frame by that much, and restart
    // the age so the next frames do not lean straight back on a history that held the particle
    // -- its old positions are then clipped away as ordinary disocclusion instead of fading out
    // over dozens of frames as a streak. Max over a cross, so the jitter never lets an edge pixel
    // of a thin streak slip outside its own mask.
    float reactive = 0.0;
    if (pc.use_reactive != 0) {
        reactive = max(texture(tex_reactive, frag_uv).r,
                       max(max(textureOffset(tex_reactive, frag_uv, ivec2(1, 0)).r, textureOffset(tex_reactive, frag_uv, ivec2(-1, 0)).r),
                           max(textureOffset(tex_reactive, frag_uv, ivec2(0, 1)).r, textureOffset(tex_reactive, frag_uv, ivec2(0, -1)).r)));
        alpha = mix(alpha, 1.0, clamp(reactive, 0.0, 1.0));
        age *= 1.0 - clamp(reactive, 0.0, 1.0);
    }

    // Inverse-luma weighting (Karis): a bright outlier on either side of the blend gets less
    // say, which converts firefly flicker into a stable slightly-dimmer average.
    float w_cur  = alpha         / (1.0 + y4.x);
    float w_hist = (1.0 - alpha) / (1.0 + clipped_ycocg.x);
    vec3 result = (s4 * w_cur + clipped_rgb * w_hist) / (w_cur + w_hist);

    // Motion-gated sharpening: reprojection's sub-pixel resampling is what softens the image,
    // and it only happens while there IS motion -- at rest the fetch degenerates to the exact
    // texel and needs no compensation (an always-on term would also re-inject the jitter's
    // per-phase high frequencies and break convergence).
    if (pc.sharpness > 0.0) {
        vec3 cross_avg = (s1 + s3 + s5 + s7) * 0.25;
        result += (s4 - cross_avg) * (pc.sharpness * motion);
    }

    float next_age = min(age + 1.0 + kRestAgeBoost * (1.0 - motion), min(age_cap + 1.0, kMaxAge));
    // Reactive: the next frame starts over (age 0 -> it takes that frame outright), so whatever
    // this pixel holds now -- the particle -- is gone the moment the particle moves on.
    next_age = mix(next_age, 0.0, clamp(reactive, 0.0, 1.0));
    out_color = vec4(clamp(result, 0.0, 1.0), next_age);
}
