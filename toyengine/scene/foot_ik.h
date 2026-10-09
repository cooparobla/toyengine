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
 * @endcode
 */

#ifndef TOYENGINE_SCENE_FOOT_IK_H
#define TOYENGINE_SCENE_FOOT_IK_H

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include <coopa/animation/ik.h>
#include <coopa/animation/ik_components.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/geometry/ray.h>
#include <physxcoopa/system/physics_system.h>

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
    void set_suspended(bool suspended) {
        if (suspended == suspended_) return;
        suspended_ = suspended;
        if (!suspended) {
            bound_ = false;   // rebind: fresh pose guards for the legs and feet
            first_frame_ = true;
            pelvis_written_valid_ = false;
            pelvis_offset_ = 0.0f;
            for (LegState& leg : legs_) leg.foot_guard = coopa::anim::IkPoseGuard{};
        }
    }
    bool is_suspended() const { return suspended_; }

    void ik_pre_solve(float dt) override {
        if (!owner || !scene || suspended_) return;
        if (!bound_) bind_();
        auto* own_tc = owner->get_transform();
        if (!own_tc) return;
        auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"));
        const float k = blend_speed > 0.0f ? 1.0f - std::exp(-blend_speed * std::max(dt, 0.0f)) : 1.0f;

        // Restore an un-animated rig (legs, pelvis) before reading any position: this frame's
        // targets come from the animated pose, never from last frame's IK output.
        for (LegState& leg : legs_) leg.solver.restore_input();
        coopa::util::Transform* pelvis_t = pelvis_obj_ && pelvis_obj_->get_transform() ? &pelvis_obj_->get_transform()->transform() : nullptr;
        if (pelvis_t) {
            if (pelvis_written_valid_ && pelvis_t->position() == pelvis_written_) pelvis_t->set_position(pelvis_input_);
            pelvis_input_ = pelvis_t->position();
        }

        const bool grounded = !controller_ || controller_->is_grounded();
        const float want = (physics && grounded) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
        const float kk = first_frame_ ? 1.0f : k;   // the first frame snaps: no fade-in from a flat pose
        blend_ += (want - blend_) * kk;
        if (blend_ < 1e-4f && want == 0.0f) blend_ = 0.0f;

        const glm::mat4 own_world = own_tc->get_world_matrix();
        const glm::vec3 base(own_world[3]);
        const glm::vec3 up(0.0f, 0.0f, 1.0f);
        coopa::physx::system::PhysicsSystem::QueryFilter filter;
        filter.layer_mask = layer_mask;
        filter.include_triggers = false;
        filter.ignore_rigidbody = rigidbody_;
        // Nor the rig's own colliders (a Ragdoll's bones sit right where the rays start).
        filter.predicate = [self = owner](const coopa::physx::components::Collider& c) {
            for (const coopa::scene::SceneObject* o = c.owner; o; o = o->parent())
                if (o == self) return false;
            return true;
        };

        for (LegState& leg : legs_) {
            leg.foot_pos = coopa::anim::ik_world_position(leg.foot_obj);
            float target_offset = 0.0f;
            glm::vec3 target_normal = up;
            if (physics && leg.foot_obj && blend_ > 0.0f) {
                coopa::physx::geometry::Ray ray;
                ray.origin = glm::vec3(leg.foot_pos.x, leg.foot_pos.y, base.z + ray_up);
                ray.direction = -up;
                ray.max_distance = ray_up + ray_down;
                coopa::physx::system::PhysicsSystem::RaycastHit hit;
                if (physics->raycast(ray, hit, filter) && !hit.started_inside) {
                    target_offset = std::clamp(hit.point.z - base.z, -ray_down, ray_up);
                    if (hit.normal.z > 0.2f) target_normal = glm::normalize(hit.normal);
                }
            }
            leg.offset += (target_offset - leg.offset) * kk;
            leg.normal = glm::normalize(glm::mix(leg.normal, target_normal, kk));
        }
        first_frame_ = false;

        // Drop the pelvis so the lower foot can reach; the higher one bends its knee.
        const float drop = std::clamp(std::min({legs_[0].offset, legs_[1].offset, 0.0f}), -max_pelvis_drop, 0.0f) * blend_;
        pelvis_offset_ = drop;
        if (pelvis_t) {
            if (drop != 0.0f) {
                const glm::quat parent_rot = coopa::anim::ik_parent_world_rotation(pelvis_obj_);
                pelvis_t->set_position(pelvis_input_ + glm::inverse(parent_rot) * (up * drop));
            }
            pelvis_written_ = pelvis_t->position();
            pelvis_written_valid_ = true;
        }

        const glm::vec3 forward = glm::normalize(glm::vec3(own_world[1]));
        for (LegState& leg : legs_) {
            if (!leg.foot_obj) continue;
            // The animated foot, raised/lowered onto the ground under it (the clip's lift kept).
            const glm::vec3 target = leg.foot_pos + up * leg.offset;
            const glm::vec3 knee = coopa::anim::ik_world_position(leg.shin_obj);
            leg.solver.set_target_position(target);
            leg.solver.set_pole_position(knee + forward);   // knees bend forward
            leg.solver.weight = blend_;
            leg.solver.solve(dt);
        }
    }

    void ik_post_solve(float) override {
        if (suspended_) return;
        for (LegState& leg : legs_) {
            if (!leg.foot_obj || !leg.foot_obj->get_transform()) continue;
            coopa::util::Transform& t = leg.foot_obj->get_transform()->transform();
            const glm::quat local_in = leg.foot_guard.begin(t);
            if (!align_feet || blend_ <= 0.0f) continue;
            const glm::vec3 up(0.0f, 0.0f, 1.0f);
            const glm::vec3 n = coopa::anim::ik::clamp_direction(up, leg.normal, glm::radians(std::max(0.0f, max_foot_angle)));
            const glm::quat tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), coopa::anim::ik::rotation_between(up, n), blend_);
            leg.foot_guard.end(t, coopa::anim::ik::to_local_delta(coopa::anim::ik_parent_world_rotation(leg.foot_obj), local_in, tilt));
        }
    }

private:
    struct LegState {
        coopa::anim::TwoBoneIK solver;          ///< Not a scene component: owned and solved here.
        coopa::scene::SceneObject* shin_obj = nullptr;
        coopa::scene::SceneObject* foot_obj = nullptr;
        coopa::anim::IkPoseGuard foot_guard;
        glm::vec3 foot_pos{0.0f};
        glm::vec3 normal{0.0f, 0.0f, 1.0f};
        float offset = 0.0f;
    };

    void bind_() {
        pelvis_obj_ = coopa::anim::resolve_ik_path(owner, scene, pelvis);
        const Leg* defs[2] = {&left, &right};
        for (int i = 0; i < 2; ++i) {
            LegState& leg = legs_[i];
            leg.solver.owner = owner;
            leg.solver.scene = scene;
            leg.solver.upper = defs[i]->thigh;
            leg.solver.lower = defs[i]->shin;
            leg.solver.end = defs[i]->foot;
            leg.solver.soft_limit = 0.02f;
            leg.solver.rebind();
            leg.shin_obj = coopa::anim::resolve_ik_path(owner, scene, defs[i]->shin);
            leg.foot_obj = coopa::anim::resolve_ik_path(owner, scene, defs[i]->foot);
        }
        controller_ = owner->get_component<CharacterController>();
        rigidbody_ = owner->get_component<coopa::physx::components::RigidbodyComponent>();
        bound_ = true;
    }

    bool bound_ = false;
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
