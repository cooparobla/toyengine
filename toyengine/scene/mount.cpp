#include <toyengine/scene/mount.h>

#include <algorithm>
#include <cmath>

#include <coopa/animation/ik_components.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <toyengine/scene/rider.h>

namespace toy {
namespace scene {

namespace {

/** @brief `deg` wrapped into (-180, 180]. */
float wrap_deg(float deg) {
    deg = std::fmod(deg + 180.0f, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg - 180.0f;
}

coopa::anim::Animator* find_animator(coopa::scene::SceneObject* obj) {
    if (auto* a = obj->get_component<coopa::anim::Animator>()) return a;
    for (const auto& child : obj->children())
        if (auto* a = find_animator(child.get())) return a;
    return nullptr;
}

} // namespace

CharacterController* Mount::controller() {
    if (!controller_ && owner) controller_ = owner->get_component<CharacterController>();
    return controller_;
}

coopa::anim::Animator* Mount::animator_() {
    if (!animator_cached_ && owner) animator_cached_ = find_animator(owner);
    return animator_cached_;
}

coopa::scene::SceneObject* Mount::socket(const std::string& path) {
    if (!owner || path.empty()) return nullptr;
    return coopa::anim::resolve_ik_path(owner, scene, path);
}

bool Mount::can_mount() {
    CharacterController* cc = controller();
    if (rider_ || !cc || cc->is_suspended()) return false;
    const glm::vec3 v = cc->velocity();
    return cc->is_grounded() && glm::length(glm::vec2(v.x, v.y)) <= mount_max_speed;
}

void Mount::steer(float dt) {
    const bool jump_pressed = jump;
    jump = false;
    CharacterController* cc = controller();
    if (!cc || dt <= 0.0f) return;
    // A rider that has gone away (destroyed with its object) no longer rides.
    if (rider_ && scene) {
        const auto riders = scene->get_components<Rider>();
        if (std::find(riders.begin(), riders.end(), rider_) == riders.end()) rider_ = nullptr;
    }
    const bool riding = rider_ && rider_->state() == Rider::State::Riding;

    float yaw = cc->yaw_deg();
    float target = 0.0f;
    yaw_rate_ = 0.0f;
    glm::vec2 input = riding ? move_input : glm::vec2(0.0f);
    const float in_len = glm::length(input);
    if (in_len > 1.0f) input /= in_len;
    if (in_len > 0.1f) {
        const float by = glm::radians(move_basis_yaw_deg);
        const glm::vec2 right(std::cos(by), std::sin(by));
        const glm::vec2 fwd(-std::sin(by), std::cos(by));
        const glm::vec2 wish = right * input.x + fwd * input.y;
        const float want_yaw = glm::degrees(std::atan2(-wish.x, wish.y));
        const float diff = wrap_deg(want_yaw - yaw);
        const float gait_speed = sprint ? gallop_speed : (walk ? walk_speed : trot_speed);
        target = gait_speed * std::min(1.0f, in_len);
        if (std::abs(diff) > 100.0f) target = std::min(target, walk_speed);   // turning around
        const float f = gallop_speed > 0.0f ? std::clamp(speed_ / gallop_speed, 0.0f, 1.0f) : 0.0f;
        const float rate = turn_rate + (gallop_turn_rate - turn_rate) * f;
        const float turn = std::clamp(diff, -rate * dt, rate * dt);
        yaw = wrap_deg(yaw + turn);
        yaw_rate_ = turn / dt;
    }
    // Ease toward the gait's speed; never far ahead of what the body really does (a wall).
    const float rate = target > speed_ ? acceleration : braking;
    speed_ = target > speed_ ? std::min(target, speed_ + rate * dt) : std::max(target, speed_ - rate * dt);
    if (cc->is_grounded()) {
        const glm::vec3 v = cc->velocity();
        const float yr = glm::radians(yaw);
        const float along = glm::dot(glm::vec2(v.x, v.y), glm::vec2(-std::sin(yr), std::cos(yr)));
        speed_ = std::min(speed_, std::max(0.0f, along) + 1.5f);
    }

    cc->set_facing_yaw(yaw);
    cc->move_basis_yaw_deg = yaw;
    cc->move_input = speed_ > 1e-3f ? glm::vec2(0.0f, 1.0f) : glm::vec2(0.0f);
    cc->move_speed = std::max(speed_, 1e-3f);
    cc->sprint = false;
    cc->jump = riding && jump_pressed;
}

void Mount::update(float) {
    CharacterController* cc = controller();
    coopa::anim::Animator* anim = animator_();
    if (!cc || !anim) return;
    const glm::vec3 v = cc->velocity();
    const float speed = glm::length(glm::vec2(v.x, v.y));

    std::string want = current_;
    float playback = 1.0f;
    if (cc->is_grounded() || current_.empty()) {
        if (speed < 0.15f) {
            want = std::abs(yaw_rate_) > 20.0f ? walk_state : idle_state;   // turning in place steps
        } else if (speed < 0.5f * (walk_speed + trot_speed)) {
            want = walk_state;
            playback = speed / walk_speed;
        } else if (speed < 0.5f * (trot_speed + gallop_speed)) {
            want = trot_state;
            playback = speed / trot_speed;
        } else {
            want = gallop_state;
            playback = speed / gallop_speed;
        }
        if (want == walk_state && speed < 0.15f) playback = 1.0f;
        else if (want != idle_state) playback = std::clamp(playback, min_anim_speed, max_anim_speed);
    } else {
        playback = anim->speed();   // airborne: hold the gait as it was
    }
    anim->set_speed(playback);
    if (want != current_ && anim->find_state(want)) {
        if (current_.empty()) anim->play(want);
        else anim->crossfade(want, crossfade);
        current_ = want;
    }
}

} // namespace scene
} // namespace toy
