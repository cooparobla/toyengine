/**
 * @file joint.h
 * @brief Joint -- a constraint between two bodies (hinge, ball-and-socket or cone-twist), and its
 *        generational handle.
 *
 * Three types share one struct (JointType selects which rows the solver runs -- see
 * solve_joint_velocity_pass() in dynamics/solver.h):
 *   - Hinge: point + axis alignment + optional one-sided angle limit. Covers a door (limited
 *     swing) and a passive cantilever (unlimited swing) equally well. A rigid "fixed joint" needs
 *     no separate implementation: `use_limits` with `min_angle == max_angle` (typically both 0)
 *     locks the one DOF a hinge leaves free.
 *   - Ball: the point constraint alone (three free rotational DOFs).
 *   - ConeTwist: the point constraint, a swing CONE (the twist axes of the two bodies may diverge
 *     by at most `swing_limit`) and a twist range about that axis -- a shoulder, hip or neck.
 * No motor, no spring/damper (not needed for doors, cantilevers or ragdolls; a natural, separable
 * future addition if ever needed).
 */

#ifndef PHYSXCOOPA_DYNAMICS_JOINT_H
#define PHYSXCOOPA_DYNAMICS_JOINT_H

#include <physxcoopa/dynamics/body.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>

namespace coopa {
namespace physx {
namespace dynamics {

/**
 * @struct JointId
 * @brief Generational handle into PhysicsWorld's joint array -- same shape as BodyId (see its
 *        own doc for why: a plain, non-atomic generation bump is enough since v1's joint array
 *        is touched only from physxcoopa's single owning thread).
 */
struct JointId {
    static constexpr uint32_t k_invalid_index = 0xFFFFFFFFu;

    uint32_t index = k_invalid_index;
    uint32_t generation = 0;

    bool is_valid() const { return index != k_invalid_index; }

    bool operator==(const JointId& other) const {
        return index == other.index && generation == other.generation;
    }
    bool operator!=(const JointId& other) const { return !(*this == other); }
};

/** @brief Which constraint rows a Joint runs -- see the file doc. */
enum class JointType : uint8_t {
    Hinge,
    Ball,
    ConeTwist,
};

/**
 * @struct Joint
 * @brief One constraint between bodies `a` and `b` -- a hinge, ball or cone-twist (`type`).
 *
 * Anchors/axes are in each body's OWN local frame (rotated/translated by that body's own
 * position/orientation to get the world-space constraint target each solve -- exactly how
 * collision::Shape's local_center/local_rotation already work, see collision/shape.h).
 *
 * Unlike ContactManifold (rediscovered fresh by broadphase every substep, so its warm-start
 * impulses need a stable cross-step key -- see dynamics/solver.h's pack_warm_start_key()),
 * a Joint is an explicit, persistent object: the same struct instance is reused substep to
 * substep (PhysicsWorld::joints_[id.index]), so its impulse accumulators just live as plain
 * fields here and are automatically "warm" every step with no key/hash-map machinery needed.
 */
struct Joint {
    JointType type = JointType::Hinge;

    BodyId a;
    BodyId b;

    glm::vec3 local_anchor_a{0.0f};
    glm::vec3 local_anchor_b{0.0f};
    /** @brief Hinge axis (Hinge) or twist axis (ConeTwist) in each body's own local frame -- need
     *         not be unit length as authored, normalized wherever it's read (see
     *         solve_joint_velocity_pass()'s doc). Unused by Ball. */
    glm::vec3 local_axis_a{0.0f, 0.0f, 1.0f};
    glm::vec3 local_axis_b{0.0f, 0.0f, 1.0f};

    bool use_limits = false;
    float min_angle = 0.0f; /**< Radians, relative to `rest_relative_rotation`. */
    float max_angle = 0.0f; /**< Radians, relative to `rest_relative_rotation`. */

    /** @brief ConeTwist only: the cone half-angle (radians) the two twist axes may diverge by.
     *         Negative (the default) leaves swing free. */
    float swing_limit = -1.0f;
    /** @brief ConeTwist only: twist range (radians) about the twist axis, relative to
     *         `rest_relative_rotation`. `twist_min > twist_max` leaves twist free. */
    float twist_min = 1.0f;
    float twist_max = -1.0f;

    /** @brief `inverse(a.orientation) * b.orientation` at the moment this joint was created --
     *         the reference the current swing angle is measured against for limits (Unity's
     *         HingeJoint.limits are relative to the joint's initial relative orientation the
     *         same way). Set once by PhysicsWorld::add_*_joint() (or by the caller of
     *         PhysicsWorld::add_joint()), never touched again. */
    glm::quat rest_relative_rotation{1.0f, 0.0f, 0.0f, 0.0f};

    /** @brief Per-world-axis (X,Y,Z) point-constraint impulse accumulator. */
    glm::vec3 point_impulse{0.0f};
    /** @brief Per-perpendicular-axis (see solve_joint_velocity_pass()'s `p`/`q` basis) axis-
     *         alignment impulse accumulator. */
    glm::vec2 axis_impulse{0.0f};
    /** @brief Angle-limit impulse accumulator -- Hinge with `use_limits`, or ConeTwist's twist
     *         limit. */
    float limit_impulse = 0.0f;
    /** @brief ConeTwist's swing-limit impulse accumulator. */
    float swing_impulse = 0.0f;

    bool enabled = true;
    /** @brief False once either body is destroyed -- mirrors ContactManifold::valid; a joint
     *         referencing a dead body is skipped by every solver pass, never dereferenced. */
    bool valid = true;

    /** @brief When false (the default -- matching Unity's `HingeJoint.enableCollision`), `a` and
     *         `b` never generate contacts against EACH OTHER, regardless of the layer matrix: a
     *         hinge's whole reason to exist is that the two bodies touch right at the anchor (a
     *         door's edge sits flush against its frame), so without this the contact solver would
     *         fight the joint solver over that same overlap every step -- the joint constraint
     *         wants them to interpenetrate exactly at the hinge line, contact resolution wants to
     *         push them apart, and the two fight to a standstill. See PhysicsWorld::narrowphase_()
     *         for where this is consulted, right alongside the layer-matrix check it sits next to. */
    bool collide_connected = false;

    bool has_swing_limit() const { return type == JointType::ConeTwist && swing_limit >= 0.0f; }
    bool has_twist_limit() const { return type == JointType::ConeTwist && twist_min <= twist_max; }
};

/** @brief The original name -- every joint was a hinge before JointType existed. */
using HingeJoint = Joint;

} // namespace dynamics
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_DYNAMICS_JOINT_H
