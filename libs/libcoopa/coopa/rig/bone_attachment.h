/**
 * @file bone_attachment.h
 * @brief Drives an object's world transform from one bone of a Rig -- Blender's bone
 *        parenting, for objects that are not themselves skinned (a rigid prop, a saddle, a
 *        ragdoll capsule).
 */

#ifndef COOPA_RIG_BONE_ATTACHMENT_H
#define COOPA_RIG_BONE_ATTACHMENT_H

#include <coopa/rig/rig_component.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <string>

namespace coopa {
namespace rig {

class BoneAttachmentSystem; // Forward declaration — BoneAttachment grants it friend access.

/**
 * @class BoneAttachment
 * @brief Carries its owner along one bone of a Rig, as if the owner were parented to that
 *        bone instead of to its SceneObject parent.
 *
 * The owner's Transform (position/rotation/scale) AS AUTHORED is the fixed local offset from
 * the bone's anchor — exactly as if it were a child of the bone. That authored value is
 * captured once, in start(): apply_() recomputes the owner's local-to-PARENT transform from
 * it fresh every frame, and never reads the offset back out of the owner's (by-then
 * apply_()-written) Transform — composing the anchor's world transform onto apply_()'s OWN
 * previous output, frame over frame, would compound without bound instead of tracking the
 * bone. Resolved every frame, after the Rig's pose for this frame is current
 * (RigPoseSystem/rig-space IK) and before TransformResolve (350) walks the scene graph for
 * rendering — see install_bone_attachment_system().
 *
 * YAML:
 * @code
 * - type: Transform
 *   position: {x: 0, y: 0, z: 0}   # local offset from the bone anchor
 * - type: BoneAttachment
 *   bone: thigh_l
 *   at: head       # head | tail
 *   rig: Character # optional; default: the nearest ancestor carrying a Rig
 * @endcode
 */
class BoneAttachment : public coopa::scene::Component {
public:
    std::string type_name() const override { return "BoneAttachment"; }

    /** @enum Anchor @brief Which end of the bone the owner rides. */
    enum class Anchor { Head, Tail };

    std::string bone;              ///< Bone name in the rig.
    Anchor      at = Anchor::Head;
    std::string rig_path;          ///< Optional explicit rig (a scene path); empty = nearest ancestor with a Rig.
    /**
     * @brief While true, apply_() is a no-op: the owner's Transform is left exactly as whatever
     *        last wrote it. For a driver (Ragdoll) that takes over writing the SAME Transform
     *        itself for a while (a limp ragdoll bone, driven by physics, not by the rig's now-frozen
     *        pose) -- without this, apply_() would keep re-deriving the Transform from the rig's
     *        stale bone matrix every frame and fight the other writer, each "correction" read back
     *        by the other system as a teleport and answered with growing velocity (see Ragdoll's own
     *        `set_bones_suspended_`).
     */
    bool suspended = false;

    void start() override {
        rebind();
        capture_offset_();
    }

    /** @brief Re-resolves the rig and bone index (after renaming a bone or reparenting). Does
     *         NOT re-capture the authored offset -- see capture_offset_()'s doc. */
    void rebind() {
        _rig = resolve_rig_();
        _bone_index = _rig ? _rig->find_bone(bone) : -1;
    }

    /** @brief The rig this attaches to, or null if none was found. */
    Rig* rig() const { return _rig; }
    /** @brief The resolved bone index, or -1 if `bone` was not found on the rig. */
    int bone_index() const { return _bone_index; }

