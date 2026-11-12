/**
 * @file fog_math.h
 * @brief CPU mirror of the global height fog's transmittance (assets/shaders/gfx/fog.glsl).
 *
 * The same closed-form optical depth the shaders integrate, so gameplay can ask "how fogged is
 * that point from here" (AI visibility, gameplay readability checks) and the tests can pin the
 * model down without a GPU. Keep it in step with gfx_fog_height_tau() / gfx_fog_eval()'s
 * transmittance: the two are the same function written twice.
 */

#ifndef TOYENGINE_RENDER_FOG_MATH_H
#define TOYENGINE_RENDER_FOG_MATH_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>

#include <toyengine/render/toy_render_config.h>

namespace toy {
namespace render {

namespace detail {
/// Exponential part of the optical depth along A -> B, both ends at/above `base`.
inline float fog_height_tau_above(const glm::vec3& A, const glm::vec3& B, float density, float base, float falloff) {
    const float seg_len = glm::length(B - A);
    const float dz = B.z - A.z;
    if (std::abs(dz) < 1e-3f) return density * seg_len * std::exp(-(0.5f * (A.z + B.z) - base) / falloff);
    const float k = falloff / dz;
    return density * seg_len * k * (std::exp(-(A.z - base) / falloff) - std::exp(-(B.z - base) / falloff));
}
} // namespace detail

/**
 * @brief Optical depth of the height fog along A -> B (Z-up): `density` per metre at/below
 *        `base`, decaying as exp(-(z - base) / falloff) above it; uniform when falloff <= 0.
 */
inline float fog_height_tau(const glm::vec3& A, const glm::vec3& B, float density, float base, float falloff) {
    const float seg_len = glm::length(B - A);
    if (seg_len < 1e-5f) return 0.0f;
    if (falloff <= 0.0f) return density * seg_len;
    const bool a_below = A.z < base, b_below = B.z < base;
    if (a_below && b_below) return density * seg_len;
    if (!a_below && !b_below) return detail::fog_height_tau_above(A, B, density, base, falloff);
    const glm::vec3 P  = glm::mix(A, B, (base - A.z) / (B.z - A.z));
    const glm::vec3 lo = a_below ? A : B;
    const glm::vec3 hi = a_below ? B : A;
    return density * glm::length(P - lo) + detail::fog_height_tau_above(P, hi, density, base, falloff);
}

/**
 * @brief Fraction of light from `target` that reaches `eye` through the global fog (1 = clear),
 *        as the renderer draws it: start/cutoff distances, max opacity, Linear vs Exponential,
 *        and -- when `eye_under_water` -- only the part of the ray above `water_level`.
 */
inline float fog_transmittance(const ToyRenderConfig& c, const glm::vec3& eye, const glm::vec3& target,
                               bool eye_under_water = false, float water_level = 0.0f) {
    if (!c.fog_enabled) return 1.0f;
    const float dist = glm::length(target - eye);
    if (dist < 1e-6f) return 1.0f;
    const glm::vec3 dir = (target - eye) / dist;

    float t_end = dist;
    if (c.fog_cutoff_distance > 0.0f) t_end = std::min(t_end, c.fog_cutoff_distance);
    float t0 = std::max(c.fog_start_distance, 0.0f);
    if (eye_under_water) {
        if (dir.z <= 1e-4f) return 1.0f;
        t0 = std::max(t0, (water_level - eye.z) / dir.z);
    }
    if (t_end <= t0) return 1.0f;

    const glm::vec3 A = eye + dir * t0, B = eye + dir * t_end;
    const float tau = fog_height_tau(A, B, std::max(c.fog_density, 0.0f), c.fog_height_base, c.fog_height_falloff);
    float T;
    if (c.fog_mode == 0) {
        const float ramp = std::clamp((c.fog_linear_end - (t_end - t0)) /
                                      std::max(c.fog_linear_end - c.fog_linear_start, 1e-4f), 0.0f, 1.0f);
        T = c.fog_height_falloff > 0.0f ? ramp * std::exp(-tau) : ramp;
    } else {
        T = std::exp(-tau);
    }
    return std::max(T, 1.0f - std::clamp(c.fog_max_opacity, 0.0f, 1.0f));
}

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_FOG_MATH_H
