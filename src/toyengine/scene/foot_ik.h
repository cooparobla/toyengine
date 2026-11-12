/**
 * @file foot_ik.h
 * @brief Plants a rig's feet on uneven ground: a ray below each foot finds the ground, the
 *        pelvis bone drops by the lower foot's offset, two-bone solves bend the legs onto the
 *        hits, and the feet tilt to the ground normal.
 *
 * A toy::scene::RigIkDriver (rig_ik_driver.h), so RigIkDriverSystem (320's bracket, at 319/321)
 * runs it: ik_pre_solve() probes and solves the legs, ik_post_solve() tilts the feet. It lives
 * in toyengine rather than libcoopa because it queries the physics world. Bones are addressed
 * by NAME within the object's own toy::rig::Rig (the nearest ancestor carrying one) --
 * there is no SceneObject per bone to hold a Transform or an IkPoseGuard, and none is needed:
 * a Rig's pose channel has exactly one writer per frame per bone and nothing to restore (see
 * toyengine/rig/ik_components.h's file doc).
 *
 * Two ways to bend the legs, picked by `mode`:
 *   - solve: FootIK owns the leg solves -- two toy::rig::TwoBoneIKs of its own (constructed
 *     directly, not scene components) aimed at the ANIMATED feet moved onto the ground (a
 *     keyframed rig; the horse).
 *   - targets: the legs are already IK-driven by toy::rig::TwoBoneIK components on the rig
 *     (a fully IK rig): FootIK probes under each leg's target instead and overrides that
 *     target's position, raised/lowered onto the ground; the legs' own solvers do the bending.
 *   - auto (default): targets when a toy::rig::TwoBoneIK on this rig drives each foot bone,
 *     else solve.
 *
 * The rays start `ray_up` above the character's base plane (its origin -- the feet) at each
 * foot's animated XY and reach `ray_down` below it, ignoring the character's own body (the
 * Rigidbody on this object, e.g. a CharacterController's). A foot's ground offset is the hit's
 * height above the base plane; the animated lift of a swinging foot is kept on top of it. Only
 * while grounded: with a CharacterController on this object, IK fades out in the air.
 *
 * Any number of legs: `legs` (a list of {thigh, shin, foot, pole, front}, bone NAMES) replaces
 * left/right -- a quadruped's four. `pole` is each knee's bend direction in the character's own
 * frame (a horse's forelegs bend their knees forward, its hind hocks back). With `body_pitch`
 * on, the `pelvis` bone (a quadruped's body) is no longer just dropped: it moves by the average
 * of the front and hind legs' ground offsets (up or down) and pitches by their difference over
 * the wheelbase, so the hooves on a slope or a step reach the ground with the legs near their
 * animated length.
 *
 * Example YAML (a quadruped, objects/animals/horse):
 * @code
 * - type: FootIK
 *   pelvis: body
 *   mode: solve
 *   body_pitch: true
 *   max_body_pitch: 20
 *   legs:
 *     - {thigh: fore_upper_l, shin: fore_lower_l, foot: fore_hoof_l, front: true}
 *     - {thigh: hind_upper_l, shin: hind_lower_l, foot: hind_hoof_l, pole: {x: 0, y: -1, z: 0}}
 * @endcode
 */

#ifndef TOYENGINE_SCENE_FOOT_IK_H
#define TOYENGINE_SCENE_FOOT_IK_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <toyengine/rig/ik_components.h>
#include <toyengine/rig/rig_component.h>

#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/rig_ik_driver.h>

namespace toy {
namespace scene {

/**
 * @class FootIK
 * @brief Ground-adapting legs for a rig (see file doc).
 */
class FootIK : public coopa::scene::Component, public RigIkDriver {
public:
    std::string type_name() const override { return "FootIK"; }

    /** @brief One leg's bones, as NAMES within the rig. */
    struct Leg {
        std::string thigh, shin, foot;
        glm::vec3 pole{0.0f, 1.0f, 0.0f}; ///< Knee bend direction, character frame (solve mode).
        bool front = false;               ///< A front leg (body_pitch).
    };

