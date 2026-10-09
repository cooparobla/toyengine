#ifndef GFX_FOG_GLSL
#define GFX_FOG_GLSL

// gfx/fog.glsl -- the GLOBAL exponential height fog (Unreal's Exponential Height Fog / HDRP's
// fog model) plus the local box/sphere volume helpers the volumetrics passes share.
//
// The global medium: extinction `density` per metre at and below `base` (constant there, the
// way HDRP's base height works -- so a camera far below it, e.g. underwater, sees a finite
// density instead of an exponential blow-up), decaying as exp(-(z - base) / falloff) above.
// Its optical depth along any segment has a closed form (gfx_fog_height_tau), so the fog is
// one evaluation per pixel, never a march. Transmittance is always exp(-tau): Unity's old
// Exp2 curve (exp(-tau^2)) has no physical meaning once height enters the integral.
//
// Applied PER MEDIUM, as Unreal and HDRP do: FogPass fogs the opaque scene and sky at the
// G-buffer distance before translucency is drawn, and every forward shader (BLEND meshes,
// water, particles, SDF glass) fogs its own fragment at its own distance with
// gfx_fog_eval() -- so a translucent surface is never fogged at the distance of whatever lies
// behind it. With the camera under water (GfxFogBlock::water.y), only the part of each ray
// above the water surface is air: the in-water part belongs to the underwater pass.
//
// Declares no uniforms, samplers, or blocks -- every input is a function parameter, since
// different passes bind their sets at different indices.

#include <gfx/sky.glsl>
#include <gfx/fog_types.glsl>

/// Fog modes (GfxFogBlock::density.x). LINEAR is the legacy distance ramp.
#define GFX_FOG_MODE_LINEAR 0.0
#define GFX_FOG_MODE_EXP    1.0

/// Closed-form optical depth of the exponential part along A -> B, for a segment with BOTH
/// ends at or above the base (so every exponent is <= 0 and nothing can overflow).
float gfx_fog_height_tau_above(vec3 A, vec3 B, float density, float base, float falloff) {
    float seg_len = length(B - A);
    float dz = B.z - A.z;
    if (abs(dz) < 1e-3) {
        return density * seg_len * exp(-(0.5 * (A.z + B.z) - base) / falloff);
    }
    float k = falloff / dz;
    return density * seg_len * k * (exp(-(A.z - base) / falloff) - exp(-(B.z - base) / falloff));
}

/// Optical depth of the height-fog medium along the world-space segment A -> B (engine is
/// Z-up): `density` per metre at/below `base`, exp(-(z - base) / falloff) above it, or a flat
/// medium when falloff <= 0. Exact for a straight segment: the part below the base is a
/// constant-density length, the part above the closed form of the exponential.
float gfx_fog_height_tau(vec3 A, vec3 B, float density, float base, float falloff) {
    float seg_len = length(B - A);
    if (seg_len < 1e-5) return 0.0;
    if (falloff <= 0.0) return density * seg_len;

    bool a_below = A.z < base;
    bool b_below = B.z < base;
    if (a_below && b_below) return density * seg_len;
    if (!a_below && !b_below) return gfx_fog_height_tau_above(A, B, density, base, falloff);

    // The segment crosses the base plane at P: constant density on the below side.
    vec3 P  = mix(A, B, (base - A.z) / (B.z - A.z));
    vec3 lo = a_below ? A : B;
    vec3 hi = a_below ? B : A;
    return density * length(P - lo) + gfx_fog_height_tau_above(P, hi, density, base, falloff);
}

/// Transmittance of the segment A -> B. `ramp_dist` is the distance the LINEAR mode's ramp is
/// evaluated at (its start/end are camera distances); Exponential ignores it.
float gfx_fog_transmittance(vec3 A, vec3 B, float ramp_dist, GfxFogBlock f) {
    float tau = gfx_fog_height_tau(A, B, f.density.y, f.height.x, f.height.y);
    if (f.density.x == GFX_FOG_MODE_LINEAR) {
        float ramp = clamp((f.density.w - ramp_dist) / max(f.density.w - f.density.z, 1e-4), 0.0, 1.0);
        // Linear has no extinction of its own; the height term (when on) still thins it upward.
        return f.height.y > 0.0 ? ramp * exp(-tau) : ramp;
    }
    return exp(-tau);
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


/// The global fog along one view ray, for one pixel or fragment:
///   returns vec4(in-scatter rgb, transmittance T); composite as `color * T + rgb`
///   (or, in place, a premultiplied blend of vec4(rgb, 1 - T)).
///
/// `cam` is the camera position, `dir` the unit view direction, `dist` the distance to the
/// surface along it. A sky pixel passes is_sky and integrates to the sky distance instead.
/// The segment starts at the start distance (and, with the camera under water, at the point
/// the ray leaves the water -- if it never does, it is all water and T = 1), and ends at the
/// surface or the cutoff distance, whichever is nearer.
///
/// In-scatter is Unreal's split: the base colour (fog colour blended toward the sky gradient
/// seen along `dir`) over the whole segment, plus a directional sun lobe (Henyey-Greenstein)
/// over the part beyond the directional start distance. Max opacity floors both
/// transmittances so a scene never fogs out entirely when it is < 1.
vec4 gfx_fog_eval(vec3 cam, vec3 dir, float dist, bool is_sky, GfxFogBlock f,
                  vec3 sky_zenith, vec3 sky_horizon, vec3 sky_ground) {
    if (f.color.w < 0.5) return vec4(0.0, 0.0, 0.0, 1.0);

    float t_end = is_sky ? f.range.z : dist;
    if (f.range.y > 0.0) t_end = min(t_end, f.range.y);

    float t0 = max(f.range.x, 0.0);
    if (f.water.y > 0.5) {
        // Camera under water: air begins where the ray crosses the surface plane going up.
        if (dir.z <= 1e-4) return vec4(0.0, 0.0, 0.0, 1.0);
        t0 = max(t0, (f.water.x - cam.z) / dir.z);
    }
    if (t_end <= t0) return vec4(0.0, 0.0, 0.0, 1.0);

    vec3  A = cam + dir * t0;
    vec3  B = cam + dir * t_end;
    float min_T = 1.0 - clamp(f.height.w, 0.0, 1.0);
    float T = max(gfx_fog_transmittance(A, B, t_end - t0, f), min_T);

    vec3 base = mix(f.color.rgb, sky_gradient(dir, sky_zenith, sky_horizon, sky_ground), f.height.z);
    vec3 inscatter = base * (1.0 - T);

    if (dot(f.sun.rgb, f.sun.rgb) > 0.0) {
        float ts = max(t0, f.sun_dir.w);
        if (t_end > ts) {
            float Ts = max(gfx_fog_transmittance(cam + dir * ts, B, t_end - ts, f), min_T);
            inscatter += f.sun.rgb * gfx_fog_hg(dot(dir, -f.sun_dir.xyz), f.sun.w) * (1.0 - Ts);
        }
    }
    return vec4(inscatter, T);
}

/// Applies gfx_fog_eval()'s result to a straight (non-premultiplied) colour.
vec3 gfx_fog_composite(vec3 color, vec4 fog) {
    return color * fog.a + fog.rgb;
}

#endif // GFX_FOG_GLSL
