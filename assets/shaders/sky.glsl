// Analytic gradient environment. Engine is Z-up (plane normal +Z, camera at z=6).
// Kept low-magnitude (well below 1.0) so the ACES tonemap curve in
// tonemapping.frag, which compresses saturation heavily as inputs approach
// its shoulder, doesn't wash this gradient out to a flat near-white grey.
const vec3 SKY_ZENITH  = vec3(0.05, 0.18, 0.55);
const vec3 SKY_HORIZON = vec3(0.25, 0.35, 0.45);
const vec3 SKY_GROUND  = vec3(0.05, 0.045, 0.04);

vec3 sky_gradient(vec3 dir) {
    float t = normalize(dir).z;
    return (t > 0.0) ? mix(SKY_HORIZON, SKY_ZENITH, pow(t, 0.6))
                     : mix(SKY_HORIZON, SKY_GROUND, pow(-t, 0.4));
}
