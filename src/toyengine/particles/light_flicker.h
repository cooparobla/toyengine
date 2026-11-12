/**
 * @file light_flicker.h
 * @brief `LightFlicker` -- makes a sibling PointLight breathe like a fire: smooth layered noise
 *        on its intensity (and optionally a small positional wobble and a warm/cool colour
 *        shift), so a campfire's light dances with its flames instead of sitting dead still.
 *
 * Example YAML:
 * @code
 * - type: PointLight
 *   color: { r: 1.0, g: 0.55, b: 0.2 }
 *   intensity: 40.0
 *   range: 9.0
 * - type: LightFlicker
 *   amount: 0.35      # +-35% intensity
 *   speed: 7.0        # flicker rate (Hz-ish)
 *   wobble: 0.04      # metres of positional jitter (moves the shadows too)
 * @endcode
 *
 * Runs in update() (play mode); the editor shows the authored, steady light.
 */

#ifndef TOYENGINE_PARTICLES_LIGHT_FLICKER_H
#define TOYENGINE_PARTICLES_LIGHT_FLICKER_H

#include <glm/glm.hpp>

#include <cmath>
#include <string>

#include <coopa/scene/component.h>


namespace toy {
namespace particles {

class LightFlicker : public coopa::scene::Component {
public:
    std::string type_name() const override { return "LightFlicker"; }

    float amount = 0.3f;          ///< Fraction of the base intensity the flicker swings by.
    float speed = 6.0f;           ///< Rate of the fastest layer.
    float wobble = 0.0f;          ///< Positional jitter (m) of the light around its authored spot.
    glm::vec3 color_shift{0.0f};  ///< Added to the light colour at the bright peaks (e.g. toward yellow).
    uint32_t seed = 0;
    /// 0..1 multiplier on the flickered intensity, for something else to fade the light without
    /// fighting the flicker (WeatherReactor puts a campfire out this way). Not authored.
    float dimmer = 1.0f;

    void start() override;

    void update(float dt) override;

    /** @brief The intensity the flicker swings around (the authored one, read in start()). */
    float base_intensity() const { return base_intensity_; }

private:
    /** @brief Smooth 1D value noise in about [-1, 1], three octaves -- a soft roll with quick licks. */
    static float noise1_(float x);
    static float flicker_(float x) {
        return noise1_(x * 0.35f) * 0.55f + noise1_(x) * 0.3f + noise1_(x * 2.7f) * 0.15f;
    }

    float base_intensity_ = 1.0f;
    glm::vec3 base_color_{1.0f};
    glm::vec3 base_pos_{0.0f};
    float t_ = 0.0f;
    float phase_ = 0.0f;
    bool started_ = false;
};

} // namespace particles
} // namespace toy

#endif // TOYENGINE_PARTICLES_LIGHT_FLICKER_H
