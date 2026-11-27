#ifndef GFX_SKY_GLSL
#define GFX_SKY_GLSL

// gfx/sky.glsl -- shared analytic sky gradient, the fallback indirect term
// used everywhere no reflection probe / GI volume covers a surface point.
//
// Engine is Z-up (plane normal +Z). Kept low-magnitude (well below 1.0) so
// an ACES tonemap curve, which compresses saturation heavily as inputs
// approach its shoulder, doesn't wash this gradient out to a flat
// near-white grey.
const vec3 SKY_ZENITH  = vec3(0.05, 0.18, 0.55);
const vec3 SKY_HORIZON = vec3(0.25, 0.35, 0.45);
const vec3 SKY_GROUND  = vec3(0.05, 0.045, 0.04);

// Explicit-colour overload. Kept as a plain function parameter list, not a
// uniform/block: different passes bind their own sets at different indices,
// so a shared gfx/*.glsl body must take every input as an argument. A
// consumer with configurable sky colours (see IndirectParams::sky_zenith/
// horizon/ground) passes its own values here; everyone else calls the
// single-argument overload below with the built-in colours.
// The flat world's up. Spelled out here rather than shared with the engine's
// gfx/world_frame.glsl GFX_WORLD_UP, which is the authoritative twin: shader
// include paths run consumer-then-gfxcoopa, so this file cannot see the
// engine's tree, and a reference to it would compile only in the handful of
// files that happen to have included world_frame.glsl first.
const vec3 SKY_UP_Z = vec3(0.0, 0.0, 1.0);

// Up-oriented overload. The hemisphere is keyed to `up` rather than to world
// +Z, which is what a world whose up is not +Z needs: up there differs at
// every point, so an irradiance evaluated at a surface NORMAL has to be told
// which way up is for that fragment (the engine's gfx_world_up()).
//
// `up` is a plain parameter for the same reason the colours are -- this file
// declares nothing and includes nothing. Expects it normalised.
vec3 sky_gradient(vec3 dir, vec3 up, vec3 zenith, vec3 horizon, vec3 ground) {
    float t = dot(normalize(dir), up);
    return (t > 0.0) ? mix(horizon, zenith, pow(t, 0.6))
                     : mix(horizon, ground, pow(-t, 0.4));
}

// Delegates with the flat up, so every existing caller is unchanged:
// dot(v, vec3(0,0,1)) reproduces v.z exactly.
vec3 sky_gradient(vec3 dir, vec3 zenith, vec3 horizon, vec3 ground) {
    return sky_gradient(dir, SKY_UP_Z, zenith, horizon, ground);
}

vec3 sky_gradient(vec3 dir) {
    return sky_gradient(dir, SKY_ZENITH, SKY_HORIZON, SKY_GROUND);
}

#endif // GFX_SKY_GLSL
