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
 *   slice_button: left         # cut along a random-angle plane through the hit point
 *   hit_button: right          # deal `damage_type` damage at the hit point
 *   damage_type: Impact        # Impact / Slash / Pierce / Explosive
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

    std::string slice_button = "left";   ///< Mouse button that slices: left / right / middle.
    std::string hit_button = "right";    ///< Mouse button that deals damage.
    std::string damage_type = "Impact";  ///< Impact / Slash / Pierce / Explosive, for hit_button.
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
