/**
 * @file ball_joint.h
 * @brief BallJointComponent -- scene-authorable ball-and-socket joint (every rotation free),
 *        resolved to a dynamics::Joint of type Ball by PhysicsSystem at bind time.
 *
 * Example YAML:
 * @code
 * - type: BallJoint
 *   connected_object: ceiling
 *   anchor: {x: 0, y: 0, z: 0.5}
 * @endcode
 */

#ifndef PHYSXCOOPA_COMPONENTS_BALL_JOINT_H
#define PHYSXCOOPA_COMPONENTS_BALL_JOINT_H

#include <physxcoopa/components/joint_component.h>

namespace coopa {
namespace physx {
namespace components {

/** @class BallJointComponent
 *  @brief Pins this object's `anchor` to the same world point on `connected_object`. */
class BallJointComponent : public JointComponent {
public:
    std::string type_name() const override { return "BallJoint"; }
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_BALL_JOINT_H
