/**
 * @file body.h
 * @brief Rigid body dynamic state and its generational handle.
 */

#ifndef PHYSXCOOPA_DYNAMICS_BODY_H
#define PHYSXCOOPA_DYNAMICS_BODY_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

namespace coopa {
namespace physx {
namespace dynamics {

/** @brief A body's simulation role. */
enum class BodyType {
    Static,    /**< inv_mass == 0, never moves; not a broadphase query initiator. */
    Kinematic, /**< inv_mass == 0, but moved externally (script/animation); imparts velocity. */
    Dynamic,   /**< Fully simulated. */
};

/**
 * @brief How a body's render transform is interpolated between fixed substeps.
 *
 * `Extrapolate` is deferred to v1.1 (see the plan) -- only None/Interpolate ship in v1.
 */
enum class Interpolation {
    None,
    Interpolate,
};

/**
 * @brief Per-axis freeze flags, zeroing the corresponding inverse-mass/inverse-inertia terms
 *        so the solver never applies an impulse along a frozen axis.
 */
enum BodyConstraints : uint32_t {
    kConstraintNone = 0,
    kFreezePosX = 1u << 0,
    kFreezePosY = 1u << 1,
    kFreezePosZ = 1u << 2,
    kFreezeRotX = 1u << 3,
    kFreezeRotY = 1u << 4,
    kFreezeRotZ = 1u << 5,
    kFreezePosition = kFreezePosX | kFreezePosY | kFreezePosZ,
    kFreezeRotation = kFreezeRotX | kFreezeRotY | kFreezeRotZ,
};

/**
 * @struct BodyId
 * @brief Generational handle into PhysicsWorld's body array.
 *
 * Shape mirrors coopa::job::JobHandle's current design ({index, generation}, libcoopa's
 * handle.h:459-461) -- both exist to stop a stale handle from addressing a slot recycled by
 * someone else. BodyId's version is simpler than JobHandle's: it needs only a plain,
 * non-atomic generation bump checked serially, since v1's body array is touched only from
 * physxcoopa's single owning thread, never JobHandle's lock-free multi-producer free list.
 */
struct BodyId {
    static constexpr uint32_t k_invalid_index = 0xFFFFFFFFu;

    uint32_t index = k_invalid_index;
    uint32_t generation = 0;

    bool is_valid() const { return index != k_invalid_index; }

    bool operator==(const BodyId& other) const {
        return index == other.index && generation == other.generation;
    }
    bool operator!=(const BodyId& other) const { return !(*this == other); }
    bool operator<(const BodyId& other) const {
        if (index != other.index) return index < other.index;
        return generation < other.generation;
    }
};

/**
 * @struct Body
 * @brief Full dynamic state of one rigid body. Geometry/material live alongside this in
 *        PhysicsWorld's parallel shape array (see collision/shape.h), not here -- Body is
 *        pure dynamics state, shape-agnostic.
 */
struct Body {
    BodyType type = BodyType::Dynamic;

