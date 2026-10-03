/**
 * @file water_waves.h
 * @brief Gerstner wave evaluation shared by the CPU (buoyancy, surface queries) and the GPU
 *        (assets/shaders/water_waves.glsl) -- the single source of truth for the water surface.
 *
 * KEEP IN SYNC WITH assets/shaders/water_waves.glsl. Every constant and every line of math below
 * has a 1:1 counterpart there; buoyancy only matches what is drawn as long as the two agree.
 *
 * A body's waves are authored as ONE base wave (amplitude, wavelength, direction, steepness --
 * WaveParams, packed into PBRMaterial::shader_params so it reaches both the forward and the
 * reflection-capture vertex stage). k_wave_count waves are derived from it deterministically:
 * shorter, smaller, fanned-out copies, which breaks up the obvious single-sine look without a
 * per-wave parameter list. Phase speed follows deep-water dispersion (omega = sqrt(g k)), so there is
 * no separate speed knob -- longer waves travel faster, as they physically do.
 *
 * Waves are attenuated by water depth (`depth_attenuation()`): a vertex's baked depth (see
 * WaterSystem) calms the surface toward the shore, and the CPU query applies the identical factor.
 */

#ifndef TOYENGINE_WATER_WATER_WAVES_H
#define TOYENGINE_WATER_WATER_WAVES_H

#include <cmath>

#include <glm/glm.hpp>

namespace toy {
namespace water {

/** @brief Number of Gerstner waves derived from one base wave. GLSL: WATER_WAVE_COUNT. */
inline constexpr int k_wave_count = 4;

inline constexpr float k_gravity = 9.81f;
inline constexpr float k_two_pi  = 6.28318530718f;

/// Per-derived-wave multipliers on the base wave. GLSL: WATER_WAVE_* arrays.
inline constexpr float k_wave_length_ratio[k_wave_count] = {1.0f, 0.61f, 0.37f, 0.23f};
inline constexpr float k_wave_amp_ratio[k_wave_count]    = {1.0f, 0.52f, 0.30f, 0.17f};
inline constexpr float k_wave_angle_offset[k_wave_count] = {0.0f, 0.54f, -0.82f, 1.27f}; // radians
inline constexpr float k_wave_phase_offset[k_wave_count] = {0.0f, 1.7f, 4.1f, 2.9f};

/**
 * @struct WaveParams
 * @brief The authored base wave; packs into PBRMaterial::shader_params as
 *        (amplitude, wavelength, direction_radians, steepness).
 */
struct WaveParams {
    float amplitude  = 0.0f;  ///< World-space crest height of the base wave (m).
    float wavelength = 8.0f;  ///< Base wavelength (m).
    float direction  = 0.0f;  ///< Travel direction, radians CCW from +X in the XY plane.
    float steepness  = 0.5f;  ///< 0 = pure sine, 1 = sharpest crests without looping.

    glm::vec4 pack() const { return {amplitude, wavelength, direction, steepness}; }
    bool calm() const { return amplitude <= 1e-5f || wavelength <= 1e-3f; }
};

/** @brief Result of evaluating the wave field at an UNDISPLACED surface point. */
struct WaveSample {
    glm::vec3 displacement{0.0f}; ///< Add to the undisplaced point.
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    float     crest = 0.0f;       ///< 0..1 crest sharpness (foam hint), see GLSL water_gerstner().
};

/**
 * @brief Wave attenuation for a point `depth` metres above the bed: waves calm in shallow water.
 *        GLSL: water_depth_attenuation().
 */
inline float depth_attenuation(float depth, float wavelength) {
    float edge = glm::max(0.3f * wavelength, 1e-3f);
    float t = glm::clamp(depth / edge, 0.0f, 1.0f);
    t = t * t * (3.0f - 2.0f * t);
    return 0.25f + 0.75f * t;
}

/**
 * @brief Sum of k_wave_count Gerstner waves at undisplaced world XY `p`, time `t`, scaled by
 *        `atten` (depth_attenuation()). GLSL: water_gerstner().
 */
inline WaveSample evaluate(const WaveParams& w, const glm::vec2& p, float t, float atten = 1.0f) {
    WaveSample out;
    if (w.calm()) return out;
    glm::vec3 n_acc(0.0f);
    float crest = 0.0f;
    const float q_total = glm::clamp(w.steepness, 0.0f, 1.0f);
    for (int i = 0; i < k_wave_count; ++i) {
        float lambda = w.wavelength * k_wave_length_ratio[i];
        float amp    = w.amplitude * k_wave_amp_ratio[i] * atten;
        float angle  = w.direction + k_wave_angle_offset[i];
        glm::vec2 d(std::cos(angle), std::sin(angle));
        float k      = k_two_pi / lambda;
        float omega  = std::sqrt(k_gravity * k);
        // Per-wave Q so the SUM of Q_i*k_i*A_i never exceeds q_total -- the no-loop condition.
        float q      = amp > 1e-6f ? q_total / (k * amp * static_cast<float>(k_wave_count)) : 0.0f;
        float f      = k * glm::dot(d, p) - omega * t + k_wave_phase_offset[i];
        float c = std::cos(f), s = std::sin(f);
        out.displacement.x += q * amp * d.x * c;
        out.displacement.y += q * amp * d.y * c;
        out.displacement.z += amp * s;
        float wa = k * amp;
        n_acc.x += d.x * wa * c;
        n_acc.y += d.y * wa * c;
        n_acc.z += q * wa * s;
        crest   += q * wa * s;
    }
    out.normal = glm::normalize(glm::vec3(-n_acc.x, -n_acc.y, 1.0f - n_acc.z));
    out.crest  = glm::clamp(crest, 0.0f, 1.0f);
    return out;
}

/**
 * @brief Displaced surface height above the undisplaced plane at world XY `target`.
 *
 * Gerstner moves points horizontally, so the crest above `target` belongs to some other
 * undisplaced point p0 with p0 + disp.xy(p0) == target. A few fixed-point iterations of
 * p0 <- target - disp.xy(p0) converge quickly for steepness < 1 (the same inversion Crest/HDRP
 * use for CPU water queries). `atten_at` maps a point to its depth attenuation.
 *
 * @param out_normal Optional: surface normal at the converged point.
 */
template <typename AttenFn>
inline float height_at(const WaveParams& w, const glm::vec2& target, float t, AttenFn&& atten_at,
                       glm::vec3* out_normal = nullptr, int iterations = 3) {
    if (w.calm()) {
        if (out_normal) *out_normal = glm::vec3(0.0f, 0.0f, 1.0f);
        return 0.0f;
    }
    glm::vec2 p0 = target;
    WaveSample s;
    for (int i = 0; i < iterations; ++i) {
        s = evaluate(w, p0, t, atten_at(p0));
        p0 = target - glm::vec2(s.displacement);
    }
    s = evaluate(w, p0, t, atten_at(p0));
    if (out_normal) *out_normal = s.normal;
    return s.displacement.z;
}

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_WAVES_H
