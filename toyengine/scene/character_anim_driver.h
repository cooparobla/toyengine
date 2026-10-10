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

    void update(float dt) override;

private:
    static coopa::anim::Animator* find_animator_(coopa::scene::SceneObject* obj);

    CharacterController* controller_ = nullptr;
    coopa::anim::Animator* animator_ = nullptr;
    std::string current_;
    float air_time_ = 0.0f;
    bool resync_ = false;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_CHARACTER_ANIM_DRIVER_H
