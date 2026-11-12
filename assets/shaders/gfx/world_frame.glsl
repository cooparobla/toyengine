#ifndef GFX_WORLD_FRAME_GLSL
#define GFX_WORLD_FRAME_GLSL

// gfx/world_frame.glsl -- the small shared answers about a curved world: which way is up, how
// lit a point is, and how far a ray travels through a shell.
//
// The GPU counterpart of toyengine/env/world_frame.h, and it follows the same rule: the frame
// is a `vec4` of `xyz = centre, w = radius`, and `w <= 0` IS the flat Z-up world. So a caller
// asks these unconditionally instead of branching on the world's shape.
//
// Declares no uniforms, samplers or blocks -- every input is a function parameter, the same
// rule gfx/fog.glsl and gfx/volumetrics.glsl keep. That is what lets the lighting pass (which
// has the frame in a push constant), the cloud march (which has it in its own UBO) and the
// volumetrics (which has it per-volume in local space) share one definition.

/// World up on a flat Z-up world.
const vec3 GFX_WORLD_UP = vec3(0.0, 0.0, 1.0);

/// Unit up at a world point: away from the centre on a curved world, +Z on a flat one.
///
/// @param p     World-space point.
/// @param frame xyz = centre, w = radius; `w <= 0` is flat.
vec3 gfx_world_up(vec3 p, vec4 frame) {
    if (frame.w <= 0.0) return GFX_WORLD_UP;
    vec3 d = p - frame.xyz;
    float len = length(d);
    return len > 1e-6 ? d / len : GFX_WORLD_UP;
}

/// How lit a point on a curved world is: 0 deep in night, 1 in full day.
///
/// A soft terminator straddling the geometric one, so the day/night edge is a dusk band a few
/// degrees wide rather than a hard line cutting across the limb. **This is the engine's one
/// terminator.** The atmosphere shell (gfx_fog_shell_weight), the star backdrop's daylight
/// fade, the cloud deck's sun and moon and the night-side ground light all read it, so the
/// shell, the sky, the clouds and the terrain cross from day to night together instead of each
/// at its own angle.
///
/// A curved world's atmosphere carries the CPU twin of these constants; the two must stay in
/// step or the shell, the sky and the terrain cross from day to night at different angles.
///
/// @param up     Unit up at the point (gfx_world_up()).
/// @param sun_to Unit direction TOWARD the sun.
float gfx_world_day(vec3 up, vec3 sun_to) {
    return smoothstep(-0.25, 0.35, dot(up, sun_to));
}

/// Airmass of a ray leaving a point inside a shell: its path out through the top, as a
/// multiple of the VERTICAL path from the same radius. 1 looking straight up.
///
/// The honest replacement for a `1 / dot(up, L)` slant. That diverges as the sun nears the
/// horizon, so any attenuation keyed to it falls off a cliff -- and because it is only ~0 well
/// inside the lit hemisphere, it dims most of the day side too. The real path is a CHORD:
///
///     path = sqrt(r_out^2 - r^2 (1 - mu^2)) - r mu
///
/// Normalised by `r_out - r` (the vertical path from the SAME radius, not the shell's full
/// thickness -- dividing by the thickness would leave the sub-solar point attenuated), this is
/// exactly 1 overhead and reduces to `1/mu` in the plane-parallel limit, so it is the quantity
/// the old expression was approximating. Clamped, because the chord's growth near the horizon
/// scales with `sqrt(r_in / thickness)` and would otherwise make limb darkness depend on how
/// big the world is.
///
/// @param r       Radius of the sample point.
/// @param mu      dot(up, L) at the point.
/// @param r_in    The shell's inner radius.
/// @param r_out   The shell's outer radius.
/// @param blocked Set when the ray exits DOWNWARD through the inner sphere -- the world itself
///                is in the way, so there is no dimmed answer and the caller must zero the
///                light. This is the true geometric horizon of a point on the deck
///                (`mu_h = -sqrt(1 - (r_in/r)^2)`, a few degrees below level on a thin deck),
///                and the honest replacement for a hand-picked cutoff.
/// @return Airmass, >= 1.
float gfx_world_airmass(float r, float mu, float r_in, float r_out, out bool blocked) {
    float h2 = r * r * (1.0 - mu * mu);          // squared radius of the ray's closest approach
    blocked = mu < 0.0 && h2 < r_in * r_in;
    if (blocked) return 1.0;
    float vertical = max(r_out - r, 1e-4);
    float path = sqrt(max(r_out * r_out - h2, 0.0)) - r * mu;
    return clamp(path / vertical, 1.0, 40.0);
}

#endif // GFX_WORLD_FRAME_GLSL
