#include <toyengine/scene/character_controller.h>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/util/transform.h>
#include <physxcoopa/components/capsule_collider.h>
#include <physxcoopa/components/rigidbody.h>

namespace toy {
namespace scene {

void CharacterController::set_suspended(bool suspended) {
    if (suspended == suspended_) return;
    suspended_ = suspended;
    ensure_body_();
    if (auto* cap = owner ? owner->get_component<coopa::physx::components::CapsuleCollider>() : nullptr) {
        cap->set_enabled(!suspended);
    }
    h_velocity_ = glm::vec2(0.0f);
    v_velocity_ = 0.0f;
    grounded_ = false;
    ground_body_ = coopa::physx::dynamics::BodyId{};
}

void CharacterController::teleport(const glm::vec3& p) {
    if (auto* tc = owner ? owner->get_transform() : nullptr) tc->transform().set_position(p);
    h_velocity_ = glm::vec2(0.0f);
    v_velocity_ = 0.0f;
    grounded_ = false;
    ground_body_ = coopa::physx::dynamics::BodyId{};
}

void CharacterController::teleport(const glm::vec3& p, float yaw_deg) {
    teleport(p);
    yaw_deg_ = yaw_deg;
    if (auto* tc = owner ? owner->get_transform() : nullptr) tc->transform().set_rotation(glm::vec3(0.0f, 0.0f, yaw_deg_));
}

void CharacterController::save(toy::save::SaveNode& out) {
    if (auto* tc = owner ? owner->get_transform() : nullptr) out.set("position", tc->transform().position());
    out.set("yaw", yaw_deg_);
}

void CharacterController::load(const toy::save::SaveNode& in) {
    auto* tc = owner ? owner->get_transform() : nullptr;
    if (tc && in.has("position")) teleport(in.get("position", tc->transform().position()));
    yaw_deg_ = in.get("yaw", yaw_deg_);
    if (tc) tc->transform().set_rotation(glm::vec3(0.0f, 0.0f, yaw_deg_));
}

void CharacterController::set_facing_yaw(float yaw_deg) {
    yaw_deg_ = yaw_deg;
    facing_override_ = true;
}

void CharacterController::add_root_motion(const glm::vec3& local_delta, float yaw_delta_deg) {
    root_motion_ += local_delta;
    root_yaw_ += yaw_delta_deg;
}

bool CharacterController::consume_root_motion(const glm::vec3& local_delta, float yaw_delta_deg) {
    if (!use_root_motion) return false;
    add_root_motion(local_delta, yaw_delta_deg);
    return true;
}

coopa::physx::character::MotorSettings CharacterController::motor_settings() const {
    coopa::physx::character::MotorSettings s;
    s.radius = radius;
    s.height = height;
    s.step_height = step_height;
    s.slope_limit_deg = slope_limit;
    s.skin = skin;
    s.snap_distance = snap_distance;
    return s;
}

void CharacterController::start() {
    if (!owner) return;
    ensure_body_();
    if (auto* tc = owner->get_transform()) yaw_deg_ = tc->transform().rotation_degrees().z;
}

void CharacterController::advance(float dt, coopa::physx::system::PhysicsSystem* physics) {
    namespace ch = coopa::physx::character;
    const bool jump_pressed = jump;
    jump = false;
    if (!owner || dt <= 0.0f || !physics || suspended_) return;
    auto* tc = owner->get_transform();
    if (!tc) return;
    ensure_body_();
    coopa::util::Transform& t = tc->transform();
    coopa::physx::PhysicsWorld& world = physics->world();
    const glm::vec3 up = ch::k_up;
    const float g = std::abs(world.gravity().z) * gravity_scale;

    // --- Horizontal velocity toward the input target ---
    glm::vec2 input = move_input;
    const float in_len = glm::length(input);
    if (in_len > 1.0f) input /= in_len;
    const float by = glm::radians(move_basis_yaw_deg);
    const glm::vec2 right(std::cos(by), std::sin(by));
    const glm::vec2 forward(-std::sin(by), std::cos(by));
    const glm::vec2 wish = (right * input.x + forward * input.y) * move_speed * (sprint ? sprint_multiplier : 1.0f);
    const float accel = acceleration * (grounded_ ? 1.0f : std::clamp(air_control, 0.0f, 1.0f));
    const glm::vec2 dv = wish - h_velocity_;
    const float dv_len = glm::length(dv);
    const float step = accel * dt;
    h_velocity_ = (dv_len <= step || dv_len < 1e-6f) ? wish : h_velocity_ + dv * (step / dv_len);

    // --- Jump: buffered press, coyote window ---
    jump_buffer_timer_ = jump_pressed ? jump_buffer + 1e-6f : std::max(0.0f, jump_buffer_timer_ - dt);
    const bool can_jump = !jumped_ && (grounded_ || air_time_ <= coyote_time);
    bool jumped_now = false;
    if (jump_buffer_timer_ > 0.0f && can_jump && jump_height > 0.0f) {
        v_velocity_ = std::sqrt(2.0f * g * jump_height);
        jumped_ = true;
        jumped_now = true;
        jump_buffer_timer_ = 0.0f;
    }

    // --- Displacement: own motion (or root motion), gravity, platform carry ---
    glm::vec3 disp(0.0f);
    // Root motion drives grounded locomotion; airborne (a jump's takeoff frame included), the
    // physical velocity carries on -- see use_root_motion's doc.
    if (use_root_motion && grounded_ && !jumped_now) {
        const float fy = glm::radians(yaw_deg_);
        const glm::vec2 fx(std::cos(fy), std::sin(fy)), ff(-std::sin(fy), std::cos(fy));
        const glm::vec2 rm = fx * root_motion_.x + ff * root_motion_.y;
        disp += glm::vec3(rm, 0.0f);
        yaw_deg_ += root_yaw_;
        h_velocity_ = rm / dt;
    } else {
        disp += glm::vec3(h_velocity_ * dt, 0.0f);
    }
    root_motion_ = glm::vec3(0.0f);
    root_yaw_ = 0.0f;
    // Exact for constant acceleration (velocity Verlet), so the apex is exactly jump_height.
    if (grounded_ && !jumped_now) v_velocity_ = std::min(v_velocity_, 0.0f);
    disp.z += v_velocity_ * dt - 0.5f * g * dt * dt;
    const float v_before = v_velocity_ - g * dt;
    v_velocity_ = v_before;
    // Ride what we stand on: its Transform has already moved this frame (movers run first,
    // see KinematicControlSystem), so carry the feet by exactly that delta -- position and
    // yaw -- rather than by its physics velocity, which is a step old.
    const glm::vec3 feet0 = t.position();
    if (grounded_ && !jumped_now) {
        glm::mat4 now;
        if (ground_matrix_(physics, ground_body_, now)) {
            const glm::mat4 delta = now * glm::inverse(ground_world_);
            disp += glm::vec3(delta * glm::vec4(feet0, 1.0f)) - feet0;
            yaw_deg_ += glm::degrees(std::atan2(delta[0][1], delta[0][0]));
        }
    }

    // --- Move ---
    coopa::physx::query::QueryFilter filter;
    filter.include_triggers = false;
    if (rigidbody_) filter.ignore = rigidbody_->body_id();
    // Nor anything parented under the character (a Ragdoll's bone bodies follow the
    // animation inside the capsule).
    filter.predicate = [physics, self = owner](coopa::physx::dynamics::BodyId id) {
        const coopa::physx::components::Collider* c = physics->collider_for(id);
        if (!c) return true;
        for (const coopa::scene::SceneObject* o = c->owner ? c->owner->parent() : nullptr; o; o = o->parent())
            if (o == self) return false;
        return true;
    };
    const glm::vec3 feet = feet0;
    const glm::vec3 centre = feet + up * (0.5f * height);
    ch::MoveOptions opt;
    opt.was_grounded = grounded_ && !jumped_now;
    opt.jumping = jumped_now || v_velocity_ > 0.0f;
    last_move_ = ch::move(world, centre, disp, filter, motor_settings(), opt);
    const ch::MoveResult& r = last_move_;

    // --- Respond to what was hit ---
    for (const ch::MotorHit& hit : r.collisions) {
        const glm::vec3 n = hit.normal;
        if (!hit.walkable) {
            glm::vec3 nh(n.x, n.y, 0.0f);
            if (glm::length(nh) > 1e-4f) {
                nh = glm::normalize(nh);
                const glm::vec2 n2(nh.x, nh.y);
                const float into = glm::dot(h_velocity_, n2);
                if (push_dynamic_bodies && into < 0.0f) push_(world, hit, -into);
                if (into < 0.0f) h_velocity_ -= n2 * into;
            }
            if (n.z < -0.3f && v_velocity_ > 0.0f) v_velocity_ = 0.0f;          // ceiling
            if (n.z > 0.02f && v_velocity_ < 0.0f) {                             // steep slope: slide
                glm::vec3 v(h_velocity_, v_velocity_);
                v -= n * std::min(0.0f, glm::dot(v, n));
                h_velocity_ = glm::vec2(v.x, v.y);
                v_velocity_ = v.z;
            }
        }
        if (on_collision.slot_count() > 0) {
            CharacterHit ch_hit;
            ch_hit.point = hit.point;
            ch_hit.normal = hit.normal;
            ch_hit.move_direction = hit.direction;
            ch_hit.collider = physics->collider_for(hit.body);
            ch_hit.object = ch_hit.collider ? ch_hit.collider->owner : nullptr;
            on_collision.emit(ch_hit);
        }
    }
    if (r.hit_ceiling && v_velocity_ > 0.0f) v_velocity_ = 0.0f;

    const bool was_grounded = grounded_;
    grounded_ = r.grounded;
    ground_normal_ = r.grounded ? r.ground_normal : up;
    ground_body_ = coopa::physx::dynamics::BodyId{};
    if (grounded_) {
        if (!was_grounded && on_landed.slot_count() > 0) on_landed.emit(std::max(0.0f, -v_before));
        if (v_velocity_ < 0.0f) v_velocity_ = 0.0f;
        jumped_ = false;
        air_time_ = 0.0f;
        if (ground_matrix_(physics, r.ground_body, ground_world_)) ground_body_ = r.ground_body;
    } else {
        air_time_ += dt;
    }
    if (jumped_now && on_jumped.slot_count() > 0) on_jumped.emit();

    // --- Facing ---
    // With root motion the clip supplies the speed and the input still supplies the heading.
    if (face_movement && !facing_override_) {
        const glm::vec2 face = in_len > 1e-3f ? wish : glm::vec2(0.0f);
        if (glm::length(face) > 1e-3f) {
            const float target = glm::degrees(std::atan2(-face.x, face.y));
            float diff = std::remainder(target - yaw_deg_, 360.0f);
            const float max_turn = turn_speed * dt;
            if (turn_speed > 0.0f) diff = std::clamp(diff, -max_turn, max_turn);
            yaw_deg_ += diff;
        }
    }
    facing_override_ = false;
    yaw_deg_ = std::remainder(yaw_deg_, 360.0f);

    t.set_position(r.position - up * (0.5f * height));
    t.set_rotation(glm::vec3(0.0f, 0.0f, yaw_deg_));
}

void CharacterController::push_(coopa::physx::PhysicsWorld& world, const coopa::physx::character::MotorHit& hit, float speed) {
    coopa::physx::dynamics::Body* b = world.get_body(hit.body);
    if (!b || b->type != coopa::physx::dynamics::BodyType::Dynamic || push_strength <= 0.0f) return;
    glm::vec3 dir(-hit.normal.x, -hit.normal.y, 0.0f);
    if (glm::length(dir) < 1e-4f) return;
    dir = glm::normalize(dir);
    const float target = speed * push_strength / (push_strength + std::max(b->mass, 1e-3f));
    const float current = glm::dot(b->linear_velocity, dir);
    if (current >= target) return;
    b->linear_velocity += dir * (target - current);
    b->wake();
}

bool CharacterController::ground_matrix_(coopa::physx::system::PhysicsSystem* physics, coopa::physx::dynamics::BodyId body,
                           glm::mat4& out) {
    if (!body.is_valid()) return false;
    coopa::physx::components::Collider* col = physics->collider_for(body);
    if (!col || !col->owner) return false;
    auto* tc = col->owner->get_transform();
    if (!tc) return false;
    out = tc->get_world_matrix();
    return true;
}

void CharacterController::ensure_body_() {
    if (rigidbody_ || !owner) return;
    using coopa::physx::components::CapsuleCollider;
    using coopa::physx::components::RigidbodyComponent;
    rigidbody_ = owner->get_component<RigidbodyComponent>();
    if (!rigidbody_) {
        rigidbody_ = owner->add_component<RigidbodyComponent>();
        rigidbody_->scene = scene;
    }
    rigidbody_->is_kinematic = true;
    rigidbody_->use_gravity = false;
    if (!owner->get_component<CapsuleCollider>()) {
        auto* cap = owner->add_component<CapsuleCollider>();
        cap->scene = scene;
        cap->set_radius(radius);
        cap->set_height(height);
        cap->set_direction(2);
        cap->set_center(glm::vec3(0.0f, 0.0f, 0.5f * height));
    }
}

} // namespace scene
} // namespace toy
