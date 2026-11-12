/**
 * @file math.h
 * @brief Shared numeric constants and small vector-math helpers used throughout physxcoopa.
 */

#ifndef PHYSXCOOPA_UTIL_MATH_H
#define PHYSXCOOPA_UTIL_MATH_H

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>

namespace coopa {
namespace physx {
namespace util {

/** @brief General-purpose tolerance for near-zero and near-equal comparisons. */
inline constexpr float k_epsilon = 1e-6f;

/** @brief Default gravity along -Z, matching the Z-up world every consumer scene uses. */
inline constexpr float k_gravity_z = -9.81f;

/** @brief Default fixed substep length (60 Hz). */
inline constexpr float k_default_fixed_dt = 1.0f / 60.0f;

/** @brief Accumulator input clamp -- caps a single frame's contribution to the substep loop. */
inline constexpr float k_max_frame_time = 0.25f;

/** @brief Hard cap on the XPBD substeps one cloth frame step may run.
 *
 *  PhysicsWorld::step_cloths_() sizes its substep count to hold the INTERNAL substep length
 *  constant as the frame dt varies (see its doc), so a long hitch would otherwise ask for
 *  arbitrarily many. Past this cap the internal substep simply grows with the hitch -- the same
 *  trade PhysicsConfig::max_substeps makes for the rigid loop, and for the same reason: a frame
 *  that is already late must not be made later by the catch-up work. */
inline constexpr uint32_t k_max_cloth_substeps = 16;

/**
 * @brief Normalizes a vector, returning a zero vector instead of NaN when the input is
 *        degenerate (length below k_epsilon).
 *
 * @param v Vector to normalize.
 * @return Unit-length vector, or glm::vec3(0) if `v` is too short to normalize safely.
 */
inline glm::vec3 safe_normalize(const glm::vec3& v) {
    float len2 = glm::dot(v, v);
    if (len2 < k_epsilon * k_epsilon) return glm::vec3(0.0f);
    return v * (1.0f / std::sqrt(len2));
}

/**
 * @brief Builds an orthonormal basis {n, t1, t2} from a unit normal, for friction directions.
 *
 * Uses the branch-free construction (Duff et al., "Building an Orthonormal Basis, Revisited"):
 * picks the tangent whose cross product with `n` cannot degenerate, based on the sign of `n.z`.
 *
 * @param n  Unit normal (not re-normalized -- caller must pass a unit vector).
 * @param t1 Output: first unit tangent, perpendicular to `n`.
 * @param t2 Output: second unit tangent, perpendicular to both `n` and `t1`.
 */
inline void orthonormal_basis(const glm::vec3& n, glm::vec3& t1, glm::vec3& t2) {
    float sign = n.z >= 0.0f ? 1.0f : -1.0f;
    float a = -1.0f / (sign + n.z);
    float b = n.x * n.y * a;
    t1 = glm::vec3(1.0f + sign * n.x * n.x * a, sign * b, -sign * n.x);
    t2 = glm::vec3(b, sign + n.y * n.y * a, -n.y);
}

/**
 * @brief Returns the skew-symmetric ("cross-product") matrix of a vector, such that
 *        skew(v) * u == cross(v, u) for any u.
 *
 * @param v Input vector.
 * @return 3x3 skew-symmetric matrix.
 */
inline glm::mat3 skew(const glm::vec3& v) {
    return glm::mat3(
        0.0f,  v.z, -v.y,
        -v.z,  0.0f,  v.x,
        v.y, -v.x,  0.0f
    );
}

} // namespace util
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_UTIL_MATH_H