    std::string pelvis = "pelvis";
    Leg left{"thigh_l", "shin_l", "foot_l"};
    Leg right{"thigh_r", "shin_r", "foot_r"};
    std::vector<Leg> legs;        ///< Non-empty: these legs instead of left/right.
    bool body_pitch = false;      ///< Quadruped: raise/drop and pitch the pelvis bone (see the file doc).
    float max_body_pitch = 20.0f; ///< Degrees.
    float ray_up = 0.5f;          ///< Ray start above the base plane (m): the highest step a foot rises onto.
    float ray_down = 0.5f;        ///< Ray reach below the base plane (m): the deepest dip a foot follows.
    float max_pelvis_drop = 0.35f;
    bool align_feet = true;       ///< Tilt the feet to the ground normal.
    float max_foot_angle = 30.0f; ///< Degrees.
    float blend_speed = 12.0f;    ///< Rate (1/s) offsets and the grounded fade ease at; 0 snaps.
    float weight = 1.0f;
    uint32_t layer_mask = ~0u;
    std::string mode = "auto";    ///< auto | solve | targets (see the file doc).

    /** @brief Whether the legs are driven through the rig's own TwoBoneIK targets (after binding). */
    bool drives_targets() const { return targets_mode_; }

    /** @brief The pelvis drop applied last frame (m, <= 0). */
    float pelvis_offset() const { return pelvis_offset_; }
    /** @brief Each foot's smoothed ground offset (m above the base plane); 0 = left, 1 = right
     *         (or the `legs` order). */
    float foot_offset(int i) const { return i >= 0 && i < static_cast<int>(legs_.size()) ? legs_[i]->offset : 0.0f; }
    /** @brief The body pitch applied last frame (degrees, + = front up; body_pitch only). */
    float body_pitch_deg() const { return body_pitch_deg_; }
    /** @brief The effective weight after the grounded fade. */
    float current_weight() const { return blend_; }

    /**
     * @brief Hands the legs over to something else (a Ragdoll's physics) or takes them back.
     *        While suspended nothing is probed or solved; resuming starts fresh, as on the
     *        first frame (no fade from a stale offset).
     */
    void set_suspended(bool suspended);
    bool is_suspended() const { return suspended_; }

    void ik_pre_solve(float dt) override;
    void ik_post_solve(float dt) override;

private:
    struct LegState {
        toy::rig::TwoBoneIK solver;             ///< Not a scene component: owned and solved here (solve mode).
        toy::rig::TwoBoneIK* rig_ik = nullptr;  ///< The rig's own leg solver (targets mode).
        int thigh_idx = -1, shin_idx = -1, end_idx = -1;
        glm::vec3 foot_pos{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
        glm::vec3 pole{0.0f, 1.0f, 0.0f};
        bool front = false;
        float offset = 0.0f;
    };

    void bind_();

    bool bound_ = false;
    bool targets_mode_ = false;
    bool suspended_ = false;
    bool first_frame_ = true;
    toy::rig::Rig* rig_ = nullptr;
    int pelvis_idx_ = -1;
    CharacterController* controller_ = nullptr;
    coopa::physx::components::RigidbodyComponent* rigidbody_ = nullptr;
    std::vector<std::unique_ptr<LegState>> legs_;
    float blend_ = 0.0f;
    float pelvis_offset_ = 0.0f;
    /// What ik_pre_solve() added to the pelvis bone last frame -- undone at the start of the
    /// next call before reading it, the same way IkPoseGuard protected a shared Transform in
    /// toy::anim: nothing else is guaranteed to refresh this bone's pose every frame (a
    /// quadruped's "body" commonly has no rotation track at all), so without an
    /// undo the drop/pitch would compose onto its own last output and run away over frames.
    glm::vec3 pelvis_translation_applied_{0.0f};
    glm::quat pelvis_rotation_applied_{1.0f, 0.0f, 0.0f, 0.0f};
    bool pelvis_applied_valid_ = false;
    /// "Up" for the feet: world +Z -- or, where gravity is a field, the character's own up.
    glm::vec3 up_{0.0f, 0.0f, 1.0f};
    float body_pitch_deg_ = 0.0f;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_FOOT_IK_H
