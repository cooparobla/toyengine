#ifndef TOY_PIXEL_SHADOW_BODY_GLSL
#define TOY_PIXEL_SHADOW_BODY_GLSL

// pixel_shadow_body.glsl -- shared calc_dir_shadow()/calc_local_shadow() body, used by every
// toyengine shading path (pixel_lighting.frag, pixel_forward_shading.glsl,
// debug_view.frag) so opaque, forward/BLEND and SDF
// surfaces can never silently diverge on shadow behaviour.
//
// A "body" file in the pixel_forward_shading.glsl/ssr_trace_body.glsl sense: the includer must,
// BEFORE including this file, already have declared (with these exact names, whatever their own
// set/binding indices are):
//   - `lights`             -- the LightUBO block (light_ubo_body.glsl).
//   - `dir_shadow_map`     -- sampler2DShadow. The directional cascade ATLAS, not a single map --
//                             calc_dir_shadow() does the tile remap.
//   - `dir_shadow_map_raw` -- sampler2D: the SAME directional atlas through a plain nearest
//                             sampler, for the PCSS blocker search.
//   - `local_shadow_atlas` -- sampler2DShadow: the point/spot shadow atlas.
//   - #include <gfx/shadow_sampling.glsl> (gfx_shadow_dir_hard/_pcf_vogel/_pcss,
//     gfx_shadow_receiver_gradient, gfx_ign_angle).

// --- Cascade helpers that read the UBO IN PLACE ---------------------------------------------
//
// gfx/shadow_sampling.glsl's gfx_csm_select()/gfx_csm_atlas_coords() take the cascade matrices as
// a `mat4[4]` PARAMETER. GLSL passes arrays by value, so every call copies all four matrices into
// registers, and the lighting shader's occupancy collapses: measured on terrain_test at 1080p the
// lighting pass cost 3.7 ms through those helpers and 1.5 ms through these, with pixel-identical
// output. The volumetrics march keeps the gfx_ versions (its loop is not register-bound); every
// toyengine shading path goes through calc_dir_shadow() below and so through these.

/// True when `world_pos` lies inside cascade `c`'s tile, at least `inset` (tile uv) from its edge.
/// `uv` returns the point's position in that tile (0..1) for the edge-distance math.
bool toy_csm_contains(int c, vec3 world_pos, float inset, out vec2 uv) {
    vec4 lsp = lights.dir_cascade_matrix[c] * vec4(world_pos, 1.0);
    uv = vec2(0.0);
    if (lsp.w <= 0.0) return false;
    vec3 pc = lsp.xyz / lsp.w;
    uv = pc.xy * 0.5 + 0.5;
    return pc.z >= 0.0 && pc.z <= 1.0 &&
           all(greaterThanEqual(uv, vec2(inset))) && all(lessThanEqual(uv, vec2(1.0 - inset)));
}

/// `world_pos` in cascade `c`'s ATLAS coordinates: xy = atlas uv inside that tile, z = the [0,1]
/// light depth to compare.
vec3 toy_csm_atlas_coords(int c, vec3 world_pos) {
    float grid_x = max(lights.dir_cascade_info.y, 1.0);
    float grid_y = ceil(max(lights.dir_cascade_info.x, 1.0) / grid_x);
    vec2  scale  = vec2(1.0 / grid_x, 1.0 / grid_y);
    vec4 lsp = lights.dir_cascade_matrix[c] * vec4(world_pos, 1.0);
    vec3 pc  = lsp.xyz / lsp.w;
    pc.xy    = pc.xy * 0.5 + 0.5;
    vec2 origin = vec2(mod(float(c), grid_x), floor(float(c) / grid_x)) * scale;
    return vec3(origin + clamp(pc.xy, vec2(0.0), vec2(1.0)) * scale, pc.z);
}

/// Shadow-map depth bias for a receiver whose normal makes N.L with the light, as a number of
/// shadow TEXELS: a constant floor plus a slope term proportional to tan(theta), the depth the
/// receiver's own plane climbs across one texel (Unreal's constant + slope-scaled depth bias).
/// The slope term is scaled by the filter's reach: a PCF tap `radius_texels` out from the centre
/// compares against stored depth that far across the receiver's slope, so a wide soft kernel
/// needs proportionally more than a single hard tap does -- without it, normal-mapped faces
/// (whose shading normal wobbles N.L per pixel) show the kernel's edge taps as acne.
/// Shared by the directional and local-light maps; each converts texels to its own depth units.
float toy_shadow_bias_texels(float ndl, float radius_texels) {
    float n = clamp(ndl, 1e-3, 1.0);
    float tan_theta = min(sqrt(max(1.0 - n * n, 0.0)) / n, lights.dir_shadow_bias_texels.z);
    return lights.dir_shadow_bias_texels.x +
           lights.dir_shadow_bias_texels.y * tan_theta * (1.0 + max(radius_texels, 0.0));
}

