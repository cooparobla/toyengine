/**
 * @file integrator.h
 * @brief Per-body force accumulation and semi-implicit-Euler integration for one substep.
 */

#ifndef PHYSXCOOPA_DYNAMICS_INTEGRATOR_H
#define PHYSXCOOPA_DYNAMICS_INTEGRATOR_H

#include <physxcoopa/dynamics/body.h>
#include <physxcoopa/util/math.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

namespace coopa {
namespace physx {
namespace dynamics {

/**
 * @brief Applies gravity into a dynamic, awake body's force accumulator.
 *
 * Static/kinematic/sleeping bodies are untouched -- their motion (if any) is driven
 * externally (scripts, animation) rather than by force accumulation.
 *
 * @param body    Body to accumulate into.
 * @param gravity World-space gravity acceleration.
 */
inline void apply_gravity(Body& body, const glm::vec3& gravity) {
    if (body.type != BodyType::Dynamic || !body.awake) return;
    if (!body.use_gravity) return;
    body.force_accum += gravity * body.mass;
}

/**
 * @brief Zeroes velocity components along axes frozen by Body::constraints.
 *
 * Applied after every velocity change (integration and, later, the solver) so a frozen
 * axis never accumulates drift from forces or impulses applied along it.
 */
inline void apply_constraints(Body& body) {
    if (body.constraints & kFreezePosX) body.linear_velocity.x = 0.0f;
    if (body.constraints & kFreezePosY) body.linear_velocity.y = 0.0f;
    if (body.constraints & kFreezePosZ) body.linear_velocity.z = 0.0f;
    if (body.constraints & kFreezeRotX) body.angular_velocity.x = 0.0f;
    if (body.constraints & kFreezeRotY) body.angular_velocity.y = 0.0f;
    if (body.constraints & kFreezeRotZ) body.angular_velocity.z = 0.0f;
}

/**
 * @brief Recomputes a dynamic body's world-space inverse inertia tensor from its current
 *        orientation: R * diag(inv_inertia_local) * R^T. Static/kinematic bodies keep an
 *        all-zero tensor (infinite inertia), matching their zero inv_mass.
 */
inline void update_world_inertia(Body& body) {
    if (body.type != BodyType::Dynamic) {
        body.inv_inertia_world = glm::mat3(0.0f);
        return;
    }
    glm::mat3 r = glm::mat3_cast(body.orientation);
    glm::mat3 diag(0.0f);
    diag[0][0] = body.inv_inertia_local.x;
    diag[1][1] = body.inv_inertia_local.y;
    diag[2][2] = body.inv_inertia_local.z;
    body.inv_inertia_world = r * diag * glm::transpose(r);
}

/**
 * @brief Integrates accumulated force/torque into velocity for one substep (semi-implicit
 *        Euler: velocity is updated from force BEFORE position is updated from velocity),
 *        applies linear/angular drag, then clears the accumulators.
 *
 * @param body Body to integrate. No-op for non-dynamic or sleeping bodies.
 * @param h    Fixed substep length in seconds.
 */
inline void integrate_forces(Body& body, float h) {
    if (body.type != BodyType::Dynamic || !body.awake) {
        body.clear_accumulators();
        return;
    }

    body.linear_velocity += body.force_accum * body.inv_mass * h;
    body.angular_velocity += body.inv_inertia_world * body.torque_accum * h;

    // Implicit damping (Unity's drag model): stable for arbitrarily large drag values,
    // unlike an explicit `v -= v * drag * h` which can overshoot past zero and reverse.
    body.linear_velocity *= 1.0f / (1.0f + h * body.linear_drag);
    body.angular_velocity *= 1.0f / (1.0f + h * body.angular_drag);

    apply_constraints(body);
    body.clear_accumulators();
}

/**
 * @brief Integrates velocity into position/orientation for one substep.
 *
 * Quaternion integration convention: `angular_velocity` is WORLD-SPACE (consistent with
 * `inv_inertia_world` above), so the derivative is the left-multiplied pure-vector product
 * `dq/dt = 0.5 * (0, w) * q`. `exact` selects the exponential-map step (`q *= exp(0.5*w*h)`)
 * instead of the default first-order form, which visibly gains energy on fast spinners.
 *
 * @param body  Body to integrate. No-op for non-dynamic or sleeping bodies.
 * @param h     Fixed substep length in seconds.
 * @param exact Use the exact exponential-map quaternion step instead of first-order.
 * @param max_linear_velocity Hard speed clamp (a tunneling backstop -- see config.h).
 */
inline void integrate_velocities(Body& body, float h, bool exact = false,
                                  float max_linear_velocity = 1e30f) {
    if (body.type != BodyType::Dynamic || !body.awake) return;

    float speed2 = glm::dot(body.linear_velocity, body.linear_velocity);
    float max2 = max_linear_velocity * max_linear_velocity;
    if (speed2 > max2 && speed2 > util::k_epsilon) {
        body.linear_velocity *= max_linear_velocity / std::sqrt(speed2);
    }

    body.position += body.linear_velocity * h;

    if (exact) {
        glm::vec3 w = body.angular_velocity;
        float angle = glm::length(w) * h;
        if (angle > util::k_epsilon) {
            glm::quat delta = glm::angleAxis(angle, w / glm::length(w));
            body.orientation = glm::normalize(delta * body.orientation);
        }
    } else {
        glm::quat w_quat(0.0f, body.angular_velocity.x, body.angular_velocity.y, body.angular_velocity.z);
        glm::quat dq = w_quat * body.orientation;
        body.orientation = glm::normalize(body.orientation + (0.5f * h) * dq);
    }
}

} // namespace dynamics
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DYNAMICS_INTEGRATOR_H
