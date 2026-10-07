#ifndef GFX_SSR_TRACE_BODY_GLSL
#define GFX_SSR_TRACE_BODY_GLSL

// gfx/ssr_trace_body.glsl -- shared Hi-Z screen-space reflection raymarch.
//
// Shared by ssr.frag and the forward transparent path (gfx/surface/transparent_fs.glsl
// via pixel_forward_shading.glsl), so a BLEND surface can trace the same reflection an opaque G-buffer pixel
// would get, from an origin that never appears in the G-buffer itself. Only
// the ORIGIN surface (P, N, roughness) is taken as a parameter -- every other
// G-buffer read below happens at the ray's HIT point, which by construction
// is opaque geometry, so it is valid to sample regardless of what kind of
// surface the ray started from.
//
// REQUIRED BEFORE INCLUDE (name-for-name; set indices are the includer's
// choice, but every name below must resolve to exactly this type):
//   uniform CameraUBO { mat4 view; mat4 proj; vec3 camera_pos; } camera;
//   sampler2D g_normal_metallic, g_position_roughness;
//   sampler2D u_velocity;        -- the G-buffer's velocity attachment (G4); only read when
//                                   GfxSsrParams::prev_frame_color is set
//   sampler2D u_hiz_map;
//   sampler2D u_scene_color;     -- prefiltered colour chain: the PREVIOUS frame's final HDR when
//                                   prev_frame_color is set, else this frame's lit colour
//
// Also requires <gfx/ssr_common.glsl> to have been included first.

/// Tunable knobs for one gfx_ssr_trace() call. Split out of a push-constant
/// block (rather than read directly from one) so ssr.frag and a consumer
/// with its own differently-laid-out push block can both populate this the
/// same way -- see transparent.frag's PushConstants for the latter.
struct GfxSsrParams {
    float max_distance;      // world-space ray length (ssr_max_distance)
    float bias_texels;       // normal bias, in full-res screen texels (ssr_bias_texels)
    float thickness_min;     // world-space floor for the thickness test (ssr_thickness)
    float thickness_scale;   // thickness as a fraction of |view z| (ssr_thickness_scale)
    float roughness_cutoff;
    int   max_iterations;
    int   max_hiz_mip;
    int   start_mip;         // Hi-Z mip the march starts at (ssr_start_mip)
    int   min_mip0_steps;    // self-reflection gate (ssr_min_mip0_steps)
    int   max_color_mip;     // top mip of the prefiltered scene-colour chain
    float jitter_strength;   // GGX lobe scale for the VNDF ray sampling, 1 = the full lobe;
                              // 0 = the deterministic mirror ray, exactly (ssr_jitter)
    int   frame_index;       // decorrelates ssr_ign2() noise frame to frame; meaningless at
                              // jitter_strength == 0
    bool  prev_frame_color;  // u_scene_color is LAST frame's final HDR: fetch hits where they were
    int   rays_per_pixel;    // VNDF rays averaged per pixel (gfx_ssr_trace only)
    float cone_prefilter;    // lobe-cone share of the hit-colour mip footprint, 0..1
    bool  skip_behind;       // stride-doubling while the ray runs behind thin geometry
};

/// A hit's reflected colour from u_scene_color. When that chain holds the PREVIOUS frame's final
/// HDR (Unreal's PrevSceneColor), the hit surface is looked up where it WAS last frame -- G4.xy
/// is its unjittered screen motion since then -- so reflections of moving objects stay put on
/// them. `color_uv` returns the uv actually sampled, for the screen-edge fade.
vec3 gfx_ssr_hit_color(vec2 hit_uv, ivec2 hit_px, float lod, bool prev_frame, out vec2 color_uv) {
    color_uv = hit_uv;
    if (prev_frame) {
        vec4 vel = texelFetch(u_velocity, hit_px, 0);
        color_uv = hit_uv - vel.xy;
    }
    return textureLod(u_scene_color, clamp(color_uv, vec2(0.0), vec2(1.0)), lod).rgb;
}

float gfx_ssr_get_view_z(float depth_ndc, mat4 inv_proj) {
    vec4 clip = inv_proj * vec4(0.0, 0.0, depth_ndc, 1.0);
    return clip.z / clip.w;
}

