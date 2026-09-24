#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_ao;

layout(set = 0, binding = 0) uniform sampler2D u_ao;                  // resolved AO (post temporal)
layout(set = 0, binding = 1) uniform sampler2D g_normal_metallic;
layout(set = 0, binding = 2) uniform sampler2D g_position_roughness;

layout(push_constant) uniform BlurPushConstants {
    // World-space plane-distance tolerance of the bilateral weight, per spacing unit
    // (ssao_blur_plane_sigma). Deliberately NOT derived from the gather radius: the
    // tolerance must track the GEOMETRY's step scale (what depth gap separates two
    // surfaces), while the radius tracks how far occlusion is gathered -- coupling them
    // makes a wider gather radius silently blend AO across faces the kernel should reject.
    float plane_sigma;
    // Accumulation cap of the temporal resolve (0 when temporal is off). The kernel below
    // widens for pixels whose accumulation count is far below this -- see the spacing doc.
    float max_accum;
    // Camera sweep speed in screen-centre pixels per frame; 0 at rest.
    float motion_px;
} pc;


void main() {
    ivec2 size = textureSize(u_ao, 0);
    ivec2 center_px = clamp(ivec2(gl_FragCoord.xy), ivec2(0), size - 1);

    vec3 Nc = texelFetch(g_normal_metallic, center_px, 0).rgb;
    vec4 resolved = texelFetch(u_ao, center_px, 0);
    float center = resolved.r;

    // Background: nothing to weight against (G2 holds no real surface here) -- pass through,
    // matching ssao.frag's own early-out for the same case.
    if (dot(Nc, Nc) < 0.001) {
        out_ao = center;
        return;
    }
    Nc = normalize(Nc);
    vec3 Pc = texelFetch(g_position_roughness, center_px, 0).rgb;

    // Count-adaptive a-trous dilation: pixels the temporal resolve has barely accumulated
    // (fresh disocclusions -- most of the screen's silhouettes during a fast pan) show the
    // raw estimate at nearly full variance, and that noise is spatially structured enough to
    // survive the plain 8x8 footprint; it reads as AO shimmering wherever the camera moves.
    // Dilating the SAME 64-tap kernel widens the footprint up to ~4x for low-count pixels,
    // trading their noise for brief softness, while converged pixels (count at the cap) keep
    // the tight kernel -- resting and slow-moving detail is untouched. The resolve stores the
    // count in .b (see ssao_resolve.frag); max_accum 0 means temporal is off and every count
    // reads 1, so the spacing is pinned to 1 to keep this pass identical in that mode.
    // Spacing is CONTINUOUS (float, per-tap rounding below), never quantized here: both of
    // its inputs vary smoothly across frames (counts climb one per frame, motion_px decays
    // with the camera smoothing), and an integer spacing turns each threshold crossing into
    // a single frame where all 64 taps of every pixel jump together -- the whole AO field's
    // texture re-renders at once, which reads as a full-screen shimmer "pop" during hand-
    // speed wobble and once or twice right after every camera stop. With float spacing the
    // individual taps shift by one texel at different values, so the footprint morphs
    // instead of snapping.
    float spacing = 1.0;
    if (pc.max_accum > 0.0) {
        float trust = clamp(resolved.b / pc.max_accum, 0.0, 1.0);
        // pow 0.6: the mid-trust band is where most of a panning frame lives (counts climb
        // one per frame from every disocclusion edge), so the curve keeps real dilation
        // there instead of only at count~0.
        spacing = 1.0 + 4.0 * pow(1.0 - trust, 0.6);
    }
    // Velocity widening. The AO field is the frame's only unfiltered pixel-sharp content
    // (albedo edges get SMAA, textures get mips), so during a fast pan its 1-2 px creases
    // strobe against the moving pixel grid -- the change is deterministic aliasing, not
    // noise (sample count and jitter scheduling measurably do nothing to it), so the ONLY
    // remedy is lowering the field's spatial frequency while it moves. The eye cannot
    // resolve pixel-scale detail sweeping tens of pixels per frame, so this softening is
    // invisible in motion; at rest motion_px is 0 and the kernel is bit-identical to the
    // resting one, which the byte-static contracts depend on (spacing 1.0 rounds every
    // tap back to its exact undilated texel).
    spacing += clamp(pc.motion_px / 30.0, 0.0, 2.0);
    spacing = min(spacing, 5.0);

    // Plane-distance sigma scales with the dilation -- a widened footprint has to
    // tolerate proportionally more in-plane depth variation, or the bilateral weights
    // reject the very taps the dilation exists to reach. The base tolerance is the
    // world-space pc.plane_sigma (see its doc on why it is not radius-derived).
    float sigma = max(pc.plane_sigma * spacing, 1e-4);

    // -4..+3 on both axes: a span of 8, which is two whole periods of the 4x4 tile ssao.frag's
    // kernel rotation is drawn from. Covering each phase of that tile an equal number of times is
    // what cancels it, and the cancellation is the point -- the tile is locked to the pixel grid
    // rather than to the surface, so any residual of it sits still on screen while the geometry
    // slides underneath. A surface point then samples a different phase every frame, and its
    // occlusion steps up and down for as long as the camera moves: the AO reads as flashing under
    // motion and as fixed-pattern noise the moment the camera stops. A span that is not a multiple
    // of 4 (a symmetric -2..+2, say) covers one phase more often than the rest and leaves exactly
    // that residual.
    //
    // Two periods rather than one, because the span also sets how much the filter smooths: 64 taps
    // against a single period's 16 measurably lowers the raw estimate's own frame-to-frame noise
    // as well. The AO's strength and shape are unaffected either way -- the weights below decide
    // those, not the tap count.
    float sum  = 0.0;
    float wsum = 0.0;
    for (int y = -4; y <= 3; ++y) {
        for (int x = -4; x <= 3; ++x) {
            ivec2 tap_px = clamp(center_px + ivec2(round(vec2(x, y) * spacing)),
                                 ivec2(0), size - 1);

            vec3 Nt = texelFetch(g_normal_metallic, tap_px, 0).rgb;
            if (dot(Nt, Nt) < 0.001) continue; // background tap -- skip, don't drag AO toward 1.0

            Nt = normalize(Nt);
            vec3 Pt = texelFetch(g_position_roughness, tap_px, 0).rgb;

            // The normal weight relaxes with the dilation: at spacing 1 the strict power
            // preserves crease definition, while a dilated (low-trust) kernel must accept
            // moderately rotated normals or terraced geometry rejects the very taps the
            // dilation exists to reach and fresh regions keep their noise.
            float wn = pow(max(dot(Nc, Nt), 0.0), 16.0 / spacing);
            float d  = dot(Nc, Pt - Pc);
            float wd = exp(-(d * d) / (2.0 * sigma * sigma));
            float w  = wn * wd;

            sum  += texelFetch(u_ao, tap_px, 0).r * w;
            wsum += w;
        }
    }

    out_ao = (wsum > 1e-5) ? (sum / wsum) : center;
}
