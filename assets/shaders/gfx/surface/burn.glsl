#ifndef GFX_SURFACE_BURN_GLSL
#define GFX_SURFACE_BURN_GLSL

// gfx/surface/burn.glsl -- an object burning across its own surface.
//
// Switched on per material by surface_ext.z bit 2, which toy::fire::FireSystem raises while an
// object is alight. The front rides in gfx_params as (origin_ws.xyz, radius):
//
//     unburnt            |  rim  |        char
//   d > front  ----------+-------+------------------>  d = 0 (the ignition point)
//
// The RIM is the whole effect. A burning object is not uniformly glowing -- it has a thin
// incandescent edge creeping outward with char behind it and untouched material ahead, and that
// travelling edge is what reads as "on fire" rather than "tinted orange".
//
// Two deliberate choices:
//   * char is the surface's OWN albedo crushed toward black, not an authored colour, so a pale
//     chair and a dark crate char plausibly without either being authored for it;
//   * the rim runs a black-body-ish ramp (deep red -> orange -> yellow-white) rather than one
//     flat ember colour, because a single colour is exactly what reads as a cheap overlay.

float gfx_burn_noise_(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n = dot(i, vec3(1.0, 57.0, 113.0));
    float a = fract(sin(n +   0.0) * 43758.5453);
    float b = fract(sin(n +   1.0) * 43758.5453);
    float c = fract(sin(n +  57.0) * 43758.5453);
    float d = fract(sin(n +  58.0) * 43758.5453);
    float e = fract(sin(n + 113.0) * 43758.5453);
    float g = fract(sin(n + 114.0) * 43758.5453);
    float h = fract(sin(n + 170.0) * 43758.5453);
    float k = fract(sin(n + 171.0) * 43758.5453);
    return mix(mix(mix(a, b, f.x), mix(c, d, f.x), f.y),
               mix(mix(e, g, f.x), mix(h, k, f.x), f.y), f.z);
}

/// Incandescence, roughly black-body from a dull red at 0 to yellow-white at 1.
vec3 gfx_burn_heat_(float t) {
    t = clamp(t, 0.0, 1.0);
    vec3 dull  = vec3(0.65, 0.07, 0.01);
    vec3 mid   = vec3(1.00, 0.33, 0.03);
    vec3 hot   = vec3(1.00, 0.78, 0.32);
    return t < 0.5 ? mix(dull, mid, t * 2.0) : mix(mid, hot, (t - 0.5) * 2.0);
}

/**
 * Burns `s`. `geo_n` is the geometric world normal; `front` is (origin_ws.xyz, radius).
 */
void gfx_burn_apply(inout GfxSurface s, vec3 geo_n, vec4 front) {
    float radius = front.w;
    if (radius <= 0.0) return;

    // Break the sphere up, and crawl the break with time so the edge is never a clean circle
    // and never still. Scaled by radius so a small object does not get a coarser edge than a
    // large one.
    vec3 p = s.position_ws;
    float wobble = gfx_burn_noise_(p * 3.1 + vec3(0.0, 0.0, gfx_time * 0.35)) - 0.5;
    float fine   = gfx_burn_noise_(p * 11.0 - vec3(0.0, 0.0, gfx_time * 0.9)) - 0.5;

    // Fire climbs. The front reaches upward faster than it creeps down, which is most of what
    // makes a burning object look like it is burning rather than dissolving.
    float up = clamp((p.z - front.z) * 0.5, -0.35, 0.35);
    float d = distance(p, front.xyz) + wobble * 0.22 + fine * 0.05 - up;

    // Nothing to draw well clear of the front -- but leave room for the tongues, which reach
    // ABOVE it and would be sliced off by an early-out keyed on the front alone.
    if (d > radius + 0.25) return;

    // --- Flame tongues licking UP off the burning edge -------------------------------------
    //
    // Sampled by asking, for a few heights below this pixel, whether the edge passes there: if
    // it does, this pixel is in the plume rising off it. That is what makes the flame ride the
    // edge for free -- it is derived from the edge's position, so it cannot lag or detach, and
    // it needs no emitter, no spawn point and no particles.
    //
    // Deliberately quiet. This is the fire ON an object, under whatever light it casts, not a
    // campfire; a `Fire` component is the thing that gets real emitters.
    const float k_reach = 0.17;          // metres the tongues climb
    float tongue = 0.0;
    for (int i = 1; i <= 3; ++i) {
        float h = k_reach * (float(i) / 3.0);
        vec3 q = p - vec3(0.0, 0.0, h);
        float dq = distance(q, front.xyz) + wobble * 0.22 + fine * 0.05 - up;
        // Only the band right at the edge throws a tongue, and only upward.
        float at_edge = 1.0 - smoothstep(0.0, 0.075, abs(dq - radius));
        // Licks: narrow vertical streaks that scroll upward, so the tongues move rather than
        // sitting on the edge as a static fringe.
        float lick = gfx_burn_noise_(vec3(q.xy * 14.0, q.z * 5.0 - gfx_time * 2.6));
        lick = smoothstep(0.42, 0.95, lick);
        tongue += at_edge * lick * (1.0 - h / k_reach);
    }
    tongue = clamp(tongue * 0.85, 0.0, 1.0);
    if (tongue > 0.0) {
        // Hottest at the base of the tongue, cooling as it rises.
        s.emissive += gfx_burn_heat_(0.8) * tongue * 3.4;
    }

    if (d > radius) return;   // untouched, and the great majority of pixels on a big object

    // A burning edge is a NARROW band and stays narrow however far the fire has travelled.
    // Scaling it with the radius made the rim 30 cm wide late in a burn, which dilutes the glow
    // into a soft gradient and loses the edge entirely -- the opposite of what sells it.
    const float rim = 0.055;
    float behind = radius - d;          // 0 at the leading edge, growing into the burnt part

    // --- Char. Deepens behind the rim and never fully recovers the original colour.
    // Deepens with distance behind the edge: a rim that has only just passed is scorched, and
    // what the fire left long ago is charcoal. The noise keeps it blotchy rather than a flat
    // wash, but its FLOOR is high -- at a 0.55 floor nearly half the original colour survived
    // everywhere and a fully burnt chair still read as brown wood.
    float scar = smoothstep(0.0, rim * 1.6, behind);
    scar *= mix(0.72, 1.0, smoothstep(0.0, 0.35, behind));
    scar *= 0.82 + 0.18 * gfx_burn_noise_(p * 7.0);
    s.albedo    = mix(s.albedo, s.albedo * 0.035, scar);
    s.roughness = mix(s.roughness, 0.95, scar);
    s.metallic  = mix(s.metallic, 0.0, scar);
    s.ao        = mix(s.ao, 0.7, scar * 0.6);

    // --- The rim: incandescent, hottest just behind the leading edge, gone further in.
    float t = clamp(behind / rim, 0.0, 1.0);
    float glow = t * (1.0 - smoothstep(0.55, 1.0, t));
    // Flicker, and a little more heat on up-facing surfaces where flame would actually sit.
    float flick = 0.75 + 0.25 * gfx_burn_noise_(p * 5.0 + vec3(gfx_time * 1.7));
    glow *= flick * (0.75 + 0.35 * clamp(geo_n.z, 0.0, 1.0));
    s.emissive += gfx_burn_heat_(t) * glow * 4.5;
}

#endif // GFX_SURFACE_BURN_GLSL