    /**
     * @brief This frame's world matrix, computed live from the rig's CURRENT pose.
     *
     * apply_() (BoneAttachmentSystem, order 345) is what actually WRITES this into the owner's
     * Transform -- so a reader that runs earlier in the frame (Rider placing itself on a horse's
     * saddle bone at order ~319, say) would see last frame's pose if it read the owner's
     * Transform directly. This recomputes the same anchor_world * _offset this frame's bone pose
     * implies, without waiting for apply_()'s write -- the fix for exactly that staleness.
     * Falls back to the owner's own (possibly stale) world matrix if not yet bound.
     */
    glm::mat4 current_world_matrix() const {
        auto* tc = owner ? owner->get_transform() : nullptr;
        if (!_rig || _bone_index < 0 || !_offset_captured) return tc ? tc->get_world_matrix() : glm::mat4(1.0f);
        glm::vec3 anchor_offset(0.0f);
        if (at == Anchor::Tail) anchor_offset = glm::vec3(0.0f, _rig->bone_length(static_cast<size_t>(_bone_index)), 0.0f);
        const glm::mat4 anchor_world = _rig->world_matrix(static_cast<size_t>(_bone_index)) *
                                       glm::translate(glm::mat4(1.0f), anchor_offset);
        return anchor_world * _offset;
    }

private:
    friend class BoneAttachmentSystem;

    Rig* resolve_rig_() const {
        if (!rig_path.empty() && scene) {
            if (auto* o = scene->find_object_by_path(rig_path)) {
                if (auto* r = o->get_component<Rig>()) return r;
            }
        }
        for (coopa::scene::SceneObject* o = owner; o; o = o->parent()) {
            if (auto* r = o->get_component<Rig>()) return r;
        }
        return nullptr;
    }

    /**
     * @brief Snapshots the owner's Transform, as authored, as the fixed local offset from the
     *        bone's anchor -- called once, from start(), before apply_() has ever run. Reading
     *        it back out of the owner's Transform after that point would read apply_()'s OWN
     *        last output instead of the authored offset (see this class's doc).
     *
     * A no-op after the first successful capture: a caller that builds its own BoneAttachment
     * programmatically (Ragdoll's physics proxies) must call start() itself, since a
     * programmatically-added component gets no automatic start() call -- and the scene's own
     * start() calls it again regardless, once the object is through the normal lifecycle. That
     * second call must not re-capture whatever the FIRST caller already wrote into the
     * Transform by then (Ragdoll's own bind-pose seeding, in this case) as if it were the
     * authored offset.
     */
    void capture_offset_() {
        if (_offset_captured) return;
        auto* tc = owner ? owner->get_transform() : nullptr;
        if (!tc) return;
        const coopa::util::Transform& t = tc->transform();
        _offset = glm::translate(glm::mat4(1.0f), t.position()) * glm::mat4_cast(t.rotation_quat()) *
                  glm::scale(glm::mat4(1.0f), t.scale());
        _offset_captured = true;
    }

    /** @brief Writes the owner's parent-relative local transform so its world pose rides the bone. Main thread only. */
    void apply_() {
        if (suspended || !owner) return;
        // A Rig's RigAsset loads asynchronously -- start()'s rebind() can run before that load
        // finishes (this component sitting on an early-processed object, right after "Rig"
        // itself with no yield point between them), leaving _bone_index at -1. Retry every
        // frame until it resolves, same reasoning as IkComponent::bind_()'s own retry.
        if (!_rig || _bone_index < 0) rebind();
        if (!_rig || _bone_index < 0) return;
        auto* tc = owner->get_transform();
        if (!tc) return;
        if (!_offset_captured) capture_offset_(); // safety net: start() should already have done this
        coopa::util::Transform& t = tc->transform();

        const glm::mat4 parent_world = t.parent() ? t.parent()->get_world_matrix() : glm::mat4(1.0f);
        const glm::mat4 new_local = glm::inverse(parent_world) * current_world_matrix();

        glm::vec3 pos, scale, skew;
        glm::quat rot;
        glm::vec4 persp;
        if (!glm::decompose(new_local, scale, rot, pos, skew, persp)) return;
        t.set_position(pos);
        t.set_rotation_quat(glm::normalize(rot));
        // Scale is left alone: it is already _offset's own scale reproduced by decompose(),
        // since anchor_world carries no scale of its own (a Rig's model matrices are rigid --
        // see rig_component.h's compose_trs doc) -- but writing it back explicitly would be
        // redundant, not wrong, so it is skipped rather than fought over.
    }

    Rig* _rig = nullptr;
    int  _bone_index = -1;
    glm::mat4 _offset{1.0f};
    bool _offset_captured = false;
};

} // namespace rig
} // namespace coopa

#endif // COOPA_RIG_BONE_ATTACHMENT_H
