#include <toyengine/scene/rider.h>

#include <algorithm>
#include <cmath>
#include <iostream>

#include <coopa/animation/animator.h>
#include <coopa/animation/ik.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/geometry/ray.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/foot_ik.h>
#include <toyengine/scene/mount.h>

namespace toy {
namespace scene {

namespace {

constexpr float kPi = 3.14159265358979f;

float smooth01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

coopa::anim::Animator* find_animator(coopa::scene::SceneObject* obj) {
    if (auto* a = obj->get_component<coopa::anim::Animator>()) return a;
    for (const auto& child : obj->children())
        if (auto* a = find_animator(child.get())) return a;
    return nullptr;
}

glm::vec3 world_pos(coopa::scene::SceneObject* o) { return coopa::anim::ik_world_position(o); }

/** @brief True when `o` is `root` or one of its descendants. */
bool under(const coopa::scene::SceneObject* o, const coopa::scene::SceneObject* root) {
    for (; o; o = o->parent())
        if (o == root) return true;
    return false;
}

} // namespace

void Rider::bind_() {
    bound_ = true;
    if (!owner) return;
    controller_ = owner->get_component<CharacterController>();
    foot_ik_ = owner->get_component<FootIK>();
    animator_ = find_animator(owner);
    spine_ = nullptr;
    for (Limb& l : legs_) l = Limb{};
    for (Limb& l : arms_) l = Limb{};
    legs_[0].end = "foot_l";
    legs_[1].end = "foot_r";
    arms_[0].end = "hand_l";
    arms_[1].end = "hand_r";
    owner->for_each_recursive([this](const coopa::scene::SceneObject& o) {
        for (const auto& comp : o.components()) {
            if (auto* chain = dynamic_cast<coopa::anim::ChainIK*>(comp.get())) {
                if (!spine_) spine_ = chain;
                continue;
            }
            auto* ik = dynamic_cast<coopa::anim::TwoBoneIK*>(comp.get());
            if (!ik) continue;
            ik->bind();
            coopa::scene::SceneObject* end = ik->end_bone();
            if (!end) continue;
            for (Limb* group : {legs_, arms_})
                for (int i = 0; i < 2; ++i)
                    if (end->name() == group[i].end) group[i].ik = ik;
        }
    });
    if (!legs_[0].ik || !legs_[1].ik || !arms_[0].ik || !arms_[1].ik) {
        std::cerr << "[Rider] '" << owner->name()
                  << "': no TwoBoneIK ending at foot_l/foot_r/hand_l/hand_r -- riding on the ride clip alone\n";
    }
}

bool Rider::mount_valid_() {
    if (!mount_ || !scene) return false;
    const auto mounts = scene->get_components<Mount>();
    if (std::find(mounts.begin(), mounts.end(), mount_) != mounts.end() && mount_->owner) return true;
    mount_ = nullptr;
    return false;
}

Mount* Rider::find_mount_in_reach() {
    if (!owner || !scene) return nullptr;
    const glm::vec3 feet = world_pos(owner);
    Mount* best = nullptr;
    float best_d = mount_radius;
    for (Mount* m : scene->get_components<Mount>()) {
        if (!m->can_mount()) continue;
        for (const std::string* path : {&m->mount_point_l, &m->mount_point_r}) {
            coopa::scene::SceneObject* mp = m->socket(*path);
            if (!mp) continue;
            const glm::vec3 p = world_pos(mp);
            if (std::abs(p.z - feet.z) > 1.0f) continue;
            const float d = glm::length(glm::vec2(p.x - feet.x, p.y - feet.y));
            if (d <= best_d) {
                best_d = d;
                best = m;
            }
        }
    }
    return best;
}

bool Rider::mount_onto(Mount* m) {
    if (!bound_) bind_();
    if (!owner || !m || state_ != State::OnFoot || !m->can_mount()) return false;
    auto* tc = owner->get_transform();
    if (!tc) return false;
    mount_ = m;
    m->set_rider(this);
    from_pos_ = tc->transform().position();
    from_rot_ = tc->transform().rotation_quat();
    t_ = 0.0f;
    state_ = State::Mounting;
    hand_over_(true);
    return true;
}

bool Rider::dismount() {
    if (state_ != State::Riding || !mount_valid_()) return false;
    // Wait (in Dismounting, see update()) for the horse to stop before picking the spot.
    state_ = State::Dismounting;
    t_ = -1.0f;
    return true;
}

bool Rider::dismount_spot_(int side, glm::vec3& feet) {
    auto* physics = scene ? dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics")) : nullptr;
    coopa::scene::SceneObject* mp = mount_ ? mount_->socket(side == 0 ? mount_->mount_point_l : mount_->mount_point_r) : nullptr;
    if (!physics || !mp || !controller_) return false;
    coopa::scene::SceneObject* horse = mount_->owner;
    coopa::physx::system::PhysicsSystem::QueryFilter filter;
    filter.include_triggers = false;
    filter.predicate = [self = owner, horse](const coopa::physx::components::Collider& c) {
        return !under(c.owner, self) && !under(c.owner, horse);
    };
    const glm::vec3 p = world_pos(mp);
    coopa::physx::geometry::Ray ray;
    ray.origin = p + glm::vec3(0.0f, 0.0f, 1.5f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    ray.max_distance = 3.5f;
    coopa::physx::system::PhysicsSystem::RaycastHit hit;
    if (!physics->raycast(ray, hit, filter) || hit.started_inside || hit.normal.z < 0.5f) return false;
    feet = hit.point;
    const coopa::physx::character::MotorSettings ms = controller_->motor_settings();
    const glm::vec3 centre = feet + glm::vec3(0.0f, 0.0f, 0.5f * ms.height + ms.skin + 0.02f);
    for (const auto& pen : physics->compute_penetration(coopa::physx::character::motor_capsule(ms), centre,
                                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), filter))
        if (pen.depth > 0.01f) return false;
    return true;
}

void Rider::hand_over_(bool to_mount) {
    if (to_mount) {
        if (controller_) controller_->set_suspended(true);
        if (foot_ik_) foot_ik_->set_suspended(true);
        if (animator_ && animator_->find_state(ride_state)) animator_->crossfade(ride_state, crossfade);
    } else {
        if (controller_) controller_->set_suspended(false);
        if (foot_ik_) foot_ik_->set_suspended(false);
    }
    auto* cam = scene ? scene->find_first_component<CameraController>() : nullptr;
    if (!cam || !owner) return;
    if (to_mount && mount_ && mount_->owner && cam->tracker == owner->name()) {
        cam_tracker_ = cam->tracker;
        cam_offset_ = cam->target_offset;
        cam_distance_ = cam->distance;
        cam_saved_ = true;
        cam->tracker = mount_->owner->name();
        cam->target_offset = glm::vec3(0.0f, 0.0f, mount_->camera_offset_z);
        cam->distance = mount_->camera_distance;
        cam->max_distance = std::max(cam->max_distance, cam->distance);
    } else if (!to_mount && cam_saved_) {
        cam->tracker = cam_tracker_;
        cam->target_offset = cam_offset_;
        cam->distance = cam_distance_;
        cam_saved_ = false;
    }
}

void Rider::release_limbs_() {
    for (Limb* group : {legs_, arms_})
        for (int i = 0; i < 2; ++i)
            if (group[i].ik) {
                group[i].ik->clear_target_override();
                group[i].ik->clear_pole_override();
            }
    if (spine_) spine_->clear_target_override();
}

void Rider::finish_dismount_() {
    release_limbs_();
    if (mount_) mount_->set_rider(nullptr);
    mount_ = nullptr;
    state_ = State::OnFoot;
    if (controller_) controller_->teleport(ground_feet_, ground_yaw_);
    else set_root_(ground_feet_, glm::angleAxis(glm::radians(ground_yaw_), glm::vec3(0.0f, 0.0f, 1.0f)));
    hand_over_(false);
}

void Rider::update(float dt) {
    if (!bound_) bind_();
    const bool pressed = interact;
    interact = false;
    if (state_ != State::OnFoot && !mount_valid_()) {
        // The horse is gone: land where the rider is.
        release_limbs_();
        state_ = State::OnFoot;
        if (owner && owner->get_transform()) {
            ground_feet_ = owner->get_transform()->transform().position();
            ground_yaw_ = controller_ ? controller_->yaw_deg() : 0.0f;
            if (controller_) controller_->teleport(ground_feet_, ground_yaw_);
        }
        hand_over_(false);
        return;
    }
    if (!start_checked_) {
        start_checked_ = true;
        coopa::scene::SceneObject* o = mount_on_start.empty() || !scene ? nullptr : scene->find_object(mount_on_start);
        Mount* m = o ? o->get_component<Mount>() : nullptr;
        if (!mount_on_start.empty() && !m) std::cerr << "[Rider] mount_on_start: no Mount on '" << mount_on_start << "'\n";
        if (m && !m->is_ridden() && state_ == State::OnFoot) {
            // Already seated: no climb, no reach or speed check.
            mount_ = m;
            m->set_rider(this);
            state_ = State::Riding;
            t_ = mount_time;
            hand_over_(true);
            if (animator_ && animator_->find_state(ride_state)) animator_->play(ride_state);
        }
    }
    switch (state_) {
    case State::OnFoot:
        if (pressed) mount_onto(find_mount_in_reach());
        break;
    case State::Mounting:
        t_ += dt;
        if (t_ >= mount_time) state_ = State::Riding;
        break;
    case State::Riding:
        if (pressed) dismount();
        break;
    case State::Dismounting:
        if (t_ < 0.0f) {
            // Still waiting for the horse to stop: then pick a clear side, or stay on.
            CharacterController* hc = mount_->controller();
            const glm::vec3 v = hc ? hc->velocity() : glm::vec3(0.0f);
            if (glm::length(glm::vec2(v.x, v.y)) > mount_->mount_max_speed) break;
            int side = -1;
            for (int s : {0, 1})
                if (dismount_spot_(s, ground_feet_)) { side = s; break; }
            if (side < 0) {
                state_ = State::Riding;
                break;
            }
            ground_yaw_ = hc ? hc->yaw_deg() : 0.0f;
            t_ = 0.0f;
            break;
        }
        t_ += dt;
        if (t_ >= dismount_time) finish_dismount_();
        break;
    }
}

bool Rider::seat_pose_(glm::vec3& pos, glm::quat& rot) {
    coopa::scene::SceneObject* saddle = mount_ ? mount_->socket(mount_->saddle) : nullptr;
    auto* tc = saddle ? saddle->get_transform() : nullptr;
    if (!tc) return false;
    const glm::mat4 m = tc->get_world_matrix();
    rot = glm::normalize(coopa::anim::ik::rotation_of(m));
    pos = glm::vec3(m[3]) + rot * seat_offset;
    return true;
}

void Rider::set_root_(const glm::vec3& pos, const glm::quat& rot) {
    auto* tc = owner ? owner->get_transform() : nullptr;
    if (!tc) return;
    tc->transform().set_position(pos);
    tc->transform().set_rotation_quat(rot);
}

void Rider::ik_pre_solve(float) {
    if (state_ == State::OnFoot || !owner || !mount_valid_()) return;
    glm::vec3 seat;
    glm::quat seat_rot;
    if (!seat_pose_(seat, seat_rot)) return;

    // The root: on the saddle, or on the arc between it and the ground.
    float w = 1.0f;   // how seated the limbs are
    if (state_ == State::Mounting) {
        const float u = smooth01(mount_time > 0.0f ? t_ / mount_time : 1.0f);
        set_root_(glm::mix(from_pos_, seat, u) + glm::vec3(0.0f, 0.0f, 0.35f * std::sin(kPi * u)),
                  glm::slerp(from_rot_, seat_rot, u));
        w = u;
    } else if (state_ == State::Dismounting && t_ >= 0.0f) {
        const float u = smooth01(dismount_time > 0.0f ? t_ / dismount_time : 1.0f);
        const glm::quat ground_rot = glm::angleAxis(glm::radians(ground_yaw_), glm::vec3(0.0f, 0.0f, 1.0f));
        set_root_(glm::mix(seat, ground_feet_, u) + glm::vec3(0.0f, 0.0f, 0.35f * std::sin(kPi * u)),
                  glm::slerp(seat_rot, ground_rot, u));
        w = 1.0f - u;
    } else {
        set_root_(seat, seat_rot);
    }

    // The limbs: feet to the stirrups (knees out round the barrel), hands to the reins, the
    // spine leaning into the speed -- blended from the ride clip's own targets by `w`.
    const glm::vec3 right = seat_rot * glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 fwd = seat_rot * glm::vec3(0.0f, 1.0f, 0.0f);
    const std::string* stirrups[2] = {&mount_->stirrup_l, &mount_->stirrup_r};
    const std::string* reins[2] = {&mount_->rein_l, &mount_->rein_r};
    for (int i = 0; i < 2; ++i) {
        const float side = i == 0 ? -1.0f : 1.0f;
        if (coopa::anim::TwoBoneIK* ik = legs_[i].ik) {
            coopa::scene::SceneObject* st = mount_->socket(*stirrups[i]);
            coopa::scene::SceneObject* clip_t = ik->target_object();
            if (st && clip_t) {
                const float lw = w * std::clamp(leg_weight, 0.0f, 1.0f);
                ik->set_target_position(glm::mix(world_pos(clip_t), world_pos(st), lw));
                coopa::scene::SceneObject* clip_pole = ik->pole.empty() ? nullptr : coopa::anim::resolve_ik_path(owner, scene, ik->pole);
                const glm::vec3 hip = world_pos(ik->upper_bone());
                const glm::vec3 pole = hip + right * (side * knee_spread) + fwd * 0.6f;
                ik->set_pole_position(clip_pole ? glm::mix(world_pos(clip_pole), pole, lw) : pole);
            }
        }
        if (coopa::anim::TwoBoneIK* ik = arms_[i].ik) {
            coopa::scene::SceneObject* rein = mount_->socket(*reins[i]);
            coopa::scene::SceneObject* clip_t = ik->target_object();
            if (rein && clip_t) ik->set_target_position(glm::mix(world_pos(clip_t), world_pos(rein), w * std::clamp(arm_weight, 0.0f, 1.0f)));
        }
    }
    if (spine_ && !spine_->target.empty()) {
        if (coopa::scene::SceneObject* st = coopa::anim::resolve_ik_path(owner, scene, spine_->target)) {
            const float g = mount_->gallop_speed > 0.0f ? std::clamp(mount_->speed() / mount_->gallop_speed, 0.0f, 1.0f) : 0.0f;
            spine_->set_target_position(world_pos(st) + fwd * (lean * g * w));
        }
    }
}

} // namespace scene
} // namespace toy
