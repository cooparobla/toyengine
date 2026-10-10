#include <toyengine/scene/camera_controller.h>

#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <coopa/util/transform.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/character_controller.h>

namespace toy {
namespace scene {

void CameraController::start() {
    if (!owner) return;
    auto* tc = owner->get_transform();
    if (!tc) return;

    if (mode == CameraControlMode::FirstPerson) {
        // Facing the tracked object's heading, looking level -- unless set in YAML.
        if (yaw_deg == kUnset) {
            coopa::scene::SceneObject* tracked = tracked_object_();
            yaw_deg = tracked && tracked->get_transform() ? tracked->get_transform()->transform().rotation_degrees().z
                                                          : tc->transform().rotation_degrees().z;
        }
        if (pitch_deg == kUnset) pitch_deg = 0.0f;
        smoothed_yaw_deg_ = yaw_deg;
        smoothed_pitch_deg_ = pitch_deg;
        return;
    }

    smoothed_target_ = resolve_target_();
    glm::vec3 offset = tc->transform().position() - smoothed_target_;
    float dist = glm::length(offset);

    if (distance < 0.0f) distance = glm::max(dist, min_distance);
    if (dist > 0.0001f) {
        if (yaw_deg == kUnset)   yaw_deg   = glm::degrees(std::atan2(offset.x, -offset.y));
        if (pitch_deg == kUnset) pitch_deg = glm::degrees(std::asin(glm::clamp(offset.z / dist, -1.0f, 1.0f)));
    } else {
        if (yaw_deg == kUnset)   yaw_deg   = 0.0f;
        if (pitch_deg == kUnset) pitch_deg = 0.0f;
    }
    pitch_deg = glm::clamp(pitch_deg, min_pitch_deg, max_pitch_deg);

    // The applied pose starts exactly at the raw seed -- no drag lag on frame one.
    smoothed_yaw_deg_   = yaw_deg;
    smoothed_pitch_deg_ = pitch_deg;
    smoothed_distance_  = distance;
}

void CameraController::update(float dt) {
    if (!owner) return;
    auto* tc = owner->get_transform();
    if (!tc) return;
    auto& t = tc->transform();

    if (mode == CameraControlMode::Orbit) {
        update_orbit_(t, dt);
    } else if (mode == CameraControlMode::FirstPerson) {
        update_first_person_(t, dt);
    } else {
        update_fly_(t, dt);
    }
}

glm::vec3 CameraController::resolve_target_() const {
    if (!tracker.empty() && scene) {
        if (auto* found = scene->find_object(tracker)) {
            if (auto* found_tc = found->get_transform()) {
                return glm::vec3(found_tc->get_world_matrix()[3]) + target_offset;
            }
        } else if (!warned_missing_tracker_) {
            std::cerr << "[toyengine] CameraController: tracker \"" << tracker
                       << "\" not found; falling back to target.\n";
            warned_missing_tracker_ = true;
        }
    }
    return target + target_offset;
}

void CameraController::update_orbit_(coopa::util::Transform& t, float dt) {
    glm::vec3 desired = resolve_target_();
    if (follow_smoothing <= 0.0f) {
        smoothed_target_ = desired;
    } else {
        float alpha = 1.0f - std::exp(-follow_smoothing * dt);
        smoothed_target_ += (desired - smoothed_target_) * alpha;
        // Same deadband reasoning as the movement_smoothing block below: finish the chase
        // once the error is sub-visible instead of ulp-walking forever.
        if (glm::length(desired - smoothed_target_) < 1e-4f) smoothed_target_ = desired;
    }

    yaw_deg += mouse_delta.x * mouse_sensitivity * (invert_x ? -1.0f : 1.0f);
    yaw_deg += auto_rotate_deg_per_sec * dt;
    pitch_deg += mouse_delta.y * mouse_sensitivity * (invert_y ? -1.0f : 1.0f);
    pitch_deg = glm::clamp(pitch_deg, min_pitch_deg, max_pitch_deg);

    if (scroll_input != 0.0f) {
        distance = glm::clamp(distance - scroll_input * zoom_speed, min_distance, max_distance);
    }

    // Drag: yaw_deg/pitch_deg/distance above are the raw targets input just set;
    // smoothed_* is what's actually applied below, chasing those targets at a rate
    // set by movement_smoothing. At 0, retention is 0 and the branch below is
    // skipped entirely, so smoothed_* == raw every frame -- bit-for-bit the
    // original undamped behavior, not merely a fast approximation of it.
    float retention = glm::clamp(movement_smoothing, 0.0f, 1.0f) * kMaxMovementRetention;
    if (retention <= 0.0f) {
        smoothed_yaw_deg_   = yaw_deg;
        smoothed_pitch_deg_ = pitch_deg;
        smoothed_distance_  = distance;
    } else {
        // retention is "fraction of error remaining after one kSmoothingReferenceFps-
        // rate frame"; raising it to (dt * rate) generalizes that to the actual dt,
        // the same trick follow_smoothing's exp(-rate*dt) uses, just parameterized as
        // a 0..1 knob instead of a raw per-second rate.
        float alpha = 1.0f - std::pow(retention, dt * kSmoothingReferenceFps);
        smoothed_yaw_deg_   += (yaw_deg - smoothed_yaw_deg_) * alpha;
        smoothed_pitch_deg_ += (pitch_deg - smoothed_pitch_deg_) * alpha;
        smoothed_distance_  += (distance - smoothed_distance_) * alpha;
        // Deadband: an exponential chase never finishes on its own -- once the remaining
        // error is far below anything visible, float rounding turns the update into a
        // perpetual ulp-scale walk of the pose, and that micro-creep re-rasterizes the
        // scene minutely differently every frame FOREVER after input stops, keeping every
        // temporal filter downstream (AO accumulation, SSGI, edge antialiasing) churning
        // at rest. Snap to the target once the error is sub-visible so rest is rest.
        if (std::abs(yaw_deg   - smoothed_yaw_deg_)   < 1e-3f) smoothed_yaw_deg_   = yaw_deg;
        if (std::abs(pitch_deg - smoothed_pitch_deg_) < 1e-3f) smoothed_pitch_deg_ = pitch_deg;
        if (std::abs(distance  - smoothed_distance_)  < 1e-4f) smoothed_distance_  = distance;
    }

    float e = glm::radians(smoothed_pitch_deg_);
    float phi = glm::radians(smoothed_yaw_deg_);
    glm::vec3 offset(
        smoothed_distance_ * std::cos(e) * std::sin(phi),
        -smoothed_distance_ * std::cos(e) * std::cos(phi),
        smoothed_distance_ * std::sin(e));

    float applied = smoothed_distance_;
    if (collide && !tracker.empty() && smoothed_distance_ > 1e-4f) {
        const float allowed = collision_limit_(smoothed_target_, offset / smoothed_distance_, smoothed_distance_);
        if (collision_distance_ < 0.0f) {
            collision_distance_ = allowed; // first frame: no easing in from a clipped view
        } else {
            const float rate = allowed < collision_distance_ ? collision_in_speed : collision_out_speed;
            if (rate <= 0.0f) {
                collision_distance_ = allowed;
            } else {
                collision_distance_ += (allowed - collision_distance_) * (1.0f - std::exp(-rate * dt));
                if (std::abs(allowed - collision_distance_) < 1e-4f) collision_distance_ = allowed;
            }
        }
        applied = collision_distance_;
    } else {
        collision_distance_ = -1.0f;
    }
    if (smoothed_distance_ > 1e-4f) offset *= applied / smoothed_distance_;

    t.set_position(smoothed_target_ + offset);
    t.set_rotation(glm::vec3(90.0f - smoothed_pitch_deg_, 0.0f, smoothed_yaw_deg_));
}

float CameraController::collision_limit_(const glm::vec3& target, const glm::vec3& dir, float distance) const {
    if (!scene) return distance;
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene->find_system("Physics"));
    if (!physics) return distance;
    coopa::scene::SceneObject* tracked = tracked_object_();
    coopa::physx::system::PhysicsSystem::QueryFilter filter;
    filter.include_triggers = false;
    if (tracked) {
        filter.ignore_rigidbody = tracked->get_component<coopa::physx::components::RigidbodyComponent>();
        filter.predicate = [tracked](const coopa::physx::components::Collider& c) {
            for (const coopa::scene::SceneObject* o = c.owner; o; o = o->parent())
                if (o == tracked) return false;
            return true;
        };
    }
    coopa::physx::system::PhysicsSystem::RaycastHit hit;
    if (!physics->sphere_cast(target, collision_radius, dir, distance, hit, filter)) return distance;
    return std::clamp(hit.distance, 0.0f, distance);
}

void CameraController::update_first_person_(coopa::util::Transform& t, float dt) {
    (void)dt;
    yaw_deg += mouse_delta.x * mouse_sensitivity * (invert_x ? -1.0f : 1.0f);
    pitch_deg += mouse_delta.y * mouse_sensitivity * (invert_y ? -1.0f : 1.0f);
    const float limit = std::clamp(first_person_pitch_limit, 0.0f, 89.0f);
    pitch_deg = glm::clamp(pitch_deg, -limit, limit);
    // No movement_smoothing here: first-person look lag reads as input latency.
    smoothed_yaw_deg_ = yaw_deg;
    smoothed_pitch_deg_ = pitch_deg;

    glm::vec3 eye = target;
    if (coopa::scene::SceneObject* tracked = tracked_object_()) {
        if (auto* ttc = tracked->get_transform()) {
            eye = glm::vec3(ttc->get_world_matrix()[3]);
            if (auto* character = tracked->get_component<CharacterController>()) {
                character->set_facing_yaw(yaw_deg);
            } else {
                glm::vec3 r = ttc->transform().rotation_degrees();
                r.z = yaw_deg;
                ttc->transform().set_rotation(r);
            }
        }
    }
    t.set_position(eye + glm::vec3(0.0f, 0.0f, eye_height) + target_offset);
    t.set_rotation(glm::vec3(90.0f - pitch_deg, 0.0f, yaw_deg));
}

void CameraController::update_fly_(coopa::util::Transform& t, float dt) {
    glm::mat4 world = t.get_world_matrix();
    glm::vec3 right   = glm::normalize(glm::vec3(world[0]));
    glm::vec3 forward = -glm::normalize(glm::vec3(world[2])); // Blender: camera looks down local -Z

    glm::vec3 pos = t.position();
    pos += right * move_input.x * move_speed * dt;
    pos += forward * move_input.y * move_speed * dt;
    pos += glm::vec3(0.0f, 0.0f, 1.0f) * move_input.z * move_speed * dt; // world-up, avoids roll
    t.set_position(pos);

    if (look_input.x != 0.0f || look_input.y != 0.0f) {
        glm::vec3 rot = t.rotation_degrees();
        rot.z += look_input.x * look_speed_deg_per_sec * dt;
        rot.x = glm::clamp(rot.x + look_input.y * look_speed_deg_per_sec * dt, 1.0f, 179.0f);
        t.set_rotation(rot);
    }
}

} // namespace scene
} // namespace toy
