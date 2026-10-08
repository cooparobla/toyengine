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

/// A derived wave fades over [START, 2 * START] wavelengths from the camera, the shortest first:
/// waves a few pixels long only alias, and coarse water LODs cannot carry them.
/// C++: k_wave_fade_start.
const float WATER_WAVE_FADE_START = 100.0;

/// C++: wave_distance_fade().
float water_wave_distance_fade(float lambda, float distance) {
    float a = WATER_WAVE_FADE_START * lambda;
    float t = clamp((distance - a) / a, 0.0, 1.0);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

/// TESSELLATED water only -- a visual LOD with no C++ counterpart (buoyancy keeps every wave;
/// the difference lives where the camera is too far to judge it). How much of a wave of
/// wavelength `lambda` the tessellated mesh at `distance` can carry: the tessellator aims at a
/// vertex spacing of lod.x metres per metre of distance, and a wave needs ~3-6 vertices a period
/// to keep its shape; past ~0.55-0.8 of the tessellation range (lod.y) the mesh hands back to the
/// baked grid, so every wave is gone by then. A function of the position alone, so tiles and
/// patches that share an edge displace it identically. lod.y <= 0: no mesh fade.
float water_wave_mesh_fade(float lambda, float distance, vec2 lod) {
    if (lod.y <= 0.0) return 1.0;
    float s = lod.x * max(distance, 0.05);
    return smoothstep(3.0 * s, 6.0 * s, lambda) * (1.0 - smoothstep(0.55 * lod.y, 0.8 * lod.y, distance));
}

/// Sum of the derived Gerstner waves at undisplaced world XY `p`, seen from `distance` metres
/// (0 = no fade). `lod`: see water_wave_mesh_fade() (vec2(0) = none). C++: evaluate(const WaveSet&, ...).
WaterWave water_gerstner(vec4 base, vec2 p, float t, float atten, float distance, vec2 lod) {
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
        float fade   = water_wave_distance_fade(lambda, distance) * water_wave_mesh_fade(lambda, distance, lod);
        if (fade <= 0.0) continue;
        float amp    = amplitude * WATER_WAVE_AMP_RATIO[i] * atten * fade;
        float angle  = direction + WATER_WAVE_ANGLE_OFFSET[i];
        vec2  d      = vec2(cos(angle), sin(angle));
        float k      = WATER_TWO_PI / lambda;
        float omega  = sqrt(WATER_GRAVITY * k);
        // q * amp == q_total / (k N), independent of the depth attenuation; the distance fade
        // scales it too, so a faded wave vanishes horizontally as well.
        float qa     = amp > 1e-6 ? q_total / (k * float(WATER_WAVE_COUNT)) * fade : 0.0;
        float f      = k * dot(d, p) - omega * t + WATER_WAVE_PHASE_OFFSET[i];
        float c = cos(f), s = sin(f);
        w.displacement.x += qa * d.x * c;
        w.displacement.y += qa * d.y * c;
        w.displacement.z += amp * s;
        float wa = k * amp;
        n_acc.x += d.x * wa * c;
        n_acc.y += d.y * wa * c;
        n_acc.z += qa * k * s;
        crest   += qa * k * s;
    }
    w.normal = normalize(vec3(-n_acc.x, -n_acc.y, 1.0 - n_acc.z));
    w.crest  = clamp(crest, 0.0, 1.0);
    return w;
}
WaterWave water_gerstner(vec4 base, vec2 p, float t, float atten, float distance) {
    return water_gerstner(base, p, t, atten, distance, vec2(0.0));
}

/// The slope (dz/dx, dz/dy) of what water_wave_mesh_fade() took OUT of the tessellated mesh at
/// world XY `p`: the fragment stage shades it back in, so the waves the far mesh cannot hold
/// still read in the lighting. Zero where the mesh carries everything.
vec2 water_gerstner_residual_slope(vec4 base, vec2 p, float t, float distance, vec2 lod) {
    vec2 slope = vec2(0.0);
    float amplitude = base.x, wavelength = base.y, direction = base.z;
    if (lod.y <= 0.0 || amplitude <= 1e-5 || wavelength <= 1e-3) return slope;
    for (int i = 0; i < WATER_WAVE_COUNT; ++i) {
        float lambda = wavelength * WATER_WAVE_LENGTH_RATIO[i];
        float keep   = water_wave_distance_fade(lambda, distance) * (1.0 - water_wave_mesh_fade(lambda, distance, lod));
        if (keep <= 0.0) continue;
        float amp    = amplitude * WATER_WAVE_AMP_RATIO[i] * keep;
        float angle  = direction + WATER_WAVE_ANGLE_OFFSET[i];
        vec2  d      = vec2(cos(angle), sin(angle));
        float k      = WATER_TWO_PI / lambda;
        float f      = k * dot(d, p) - sqrt(WATER_GRAVITY * k) * t + WATER_WAVE_PHASE_OFFSET[i];
        slope += d * (k * amp * cos(f));
    }
    return slope;
}

#endif // TOY_WATER_WAVES_GLSL
