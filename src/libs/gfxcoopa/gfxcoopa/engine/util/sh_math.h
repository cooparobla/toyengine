/**
 * @file sh_math.h
 * @brief 2nd-order (9 coefficient) Spherical Harmonics math functions.
 */

#ifndef GFXCOOPA_ENGINE_UTIL_SH_MATH_H
#define GFXCOOPA_ENGINE_UTIL_SH_MATH_H

#include <glm/glm.hpp>
#include <cmath>
#include <array>

namespace coopa {
namespace gfx {
namespace engine {
namespace util {

/// Evaluates L2 SH basis functions for direction `d` (must be normalized).
/// Returns 9 coefficients: Y_00, Y_1-1, Y_10, Y_11, Y_2-2, Y_2-1, Y_20, Y_21, Y_22
inline std::array<float, 9> sh_basis(const glm::vec3& d) {
    const float SH_C0   = 0.282094791f;                // 1 / (2*sqrt(PI))
    const float SH_C1   = 0.488602512f;                // sqrt(3) / (2*sqrt(PI))
    const float SH_C2_0 = 1.092548431f;              // sqrt(15) / (2*sqrt(PI))
    const float SH_C2_1 = 0.315391565f;              // sqrt(5) / (4*sqrt(PI))
    const float SH_C2_2 = 0.546274215f;              // sqrt(15) / (4*sqrt(PI))

    return {{
        SH_C0,                              // Y_00
        SH_C1 * d.y,                        // Y_1-1
        SH_C1 * d.z,                        // Y_10
        SH_C1 * d.x,                        // Y_11
        SH_C2_0 * d.x * d.y,               // Y_2-2
        SH_C2_0 * d.y * d.z,               // Y_2-1
        SH_C2_1 * (3.0f * d.z*d.z - 1.0f), // Y_20
        SH_C2_0 * d.x * d.z,               // Y_21
        SH_C2_2 * (d.x*d.x - d.y*d.y)      // Y_22
    }};
}

/// Projects a single radiance sample (direction `d`, color `radiance`) onto
/// an existing set of SH coefficients (9 vec3s). Accumulates with weight `w`.
inline void sh_project_sample(std::array<glm::vec3, 9>& coeffs,
                              const glm::vec3& d, const glm::vec3& radiance, float w) {
    auto basis = sh_basis(d);
    for (int i = 0; i < 9; ++i) {
        coeffs[i] += radiance * basis[i] * w;
    }
}

/// Evaluates SH irradiance for normal `n` from 9 SH coefficients.
/// Applies standard cosine-lobe convolution (Ramamoorthi & Hanrahan 2001):
///   A_0 = PI, A_1 = 2PI/3, A_2 = PI/4
inline glm::vec3 sh_evaluate_irradiance(const std::array<glm::vec3, 9>& coeffs,
                                         const glm::vec3& n) {
    auto basis = sh_basis(n);
    const float A[3] = { 3.14159265f, 2.09439510f, 0.78539816f };
    const int band_index[9] = {0, 1,1,1, 2,2,2,2,2};

    glm::vec3 irradiance(0.0f);
    for (int i = 0; i < 9; ++i) {
        irradiance += coeffs[i] * basis[i] * A[band_index[i]];
    }
    return glm::max(irradiance, glm::vec3(0.0f));
}

} // namespace util
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_UTIL_SH_MATH_H
