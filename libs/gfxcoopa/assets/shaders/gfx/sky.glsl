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
vec3 sky_gradient(vec3 dir, vec3 zenith, vec3 horizon, vec3 ground) {
    float t = normalize(dir).z;
    return (t > 0.0) ? mix(horizon, zenith, pow(t, 0.6))
                     : mix(horizon, ground, pow(-t, 0.4));
}

vec3 sky_gradient(vec3 dir) {
    return sky_gradient(dir, SKY_ZENITH, SKY_HORIZON, SKY_GROUND);
}

#endif // GFX_SKY_GLSL
