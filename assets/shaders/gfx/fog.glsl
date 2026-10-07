#ifndef GFX_FOG_GLSL
#define GFX_FOG_GLSL

// gfx/fog.glsl -- shared Unity-style global fog + local box/sphere fog volume
// math, used by fog.frag (see engine/passes/fog_pass.h).
//
// Declares no uniforms, samplers, or blocks -- same rule as gfx/ssr_common.glsl
// and gfx/ibl.glsl: every input is a function parameter, since different
// passes bind their camera/fog sets at different indices.

#include <gfx/sky.glsl>

/// Fog falloff modes, matching Unity's built-in fog (and FogUBO::mode_density.x).
#define GFX_FOG_MODE_LINEAR 0.0
#define GFX_FOG_MODE_EXP    1.0
#define GFX_FOG_MODE_EXP2   2.0

/// Unity's three global fog falloff curves, flat (no height decay) case.
/// `mode_density` = FogUBO::mode_density (x=mode, y=density, z=linear_start, w=linear_end).
float gfx_fog_flat_transmittance(float d, vec4 mode_density) {
    float mode    = mode_density.x;
    float density = mode_density.y;

    if (mode == GFX_FOG_MODE_LINEAR) {
        float start = mode_density.z;
        float end   = mode_density.w;
        return clamp((end - d) / max(end - start, 1e-4), 0.0, 1.0);
    }

    float tau = density * d;
    if (mode == GFX_FOG_MODE_EXP2) {
        tau *= tau;
    }
    return exp(-tau);
}

/// Closed-form optical depth of an exponential height-density profile
/// (engine is Z-up) along the world-space segment A -> B, for `density`
/// per world unit at height `base` decaying over `falloff` world units.
/// Degenerates to a flat-slab approximation as |B.z - A.z| -> 0, avoiding
/// the 0/0 the closed form hits for a horizontal ray.
float gfx_fog_height_tau(vec3 A, vec3 B, float density, float base, float falloff) {
    float seg_len = length(B - A);
    if (seg_len < 1e-5 || falloff <= 0.0) {
        return 0.0;
    }
    float dz = B.z - A.z;
    if (abs(dz) < 1e-3) {
        return density * seg_len * exp(-(A.z - base) / falloff);
    }
    float k = falloff / dz;
    return density * seg_len * k * (exp(-(A.z - base) / falloff) - exp(-(B.z - base) / falloff));
}

/// Global fog transmittance along the world-space segment A -> B, combining
/// Unity's flat mode curve with an optional exponential height falloff
/// (`height_params` = FogUBO::height_params, x=base, y=falloff <= 0 disables).
/// Linear mode keeps its own ramp and multiplies by the height term; Exp/Exp2
/// substitute the height-integrated optical depth for their flat `density * d`.
float gfx_fog_transmittance(vec3 A, vec3 B, vec4 mode_density, vec4 height_params) {
    float height_falloff = height_params.y;
    if (height_falloff <= 0.0) {
        return gfx_fog_flat_transmittance(distance(A, B), mode_density);
    }

    float tau_h = gfx_fog_height_tau(A, B, mode_density.y, height_params.x, height_falloff);
    float mode  = mode_density.x;
    if (mode == GFX_FOG_MODE_LINEAR) {
        float d = distance(A, B);
        float flat_t = clamp((mode_density.w - d) / max(mode_density.w - mode_density.z, 1e-4), 0.0, 1.0);
        return flat_t * exp(-tau_h);
    }
    if (mode == GFX_FOG_MODE_EXP2) {
        return exp(-tau_h * tau_h);
    }
    return exp(-tau_h);
}

