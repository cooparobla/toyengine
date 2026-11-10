/**
 * @file cone_twist_joint.h
 * @brief ConeTwistJointComponent -- scene-authorable cone-twist joint (a shoulder, hip or neck),
 *        resolved to a dynamics::Joint of type ConeTwist by PhysicsSystem at bind time.
 *
 * The anchor is held like a ball joint; this object's `axis` (the twist axis, usually along the
 * limb) may swing at most `swing_limit_deg` away from where it sat relative to
 * `connected_object` at bind time, and the twist about it stays within
 * [twist_min_deg, twist_max_deg] of the bind-time twist.
 *
 * Example YAML:
 * @code
 * - type: ConeTwistJoint
 *   connected_object: torso
 *   anchor: {x: 0, y: 0, z: 0}
 *   axis: {x: 0, y: 0, z: -1}
 *   swing_limit: 60      # degrees; omit (or negative) = free swing
 *   twist: {min: -30, max: 30}
 * @endcode
 */

#ifndef PHYSXCOOPA_COMPONENTS_CONE_TWIST_JOINT_H
#define PHYSXCOOPA_COMPONENTS_CONE_TWIST_JOINT_H

#include <physxcoopa/components/joint_component.h>

namespace coopa {
namespace physx {
namespace components {

/** @class ConeTwistJointComponent
 *  @brief A ball joint with a swing cone and a twist range (see file doc). */
class ConeTwistJointComponent : public JointComponent {
public:
    std::string type_name() const override { return "ConeTwistJoint"; }

    /** @brief Twist axis, in this object's own local frame (normalized at bind time). */
    glm::vec3 axis{0.0f, 0.0f, 1.0f};
    /** @brief Cone half-angle in degrees; negative leaves swing free. */
    float swing_limit_deg = 45.0f;
    /** @brief Twist range in degrees; `twist_min_deg > twist_max_deg` leaves twist free. */
    float twist_min_deg = -30.0f;
    float twist_max_deg = 30.0f;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_CONE_TWIST_JOINT_H
