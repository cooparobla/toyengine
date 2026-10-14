#include <toyengine/scene/character_controller.h>

#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/util/transform.h>
#include <glm/gtx/quaternion.hpp>
#include <physxcoopa/components/capsule_collider.h>
#include <physxcoopa/components/rigidbody.h>

namespace toy {
namespace scene {

CharacterController* find_player_controller(coopa::scene::Scene& scene) {
    for (CharacterController* cc : scene.get_components<CharacterController>())
        if (cc->player_input) return cc;
    return nullptr;
}

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
    step_offset_ = 0.0f;
    ground_body_ = coopa::physx::dynamics::BodyId{};
}

void CharacterController::set_vertical_drive(float target_speed, float response) {
    vertical_drive_ = true;
    vertical_target_ = target_speed;
    vertical_response_ = response;
}

void CharacterController::clear_vertical_drive() {
    vertical_drive_ = false;
    vertical_target_ = 0.0f;
    vertical_response_ = 0.0f;
}

void CharacterController::teleport(const glm::vec3& p) {
    if (auto* tc = owner ? owner->get_transform() : nullptr) tc->transform().set_position(p);
    h_velocity_ = glm::vec2(0.0f);
    v_velocity_ = 0.0f;
    grounded_ = false;
    step_offset_ = 0.0f;
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
    if (world.gravity_source_count() > 0) {
        advance_field_(dt, physics, jump_pressed);
        return;
    }
    field_ = false;
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
    // `g > 0`: an effective gravity of zero has no apex to reach, and a negative one -- a
    // gravity_scale turned upward -- would make this sqrt() of a negative number and strand the
    // character at NaN for the rest of the scene. Neither has a jump worth firing.
    if (jump_buffer_timer_ > 0.0f && can_jump && jump_height > 0.0f && g > 0.0f) {
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
    float v_before = v_velocity_;
    if (vertical_drive_) {
        // Gravity replaced by the held speed (set_vertical_drive). The ground clamp below is
        // skipped on purpose: it exists to stop gravity fighting the ground snap, and applying
        // it here would forbid the one thing a drive is for -- lifting a character that is
        // standing on something.
        const float blend = vertical_response_ > 0.0f ? std::min(1.0f, vertical_response_ * dt) : 1.0f;
        v_velocity_ += (vertical_target_ - v_velocity_) * blend;
        disp.z += v_velocity_ * dt;
        v_before = v_velocity_;
    } else {
        if (grounded_ && !jumped_now) v_velocity_ = std::min(v_velocity_, 0.0f);
        disp.z += v_velocity_ * dt - 0.5f * g * dt * dt;
        v_before = v_velocity_ - g * dt;
        v_velocity_ = v_before;
    }
    // Ride what we stand on: its Transform has already moved this frame (movers run first,
    // see KinematicControlSystem), so carry the feet by exactly that delta -- position and
    // yaw -- rather than by its physics velocity, which is a step old.
    // The Transform may be carrying a step-smoothing lag (see step_smoothing); the motor works
    // from where the character really is, so it is added back before anything reads the pose.
    const glm::vec3 feet0 = t.position() + up * step_offset_;
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
    const float air_before = air_time_;   ///< Time aloft entering this frame (step smoothing).
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

    // A step up is instantaneous in the motor -- it sweeps over the ledge and lands on top of it
    // within one frame -- which reads as the character snapping upward. Holding the drawn
    // position below the real one and easing it up turns that into a stride without the motor
    // ever knowing: collision, ground tests and the next frame's move all run on the real pose.
    if (step_smoothing > 0.0f) {
        // Both directions. Going up, the motor sweeps over the ledge; coming down, the ground
        // snap pulls the character onto the lower surface -- each lands a whole step inside one
        // frame, and each is the same jolt. Only while the character stayed grounded across the
        // move, though: a landing after a fall has to arrive at once, not drift down afterwards.
        constexpr float k_min_drop = 0.25f;  // Clear of the per-frame descent of a run down a slope.
        // Walking off a ledge is briefly airborne -- the character leaves the lip a frame or two
        // before the ground snap catches it below -- so "stayed grounded" alone would exclude the
        // very case this is for. A short grace covers that while still letting a real fall land
        // hard: by the time anything has been aloft this long, it is falling, not stepping down.
        constexpr float k_step_grace = 0.2f;
        const float limit = std::max(step_height, snap_distance);
        if (grounded_ && (was_grounded || air_before < k_step_grace)) {
            const float rise = r.position.z - centre.z;
            if (r.stepped || rise < -k_min_drop) {
                step_offset_ = std::clamp(step_offset_ + rise, -limit, limit);
            }
        }
        const float decay = (limit / step_smoothing) * dt;
        step_offset_ = step_offset_ > 0.0f ? std::max(0.0f, step_offset_ - decay)
                                           : std::min(0.0f, step_offset_ + decay);
    } else {
        step_offset_ = 0.0f;
    }
    t.set_position(r.position - up * (0.5f * height + step_offset_));
    t.set_rotation(glm::vec3(0.0f, 0.0f, yaw_deg_));
}

void CharacterController::advance_field_(float dt, coopa::physx::system::PhysicsSystem* physics, bool jump_pressed) {
    namespace ch = coopa::physx::character;
    coopa::util::Transform& t = owner->get_transform()->transform();
    coopa::physx::PhysicsWorld& world = physics->world();
    const glm::vec3 feet0 = t.position();

    // --- The frame: up from the field at the feet; yaw-0 forward carried over from last frame ---
    const glm::vec3 pull = world.gravity_at(feet0);
    const float g = glm::length(pull) * gravity_scale;
    const glm::vec3 up = world.up_at(feet0);
    if (!field_) {
        // Entering the field: start from the transform's own orientation.
        field_y_ = glm::vec3(t.get_world_matrix()[1]);
        field_up_ = up;
    }
    glm::vec3 fy = field_y_ - up * glm::dot(field_y_, up);
    if (glm::length(fy) < 1e-4f) {
        fy = field_up_ - up * glm::dot(field_up_, up);   // walked over the old forward: use the old up
        if (glm::length(fy) < 1e-4f) fy = std::abs(up.x) < 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        fy -= up * glm::dot(fy, up);
    }
    fy = glm::normalize(fy);
    const glm::vec3 fx = glm::cross(fy, up);
    field_ = true;
    field_up_ = up;
    field_y_ = fy;
    const auto to_world = [&](const glm::vec3& v) { return fx * v.x + fy * v.y + up * v.z; };
    const auto to_frame = [&](const glm::vec3& v) { return glm::vec3(glm::dot(v, fx), glm::dot(v, fy), glm::dot(v, up)); };

    // --- Horizontal velocity toward the input target (in the frame) ---
    glm::vec2 input = move_input;
    const float in_len = glm::length(input);
    if (in_len > 1.0f) input /= in_len;
    float by = glm::radians(move_basis_yaw_deg);
    if (glm::length(move_basis_forward) > 1e-4f) {
        glm::vec3 f = to_frame(move_basis_forward);
        if (glm::length(glm::vec2(f)) > 1e-4f) by = std::atan2(-f.x, f.y);
    }
    const glm::vec2 right(std::cos(by), std::sin(by));
    const glm::vec2 forward(-std::sin(by), std::cos(by));
    const glm::vec2 wish = (right * input.x + forward * input.y) * move_speed * (sprint ? sprint_multiplier : 1.0f);
    const float accel = acceleration * (grounded_ ? 1.0f : std::clamp(air_control, 0.0f, 1.0f));
    const glm::vec2 dv = wish - h_velocity_;
    const float dv_len = glm::length(dv);
    const float step = accel * dt;
    h_velocity_ = (dv_len <= step || dv_len < 1e-6f) ? wish : h_velocity_ + dv * (step / dv_len);

    // --- Jump ---
    jump_buffer_timer_ = jump_pressed ? jump_buffer + 1e-6f : std::max(0.0f, jump_buffer_timer_ - dt);
    const bool can_jump = !jumped_ && (grounded_ || air_time_ <= coyote_time);
    bool jumped_now = false;
    // `g > 0`: an effective gravity of zero has no apex to reach, and a negative one -- a
    // gravity_scale turned upward -- would make this sqrt() of a negative number and strand the
    // character at NaN for the rest of the scene. Neither has a jump worth firing.
    if (jump_buffer_timer_ > 0.0f && can_jump && jump_height > 0.0f && g > 0.0f) {
        v_velocity_ = std::sqrt(2.0f * g * jump_height);
        jumped_ = true;
        jumped_now = true;
        jump_buffer_timer_ = 0.0f;
    }

    // --- Displacement (frame) ---
    glm::vec3 disp(0.0f);
    if (use_root_motion && grounded_ && !jumped_now) {
        const float fyaw = glm::radians(yaw_deg_);
        const glm::vec2 ax(std::cos(fyaw), std::sin(fyaw)), af(-std::sin(fyaw), std::cos(fyaw));
        const glm::vec2 rm = ax * root_motion_.x + af * root_motion_.y;
        disp += glm::vec3(rm, 0.0f);
        yaw_deg_ += root_yaw_;
        h_velocity_ = rm / dt;
    } else {
        disp += glm::vec3(h_velocity_ * dt, 0.0f);
    }
    root_motion_ = glm::vec3(0.0f);
    root_yaw_ = 0.0f;
    float v_before = v_velocity_;
    if (vertical_drive_) {
        // Gravity replaced by the held speed (set_vertical_drive). The ground clamp below is
        // skipped on purpose: it exists to stop gravity fighting the ground snap, and applying
        // it here would forbid the one thing a drive is for -- lifting a character that is
        // standing on something.
        const float blend = vertical_response_ > 0.0f ? std::min(1.0f, vertical_response_ * dt) : 1.0f;
        v_velocity_ += (vertical_target_ - v_velocity_) * blend;
        disp.z += v_velocity_ * dt;
        v_before = v_velocity_;
    } else {
        if (grounded_ && !jumped_now) v_velocity_ = std::min(v_velocity_, 0.0f);
        disp.z += v_velocity_ * dt - 0.5f * g * dt * dt;
        v_before = v_velocity_ - g * dt;
        v_velocity_ = v_before;
    }

    // --- Move ---
    coopa::physx::query::QueryFilter filter;
    filter.include_triggers = false;
    if (rigidbody_) filter.ignore = rigidbody_->body_id();
    filter.predicate = [physics, self = owner](coopa::physx::dynamics::BodyId id) {
        const coopa::physx::components::Collider* c = physics->collider_for(id);
        if (!c) return true;
        for (const coopa::scene::SceneObject* o = c->owner ? c->owner->parent() : nullptr; o; o = o->parent())
            if (o == self) return false;
        return true;
    };
    const glm::vec3 centre = feet0 + up * (0.5f * height);
    ch::MoveOptions opt;
    opt.was_grounded = grounded_ && !jumped_now;
    opt.jumping = jumped_now || v_velocity_ > 0.0f;
    opt.up = up;
    last_move_ = ch::move(world, centre, to_world(disp), filter, motor_settings(), opt);
    const ch::MoveResult& r = last_move_;

    // --- Respond to what was hit (normals taken into the frame: the flat rules then apply) ---
    for (const ch::MotorHit& hit : r.collisions) {
        const glm::vec3 n = to_frame(hit.normal);
        if (!hit.walkable) {
            glm::vec3 nh(n.x, n.y, 0.0f);
            if (glm::length(nh) > 1e-4f) {
                nh = glm::normalize(nh);
                const glm::vec2 n2(nh.x, nh.y);
                const float into = glm::dot(h_velocity_, n2);
                if (push_dynamic_bodies && into < 0.0f) {
                    ch::MotorHit flat = hit;
                    flat.normal = to_world(nh);   // push_ pushes along the normal's level part
                    push_(world, flat, -into);
                }
                if (into < 0.0f) h_velocity_ -= n2 * into;
            }
            if (n.z < -0.3f && v_velocity_ > 0.0f) v_velocity_ = 0.0f;
            if (n.z > 0.02f && v_velocity_ < 0.0f) {
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
    const float air_before = air_time_;   ///< Time aloft entering this frame (step smoothing).
    grounded_ = r.grounded;
    ground_normal_ = r.grounded ? r.ground_normal : up;
    ground_body_ = coopa::physx::dynamics::BodyId{};
    if (grounded_) {
        if (!was_grounded && on_landed.slot_count() > 0) on_landed.emit(std::max(0.0f, -v_before));
        if (v_velocity_ < 0.0f) v_velocity_ = 0.0f;
        jumped_ = false;
        air_time_ = 0.0f;
    } else {
        air_time_ += dt;
    }
    if (jumped_now && on_jumped.slot_count() > 0) on_jumped.emit();

    // --- Facing (yaw in the frame) ---
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

    // The frame moves with the feet: carry it to where they ended up, so next frame's velocity
    // (held in the frame) points the same way along the ground.
    const glm::vec3 feet1 = r.position - up * (0.5f * height);
    const glm::vec3 up1 = world.up_at(feet1);
    const glm::quat carry = glm::rotation(up, up1);
    field_up_ = up1;
    field_y_ = glm::normalize(carry * fy);
    const glm::vec3 fx1 = glm::cross(field_y_, up1);
    const glm::quat frame = glm::quat_cast(glm::mat3(fx1, field_y_, up1));
    t.set_position(r.position - up1 * (0.5f * height));
    t.set_rotation_quat(frame * glm::angleAxis(glm::radians(yaw_deg_), glm::vec3(0.0f, 0.0f, 1.0f)));
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