/// Ray/AABB slab test against a box centered at the origin with half-extent
/// `extent`, in the box's own local space (ray already transformed by
/// VolumeGPU::inv_world). Returns [t_enter, t_exit]; t_enter > t_exit
/// means no intersection.
///
/// Guards the classic slab-test instability: when a ray is nearly parallel to an axis (that
/// component of `rd` near zero) AND the ray origin sits outside that axis's slab, naive
/// `1.0/rd` division produces a spuriously large-but-FINITE t for that axis instead of the
/// mathematically-correct signed infinity -- min()/max() then can't reject it the way it
/// would reject a genuine +-Inf, so it corrupts the combined bound with a bogus finite "hit"
/// far from the box. (This is generic to any box/ray configuration where a ray's local
/// direction happens to cross near-zero on one axis somewhere in view -- not specific to any
/// one volume's rotation or aspect ratio.) For a truly axis-parallel ray, the correct
/// contribution is "no constraint" if the origin is within the slab on that axis, or
/// "never intersects" if it's outside.
vec2 gfx_fog_box_intersect(vec3 ro, vec3 rd, vec3 extent) {
    bvec3 parallel = lessThan(abs(rd), vec3(1e-8));
    vec3  safe_rd  = mix(rd, vec3(1.0), parallel);  // avoid a literal 1/0 on the guarded axes
    vec3  inv_rd   = 1.0 / safe_rd;
    vec3  t0 = (-extent - ro) * inv_rd;
    vec3  t1 = ( extent - ro) * inv_rd;
    vec3  tmin = min(t0, t1);
    vec3  tmax = max(t0, t1);

    bvec3 inside_slab = lessThanEqual(abs(ro), extent);
    tmin = mix(tmin, mix(vec3(1e30), vec3(-1e30), inside_slab), parallel);
    tmax = mix(tmax, mix(vec3(-1e30), vec3(1e30), inside_slab), parallel);

    return vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));
}

/// Analytic ray/sphere test, radius `r`, sphere centered at the local origin.
/// Same [t_enter, t_exit] contract as gfx_fog_box_intersect; returns (1, 0)
/// (empty interval) on a miss so callers don't need a separate hit flag.
vec2 gfx_fog_sphere_intersect(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float disc = b * b - c;
    if (disc < 0.0) {
        return vec2(1.0, 0.0);
    }
    float s = sqrt(disc);
    return vec2(-b - s, -b + s);
}

/// View-independent, occlusion-independent soft-edge weight for a box volume, evaluated at a
/// single representative point INSIDE the box -- the natural midpoint of the ray's
/// traversal (see the fog.frag call site for why "natural": it must be computed from the
/// box's own unclamped entry/exit, not from a segment length nearby geometry has clipped).
///
/// Earlier approaches here evaluated a "closeness to the nearest edge" metric at the ray's
/// entry point alone (view-dependent: chord length rises at a screen-space rate that itself
/// depends on viewing angle), then at entry-and-exit combined via max() (still broke down
/// right at a box CORNER, where entry and exit can each independently read as "near an edge"
/// -- of two different edges meeting at that corner -- for an ordinary ray that isn't
/// actually grazing the silhouette at all).
///
/// This version sidesteps both: rather than asking "is either boundary point near an edge",
/// it asks "how far is this ONE interior point from the box's NEAREST face" -- the standard
/// per-axis interior distance, normalized by that axis's half-extent, minimum across axes.
/// ~0 right at any face/edge/corner (fully faded), ~1 at the box's exact center (full
/// density). A single evaluation, at the segment's own midpoint, naturally handles every
/// case: a true silhouette ray's midpoint sits close to the very edge/corner it grazes
/// (fades, identically from any angle, since this is a pure function of local-space
/// position); an interior-crease ray's midpoint sits well inside the box even though its
/// entry alone was close to the crease (no false-positive fade); and it has no coupling to
/// occlusion at all, since the caller evaluates it at the volume's own natural extent.
float gfx_fog_box_edge_weight(vec3 p_local, vec3 extent, float falloff_frac) {
    vec3 face_dist = (extent - abs(p_local)) / max(extent, vec3(1e-4));
    float min_face_dist = min(face_dist.x, min(face_dist.y, face_dist.z));
    float f = clamp(falloff_frac, 1e-3, 1.0);
    return smoothstep(0.0, f, min_face_dist);
}

/// View-independent soft-edge weight for a sphere volume. A sphere has no
/// edges, but the same chord-length flaw shows up as an infinitely-steep
/// falloff right at the true silhouette regardless of viewing angle (chord
/// length through a sphere has an infinite derivative there). The
/// rotation-invariant analogue of "near an edge" is "the ray grazes the
/// surface": the local surface normal at the entry point, `normalize(p_local)`
/// (the sphere is centered at the local origin), is nearly perpendicular to
/// the ray direction. 1 = ray passes through dead-center (full density),
/// 0 = tangent/grazing (fully faded).
/// Soft containment weight of a POINT inside a local sphere: 1 deep inside,
/// ramping to 0 at the surface. The sphere counterpart to
/// gfx_fog_box_edge_weight, which is already point-based.
///
/// Distinct from gfx_fog_sphere_edge_weight below, which takes a ray DIRECTION
/// and fades on grazing alignment -- that one answers "how softly does this ray
/// graze the volume", which is what an analytic single-sample integral needs. A
/// raymarcher evaluating density per sample needs this one instead.
///
/// @param p_local      Sample point in the volume's local space.
/// @param radius       Sphere radius in that space.
/// @param falloff_frac Edge softness as a fraction of the radius, in [0,1].
float gfx_fog_sphere_point_weight(vec3 p_local, float radius, float falloff_frac) {
    float d = 1.0 - length(p_local) / max(radius, 1e-4);
    float f = clamp(falloff_frac, 1e-3, 1.0);
    return smoothstep(0.0, f, d);
}

