#include <toyengine/scene/foot_ik.h>

#include <coopa/animation/ik.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/geometry/ray.h>
#include <physxcoopa/system/physics_system.h>

namespace toy {
namespace scene {

void FootIK::set_suspended(bool suspended) {
    if (suspended == suspended_) return;
    suspended_ = suspended;
    if (!suspended) {
        bound_ = false;   // rebind: fresh pose guards for the legs and feet
        first_frame_ = true;
        pelvis_written_valid_ = false;
        pelvis_offset_ = 0.0f;
        body_pitch_deg_ = 0.0f;
        pelvis_rot_guard_ = coopa::anim::IkPoseGuard{};
        for (auto& leg : legs_) leg->foot_guard = coopa::anim::IkPoseGuard{};
    }
}

void FootIK::ik_pre_solve(float dt) {
    if (!owner || !scene || suspended_) return;
    if (!bound_) bind_();
    auto* own_tc = owner->get_transform();
    if (!own_tc) return;
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"));
    const float k = blend_speed > 0.0f ? 1.0f - std::exp(-blend_speed * std::max(dt, 0.0f)) : 1.0f;

    // Restore an un-animated rig (legs, pelvis) before reading any position: this frame's
    // targets come from the animated pose, never from last frame's IK output. (Targets mode:
    // the rig's solvers restore their own legs when they run.)
    if (!targets_mode_) for (auto& leg : legs_) leg->solver.restore_input();
    coopa::util::Transform* pelvis_t = pelvis_obj_ && pelvis_obj_->get_transform() ? &pelvis_obj_->get_transform()->transform() : nullptr;
    glm::quat pelvis_rot_in(1.0f, 0.0f, 0.0f, 0.0f);
    if (pelvis_t) {
        if (pelvis_written_valid_ && pelvis_t->position() == pelvis_written_) pelvis_t->set_position(pelvis_input_);
        pelvis_input_ = pelvis_t->position();
        if (body_pitch) pelvis_rot_in = pelvis_rot_guard_.begin(*pelvis_t);
    }

    const bool grounded = !controller_ || controller_->is_grounded();
    const float want = (physics && grounded) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    const float kk = first_frame_ ? 1.0f : k;   // the first frame snaps: no fade-in from a flat pose
    blend_ += (want - blend_) * kk;
    if (blend_ < 1e-4f && want == 0.0f) blend_ = 0.0f;

    const glm::mat4 own_world = own_tc->get_world_matrix();
    const glm::vec3 base(own_world[3]);
    // In a gravity field (a planet) the ground is "below" along the character's own up; the
    // flat world keeps world +Z exactly.
    const bool field = physics && physics->world().gravity_source_count() > 0;
    up_ = field ? glm::normalize(glm::vec3(own_world[2])) : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::vec3 up = up_;
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

    for (auto& leg_ptr : legs_) {
        LegState& leg = *leg_ptr;
        // Where the foot is going this frame: the animated foot, or (targets mode) its target.
        if (targets_mode_) {
            coopa::scene::SceneObject* t = leg.rig_ik ? leg.rig_ik->target_object() : nullptr;
            leg.foot_pos = t ? coopa::anim::ik_world_position(t) : coopa::anim::ik_world_position(leg.foot_obj);
        } else {
            leg.foot_pos = coopa::anim::ik_world_position(leg.foot_obj);
        }
        float target_offset = 0.0f;
        glm::vec3 target_normal = up;
        if (physics && leg.foot_obj && blend_ > 0.0f) {
            coopa::physx::geometry::Ray ray;
            ray.origin = field ? leg.foot_pos + up * (glm::dot(base - leg.foot_pos, up) + ray_up)
                               : glm::vec3(leg.foot_pos.x, leg.foot_pos.y, base.z + ray_up);
            ray.direction = -up;
            ray.max_distance = ray_up + ray_down;
            coopa::physx::system::PhysicsSystem::RaycastHit hit;
            if (physics->raycast(ray, hit, filter) && !hit.started_inside) {
                target_offset = std::clamp(field ? glm::dot(hit.point - base, up) : hit.point.z - base.z, -ray_down, ray_up);
                if ((field ? glm::dot(hit.normal, up) : hit.normal.z) > 0.2f) target_normal = glm::normalize(hit.normal);
            }
        }
        leg.offset += (target_offset - leg.offset) * kk;
        leg.normal = glm::normalize(glm::mix(leg.normal, target_normal, kk));
    }
    first_frame_ = false;

    const glm::vec3 forward = glm::normalize(glm::vec3(own_world[1]));
    float drop = 0.0f;
    float pitch = 0.0f;
    if (body_pitch) {
        // Quadruped: the body moves by the front/hind average and pitches by their difference
        // over the wheelbase, so every hoof reaches with its leg near the animated length.
        float front = 0.0f, hind = 0.0f, front_y = 0.0f, hind_y = 0.0f;
        int nf = 0, nh = 0;
        for (const auto& leg : legs_) {
            const float along = glm::dot(leg->foot_pos - base, forward);
            if (leg->front) { front += leg->offset; front_y += along; ++nf; }
            else            { hind += leg->offset;  hind_y += along;  ++nh; }
        }
        if (nf > 0 && nh > 0) {
            front /= nf; hind /= nh; front_y /= nf; hind_y /= nh;
            const float wheelbase = std::max(front_y - hind_y, 0.1f);
            const float limit = glm::radians(std::max(0.0f, max_body_pitch));
            pitch = std::clamp(std::atan2(front - hind, wheelbase), -limit, limit) * blend_;
            drop = std::clamp(0.5f * (front + hind), -max_pelvis_drop, max_pelvis_drop) * blend_;
        } else {
            drop = std::clamp(std::min(front + hind, 0.0f), -max_pelvis_drop, 0.0f) * blend_;
        }
    } else {
        // Drop the pelvis so the lowest foot can reach; the higher ones bend their knees.
        float lowest = 0.0f;
        for (const auto& leg : legs_) lowest = std::min(lowest, leg->offset);
        drop = std::clamp(lowest, -max_pelvis_drop, 0.0f) * blend_;
    }
    pelvis_offset_ = drop;
    body_pitch_deg_ = glm::degrees(pitch);
    if (pelvis_t) {
        if (drop != 0.0f) {
            const glm::quat parent_rot = coopa::anim::ik_parent_world_rotation(pelvis_obj_);
            pelvis_t->set_position(pelvis_input_ + glm::inverse(parent_rot) * (up * drop));
        }
        pelvis_written_ = pelvis_t->position();
        pelvis_written_valid_ = true;
        if (body_pitch) {
            // Pitch about the character's right axis: + raises the front.
            const glm::vec3 right = glm::normalize(glm::vec3(own_world[0]));
            const glm::quat delta = glm::angleAxis(pitch, right);
            pelvis_rot_guard_.end(*pelvis_t, coopa::anim::ik::to_local_delta(
                                                 coopa::anim::ik_parent_world_rotation(pelvis_obj_), pelvis_rot_in, delta));
        }
    }

    if (targets_mode_) {
        // The rig's own leg solvers bend the legs (they run after this): move their targets.
        for (auto& leg : legs_) {
            if (leg->rig_ik) leg->rig_ik->set_target_position(leg->foot_pos + up * leg->offset);
        }
        return;
    }

    const glm::mat3 own_rot(glm::normalize(glm::vec3(own_world[0])), forward, glm::normalize(glm::vec3(own_world[2])));
    for (auto& leg_ptr : legs_) {
        LegState& leg = *leg_ptr;
        if (!leg.foot_obj) continue;
        // The animated foot, raised/lowered onto the ground under it (the clip's lift kept).
        const glm::vec3 target = leg.foot_pos + up * leg.offset;
        const glm::vec3 knee = coopa::anim::ik_world_position(leg.shin_obj);
        leg.solver.set_target_position(target);
        leg.solver.set_pole_position(knee + own_rot * leg.pole);   // a knee bends its leg's way
        leg.solver.weight = blend_;
        leg.solver.solve(dt);
    }
}

void FootIK::ik_post_solve(float) {
    if (suspended_) return;
    for (auto& leg_ptr : legs_) {
        LegState& leg = *leg_ptr;
        if (!leg.foot_obj || !leg.foot_obj->get_transform()) continue;
        coopa::util::Transform& t = leg.foot_obj->get_transform()->transform();
        const glm::quat local_in = leg.foot_guard.begin(t);
        if (!align_feet || blend_ <= 0.0f) continue;
        const glm::vec3 up = up_;
        const glm::vec3 n = coopa::anim::ik::clamp_direction(up, leg.normal, glm::radians(std::max(0.0f, max_foot_angle)));
        const glm::quat tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), coopa::anim::ik::rotation_between(up, n), blend_);
        leg.foot_guard.end(t, coopa::anim::ik::to_local_delta(coopa::anim::ik_parent_world_rotation(leg.foot_obj), local_in, tilt));
    }
}

