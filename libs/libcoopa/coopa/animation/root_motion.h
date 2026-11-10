/**
 * @file root_motion.h
 * @brief The hand-off point for an Animator's root motion: a component on the Animator's owner
 *        that implements IRootMotionReceiver consumes the per-frame delta instead of the Animator
 *        moving the owner's Transform itself.
 */

#ifndef COOPA_ANIMATION_ROOT_MOTION_H
#define COOPA_ANIMATION_ROOT_MOTION_H

#include <glm/glm.hpp>

namespace coopa {
namespace anim {

/**
 * @struct RootMotionDelta
 * @brief One frame of extracted root motion, in the Animator owner's LOCAL frame (the frame the
 *        root bone's clip-space position is authored in) -- for the engine's rigs, +Y forward,
 *        Z up.
 */
struct RootMotionDelta {
    glm::vec3 translation{0.0f};
    float     yaw_deg = 0.0f;   ///< Rotation about +Z.

    bool is_zero() const { return translation == glm::vec3(0.0f) && yaw_deg == 0.0f; }
};

/**
 * @class IRootMotionReceiver
 * @brief Implemented by a component that wants to move its object by an Animator's root motion
 *        itself (e.g. a character controller that still has to collide).
 *
 * With Animator::apply_root_motion on, the Animator offers each frame's delta to the first
 * component on its owner implementing this interface; a receiver returning false (say, a
 * controller whose own root-motion switch is off) leaves the Animator to move the owner's
 * Transform directly.
 */
class IRootMotionReceiver {
public:
    virtual ~IRootMotionReceiver() = default;

    /**
     * @param local_delta Translation in the owner's local frame.
     * @param yaw_delta_deg Turn about +Z, degrees.
     * @return true if consumed; false to let the Animator apply it to the Transform.
     */
    virtual bool consume_root_motion(const glm::vec3& local_delta, float yaw_delta_deg) = 0;
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_ROOT_MOTION_H
