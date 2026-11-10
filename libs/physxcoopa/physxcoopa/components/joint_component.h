/**
 * @file joint_component.h
 * @brief JointComponent -- the scene-authorable base every joint component shares (which object
 *        to connect to, where, and the bound dynamics::Joint's id). HingeJointComponent,
 *        BallJointComponent and ConeTwistJointComponent add their own type's axis and limits.
 */

#ifndef PHYSXCOOPA_COMPONENTS_JOINT_COMPONENT_H
#define PHYSXCOOPA_COMPONENTS_JOINT_COMPONENT_H

#include <physxcoopa/dynamics/joint.h>

#include <coopa/scene/component.h>

#include <glm/glm.hpp>

#include <string>

namespace coopa {
namespace physx {
namespace components {

/**
 * @class JointComponent
 * @brief Connects the object this is attached to, to `connected_object`, with the joint type
 *        the subclass names. Binding is one-shot, at the moment both sides first have a bound
 *        Collider -- see HingeJointComponent's doc for the full convention (it was the first).
 */
class JointComponent : public coopa::scene::Component {
public:
    /** @brief Name of the other SceneObject this joint connects to (Scene::find_object(),
     *         resolved once at bind time). Empty means this component stays inert -- no joint
     *         is ever created. */
    std::string connected_object;

    /** @brief Joint anchor, in this component's own object's local frame. */
    glm::vec3 anchor{0.0f};

    /** @brief Whether the two connected bodies still collide with each other (Unity's
     *         `enableCollision`); off by default -- see dynamics::Joint::collide_connected. */
    bool enable_collision = false;

    /** @brief Set only by PhysicsSystem's reconcile pass, once the joint is actually created. */
    void set_joint_id(dynamics::JointId id) { joint_id_ = id; }
    dynamics::JointId joint_id() const { return joint_id_; }

private:
    dynamics::JointId joint_id_;
};

} // namespace components
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_COMPONENTS_JOINT_COMPONENT_H
