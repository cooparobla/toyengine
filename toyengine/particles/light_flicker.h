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
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/point_light.h>

#include <toyengine/particles/particle_math.h>

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

    void start() override {
        if (!owner) return;
        if (auto* pl = owner->get_component<coopa::gfx::engine::components::PointLightComponent>()) {
            base_intensity_ = pl->intensity;
            base_color_ = pl->color;
        }
        if (auto* tc = owner->get_transform()) base_pos_ = tc->transform().position();
        if (seed == 0) {
            uint32_t h = 2166136261u;
            for (char c : owner->name()) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
            seed = h;
        }
        phase_ = hash01(seed) * 100.0f;
        started_ = true;
    }

    void update(float dt) override {
        if (!started_ || !owner) return;
        t_ += dt;
        auto* pl = owner->get_component<coopa::gfx::engine::components::PointLightComponent>();
        if (!pl) return;
        const float n = flicker_(t_ * speed + phase_);
        pl->intensity = base_intensity_ * std::max(0.0f, 1.0f + amount * n) * dimmer;
        pl->color = base_color_ + color_shift * std::max(0.0f, n);
        if (wobble > 0.0f) {
            if (auto* tc = owner->get_transform()) {
                const glm::vec3 j(flicker_(t_ * speed * 0.7f + phase_ + 13.0f), flicker_(t_ * speed * 0.6f + phase_ + 29.0f),
                                  flicker_(t_ * speed * 0.5f + phase_ + 47.0f) * 0.5f);
                tc->transform().set_position(base_pos_ + j * wobble);
            }
        }
    }

    /** @brief The intensity the flicker swings around (the authored one, read in start()). */
    float base_intensity() const { return base_intensity_; }

private:
    /** @brief Smooth 1D value noise in about [-1, 1], three octaves -- a soft roll with quick licks. */
    static float noise1_(float x) {
        const float i = std::floor(x);
        const float f = x - i;
        const float a = hash01(static_cast<uint32_t>(static_cast<int32_t>(i)) * 2654435761u) * 2.0f - 1.0f;
        const float b = hash01(static_cast<uint32_t>(static_cast<int32_t>(i) + 1) * 2654435761u) * 2.0f - 1.0f;
        const float s = f * f * (3.0f - 2.0f * f);
        return a + (b - a) * s;
    }
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
