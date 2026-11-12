/**
 * @file hinge_joint.h
 * @brief HingeJointComponent -- Scene-authorable revolute joint, resolved to a
 *        dynamics::HingeJoint by PhysicsSystem at bind time.
 */

#ifndef PHYSXCOOPA_COMPONENTS_HINGE_JOINT_H
#define PHYSXCOOPA_COMPONENTS_HINGE_JOINT_H

#include <physxcoopa/components/joint_component.h>

#include <glm/glm.hpp>

#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class HingeJointComponent
 * @brief Connects the object this is attached to, to `connected_object`, via a hinge.
 *
 * Unlike dynamics::HingeJoint (which needs both sides' anchor/axis explicitly, each already in
 * that body's own local frame), this component only takes ONE anchor/axis -- in THIS object's
 * own local frame -- matching Unity's HingeJoint default (`autoConfigureConnectedAnchor`):
 * PhysicsSystem computes the connected side's anchor/axis automatically from wherever
 * `connected_object` currently sits, at the moment this joint binds. This is deliberately a
 * one-shot, creation-time-only resolution -- editing this component's fields at runtime (or
 * `connected_object` itself) after the joint has already been created has no effect until the
 * whole joint is torn down and rebuilt (a structural scene change: this component or its owner
 * being removed and re-added), the same "creation-time only" trim already used elsewhere in
 * this project for lower-priority runtime-reconciliation cases (e.g. RigidbodyComponent's
 * center_of_mass_override).
 *
 * Both this object and `connected_object` must already have a bound Collider (the same
 * "Collider is the binding key" convention every other physics component in this codebase
 * follows -- a bare Rigidbody/Transform with no Collider isn't reachable from scene authoring
 * today, see RigidbodyComponent's own file doc for the identical existing limitation).
 */
class HingeJointComponent : public JointComponent {
public:
    std::string type_name() const override { return "HingeJoint"; }

    // connected_object, anchor and enable_collision: see JointComponent.

    /** @brief Hinge axis, in this component's own object's local frame. Need not be unit
     *         length as authored -- normalized at bind time. */
    glm::vec3 axis{0.0f, 0.0f, 1.0f};

    /** @brief Clamps rotation about the hinge axis to [min_angle_deg, max_angle_deg], relative
     *         to the two objects' relative orientation at the moment this joint binds (Unity's
     *         own HingeJoint.limits convention). `min_angle_deg == max_angle_deg` (0 is the
     *         natural choice) locks the hinge rigidly -- see dynamics/joint.h's file doc. */
    bool use_limits = false;
    float min_angle_deg = 0.0f;
    float max_angle_deg = 0.0f;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_HINGE_JOINT_H