/// Result of one gfx_ssr_trace() call. `color` is premultiplied by `confidence` -- the
/// convention ssr.frag's out_ssr_color uses (it writes vec4(color, confidence)). `travel`
/// (world-space distance from the ray's origin P to the hit point) and `hit` exist because a
/// caller juggling more than one independent trace (or several rays) needs a way to pick whichever hit is physically NEARER
/// along the ray, and `confidence` alone can't answer that: it encodes screen-edge/
/// roughness/grazing/distance FADES, not proximity. `hit` makes "no hit" unambiguous rather
/// than inferred from `confidence <= 0.0` (a found-but-fully-faded hit is a real distinction
/// from no hit at all, even though both currently zero out `color`).
struct GfxSsrHit {
    vec3  color;
    float confidence;
    float travel;
    bool  hit;
};

/// Traces one ray from world-space origin (P, N, roughness) along the EXPLICIT direction R
/// against the Hi-Z map -- the direction-agnostic core gfx_ssr_trace() wraps with its own
/// mirror-plus-GGX-jitter direction. A consumer marching a NON-specular ray (toyengine's
/// ssgi.frag traces a cosine-hemisphere diffuse bounce) calls this directly with its own R.
/// Caller is responsible for the background early-out and for dot(R, N) > 0; `roughness`
/// still drives the cone-footprint mip selection and the roughness fade at the end.
GfxSsrHit gfx_ssr_trace_dir(vec3 P, vec3 N, vec3 R, float roughness, mat4 inv_proj, GfxSsrParams sp) {
    vec3 V = normalize(P - camera.camera_pos);

#ifdef GFX_SSR_SKIP_ZERO_WEIGHT
    // Exact early-out, opt-in. Three of the confidence fades applied after a hit (dir_fade,
    // roughness_fade, grazing_fade -- computed identically below) depend only on the origin
    // and the ray, not on what the ray hits. When their product is 0 the result is
    // zero-confidence whatever the march finds, so don't march. Returned as a MISS, which is
    // only equivalent for a caller that uses nothing but colour * confidence (ssr.frag,
    // ssgi.frag define GFX_SSR_SKIP_ZERO_WEIGHT). A caller that branches on .hit -- the forward
    // transparent shading's miss fallback -- must not define it, or its grazing band changes.
    {
        float pre_dir   = 1.0 - smoothstep(0.25, 0.85, dot(-V, R));
        float pre_rough = 1.0 - smoothstep(sp.roughness_cutoff - 0.3, sp.roughness_cutoff, roughness);
        float pre_graze = smoothstep(0.0, 0.05, max(dot(N, -V), 0.0));
        if (pre_dir * pre_rough * pre_graze <= 0.0) return GfxSsrHit(vec3(0.0), 0.0, 0.0, false);
    }
#endif

    // Depth-scaled bias. The self-reflection this exists to prevent is a screen-space
    // phenomenon -- the ray must clear roughly one texel's worth of the source surface -- so
    // the correct model is a bias constant in TEXELS, not in metres.
    ivec2 gsize    = textureSize(g_position_roughness, 0);
    float view_z   = (camera.view * vec4(P, 1.0)).z;  // negative: RH + DEPTH_ZERO_TO_ONE
    float p11      = abs(camera.proj[1][1]);          // untouched by TAA jitter, which only
                                                       // perturbs proj[2][0] / proj[2][1]
    float px_world = ssr_texel_world_size(view_z, p11, float(gsize.y));

    // 0.002 floor: px_world -> 0 near the near plane, and a zero bias reintroduces the
    // silhouette ring outright. This is the one case where depth-scaling is strictly WORSE
    // than the old constant, so the floor is not optional.
    float normal_bias = max(sp.bias_texels * px_world, 0.002);
    float ray_bias    = normal_bias * 1.35;
    float self_hit_r  = normal_bias * 1.0;    // relaxed from 1.35x in Phase 2 tuning

    // Ray start and end in world space, biased along N and R to prevent self-reflection.
    vec3 P0 = P + N * normal_bias + R * ray_bias;
    vec3 P1 = P + N * normal_bias + R * sp.max_distance;

    vec4 clip0 = camera.proj * camera.view * vec4(P0, 1.0);
    vec4 clip1 = camera.proj * camera.view * vec4(P1, 1.0);

    if (clip0.w <= 0.0 || clip1.w <= 0.0) return GfxSsrHit(vec3(0.0), 0.0, 0.0, false);

    vec3 ndc0 = clip0.xyz / clip0.w;
    vec3 ndc1 = clip1.xyz / clip1.w;

    vec3 ray_start = vec3(ssr_ndc_to_uv(ndc0.xy), ndc0.z);
    vec3 ray_end   = vec3(ssr_ndc_to_uv(ndc1.xy), ndc1.z);
    vec3 ray_dir   = ray_end - ray_start;

    if (length(ray_dir.xy) < 0.0001) return GfxSsrHit(vec3(0.0), 0.0, 0.0, false);

    // Sub-texel start offset: shifts the ray's first sample point a fraction of one mip-0
    // texel along its own direction, decorrelating which Hi-Z cell each frame's march first
    // tests. Without this, a static sub-pixel camera offset makes every frame snap to
    // identical cell boundaries even with the direction jitter above -- the hit/miss boundary
    // would still sit at the same on-screen location every frame.
    if (sp.jitter_strength > 0.0) {
        vec2 mip0_size = vec2(textureSize(u_hiz_map, 0));
        float texel_size = 1.0 / max(mip0_size.x, mip0_size.y);
        float t_offset = (ssr_ign2(gl_FragCoord.xy + vec2(13.0, 7.0), sp.frame_index).x - 0.5) * texel_size;
        ray_start += (ray_dir / max(length(ray_dir.xy), 1e-5)) * t_offset;
    }

    vec3 current_pos = ray_start;
    // Starting at a coarse mip is safe: cell_min_depth is a conservative MIN over the cell, so
    // "ray is in front of the cell" at a coarse mip provably means no hit anywhere in that
    // cell. The only cost is that a ray starting behind its own cell's min burns start_mip
    // iterations descending, which is why this is a knob rather than a fixed 3 or 4.
    int current_mip = clamp(sp.start_mip, 0, sp.max_hiz_mip);
    bool hit_found = false;
    vec2 hit_uv = vec2(0.0);
    vec3 hit_P = vec3(0.0);
    float hit_view_z = -1.0;
    ivec2 hit_px_final = ivec2(0);
    int mip0_steps = 0;
    int behind_run = 0;   // consecutive mip-0 texels the ray has run BEHIND the depth buffer

    for (int i = 0; i < sp.max_iterations; ++i) {
        if (current_pos.x < 0.0 || current_pos.x > 1.0 ||
            current_pos.y < 0.0 || current_pos.y > 1.0 ||
            current_pos.z < 0.0 || current_pos.z > 1.0) {
            break;
        }

        vec2 mip_size = vec2(textureSize(u_hiz_map, current_mip));
        vec2 cell_idx = floor(current_pos.xy * mip_size);

        float cell_min_depth = textureLod(u_hiz_map, (cell_idx + 0.5) / mip_size, float(current_mip)).r;

        if (current_pos.z < cell_min_depth) {
            // Ray is in front of surface geometry in cell -> step to cell boundary & step up mip
            vec2 cell_min = cell_idx / mip_size;
            vec2 cell_max = (cell_idx + 1.0) / mip_size;

            vec2 t_planes;
            t_planes.x = (ray_dir.x > 0.0) ? (cell_max.x - current_pos.x) / ray_dir.x : (cell_min.x - current_pos.x) / ray_dir.x;
            t_planes.y = (ray_dir.y > 0.0) ? (cell_max.y - current_pos.y) / ray_dir.y : (cell_min.y - current_pos.y) / ray_dir.y;

            // min_t_step: a UV-space nudge, converted into t-space by dividing by the ray's own
            // length -- ray_dir is the full un-normalized on-screen span (see the mip-0 texel
            // advance below, which normalizes for exactly this reason), so a bare constant here
            // would be a wildly different absolute distance for a screen-spanning ray than for a
            // short one, drifting continuously as the camera moves and each pixel's ray length
            // changes. That produced a genuine, deterministic, camera-rotation-keyed instability
            // in which Hi-Z cell boundary got crossed frame to frame. Capped at 0.01 of the t
            // range: for a very short on-screen ray (small length(ray_dir.xy)) the raw quotient
            // can exceed 1.0 -- an epsilon meant to be a tiny nudge would instead jump past the
            // entire remaining ray in one step and terminate the march immediately, turning
            // every such ray into a guaranteed miss. The cap keeps this a small nudge (at most
            // 1% of the ray) in that regime instead, while leaving normal-length rays (where the
            // quotient is already far below the cap) unaffected.
            float min_t_step = min(0.0001 / max(length(ray_dir.xy), 1e-5), 0.01);
            float t_cell = max(min(t_planes.x, t_planes.y), min_t_step) + min_t_step;

            // Where the ray reaches this cell's min-depth plane (Uludag's Hi-Z traversal). NDC
            // depth is affine in screen space, so the crossing is a plain linear solve along the
            // ray. If it comes before the cell's exit, the ray may intersect geometry INSIDE this
            // cell: advance only onto the plane and refine one mip down. Always running to the
            // cell exit instead overshoots the surface by up to a whole coarse cell, so the ray
            // reaches mip 0 already behind the geometry, fails the thickness test, and only
            // registers a hit where the overshoot happens to fall within it -- a pattern keyed
            // to the Hi-Z cell grid, which reads as stair-stepped edges and horizontal streaks
            // in every sharp reflection.
            float t_depth = (ray_dir.z > 0.0) ? (cell_min_depth - current_pos.z) / ray_dir.z : 1e30;
            if (t_depth < t_cell) {
                current_pos += ray_dir * max(t_depth, 0.0);
                if (current_mip == 0) {
                    // On the mip-0 depth plane: the next iteration tests this texel exactly at
                    // its surface (depth_diff ~ 0). Snap z onto the plane so float round-off in
                    // the solve cannot leave the ray a hair in front and bounce it back up.
                    current_pos.z = max(current_pos.z, cell_min_depth);
                } else {
                    current_mip -= 1;
                }
            } else {
                current_pos += ray_dir * t_cell;
                current_mip = min(current_mip + 1, sp.max_hiz_mip);
            }
            behind_run = 0;
        } else {
            // Ray penetrated cell surface
            if (current_mip == 0) {
                // Require at least a couple of mip-0 steps before honoring a hit. Right at a
                // silhouette (grazing angle, N nearly perpendicular to R), the normal bias barely
                // projects along the ray, so the very first texel(s) sampled after the bias can
                // still land back on the *same* reflecting surface just past the self-hit radius
                // -- a false self-reflection ring traced right along every silhouette edge.
                ivec2 hit_px = clamp(ivec2(current_pos.xy * vec2(gsize)), ivec2(0), gsize - 1);
                vec3 hit_world_pos = texelFetch(g_position_roughness, hit_px, 0).rgb;
                bool behind_thick = false;
                if (mip0_steps >= sp.min_mip0_steps && length(hit_world_pos - P) >= self_hit_r) {
                    float ray_z  = gfx_ssr_get_view_z(current_pos.z, inv_proj);
                    float surf_z = gfx_ssr_get_view_z(cell_min_depth, inv_proj);
                    float depth_diff_m = surf_z - ray_z;

                    // Thickness as a fraction of view depth with a world-space floor. A flat
                    // 0.1 m tolerance is far too tight for distant geometry (one Hi-Z texel
                    // already spans more than that in depth) and needlessly loose up close.
                    float thickness = max(sp.thickness_min, sp.thickness_scale * abs(ray_z));
                    behind_thick = depth_diff_m > thickness;

                    if (depth_diff_m >= 0.0 && depth_diff_m <= thickness) {
                        vec3 hit_normal = texelFetch(g_normal_metallic, hit_px, 0).rgb;
                        if (dot(hit_normal, R) < -0.05) {
                            hit_found  = true;
                            hit_uv     = current_pos.xy;
                            hit_P      = hit_world_pos;
                            hit_view_z = ray_z;
                            hit_px_final = hit_px;
                            break;
                        }
                    }
                }
                // Advance exactly one mip-0 texel along the ray direction. ray_dir is the
                // full un-normalized span of the whole ray (P0 -> P1 in UV space), not a
                // unit vector, so it must be normalized here -- otherwise this step is
                // "1/1920 of however long the ray happens to be on screen", which is far
                // too small for short on-screen rays (stalling the iteration budget) and
                // far too large for long ones (skipping over thin geometry).
                vec2 mip0_size = vec2(textureSize(u_hiz_map, 0));
                float texel_size = 1.0 / max(mip0_size.x, mip0_size.y);
                // Behind thin geometry (sp.skip_behind): the ray is occluded until it re-emerges,
                // and a min-only Hi-Z cannot tell how far that is -- climbing a mip just lands back
                // at mip 0 next iteration. Doubling the stride on each consecutive behind-texel
                // (1, 2, 4 .. 16) crosses such a region in a few iterations instead of one per
                // texel; the first in-front step resets it.
                behind_run = (sp.skip_behind && behind_thick) ? behind_run + 1 : 0;
                float stride = float(1 << min(max(behind_run - 1, 0), 4));
                current_pos += (ray_dir / max(length(ray_dir.xy), 1e-5)) * (texel_size * stride);
                mip0_steps++;
            } else {
                current_mip = current_mip - 1;
            }
        }
    }

    if (!hit_found) return GfxSsrHit(vec3(0.0), 0.0, 0.0, false);

    // Confidence / Fading factors. The screen-edge fade is evaluated where the colour is
    // FETCHED (below) as well as where the ray hit: with the previous-frame source a hit whose
    // surface was off-screen last frame has no colour to give.
    vec2 edge = smoothstep(vec2(0.0), vec2(0.08), hit_uv) * smoothstep(vec2(1.0), vec2(0.92), hit_uv);
    float screen_fade = edge.x * edge.y;

    // Fade out reflections whose ray points back toward the camera -- these are the ones
    // most prone to grazing-angle stretching/parallax error near the viewer's own reflection.
    // dot(-V, R) approaches 1 as R points back at the camera, so this fade must go toward
    // 0 (not 1) as that dot product increases.
    float dir_fade = 1.0 - smoothstep(0.25, 0.85, dot(-V, R));
    // A wide (0.3) band: with cone tracing there is no sharp-to-nothing pop to hide, so the
    // fade's job is to hand over to the probe/sky prefilter, which
    // above ~0.8 roughness is genuinely the better answer anyway -- a single screen-space ray
    // with a cone that wide is sampling a mip so coarse that it has stopped being a reflection
    // of anything local, and the screen-space blur ignores the depth discontinuities the probe
    // does not have.
    float roughness_fade = 1.0 - smoothstep(sp.roughness_cutoff - 0.3, sp.roughness_cutoff, roughness);

    // Fade out reflections originating right at a silhouette (view direction nearly tangent
    // to the surface, NdotV near 0). Hit data there is the least reliable: the normal bias
    // barely projects along the ray at grazing angles, so even with the minimum-step gate
    // above, occasional false self-hits still show up as a thin ring right on every silhouette.
    float NdotV_origin = max(dot(N, -V), 0.0);
    float grazing_fade = smoothstep(0.0, 0.05, NdotV_origin);

    // Distance fade. Rays that run to max_distance are the ones whose screen-space error is
    // largest, and without this they pop out abruptly as the camera moves.
    float travel    = length(hit_P - P);
    float dist_fade = 1.0 - smoothstep(sp.max_distance * 0.7, sp.max_distance, travel);

    // Hit-colour footprint -> mip level, in full-res texels at the HIT's depth (the reflected
    // image lives there). Two parts: the pixel's own ray cone continued past the bounce -- a
    // pixel spanning px_world at the reflector spans px_world * (d_cam + travel) / d_cam by the
    // time it reaches the hit -- and the GGX lobe's cone, scaled by cone_prefilter. With the
    // lobe importance-sampled by the rays themselves (jitter_strength 1), a partial prefilter
    // smooths the noise those rays leave without blurring a glossy reflection twice; 1 is the
    // old behaviour (the whole lobe from one prefiltered tap).
    float px_world_hit  = ssr_texel_world_size(hit_view_z, p11, float(gsize.y));
    float dist_cam      = max(length(P - camera.camera_pos), 1e-4);
    float ray_footprint = px_world * (dist_cam + travel) / dist_cam;
    float cone_diameter = ray_footprint + 2.0 * ssr_ggx_cone_tan(roughness) * travel * sp.cone_prefilter;
    float lod = clamp(log2(max(cone_diameter / max(px_world_hit, 1e-6), 1.0)),
                      0.0, float(sp.max_color_mip));

    vec2 color_uv;
    vec3 hit_color = gfx_ssr_hit_color(hit_uv, hit_px_final, lod, sp.prev_frame_color, color_uv);
    vec2 cedge = smoothstep(vec2(0.0), vec2(0.08), color_uv) * smoothstep(vec2(1.0), vec2(0.92), color_uv);
    float confidence = screen_fade * cedge.x * cedge.y * dir_fade * roughness_fade * grazing_fade * dist_fade;

    // Premultiplied by confidence -- see GfxSsrHit's own doc above for why.
    return GfxSsrHit(hit_color * confidence, confidence, travel, true);
}

