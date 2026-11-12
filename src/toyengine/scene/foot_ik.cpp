#include <toyengine/scene/foot_ik.h>

#include <toyengine/anim/ik.h>
#include <toyengine/rig/rig_pose_math.h>
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
        bound_ = false; // rebind on resume
        first_frame_ = true;
        pelvis_offset_ = 0.0f;
        body_pitch_deg_ = 0.0f;
        pelvis_applied_valid_ = false;
    }
}

void FootIK::ik_pre_solve(float dt) {
    if (!owner || !scene || suspended_) return;
    if (!bound_) bind_();
    if (!rig_) return;
    auto* own_tc = owner->get_transform();
    if (!own_tc) return;
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"));
    const float k = blend_speed > 0.0f ? 1.0f - std::exp(-blend_speed * std::max(dt, 0.0f)) : 1.0f;

    // Undo last frame's pelvis drop/pitch before reading anything: nothing is guaranteed to
    // refresh this bone's pose every frame (see pelvis_applied_valid_'s doc), so composing a
    // fresh delta onto last frame's OWN output -- instead of onto whatever the clip alone put
    // there -- would run away over frames.
    if (pelvis_idx_ >= 0 && pelvis_applied_valid_) {
        toy::rig::apply_world_translation_delta(*rig_, pelvis_idx_, -pelvis_translation_applied_);
        toy::rig::apply_world_rotation_delta(*rig_, pelvis_idx_, glm::inverse(pelvis_rotation_applied_), 1.0f);
    }

    const bool grounded = !controller_ || controller_->is_grounded();
    const float want = (physics && grounded) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    const float kk = first_frame_ ? 1.0f : k; // the first frame snaps: no fade-in from a flat pose
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
            leg.foot_pos = t ? glm::vec3(t->get_transform()->get_world_matrix()[3])
                              : toy::rig::bone_world_position(*rig_, leg.end_idx);
        } else {
            leg.foot_pos = toy::rig::bone_world_position(*rig_, leg.end_idx);
        }
        float target_offset = 0.0f;
        glm::vec3 target_normal = up;
        if (physics && leg.end_idx >= 0 && blend_ > 0.0f) {
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
    if (pelvis_idx_ >= 0) {
        pelvis_translation_applied_ = up * drop;
        if (drop != 0.0f) toy::rig::apply_world_translation_delta(*rig_, pelvis_idx_, pelvis_translation_applied_);
        pelvis_rotation_applied_ = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        if (body_pitch) {
            // Pitch about the character's right axis: + raises the front.
            const glm::vec3 right = glm::normalize(glm::vec3(own_world[0]));
            pelvis_rotation_applied_ = glm::angleAxis(pitch, right);
            toy::rig::apply_world_rotation_delta(*rig_, pelvis_idx_, pelvis_rotation_applied_, 1.0f);
        }
        pelvis_applied_valid_ = true;
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
        if (leg.end_idx < 0) continue;
        // The animated foot, raised/lowered onto the ground under it (the clip's lift kept).
        const glm::vec3 target = leg.foot_pos + up * leg.offset;
        const glm::vec3 knee = toy::rig::bone_world_position(*rig_, leg.shin_idx);
        leg.solver.set_target_position(target);
        leg.solver.set_pole_position(knee + own_rot * leg.pole); // a knee bends its leg's way
        leg.solver.weight = blend_;
        leg.solver.solve(dt);
    }
}

void FootIK::ik_post_solve(float) {
    if (suspended_ || !rig_) return;
    for (auto& leg_ptr : legs_) {
        LegState& leg = *leg_ptr;
        if (leg.end_idx < 0 || !align_feet || blend_ <= 0.0f) continue;
        const glm::vec3 up = up_;
        const glm::vec3 n = toy::anim::ik::clamp_direction(up, leg.normal, glm::radians(std::max(0.0f, max_foot_angle)));
        const glm::quat tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), toy::anim::ik::rotation_between(up, n), blend_);
        toy::rig::apply_world_rotation_delta(*rig_, leg.end_idx, tilt, 1.0f);
    }
}

void FootIK::bind_() {
    rig_ = nullptr;
    for (coopa::scene::SceneObject* o = owner; o; o = o->parent()) {
        if (auto* r = o->get_component<toy::rig::Rig>()) { rig_ = r; break; }
    }
    pelvis_idx_ = rig_ ? rig_->find_bone(pelvis) : -1;

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
        leg->thigh_idx = rig_ ? rig_->find_bone(def->thigh) : -1;
        leg->shin_idx = rig_ ? rig_->find_bone(def->shin) : -1;
        leg->end_idx = rig_ ? rig_->find_bone(def->foot) : -1;
        leg->pole = glm::length(def->pole) > 1e-6f ? glm::normalize(def->pole) : glm::vec3(0.0f, 1.0f, 0.0f);
        leg->front = def->front;
        legs_.push_back(std::move(leg));
    }
    // Targets mode: a TwoBoneIK on this rig whose end bone is the leg's foot.
    if (mode != "solve" && rig_) {
        owner->for_each_recursive([this](const coopa::scene::SceneObject& o) {
            for (const auto& comp : o.components()) {
                auto* ik = dynamic_cast<toy::rig::TwoBoneIK*>(comp.get());
                if (!ik) continue;
                const int end_idx = ik->end.empty() ? -1 : rig_->find_bone(ik->end);
                if (end_idx < 0) continue;
                for (auto& leg : legs_) {
                    if (leg->end_idx == end_idx) leg->rig_ik = ik;
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