/// The directional shadow from ONE cascade: normal offset, texel-scaled depth bias, then the
/// hard / PCF / PCSS / receiver-plane kernel. Split out of calc_dir_shadow() so the transition
/// band can evaluate two cascades and blend them.
float toy_dir_shadow_cascade(int cascade, vec3 world_pos, vec3 N, vec3 L, float angle) {
    // Normal-offset shadows: push the sample off its own surface along the geometric normal
    // before projecting, by this cascade's own world-unit offset. Scaled up as the surface
    // tilts away from the light, which is where one shadow texel covers the most surface and
    // the acne it causes is worst.
    float ndl = max(dot(N, L), 0.0);
    float normal_bias_scale = clamp(1.0 - ndl, 0.0, 1.0);
    vec3  biased_pos = world_pos +
        N * (lights.dir_cascade_normal_bias[cascade] * (0.5 + 0.5 * normal_bias_scale));
    vec3 proj_coords = toy_csm_atlas_coords(cascade, biased_pos);

    // Depth bias in THIS cascade's texels (see toy_shadow_bias_texels), converted to its [0,1]
    // light depth. The old constant was a fraction of the whole depth range, which every cascade
    // stretches over the full shadow distance toward the sun -- ~0.35 m at grazing angles on the
    // finest cascade, which detached contact shadows from their casters.
    float radius_texels = lights.dir_cascade_pcf_texels[cascade];
    float bias = toy_shadow_bias_texels(ndl, radius_texels) * lights.dir_cascade_depth_bias[cascade];

    if (radius_texels <= 0.0) {
        return gfx_shadow_dir_hard(dir_shadow_map, proj_coords, bias);
    }
    if (lights.pcss_params.x > 0.5) {
        // PCSS contact hardening: the constant radius above becomes the penumbra's
        // MAX; the blocker search shrinks it toward the contact point. See
        // gfx_shadow_dir_pcss's doc for the directional (linear-in-gap) penumbra model.
        return gfx_shadow_dir_pcss(dir_shadow_map, dir_shadow_map_raw, proj_coords, bias,
                                   lights.dir_cascade_pcss_scale[cascade], lights.pcss_params.z,
                                   radius_texels, angle, int(lights.dir_shadow_extra.z),
                                   int(lights.pcss_params.w));
    }
    // textureSize() is the whole ATLAS, which is exactly right: a tile texel and an atlas texel
    // are the same physical texel, so a radius in texels converts to uv against the atlas.
    vec2 texel_size = radius_texels / textureSize(dir_shadow_map, 0);
    if (lights.dir_shadow_receiver.x > 0.5) {
        // Receiver-plane depth bias: the taps follow this surface's own plane, so the normal
        // offset only covers rasterisation error -- see gfx_shadow_dir_pcf_vogel_rpdb. The
        // cascade transform is affine, so the plane's gradient needs only its linear part.
        mat3 J = mat3(lights.dir_cascade_matrix[cascade]);
        float grid_x = max(lights.dir_cascade_info.y, 1.0);
        float grid_y = ceil(max(lights.dir_cascade_info.x, 1.0) / grid_x);
        vec2 uv_scale = 0.5 / vec2(grid_x, grid_y);
        vec3 t1 = normalize(abs(N.z) < 0.9 ? cross(N, vec3(0.0, 0.0, 1.0)) : cross(N, vec3(1.0, 0.0, 0.0)));
        vec3 t2 = cross(N, t1);
        vec3 w  = normalize(abs(L.z) < 0.9 ? cross(L, vec3(0.0, 0.0, 1.0)) : cross(L, vec3(1.0, 0.0, 0.0)));
        vec3 j1 = J * t1, j2 = J * t2, jw = J * w, jl = J * L;
        vec2 gradient = gfx_shadow_receiver_gradient(
            vec3(0.0), vec3(j1.xy * uv_scale, j1.z), vec3(j2.xy * uv_scale, j2.z),
            vec3(jw.xy * uv_scale, jw.z), abs(jl.z), lights.dir_shadow_receiver.y);
        return gfx_shadow_dir_pcf_vogel_rpdb(dir_shadow_map, proj_coords, bias, texel_size,
                                             angle, int(lights.dir_shadow_extra.z), gradient);
    }
    return gfx_shadow_dir_pcf_vogel(dir_shadow_map, proj_coords, bias, texel_size,
                                    angle, int(lights.dir_shadow_extra.z));
}