    glm::vec3 position{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 linear_velocity{0.0f};
    glm::vec3 angular_velocity{0.0f};

    glm::vec3 force_accum{0.0f};
    glm::vec3 torque_accum{0.0f};

    float mass = 1.0f;
    float inv_mass = 1.0f;

    /** @brief Diagonal of the local-space inverse inertia tensor (see dynamics/inertia.h). */
    glm::vec3 inv_inertia_local{1.0f};

    /** @brief World-space inverse inertia tensor, recomputed every substep as
     *         R * diag(inv_inertia_local) * R^T from the body's current orientation. */
    glm::mat3 inv_inertia_world{1.0f};

    float linear_drag = 0.0f;
    float angular_drag = 0.05f;
    bool use_gravity = true;

    uint32_t constraints = kConstraintNone;

    bool awake = true;
    float sleep_timer = 0.0f;

    /** @brief Transform at the end of the previous substep, for render interpolation. */
    glm::vec3 prev_position{0.0f};
    glm::quat prev_orientation{1.0f, 0.0f, 0.0f, 0.0f};
    Interpolation interpolation = Interpolation::Interpolate;

    /** @brief Position/orientation physics itself last wrote to this body's Transform --
     *         PhysicsSystem's sync-in pass compares the Transform's CURRENT value against
     *         this to notice a script-driven set_position() on a dynamic body, distinct
     *         from physics' own last write. Kinematic bodies use the same fields to derive
     *         (new - old)/h as their velocity each step (see PhysicsSystem). */
    glm::vec3 last_written_position{0.0f};
    glm::quat last_written_orientation{1.0f, 0.0f, 0.0f, 0.0f};

    /** @brief Union-find island root index, rebuilt every substep by dynamics/island.h. */
    uint32_t island_id = 0;

    /** @brief Applies a world-space force at the center of mass; cleared after each integrate.
     *         Wakes a sleeping Dynamic body (matches Unity's AddForce) -- a force accumulated
     *         into a sleeping body's force_accum would otherwise sit inert, since
     *         integrate_forces()/integrate_velocities() both early-out on `!awake`. */
    void add_force(const glm::vec3& f) {
        force_accum += f;
        if (type == BodyType::Dynamic) wake();
    }

    /** @brief Applies a world-space torque; cleared after each integrate. See add_force()'s doc
     *         for why this wakes the body. */
    void add_torque(const glm::vec3& t) {
        torque_accum += t;
        if (type == BodyType::Dynamic) wake();
    }

    /** @brief Applies a world-space force at a world-space point, deriving the resulting torque.
     *         See add_force()'s doc for why this wakes the body. */
    void add_force_at_position(const glm::vec3& f, const glm::vec3& world_point) {
        force_accum += f;
        torque_accum += glm::cross(world_point - position, f);
        if (type == BodyType::Dynamic) wake();
    }

    /** @brief Applies an instantaneous world-space impulse at the center of mass, changing
     *         linear_velocity immediately (unlike add_force(), which accumulates for the next
     *         integrate_forces() call). Wakes a sleeping Dynamic body -- see add_force()'s doc. */
    void apply_impulse(const glm::vec3& impulse) {
        linear_velocity += impulse * inv_mass;
        if (type == BodyType::Dynamic) wake();
    }

    /** @brief Applies an instantaneous world-space angular impulse, changing angular_velocity
     *         immediately. See apply_impulse()'s doc. */
    void apply_angular_impulse(const glm::vec3& angular_impulse) {
        angular_velocity += inv_inertia_world * angular_impulse;
        if (type == BodyType::Dynamic) wake();
    }

    /** @brief Applies an instantaneous world-space impulse at a world-space point: the linear
     *         part changes linear_velocity and the induced angular impulse
     *         `cross(world_point - position, impulse)` changes angular_velocity, both immediately.
     *         The impulse counterpart of add_force_at_position() -- for callers acting from inside
     *         PhysicsWorld::on_substep, which fires AFTER integrate_forces() (see world.h's
     *         step_fixed()), so a force added there lands one substep late while an impulse
     *         reaches this substep's solve. Wakes a sleeping Dynamic body -- see add_force() --
     *         unless `wake_body` is false: a CONTINUOUS per-substep field (buoyancy, wind) must
     *         not reset sleep_timer every substep, or a body resting in it could never fall
     *         asleep; such a caller wakes the body itself when the field actually changes. */
    void apply_impulse_at_position(const glm::vec3& impulse, const glm::vec3& world_point,
                                   bool wake_body = true) {
        linear_velocity += impulse * inv_mass;
        angular_velocity += inv_inertia_world * glm::cross(world_point - position, impulse);
        if (wake_body && type == BodyType::Dynamic) wake();
    }

    /** @brief World-space velocity of the material point currently at `world_point`:
     *         `linear_velocity + cross(angular_velocity, world_point - position)` (position is
     *         the center of mass). */
    glm::vec3 velocity_at_point(const glm::vec3& world_point) const {
        return linear_velocity + glm::cross(angular_velocity, world_point - position);
    }

    /** @brief Clears accumulated force/torque -- called once per substep after integration. */
    void clear_accumulators() {
        force_accum = glm::vec3(0.0f);
        torque_accum = glm::vec3(0.0f);
    }

    /** @brief Wakes the body and resets its sleep timer. */
    void wake() {
        awake = true;
        sleep_timer = 0.0f;
    }

    /** @brief Puts the body to sleep immediately, bypassing the usual sleep_time-below-threshold
     *         accumulation in update_islands_and_sleep() -- for explicit script control (Unity's
     *         Rigidbody.Sleep()). Also zeroes velocity, matching Unity: a sleeping body is meant
     *         to be at rest, not merely "not yet re-evaluated." */
    void sleep() {
        awake = false;
        sleep_timer = 0.0f;
        linear_velocity = glm::vec3(0.0f);
        angular_velocity = glm::vec3(0.0f);
    }
};

} // namespace dynamics
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DYNAMICS_BODY_H
