/**
 * @file rig_system.h
 * @brief RigPoseSystem: resolves every Rig's pose into model matrices once per frame.
 */

#ifndef TOYENGINE_RIG_RIG_SYSTEM_H
#define TOYENGINE_RIG_RIG_SYSTEM_H

#include <toyengine/rig/rig_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <memory>

namespace toy {
namespace rig {

/**
 * @brief RigPoseSystem's update order: after the Animation phase (300), any rig-space IK
 *        (320) and a ragdoll's post-animation blend (330), and before BoneAttachment (345)
 *        and TransformResolve (350) — a dependent needs this frame's final model matrices,
 *        not an intermediate one a still-to-run pose writer would overwrite.
 */
constexpr int k_rig_pose_update_order = 340;

/**
 * @class RigPoseSystem
 * @brief Registered at k_rig_pose_update_order; the sole driver of Rig::resolve_().
 *
 * A Rig has no update()/late_update() of its own, mirroring toy::anim::Animator — only
 * this system turns its pose channels into model matrices, once, after everything that
 * might have posed it this frame (a SkeletalAnimator, an IK solver, a ragdoll, or a hand
 * edit in the editor) has already run.
 */
class RigPoseSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "RigPose"; }

    /** @brief True: turning pose channels into model matrices keeps what is DRAWN consistent
     *         with the authored rest pose and any hand edit, which the editor needs as much as
     *         a running game -- everything riding those matrices (BoneAttachment at 345, the
     *         skinning palette) is stale or unplaced without it. */
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext&) override {
        for (Rig* r : scene.get_components<Rig>()) r->resolve_();
    }
};

/**
 * @brief Constructs a RigPoseSystem and registers it at k_rig_pose_update_order (340).
 * @return Non-owning pointer to the installed system.
 */
inline RigPoseSystem* install_rig_pose_system(coopa::scene::Scene& scene) {
    auto sys = std::make_unique<RigPoseSystem>();
    RigPoseSystem* raw = sys.get();
    scene.add_system(std::move(sys), k_rig_pose_update_order);
    return raw;
}

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_RIG_SYSTEM_H
