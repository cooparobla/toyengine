#ifndef TOY_CONTACT_SHADOW_BODY_GLSL
#define TOY_CONTACT_SHADOW_BODY_GLSL

// contact_shadow_body.glsl -- shared toy_contact_shadow() body, extracted out of
// pixel_lighting.frag so debug_view.frag's "contact_shadows" channel reads the EXACT
// same march the shipped lighting term does -- one body, so the diagnostic and the
// shipped term structurally cannot drift apart (the same reasoning
// gfx/indirect_specular.glsl documents for the lighting/SSR-composite pair).
//
// A "body" file in the pixel_shadow_body.glsl/ssr_trace_body.glsl sense: the includer
// must, BEFORE including this file, already have declared (with these exact names,
// whatever their own set/binding indices are):
//   - `camera`               -- CameraUBO { mat4 view; mat4 proj; vec3 camera_pos; }.
//   - `lights`                -- the LightUBO block (contact_params, contact_soft_params,
//                                dir_shadow_extra.w -- see light_data.h's C++ doc).
//   - `g_position_roughness` -- sampler2D.
//   - `g_normal_metallic`    -- sampler2D.
//   - #include <gfx/ssr_common.glsl> (for ssr_texel_world_size(), ssr_ndc_to_uv()).
//
// CALLER CONTRACT for dpdx/dpdy: the screen-space derivatives of `world_pos`, which the
// jitter-stable origin below needs. They are parameters rather than dFdx() calls inside this
// function for two reasons, both of which produced real artifacts when violated:
//   1. They must be taken on a SMOOTHLY INTERPOLATED position -- a `texture()` read through a
//      LINEAR sampler. Differentiating a `texelFetch` result instead gives 0 or a whole texel
//      depending on where the 2x2 quad falls on the texel grid, which turns `corr` below into a
//      screen-aligned checker, corrupts `bias`, and self-intersects the ray: a per-texel hatch
//      over every face that flickers as TAA moves the quad phase.
//   2. Derivatives are undefined under non-uniform control flow, so the caller must evaluate
//      them BEFORE any background early-out, not after.

