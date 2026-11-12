/**
 * @file bone_attachment_system.h
 * @brief BoneAttachmentSystem: applies every BoneAttachment after this frame's rig poses
 *        (and any rig-space IK) are resolved, before TransformResolve.
 */

#ifndef TOYENGINE_RIG_BONE_ATTACHMENT_SYSTEM_H
#define TOYENGINE_RIG_BONE_ATTACHMENT_SYSTEM_H

#include <toyengine/rig/bone_attachment.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <memory>

namespace toy {
namespace rig {

/** @brief BoneAttachmentSystem's update order: after RigPoseSystem (340), before TransformResolve (350). */
constexpr int k_bone_attachment_update_order = 345;

/**
 * @class BoneAttachmentSystem
 * @brief Registered at k_bone_attachment_update_order; the sole driver of BoneAttachment::apply_().
 */
class BoneAttachmentSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "BoneAttachment"; }

    /**
     * @brief True: this is transform resolve for bone-parented objects, not gameplay.
     *
     * A BoneAttachment IS the only thing placing its owner -- a bone is not a SceneObject, so
     * the scene graph alone cannot put a rig's mesh parts where they belong. Left out of edit
     * mode, every part of a rigged character collapses onto its authored local offset at the
     * rig's own origin (a pile of capsules at the feet), which is what the editor showed before
     * this override existed.
     */
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext&) override {
        for (BoneAttachment* a : scene.get_components<BoneAttachment>()) a->apply_();
    }
};

/**
 * @brief Constructs a BoneAttachmentSystem and registers it at k_bone_attachment_update_order (345).
 * @return Non-owning pointer to the installed system.
 */
inline BoneAttachmentSystem* install_bone_attachment_system(coopa::scene::Scene& scene) {
    auto sys = std::make_unique<BoneAttachmentSystem>();
    BoneAttachmentSystem* raw = sys.get();
    scene.add_system(std::move(sys), k_bone_attachment_update_order);
    return raw;
}

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_BONE_ATTACHMENT_SYSTEM_H
