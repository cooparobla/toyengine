/**
 * @file character_anim_driver.h
 * @brief A tiny gameplay-side demo: crossfades a character's Animator between idle, walk, run
 *        and jump states from its CharacterController's speed and grounded state.
 *
 * With the controller's `use_root_motion` on, the state is picked from the REQUESTED speed
 * (move_input x move_speed, sprint included) rather than the measured one, since the clip's own
 * root motion is what produces the speed.
 *
 * This is the "hardcode it per game" layer the engine deliberately has instead of animation
 * state machines or blend trees: a few lines that pick a state name and call
 * Animator::crossfade() when it changes. Copy it into a game and grow it there.
 *
 * Example YAML (next to a CharacterController; the Animator may be on this object or under it):
 * @code
 * - type: CharacterAnimDriver
 *   idle_state: idle
 *   walk_state: walk
 *   run_state: run
 *   jump_state: jump
 *   walk_speed: 0.2      # above this (m/s): walk
 *   run_speed: 5.0       # above this: run
 *   crossfade: 0.2
 * @endcode
 */

#ifndef TOYENGINE_SCENE_CHARACTER_ANIM_DRIVER_H
#define TOYENGINE_SCENE_CHARACTER_ANIM_DRIVER_H

#include <glm/glm.hpp>

#include <algorithm>
#include <string>

#include <coopa/animation/animator.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

#include <toyengine/scene/character_controller.h>

namespace toy {
namespace scene {

/**
 * @class CharacterAnimDriver
 * @brief Picks idle / walk / run / jump from the sibling CharacterController each frame and
 *        crossfades the Animator to it.
 */
class CharacterAnimDriver : public coopa::scene::Component {
public:
    std::string type_name() const override { return "CharacterAnimDriver"; }

    std::string idle_state = "idle";
    std::string walk_state = "walk";
    std::string run_state = "run";
    std::string jump_state = "jump";   ///< Played while airborne; empty keeps the ground state.
    float walk_speed = 0.2f;           ///< Horizontal speed (m/s) above which the character walks.
    float run_speed = 5.0f;            ///< ...and above which it runs.
    float crossfade = 0.2f;            ///< Crossfade duration (s).
    float air_delay = 0.15f;           ///< Airborne this long before the jump state plays (steps and
                                       ///< small drops stay in the ground state).

    /** @brief The state last crossfaded to (empty before the first update). */
    const std::string& current() const { return current_; }

    void update(float dt) override {
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

private:
    static coopa::anim::Animator* find_animator_(coopa::scene::SceneObject* obj) {
        if (auto* a = obj->get_component<coopa::anim::Animator>()) return a;
        for (const auto& child : obj->children()) {
            if (auto* a = find_animator_(child.get())) return a;
        }
        return nullptr;
    }

    CharacterController* controller_ = nullptr;
    coopa::anim::Animator* animator_ = nullptr;
    std::string current_;
    float air_time_ = 0.0f;
    bool resync_ = false;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_CHARACTER_ANIM_DRIVER_H