float gfx_fog_sphere_edge_weight(vec3 p_local, vec3 rd_local, float falloff_frac) {
    float alignment = abs(dot(normalize(p_local), normalize(rd_local)));
    float f = clamp(falloff_frac, 1e-3, 1.0);
    return smoothstep(0.0, f, alignment);
}

/// Henyey-Greenstein phase function, cos_theta = dot(view_dir, -sun_dir).
/// g in (-1, 1); positive g biases toward forward (sun-facing) scattering.
float gfx_fog_hg(float cos_theta, float g) {
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cos_theta;
    return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(denom, 1e-4), 1.5));
}

/// Explicit-colour overload (see gfx/sky.glsl's own overload for why this is a
/// second function, not a signature change, on the existing 6-arg one below).
vec3 gfx_fog_base_color(vec3 fog_color, vec3 view_dir, vec3 sun_dir, vec3 sun_color,
                        vec4 height_params, vec4 misc_params,
                        vec3 sky_zenith, vec3 sky_horizon, vec3 sky_ground) {
    vec3 color = mix(fog_color, sky_gradient(view_dir, sky_zenith, sky_horizon, sky_ground), height_params.z);
    float sun_amount = height_params.w;
    if (sun_amount > 0.0) {
        float cos_theta = dot(view_dir, -sun_dir);
        color += sun_color * gfx_fog_hg(cos_theta, misc_params.x) * sun_amount;
    }
    return color;
}

/// Base fog colour: config colour, optionally blended toward the shared sky
/// gradient, plus an additive Henyey-Greenstein sun tint. `height_params` =
/// FogUBO::height_params (z=sky_blend, w=sun_amount), `misc_params.x` = HG g.
vec3 gfx_fog_base_color(vec3 fog_color, vec3 view_dir, vec3 sun_dir, vec3 sun_color,
                        vec4 height_params, vec4 misc_params) {
    return gfx_fog_base_color(fog_color, view_dir, sun_dir, sun_color, height_params, misc_params,
                              SKY_ZENITH, SKY_HORIZON, SKY_GROUND);
}

/// The whole GLOBAL fog composite for one pixel: transmittance along the view
/// ray, blended toward the base colour. Shared by fog.frag (which is nothing but
/// this call) and volumetrics_composite.frag (which applies fog itself when the two passes
/// are merged into one, so the frame pays for one fullscreen HDR pass instead of
/// two) -- one definition, so the merged path cannot drift from the separate one.
///
/// `d_geo` is the distance to the geometry this pixel sees; pass any value for a
/// sky pixel and set `is_sky`. Both sides are evaluated at a distance capped to
/// `misc_params.w`: a narrow-FOV camera grazing a large flat surface has its
/// apparent per-pixel distance blow up near the vanishing point, which would fog
/// the last rows of geometry far more than anything else in view, right against
/// an unfogged sky -- a hard seam at the horizon. Capping both sides at the SAME
/// distance makes them converge to the same transmittance instead.
vec3 gfx_fog_apply(vec3 color, vec3 camera_pos, vec3 view_dir, float d_geo, bool is_sky,
                   vec4 mode_density, vec4 height_params, vec4 misc_params,
                   vec3 fog_color, vec3 sun_dir, vec3 sun_color,
                   vec3 sky_zenith, vec3 sky_horizon, vec3 sky_ground) {
    float max_distance = max(misc_params.w, 1.0);
    float d_fog = is_sky ? max_distance : min(d_geo, max_distance);
    float T_global = gfx_fog_transmittance(camera_pos, camera_pos + view_dir * d_fog,
                                           mode_density, height_params);

    vec3 base_color = gfx_fog_base_color(fog_color, view_dir, sun_dir, sun_color,
                                         height_params, misc_params,
                                         sky_zenith, sky_horizon, sky_ground);

    float T = clamp(T_global, 1.0 - misc_params.y, 1.0);
    return mix(base_color, color, T);
}

#endif // GFX_FOG_GLSL
