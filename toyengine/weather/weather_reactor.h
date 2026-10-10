/**
 * @file weather_reactor.h
 * @brief `WeatherReactor` -- switches things on and off with the time of day and the weather,
 *        with no code: street lamps at night, a campfire that goes out in the rain, a shelter's
 *        drip effect only while it rains.
 *
 * @code
 * - type: WeatherReactor
 *   hours: [18.5, 6]          # on between these hours (wraps past midnight; or {x: from, y: to}); omit = any hour
 *   phases: [night, dusk]     # ...and/or in these parts of the day (night, dawn, day, dusk)
 *   conditions: [rain, storm] # ...and/or while the weather is (moving to) one of these
 *   min_precipitation: 0.0    # ...and/or at least this much rain / snow
 *   invert: false             # on when the rule is NOT met (a fire that is out in the rain)
 *   target: [lights, effects] # what switches (a list, or one word); the default is lights + effects:
 *                             #   lights   fade every PointLight / SpotLight on this object and under it
 *                             #   effects  stop / play every ParticleSystem on this object and under it
 *                             #            (stopped effects let their live particles finish)
 *                             #   children set_active() on its direct children (meshes, anything)
 *   fade: 1.5                 # seconds a light takes to fade in or out
 * @endcode
 *
 * Every listed rule must hold (an omitted rule always holds). Runs while the scene simulates
 * (a component update); it reads toy::weather::current(), so with no weather in the scene it sees
 * noon, clear. A light with a LightFlicker is faded through the flicker's `dimmer`, so the two
 * compose (the campfire prefab goes out in the rain this way).
 */

#ifndef TOYENGINE_WEATHER_WEATHER_REACTOR_H
#define TOYENGINE_WEATHER_WEATHER_REACTOR_H

#include <algorithm>
#include <string>
#include <vector>

#include <fkYAML/node.hpp>

#include <gfxcoopa/engine/components/spot_light.h>

#include <gfxcoopa/engine/components/point_light.h>
#include <toyengine/particles/light_flicker.h>
#include <toyengine/weather/weather_system.h>

namespace toy {
namespace weather {

class WeatherReactor : public coopa::scene::Component {
public:
    bool use_hours = false;
    float from_hour = 18.0f;
    float to_hour = 6.0f;
    std::vector<std::string> phases;       ///< night / dawn / day / dusk; empty = any.
    std::vector<std::string> conditions;   ///< Empty = any.
    float min_precipitation = 0.0f;
    bool invert = false;
    bool target_lights = true;
    bool target_effects = true;
    bool target_children = false;
    float fade = 1.5f;

    std::string type_name() const override { return "WeatherReactor"; }

    /** @brief Does `w` meet the rule (before `invert`)? */
    bool matches(const WeatherState& w) const;
    /** @brief Whether it is switched on now. */
    bool on() const { return on_; }
    /** @brief The lights' fade level, 0..1. */
    float level() const { return level_; }

    void start() override;

    void update(float dt) override;

private:
    struct Light {
        coopa::gfx::engine::components::PointLightComponent* point = nullptr;
        coopa::gfx::engine::components::SpotLightComponent* spot = nullptr;
        toy::particles::LightFlicker* flicker = nullptr;
        float base = 0.0f;
    };

    void apply_(bool switched);

    bool on_ = true;
    float level_ = 1.0f;
    std::vector<Light> lights_;
};

/** @brief Registers the weather's component parsers: "WeatherReactor", "WeatherSurface", "WeatherDistantLandings". */
void register_weather_components();

} // namespace weather
} // namespace toy

#endif // TOYENGINE_WEATHER_WEATHER_REACTOR_H
