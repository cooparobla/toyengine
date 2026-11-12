/**
 * @file damage_cursor.h
 * @brief Click-to-cut and click-to-hit, the demo's input surface for slicing and typed damage.
 *
 * The ray and button edges are PUSHED in by `Engine::drive_damage_cursors_()`, the same
 * push-not-pull contract `FireIgniter` (toyengine/fire/fire.h) already uses -- so nothing here
 * reads `coopa::input::Input` directly, and `NO_INPUT=1` silences it for headless runs.
 *
 * ```yaml
 * - type: DamageCursor
 *   slice_button: left         # cut along a random-angle plane through the hit point; "none"
 *                              # (or empty) turns slicing off
 *   hit_button: right          # deal `damage_type` damage at the hit point
 *   damage_type: Impact        # Impact / Slash / Pierce / Explosive -- the one hit_button deals
 *   type_keys: "12345"         # press to SWITCH damage_type: 1=Impact 2=Slash 3=Pierce
 *                              # 4=Explosive 5=Fire; empty disables the keys entirely
 *   amount: 400
 *   seed: 7                    # seeds the random slice angle, so a run repeats
 *   max_distance: 1000
 * ```
 */

#ifndef TOYENGINE_SCENE_DAMAGE_CURSOR_H
#define TOYENGINE_SCENE_DAMAGE_CURSOR_H

#include <coopa/scene/component.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <random>
#include <string>

namespace toy {
namespace scene {

/**
 * @class DamageCursor
 * @brief Opts a scene into click-to-cut / click-to-hit; DestructionSystem consumes the requests.
 */
class DamageCursor : public coopa::scene::Component {
public:
    std::string type_name() const override { return "DamageCursor"; }

    /** @brief Mouse button that slices: left / right / middle, or "none" (same as empty) to
     *         turn slicing off -- unlike the others, an unrecognised name here is NOT a silent
     *         fallback to left, since that would make it impossible to disable. */
    std::string slice_button = "left";
    std::string hit_button = "right";    ///< Mouse button that deals damage; "none" disables it too.
    std::string damage_type = "Impact";  ///< Impact / Slash / Pierce / Explosive, for hit_button.

    /** @brief Five key names, one per DamageType in enum order (Impact/Slash/Pierce/Explosive/
     *         Fire); pressing one sets `damage_type` to that type, so a click can hit the same
     *         object a different way without editing the inspector. A key name
     *         `coopa::input::key_name()` does not recognise (or `type_keys` shorter than 5
     *         characters) silently disables that one slot; empty disables all of them. */
    std::string type_keys = "12345";

    float amount = 400.0f;               ///< DamageEvent::amount for hit_button's blow.
    std::uint32_t seed = 7;              ///< Seeds the slice plane's random angle; 0 means 1.
    float max_distance = 1000.0f;        ///< Metres to search along the ray.

    /** @brief A seeded, per-cursor angle for the next slice plane, perpendicular-ish to the
     *         view ray so the cut is visible rather than edge-on. Advances each call -- what
     *         keeps a scripted run of clicks deterministic without the angle repeating. */
    float next_slice_angle() {
        if (!seeded_) {
            rng_.seed(seed != 0 ? seed : 1u);
            seeded_ = true;
        }
        return angle_(rng_);
    }

    /// @name Written by Engine each frame, before the scene updates.
    /// @{
    bool slice_requested = false;
    bool hit_requested = false;
    glm::vec3 ray_origin{0.0f};
    glm::vec3 ray_direction{0.0f, 1.0f, 0.0f};
    /// @}

private:
    std::mt19937 rng_;
    std::uniform_real_distribution<float> angle_{0.0f, 6.28318530718f};
    bool seeded_ = false;
};

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_DAMAGE_CURSOR_H