// Screen-space contact shadows (HDRP's own feature of that name): a short
// G-buffer march from the surface toward the light, catching the
// small-scale occlusion the shadow map's normal-offset bias necessarily
// recedes from (see PixelRenderConfig::shadow_normal_bias's ~2% trade).
//
// Returns the RAW geometric occlusion (0..1) of the march alone -- NOT scaled by
// lights.contact_params.x (strength) or lights.dir_shadow_extra.x (per-light darkness);
// the caller max()-combines and scales those in (see pixel_lighting.frag), so
// shadows_enabled: false + contact_shadows_enabled: true renders a contact-only view,
// and debug_view: contact_shadows can show the term completely unscaled.
//
// The estimator is deliberately SMOOTH and COMB-INDEPENDENT: every fetched
// texel's occlusion is an analytic function of that texel and the ray
// geometry alone (evaluated over the whole ray, at the texel's nearest
// approach to the ray), so WHERE the samples land only decides which texels
// are visited -- with nothing downstream to average per-pixel estimator
// disagreement (unlike the traced SSGI, which has its own denoise), any
// sample-position dependence in the VALUE reads as salt-and-pepper grain
// over every penumbra. The strongest texel wins, and soft fades take the
// result to zero wherever the march is least trustworthy.
float toy_contact_shadow(vec3 world_pos, vec3 N, vec3 V, vec3 L, vec3 dpdx, vec3 dpdy) {
    if (lights.contact_params.x <= 0.0) return 0.0;

    float ndl = dot(N, L);
    // Grazing fade: as N.L -> 0 the ray travels nearly parallel to the surface
    // it started from, so every depth comparison is against that same surface.
    float graze = smoothstep(0.0, 0.15, ndl);
    if (graze <= 0.0) return 0.0;

    ivec2 gsize    = textureSize(g_position_roughness, 0);

    // Jitter-stable march origin. world_pos is the surface sampled at THIS
    // frame's TAA sub-pixel offset, and on a grazing receiver one screen
    // texel spans px_world/N.V world units -- so the raw origin slides a
    // large distance ALONG the surface every frame, translating the whole
    // march onto different occluder texels and flickering the contact line
    // (worst on close, grazing geometry, where the march spans the most
    // texels). The perspective jitter is recoverable from the projection
    // (apply_taa_jitter_ adds it to proj[2][0]/[2][1], where perspectiveRH_ZO's
    // own terms are zero; the NDC displacement is their negation), and dpdx/dpdy
    // -- the caller's screen-space derivatives of world_pos, see this file's
    // CALLER CONTRACT -- are the surface's world footprint per pixel. Together
    // they slide the origin back to the point under the texel CENTRE, the same
    // world point every frame on a planar receiver.
    // This is the origin half of "march in the unjittered frame"; proj_march
    // below is the projection half.
    float px_raw = ssr_texel_world_size(
        (camera.view * vec4(world_pos, 1.0)).z, abs(camera.proj[1][1]),
        float(gsize.y));
    vec3 origin = world_pos;
    if (camera.proj[2][3] != 0.0) {
        vec2 jitter_ndc = vec2(-camera.proj[2][0], -camera.proj[2][1]);
        // ndc-per-pixel: +x pixel = +2/W ndc x; +y pixel (down) =
        // -2/H ndc y (ssr_ndc_to_uv's flip) -- hence the minus.
        vec3 corr = dpdx * (0.5 * float(gsize.x) * jitter_ndc.x)
                  - dpdy * (0.5 * float(gsize.y) * jitter_ndc.y);
        // In-plane only, and capped: where the derivative quad straddles a
        // depth edge the "footprint" is garbage metres of cross-surface
        // distance, not slope.
        corr -= N * dot(N, corr);
        float cap = px_raw * 4.0 / max(abs(dot(N, V)), 0.05);
        if (dot(corr, corr) < cap * cap) origin += corr;
    }

    // px_world (hence bias/ray_len/depth_eps) from the CORRECTED origin's
    // view-z, so the march LENGTH is also stable frame to frame -- otherwise
    // ray_len = px_world*64 tracks the jittered depth and its far-distance
    // fade wobbles on close geometry.
    float view_z   = (camera.view * vec4(origin, 1.0)).z;
    float px_world = ssr_texel_world_size(view_z, abs(camera.proj[1][1]),
                                          float(gsize.y));

    // Start offset sized to the receiver's own screen texel, so the first
    // sample always clears the surface it came from. Divided by N.L because a
    // grazing ray must travel further to gain the same height off its surface,
    // and floored for the near plane where px_world -> 0 (the same floor, for
    // the same reason, as gfx/ssr_trace_body.glsl's).
    float bias = max(px_world * 2.0 / max(ndl, 0.15), 0.002);

    // Clamp the march's SCREEN extent. A fixed world length spans hundreds of
    // texels up close (so the samples straddle them and step over thin
    // occluders) and less than one at range (so every sample lands in the same
    // texel and the whole march is wasted). 64 px is the same order as SSAO's
    // own ssao_max_radius_px cap.
    float ray_len = min(lights.contact_params.y, px_world * 64.0);

    int   steps      = int(max(lights.contact_params.w, 1.0));
    float thickness  = max(lights.contact_params.z, 1e-4);
    float depth_eps  = max(px_world, 0.002);  // depth-precision floor

    // The ray point is stepped in VIEW space for the projection (one mat4
    // multiply per sample) and in WORLD space for the occlusion test below,
    // which measures against the fetched surface's world-space plane.
    vec3 view_o = (camera.view * vec4(origin, 1.0)).xyz;

    // Project the march with the UNJITTERED projection so the sample
    // fetch path does not slide with TAA's sub-pixel offset every frame.
    // apply_taa_jitter_ adds the perspective jitter to proj[2][0]/[2][1],
    // where perspectiveRH_ZO's own terms are zero, so zeroing them
    // recovers the clean projection.
    mat4 proj_march = camera.proj;
    if (camera.proj[2][3] != 0.0) {
        proj_march[2][0] = 0.0;
        proj_march[2][1] = 0.0;
    }

    // Per-pixel, per-frame PHASE for the sample comb: interleaved gradient
    // noise, staggered spatially so neighbouring pixels visit different
    // texels (a G-buffer seam flip then never moves a whole coherent block
    // of TAA's variance box at once), and rotated each frame by the golden
    // ratio -- the same recipe as pixel_shadow_body.glsl's PCF rotation.
    // The rotation lets TAA integrate the sampling pattern to a smooth
    // fixed point rather than leaving a screen-locked comb; a still camera
    // still converges because the per-texel occlusion VALUE is comb-
    // independent (whole-ray, below) -- the comb only decides which texels
    // are visited, and TAA averages that choice out.
    float phase = fract(fract(52.9829189 *
                              fract(0.06711056 * gl_FragCoord.x +
                                    0.00583715 * gl_FragCoord.y))
                        + lights.dir_shadow_extra.w * 0.61803398875);

    // Soft contact shadows: AVERAGE occlusion over K rays spread across the
    // sun's angular cone (half-angle atan(cone_tan), cone_tan = the sun's
    // angular size, shared with the shadow map's PCSS growth dial). Each ray
    // is the same short march, in a slightly rotated direction; the cone
    // widens with distance, so a blocker touching the receiver stays sharp
    // (all rays hit) while one seen far along the ray blocks only part of the
    // cone (a penumbra that grows with blocker distance -- the PCSS relation).
    // Averaging IS what a soft shadow is, and it is far steadier frame to
    // frame than the single-ray max. cone_tan == 0 (soft_shadows off)
    // collapses to one tap with no offset: the hard march, unchanged.
    // The march is short (<=contact_shadow_length, ~0.5 m), so the sun's true
    // angular penumbra over it is sub-centimetre -- physically correct but
    // invisible. Contact shadows are a stylized near-field effect; scale the
    // effective cone so the outer edge reads as soft at the default sun size,
    // still proportional to shadow_pcss_light_size so the dial keeps working.
    const float kContactPenumbraScale = 6.0;
    float cone_tan  = lights.contact_soft_params.x * kContactPenumbraScale;
    int   cone_taps = cone_tan > 0.0 ? 4 : 1;

    // Orthonormal basis spanning the plane perpendicular to the sun.
    vec3 up     = abs(L.z) < 0.9 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 cone_t = normalize(cross(up, L));
    vec3 cone_b = cross(L, cone_t);
    // Per-pixel STATIC rotation of the Vogel disk (no per-frame term: the cone
    // must add no new temporal variance; the along-ray step comb already
    // carries the per-frame stagger TAA integrates).
    float cone_rot = 6.28318530 * fract(52.9829189 *
                        fract(0.11267 * gl_FragCoord.x +
                              0.17383 * gl_FragCoord.y));

    float occ = 0.0;
    for (int c = 0; c < cone_taps; ++c) {
        // Vogel-disk direction for this tap, rotated per pixel. u is the
        // sqrt-spaced disk radius (equal-area), scaled by cone_tan into a
        // small tilt of the sun direction.
        float cu   = cone_tan > 0.0
                   ? sqrt((float(c) + 0.5) / float(cone_taps)) : 0.0;
        float cang = cone_rot + float(c) * 2.39996323;
        vec3  Lc   = normalize(L + (cone_t * (cu * cos(cang)) +
                                    cone_b * (cu * sin(cang))) * cone_tan);
        vec3  view_Lc = mat3(camera.view) * Lc;

        float tap = 0.0;
        for (int s = 0; s < steps; ++s) {
        // Quadratic spacing concentrates samples near the contact point --
        // where this effect lives -- and spends least on the far end, where
        // screen-space data is least reliable. The samples only decide
        // WHICH texels the march visits: each visited texel's occlusion is
        // evaluated analytically over the whole ray below, so the comb's
        // phase does not enter the per-texel value.
        float f = (float(s) + phase) / float(steps);
        float t = ray_len * f * f;
        vec3  p_view  = view_o + view_Lc * (t + bias);

        vec4 clip = proj_march * vec4(p_view, 1.0);
        if (clip.w <= 0.0) break;
        vec2 uv = ssr_ndc_to_uv(clip.xy / clip.w);

        // texelFetch, NOT texture(): this set binds the G-buffer through a
        // LINEAR sampler (harmless for the lighting pass's own texel-centred
        // reads, where linear == nearest), but a march samples at arbitrary
        // UVs, and there bilinear filtering blends world positions ACROSS
        // silhouette edges into positions that exist on no real surface.
        // The G-buffer is discrete per-pixel data; fetch texels. (SsrPass
        // keeps a whole separate nearest sampler for exactly this reason.)
        //
        // The occlusion RESULT, however, IS blended -- a normalized 3x3
        // tent (radius 1.5 texels) around the sample point. Where two
        // surfaces meet on screen (a wall-floor corner, a silhouette),
        // seam texels' rasterized identity flips between the two every
        // frame under TAA's sub-pixel jitter -- one plane registers a
        // hit, the other a miss, and no per-plane test can stabilize
        // that pop. Running the plane test PER TEXEL and blending the
        // scalar hits spatially antialiases the estimate, and the tent's
        // support caps any single flipping texel's weight well below 1,
        // so a seam flip moves the estimate by a fraction instead of
        // toggling it.
        vec2  st = uv * vec2(gsize) - 0.5;
        ivec2 p00 = ivec2(round(st));
        float hit = 0.0;
        float wsum = 0.0;
        for (int j = -1; j <= 1; ++j)
        for (int i = -1; i <= 1; ++i) {
            ivec2 px = clamp(p00 + ivec2(i, j), ivec2(0), gsize - 1);
            vec2  d  = vec2(p00 + ivec2(i, j)) - st;
            float w = max(0.0, 1.5 - abs(d.x)) * max(0.0, 1.5 - abs(d.y));
            if (w <= 0.0) continue;
            vec3 vis_pos = texelFetch(g_position_roughness, px, 0).rgb;
            if (dot(vis_pos, vis_pos) < 1e-6) continue;  // sky: no occluder there
            vec3 vis_n = texelFetch(g_normal_metallic, px, 0).rgb;
            if (dot(vis_n, vis_n) < 0.5) continue;  // unwritten texel: no plane to test

            // Signed distance of the ray point BEHIND the visible surface's
            // PLANE, in world units along its normal -- not a raw view-z
            // difference. A z compare is blind to slope: where the camera
            // grazes a surface, one screen texel of it spans px_world/N.V
            // world units, and neighbouring texels' stored depths differ by
            // more than any fixed tolerance, so the march false-hits the
            // receiver's own surface. Against the plane, every texel of one
            // flat surface measures the same ~0 for a ray point on that
            // surface, whatever the view angle.
            vec3  vis_nn = normalize(vis_n);

            // Signed distance of the ray point BEHIND this texel's plane is
            // LINEAR in t: ahead(t) = A0 - (t + bias) * k. That makes the
            // best (most-occluding) point over the WHOLE ray analytic --
            // clamp the acceptance window's centre into the ray's
            // ahead-range and evaluate there. Whole-ray, not per sample
            // interval: with interval bounds in this expression a texel's
            // contribution depends on WHICH sample interval evaluates it,
            // i.e. on the comb phase -- and near the contact point the
            // comb's step spacing is coarser than the acceptance ramps, so
            // the estimate swings with the per-pixel phase and the
            // penumbra renders as checkerboard grain. Evaluated over
            // [0, ray_len], the value is a function of the fetched texel
            // and the ray geometry alone; the samples only decide which
            // texels are visited, and revisiting one is idempotent under
            // the max below.
            float A0 = dot(vis_nn, vis_pos - origin);
            float k  = dot(vis_nn, Lc);
            float a_start = A0 - bias * k;
            float a_end   = A0 - (ray_len + bias) * k;
            float a_min = min(a_start, a_end);
            float a_max = max(a_start, a_end);
            float ahead = clamp(0.5 * (depth_eps + thickness * 1.5), a_min, a_max);

            // Locality: `ahead` measures distance behind the texel's PLANE,
            // and a plane is infinite -- on gridded terrain every distant
            // wall's extended plane slices through open floor, and a ray
            // whose screen path crosses that wall's texels would take a
            // phantom soft hit meters away from any real occluder (the
            // mid-floor blotch field). A real occluder's fetched surface
            // POINT lies within the thickness scale of the RAY, so gate on
            // the point's distance to the ray at its nearest approach.
            // That nearest-approach parameter is analytic in the fetched
            // point alone -- NOT a function of which sample interval is
            // being evaluated. Evaluating this distance at a per-sample t
            // ties the prox ramp to the sample comb, and the comb's step
            // spacing near the contact point is coarser than the ramp
            // itself -- the estimate then swings with the comb's phase,
            // which renders as checkerboard grain across every penumbra.
            float t_near = clamp(dot(vis_pos - origin, Lc), 0.0,
                                 ray_len + bias);
            vec3  dvec = vis_pos - (origin + Lc * t_near);
            float prox = 1.0 - smoothstep(2.0 * thickness, 3.0 * thickness,
                                          length(dvec));

            // Elevation gate: a blocker can only shade the receiver if it
            // STANDS ABOVE the receiver's tangent plane -- the sun ray climbs
            // away from that plane, so anything at or below it (the receiver's
            // own micro-relief on a displaced terrain top, or a pit wall seen
            // past a convex edge) can never be between the surface and the sun.
            // The plane/prox pair is blind to this: bumpy same-surface texels
            // and below-edge walls both pass it, and those marginal hits are
            // per-pixel bistable -- they render as salt-and-pepper stipple over
            // whole faces and as isolated full-occlusion (sky-ambient blue)
            // speckles at convex corners. Ramp over [0.5, 1.5] x thickness of
            // elevation so real occluders (walls, steps -- their fetched texels
            // climb well above the receiver) keep a smooth penumbra while
            // relief on the thickness scale itself stays inert.
            float elev = dot(N, vis_pos - origin);
            float standing = smoothstep(0.5 * thickness, 1.5 * thickness, elev);

            // Soft acceptance: occlusion ramps in once the ray is meaningfully
            // behind the visible surface, and back out again as the gap exceeds
            // the assumed occluder thickness (beyond that the "occluder" is just
            // distant background that happens to be in the way on screen). A hard
            // in/out test here is the other half of what made this grainy.
            wsum += w;
            hit += w * prox * standing
                     * smoothstep(depth_eps, depth_eps + thickness * 0.5, ahead)
                     * (1.0 - smoothstep(thickness, thickness * 2.0, ahead));
        }
        if (wsum > 0.0) hit /= wsum;

        // Fade with distance along the ray, and toward the screen edge where
        // the sample may have left the G-buffer entirely -- the same edge ramp
        // gfx/ssr_trace_body.glsl uses, so leaving the screen fades rather
        // than cutting off at a hard seam.
        vec2 e = smoothstep(vec2(0.0), vec2(0.08), uv)
               * smoothstep(vec2(1.0), vec2(0.92), uv);
        hit *= (1.0 - smoothstep(0.6, 1.0, f)) * e.x * e.y;

        // MAX along this ray: one occluder must not darken more for being
        // sampled several times as the march steps past it.
        tap = max(tap, hit);
        }

        // AVERAGE across the cone: the fraction of the sun's disk this
        // receiver point cannot see is the soft-shadow coverage.
        occ += tap;
    }
    occ /= float(cone_taps);

    return occ * graze;
}

#endif // TOY_CONTACT_SHADOW_BODY_GLSL
