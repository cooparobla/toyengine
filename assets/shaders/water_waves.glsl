#ifndef TOY_WATER_WAVES_GLSL
#define TOY_WATER_WAVES_GLSL

// water_waves.glsl -- Gerstner wave sum for the water surface shader.
//
// KEEP IN SYNC WITH toyengine/water/water_waves.h. That header is the CPU mirror buoyancy
// samples; every constant and line of math here has a 1:1 counterpart there, and floating
// objects only ride the waves that are drawn as long as the two agree.
//
// One authored base wave (gfx_params = amplitude, wavelength, direction radians, steepness)
// fans out into WATER_WAVE_COUNT derived waves; phase speed follows deep-water dispersion
// (omega = sqrt(g k)). See the header's file doc for the reasoning.

#define WATER_WAVE_COUNT 4
const float WATER_GRAVITY = 9.81;
const float WATER_TWO_PI  = 6.28318530718;

const float WATER_WAVE_LENGTH_RATIO[WATER_WAVE_COUNT] = float[](1.0, 0.61, 0.37, 0.23);
const float WATER_WAVE_AMP_RATIO[WATER_WAVE_COUNT]    = float[](1.0, 0.52, 0.30, 0.17);
const float WATER_WAVE_ANGLE_OFFSET[WATER_WAVE_COUNT] = float[](0.0, 0.54, -0.82, 1.27);
const float WATER_WAVE_PHASE_OFFSET[WATER_WAVE_COUNT] = float[](0.0, 1.7, 4.1, 2.9);

struct WaterWave {
    vec3  displacement;
    vec3  normal;
    float crest;   // 0..1 crest sharpness, a foam hint
};

/// Waves calm in shallow water. C++: depth_attenuation().
float water_depth_attenuation(float depth, float wavelength) {
    float edge = max(0.3 * wavelength, 1e-3);
    float t = clamp(depth / edge, 0.0, 1.0);
    t = t * t * (3.0 - 2.0 * t);
    return 0.25 + 0.75 * t;
}

/// Sum of the derived Gerstner waves at undisplaced world XY `p`. C++: evaluate().
WaterWave water_gerstner(vec4 base, vec2 p, float t, float atten) {
    WaterWave w;
    w.displacement = vec3(0.0);
    w.normal = vec3(0.0, 0.0, 1.0);
    w.crest = 0.0;
    float amplitude = base.x, wavelength = base.y, direction = base.z;
    if (amplitude <= 1e-5 || wavelength <= 1e-3) return w;
    float q_total = clamp(base.w, 0.0, 1.0);
    vec3 n_acc = vec3(0.0);
    float crest = 0.0;
    for (int i = 0; i < WATER_WAVE_COUNT; ++i) {
        float lambda = wavelength * WATER_WAVE_LENGTH_RATIO[i];
        float amp    = amplitude * WATER_WAVE_AMP_RATIO[i] * atten;
        float angle  = direction + WATER_WAVE_ANGLE_OFFSET[i];
        vec2  d      = vec2(cos(angle), sin(angle));
        float k      = WATER_TWO_PI / lambda;
        float omega  = sqrt(WATER_GRAVITY * k);
        float q      = amp > 1e-6 ? q_total / (k * amp * float(WATER_WAVE_COUNT)) : 0.0;
        float f      = k * dot(d, p) - omega * t + WATER_WAVE_PHASE_OFFSET[i];
        float c = cos(f), s = sin(f);
        w.displacement.x += q * amp * d.x * c;
        w.displacement.y += q * amp * d.y * c;
        w.displacement.z += amp * s;
        float wa = k * amp;
        n_acc.x += d.x * wa * c;
        n_acc.y += d.y * wa * c;
        n_acc.z += q * wa * s;
        crest   += q * wa * s;
    }
    w.normal = normalize(vec3(-n_acc.x, -n_acc.y, 1.0 - n_acc.z));
    w.crest  = clamp(crest, 0.0, 1.0);
    return w;
}

#endif // TOY_WATER_WAVES_GLSL