/// Traces one reflection ray from world-space origin (P, N, roughness) against the Hi-Z
/// map. Caller is responsible for the background/roughness-cutoff early-out and the
/// dot(reflect(V,N), N) <= 0 check -- both are about the ORIGIN, which only the caller knows
/// how to fetch (G-buffer texelFetch for ssr.frag, forward interpolants for a transparent
/// pass). The march itself lives in gfx_ssr_trace_dir above; this wrapper only chooses the
/// direction: the mirror ray, GGX-jittered when jitter_strength > 0.
GfxSsrHit gfx_ssr_trace(vec3 P, vec3 N, float roughness, mat4 inv_proj, GfxSsrParams sp) {
    vec3 V = normalize(P - camera.camera_pos);
    vec3 R = reflect(V, N);

    // jitter_strength == 0: the single deterministic mirror ray, exactly.
    if (sp.jitter_strength <= 0.0) return gfx_ssr_trace_dir(P, N, R, roughness, inv_proj, sp);

    // GGX importance sampling of the visible normals (Heitz 2018 VNDF), Unreal's SSR ray
    // generation: each ray reflects the view about a microfacet normal drawn in proportion to the
    // BRDF's own visible-normal density, so the rays already carry the lobe's shape (no PDF
    // weighting needed for a split-sum composite). jitter_strength scales the lobe (1 = true
    // GGX). Fresh IGN draws per ray and per frame; the temporal resolve and the roughness-aware
    // blur integrate the noise. A ray that would leave below the surface falls back to the mirror.
    vec3 up = (abs(N.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 T  = normalize(cross(up, N));
    vec3 B  = cross(N, T);
    vec3 Ve = normalize(vec3(dot(-V, T), dot(-V, B), dot(-V, N)));
    float a = max(roughness * roughness * sp.jitter_strength, 1e-4);

    int   n = max(sp.rays_per_pixel, 1);
    vec3  color = vec3(0.0);
    float conf  = 0.0;
    float travel = 1e30;
    bool  any_hit = false;
    for (int k = 0; k < 8; ++k) {
        if (k >= n) break;
        vec2 xi = ssr_ign2(gl_FragCoord.xy + float(k) * vec2(53.0, 97.0), sp.frame_index);
        vec3 h  = ssr_sample_ggx_vndf(Ve, a, a, xi);
        vec3 H  = T * h.x + B * h.y + N * h.z;
        vec3 Rk = reflect(V, H);
        if (dot(Rk, N) <= 0.0) Rk = R;
        GfxSsrHit hk = gfx_ssr_trace_dir(P, N, Rk, roughness, inv_proj, sp);
        color += hk.color;
        conf  += hk.confidence;
        if (hk.hit) { any_hit = true; travel = min(travel, hk.travel); }
    }
    float inv_n = 1.0 / float(n);
    return GfxSsrHit(color * inv_n, conf * inv_n, any_hit ? travel : 0.0, any_hit);
}

#endif // GFX_SSR_TRACE_BODY_GLSL