// Directional shadow, against the CASCADED shadow atlas. Cascade c is the first whose tile holds
// the point at least the selection inset from its edge; inside the outer transition band of that
// tile the next cascade takes over gradually -- blended (two evaluations, a seamless gradient
// with or without TAA) when dir_shadow_fade_params.z is set, else dithered per pixel for TAA to
// average (one evaluation). The LAST cascade fades to unshadowed across its own outer band and
// with camera distance toward shadow_distance, so shadows end in a gradient instead of a line.
//
// Takes the RAW shading point and applies the normal-offset bias itself: the offset is
// per-cascade, so the caller cannot know it until the cascade is chosen. Selecting on the
// UNBIASED position is deliberate: the offset is centimetres, far too small to change the choice
// anywhere but exactly on a boundary, where either cascade is equally correct.
float calc_dir_shadow(vec3 world_pos, vec3 N, vec3 L) {
    if (lights.dir_shadow_params.z < 0.5) return 0.0;

    // Distance fade toward shadow_distance (Unreal's "shadow distance fadeout"): skip all work
    // past the end.
    float dist_fade = 1.0;
    if (lights.dir_shadow_fade.w > 0.0) {
        float d = length(world_pos - lights.dir_shadow_fade.xyz);
        dist_fade = 1.0 - smoothstep(lights.dir_shadow_fade_params.x, lights.dir_shadow_fade.w, d);
        if (dist_fade <= 0.0) return 0.0;
    }

    // IGN, not a fract(sin(...)) hash: a pixel-frequency hash puts its error exactly where TAA
    // resolves worst; IGN's smooth-ramp-per-tile structure reads as fine dither instead. The frame
    // offset (dir_shadow_extra.w, sourced from frame_index_) gives TAA's history a genuinely
    // different sample set to average over time.
    float angle = gfx_ign_angle(gl_FragCoord.xy) + lights.dir_shadow_extra.w * 2.39996323;

    int   count = int(lights.dir_cascade_info.x);
    float inset = lights.dir_cascade_info.z;
    float band  = lights.dir_cascade_info.w;

    int  cascade = -1;
    vec2 uv = vec2(0.0);
    for (int c = 0; c < 4; ++c) {
        if (c >= count) break;
        if (toy_csm_contains(c, world_pos, inset, uv)) { cascade = c; break; }
    }
    // Past the last cascade -- beyond shadow_distance, or off to the side of every tile.
    if (cascade < 0) return 0.0;

    // Distance to the selected tile's inset edge, in tile uv.
    vec2  dd   = min(uv - vec2(inset), vec2(1.0 - inset) - uv);
    float edge = min(dd.x, dd.y);

    float shadow;
    if (cascade + 1 < count && band > 0.0 && edge < band) {
        vec2 uv_next;
        bool next_ok = toy_csm_contains(cascade + 1, world_pos, inset, uv_next);
        float t = edge / band;   // 0 at the edge (all next cascade), 1 a band inside (all this one)
        if (next_ok && lights.dir_shadow_fade_params.z > 0.5) {
            shadow = mix(toy_dir_shadow_cascade(cascade + 1, world_pos, N, L, angle),
                         toy_dir_shadow_cascade(cascade, world_pos, N, L, angle), t);
        } else {
            // One pixel near the boundary takes the coarser tile, its neighbour the finer, and
            // TAA averages the pair into a gradient.
            float cascade_dither = fract(angle * 0.15915494); // angle / 2pi
            int pick = (next_ok && cascade_dither > t) ? cascade + 1 : cascade;
            shadow = toy_dir_shadow_cascade(pick, world_pos, N, L, angle);
        }
    } else {
        shadow = toy_dir_shadow_cascade(cascade, world_pos, N, L, angle);
        // The last cascade has no successor: fade out across its own outer band.
        if (cascade + 1 >= count && lights.dir_shadow_fade_params.y > 0.0) {
            shadow *= smoothstep(0.0, lights.dir_shadow_fade_params.y, edge);
        }
    }

    // Per-light darkness (DirectionalLightComponent::shadow_intensity), applied HERE rather than
    // at each (1.0 - shadow) call site, so every shading path inherits it for free.
    return shadow * dist_fade * lights.dir_shadow_extra.x;
}

// Point and spot shadows: every shadowed local light owns a slot in the local-light shadow
// atlas (LightUBO::local_shadows), sampled by gfx/local_shadow.glsl -- one implementation shared
// with the volumetrics. `slot_1based` is the light's own PointLightGPU::attenuation.w or
// SpotLightGPU::params.z (0 = unshadowed). The PCF tap count and per-frame rotation are the
// directional map's (dir_shadow_extra.z/.w), so every shadow in a frame shares one TAA
// decorrelation scheme.
#define GFX_LOCAL_SHADOWS lights.local_shadows
#include <gfx/local_shadow.glsl>

float calc_local_shadow(float slot_1based, vec3 world_pos, vec3 N, vec3 L) {
    int slot = int(slot_1based + 0.5) - 1;
    if (slot < 0) return 0.0;
    float angle = gfx_ign_angle(gl_FragCoord.xy) + lights.dir_shadow_extra.w * 2.39996323;
    return gfx_local_shadow(slot, world_pos, N, L, angle, int(lights.dir_shadow_extra.z));
}

#endif // TOY_PIXEL_SHADOW_BODY_GLSL
