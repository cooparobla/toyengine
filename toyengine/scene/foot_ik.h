/**
 * @file foot_ik.h
 * @brief Plants a two-legged rig's feet on uneven ground: a ray below each foot finds the
 *        ground, the pelvis drops by the lower foot's offset, two TwoBoneIK solves bend the legs
 *        onto the hits, and the feet tilt to the ground normal.
 *
 * An coopa::anim::IkDriver, so coopa::anim::IkSystem (order 320, after the Animator) runs it:
 * ik_pre_solve() probes and solves the legs, ik_post_solve() tilts the feet. It lives in
 * toyengine rather than libcoopa because it queries the physics world.
 *
 * Two ways to bend the legs, picked by `mode`:
 *   - solve: FootIK owns the leg solves -- two TwoBoneIKs of its own aimed at the ANIMATED feet
 *     moved onto the ground (a keyframed rig, the plain mannequin).
 *   - targets: the legs are already IK-driven by TwoBoneIK components on the rig (a fully IK
 *     rig, objects/characters/mannequin_ik): FootIK probes under each leg's target instead and
 *     overrides that target's position, raised/lowered onto the ground; the legs' own solvers do
 *     the bending.
 *   - auto (default): targets when a TwoBoneIK on this rig drives each foot, else solve.
 *
 * The rays start `ray_up` above the character's base plane (its origin -- the feet) at each
 * foot's animated XY and reach `ray_down` below it, ignoring the character's own body (the
 * Rigidbody on this object, e.g. a CharacterController's). A foot's ground offset is the hit's
 * height above the base plane; the animated lift of a swinging foot is kept on top of it. Only
 * while grounded: with a CharacterController on this object, IK fades out in the air.
 *
 * Example YAML (the mannequin's default bone paths):
 * @code
 * - type: FootIK
 *   pelvis: pelvis
 *   left: [pelvis/thigh_l, pelvis/thigh_l/shin_l, pelvis/thigh_l/shin_l/foot_l]
 *   right: [pelvis/thigh_r, pelvis/thigh_r/shin_r, pelvis/thigh_r/shin_r/foot_r]
 *   ray_up: 0.5
 *   ray_down: 0.5
 *   max_pelvis_drop: 0.35
 *   align_feet: true
 *   max_foot_angle: 30
 *   blend_speed: 12
 *   mode: auto                  # auto | solve | targets
 * @endcode
 */

#ifndef TOYENGINE_SCENE_FOOT_IK_H
#define TOYENGINE_SCENE_FOOT_IK_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include <coopa/animation/ik_components.h>


#include <toyengine/scene/character_controller.h>

namespace toy {
namespace scene {

/**
 * @class FootIK
 * @brief Ground-adapting legs for a biped rig (see file doc).
 */
class FootIK : public coopa::anim::IkDriver {
public:
    std::string type_name() const override { return "FootIK"; }

    /** @brief One leg's bones, as paths from this object. */
    struct Leg {
        std::string thigh, shin, foot;
    };

    std::string pelvis = "pelvis";
    Leg left{"pelvis/thigh_l", "pelvis/thigh_l/shin_l", "pelvis/thigh_l/shin_l/foot_l"};
    Leg right{"pelvis/thigh_r", "pelvis/thigh_r/shin_r", "pelvis/thigh_r/shin_r/foot_r"};
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
    /** @brief Each foot's smoothed ground offset (m above the base plane); 0 = left, 1 = right. */
    float foot_offset(int i) const { return legs_[i].offset; }
    /** @brief The effective weight after the grounded fade. */
    float current_weight() const { return blend_; }

    /**
     * @brief Hands the legs over to something else (a Ragdoll's physics) or takes them back.
     *        While suspended nothing is probed, solved or restored; resuming starts fresh, as on
     *        the first frame (no fade from a stale offset, no restore of a stale input pose).
     */
    void set_suspended(bool suspended);
    bool is_suspended() const { return suspended_; }

    void ik_pre_solve(float dt) override;

    void ik_post_solve(float) override;

private:
    struct LegState {
        coopa::anim::TwoBoneIK solver;          ///< Not a scene component: owned and solved here (solve mode).
        coopa::anim::TwoBoneIK* rig_ik = nullptr; ///< The rig's own leg solver (targets mode).
        coopa::scene::SceneObject* shin_obj = nullptr;
        coopa::scene::SceneObject* foot_obj = nullptr;
        coopa::anim::IkPoseGuard foot_guard;
        glm::vec3 foot_pos{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
        float offset = 0.0f;
    };

    void bind_();

    bool bound_ = false;
    bool targets_mode_ = false;
    bool suspended_ = false;
    bool first_frame_ = true;
    coopa::scene::SceneObject* pelvis_obj_ = nullptr;
    CharacterController* controller_ = nullptr;
    coopa::physx::components::RigidbodyComponent* rigidbody_ = nullptr;
    LegState legs_[2];
    float blend_ = 0.0f;
    float pelvis_offset_ = 0.0f;
    glm::vec3 pelvis_input_{0.0f};
    glm::vec3 pelvis_written_{0.0f};
    bool pelvis_written_valid_ = false;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_FOOT_IK_H
