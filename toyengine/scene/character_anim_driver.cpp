#include <toyengine/scene/character_anim_driver.h>

#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

namespace toy {
namespace scene {

void CharacterAnimDriver::update(float dt) {
    if (!owner) return;
    if (!controller_) controller_ = owner->get_component<CharacterController>();
    if (!animator_) animator_ = find_animator_(owner);
    if (!controller_ || !animator_) return;
    // A suspended controller (a Ragdoll has the body) is not walking: drive nothing, and
    // afterwards pick up from whatever state the Animator was handed back in.
    if (controller_->is_suspended()) {
        resync_ = true;
        return;
    }
    if (resync_) {
        current_ = animator_->current_state();
        air_time_ = 0.0f;
        resync_ = false;
    }

    air_time_ = controller_->is_grounded() ? 0.0f : air_time_ + dt;
    // With root motion the clip makes the speed, so pick the clip from the requested speed
    // (input x move_speed) instead -- else a standing character could never start walking.
    float speed;
    if (controller_->use_root_motion) {
        speed = std::min(1.0f, glm::length(controller_->move_input)) * controller_->move_speed *
                (controller_->sprint ? controller_->sprint_multiplier : 1.0f);
    } else {
        const glm::vec3 v = controller_->velocity();
        speed = glm::length(glm::vec2(v.x, v.y));
    }
    std::string want = idle_state;
    if (!jump_state.empty() && air_time_ > air_delay) want = jump_state;
    else if (speed > run_speed) want = run_state;
    else if (speed > walk_speed) want = walk_state;

    if (want != current_ && animator_->find_state(want)) {
        if (current_.empty()) animator_->play(want);
        else animator_->crossfade(want, crossfade);
        current_ = want;
    }
}

coopa::anim::Animator* CharacterAnimDriver::find_animator_(coopa::scene::SceneObject* obj) {
    if (auto* a = obj->get_component<coopa::anim::Animator>()) return a;
    for (const auto& child : obj->children()) {
        if (auto* a = find_animator_(child.get())) return a;
    }
    return nullptr;
}

} // namespace scene
} // namespace toy