void FootIK::bind_() {
    pelvis_obj_ = coopa::anim::resolve_ik_path(owner, scene, pelvis);
    std::vector<const Leg*> defs;
    if (legs.empty()) defs = {&left, &right};
    else for (const Leg& l : legs) defs.push_back(&l);
    legs_.clear();
    for (const Leg* def : defs) {
        auto leg = std::make_unique<LegState>();
        leg->solver.owner = owner;
        leg->solver.scene = scene;
        leg->solver.upper = def->thigh;
        leg->solver.lower = def->shin;
        leg->solver.end = def->foot;
        leg->solver.soft_limit = 0.02f;
        leg->solver.rebind();
        leg->shin_obj = coopa::anim::resolve_ik_path(owner, scene, def->shin);
        leg->foot_obj = coopa::anim::resolve_ik_path(owner, scene, def->foot);
        leg->pole = glm::length(def->pole) > 1e-6f ? glm::normalize(def->pole) : glm::vec3(0.0f, 1.0f, 0.0f);
        leg->front = def->front;
        legs_.push_back(std::move(leg));
    }
    // Targets mode: a TwoBoneIK on this rig whose end bone is the leg's foot.
    if (mode != "solve") {
        owner->for_each_recursive([this](const coopa::scene::SceneObject& o) {
            for (const auto& comp : o.components()) {
                auto* ik = dynamic_cast<coopa::anim::TwoBoneIK*>(comp.get());
                if (!ik) continue;
                ik->bind();
                for (auto& leg : legs_) {
                    if (leg->foot_obj && ik->end_bone() == leg->foot_obj) leg->rig_ik = ik;
                }
            }
        });
    }
    targets_mode_ = mode != "solve" && !legs_.empty();
    for (const auto& leg : legs_) targets_mode_ = targets_mode_ && leg->rig_ik;
    controller_ = owner->get_component<CharacterController>();
    rigidbody_ = owner->get_component<coopa::physx::components::RigidbodyComponent>();
    bound_ = true;
}

} // namespace scene
} // namespace toy
