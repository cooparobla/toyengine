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
 * A gameplay action that wants its own clip (a swing, a roll, a glide) cannot simply play one:
 * the driver would crossfade straight back to the state it picks from speed. It takes a CLAIM
 * on the four names instead -- push_states() / release_states() -- which leaves the driver doing
 * its usual speed-based pick inside a new vocabulary, so swimming still tells a paddle from a
 * tread. Claims stack, and the stack is what makes overlapping actions safe: see
 * push_states()'s doc.
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
#include <cstddef>
#include <string>
#include <vector>

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

    /**
     * @brief Points the four state names at `moving` / `still` until the claim is released.
     *
     * The names in effect are the TOP claim's, and the driver's own four (whatever they were
     * when the first claim arrived) come back once the last claim is gone. Nothing about the
     * order claims are released in matters: each one is remembered independently, so two
     * actions that overlap -- a roll started mid-swing, a cast that outlives the swing it
     * began during -- cannot hand each other's clip back as the character's resting state.
     * Save-and-restore by the caller cannot do this: whichever action released second would
     * restore what it found, which is the other action's clip, and the driver would then want
     * that clip for ever -- leaving the hero stuck in a swing, and (under root motion) unable
     * to move at all.
     *
     * @param moving State for walking, running and being airborne. Airborne deliberately does
     *        not fall back to the jump clip: swimming and gliding are airborne as far as the
     *        motor is concerned.
     * @param still State at rest; empty reuses `moving`.
     * @return A token to pass to release_states(). Never 0.
     */
    int push_states(const std::string& moving, const std::string& still = std::string());

    /**
     * @brief Re-points claim `token` at new names, keeping its place in the stack.
     *
     * What a holder that re-asserts itself every frame wants: pushing again instead would climb
     * over a claim taken after it (so a swing begun mid-climb would never be seen) and leave a
     * claim per frame behind.
     *
     * @param token The claim to re-point.
     * @param moving See push_states().
     * @param still See push_states().
     * @return False if `token` is not an outstanding claim; nothing changes then.
     */
    bool repoint_states(int token, const std::string& moving,
                        const std::string& still = std::string());

    /**
     * @brief Drops the claim `token` names (see push_states()). Ignores 0, an unknown token and
     *        a token released twice, so a caller may release unconditionally.
     */
    void release_states(int token);

    /** @brief How many claims are outstanding; 0 means the driver's own states are in effect. */
    std::size_t claim_count() const { return claims_.size(); }

    void update(float dt) override;

private:
    /** @brief One outstanding push_states() claim. */
    struct StateClaim {
        int token = 0;
        std::string moving;
        std::string still;
    };

    static coopa::anim::Animator* find_animator_(coopa::scene::SceneObject* obj);

    /** @brief Writes the top claim's names into the four fields, or the driver's own when the
     *         stack is empty. */
    void apply_claims_();

    CharacterController* controller_ = nullptr;
    coopa::anim::Animator* animator_ = nullptr;
    std::string current_;
    float air_time_ = 0.0f;
    bool resync_ = false;

    std::vector<StateClaim> claims_;
    int next_token_ = 1;
    /** @brief The four names as they were before the first claim; restored when the last goes. */
    std::string own_idle_, own_walk_, own_run_, own_jump_;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_CHARACTER_ANIM_DRIVER_H
