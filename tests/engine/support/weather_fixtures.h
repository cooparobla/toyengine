#pragma once

/**
 * @file weather_fixtures.h
 * @brief A weather block for the device-free weather and snow suites -- two simple conditions
 *        (sunny, wet) and a never-picked one, no effects -- and a stepper that drives a
 *        WeatherSystem's execute() by hand on a bare Scene.
 */

#include <string>

#include <coopa/scene/scene.h>
#include <coopa/yaml/document.h>
#include <toyengine/weather/weather_system.h>

#include <glm/glm.hpp>

#include "engine/support/scene_variant.h"

namespace toy::test {

/** @brief A weather block for the device-free tests: two simple conditions, no effects. */
inline fkyaml::node weather_test_block(const std::string& extra = "") {
    fkyaml::node n = fkyaml::node::deserialize(std::string(
        "enabled: true\n"
        "time_of_day: 11.5\n"
        "day_length_minutes: 24\n"
        "condition: sunny\n"
        "drive_sun: true\n"
        "seed: 3\n"
        "conditions:\n"
        "  - {name: sunny, cloud_cover: 0.0, sun: 1.0, fog_density: 0.0, wind_strength: 0, transition: 10, duration: [0.1, 0.1], next: [wet]}\n"
        "  - {name: wet, cloud_cover: 1.0, sun: 0.2, fog_density: 0.04, precipitation: 1.0, wind_strength: 6, transition: 10, duration: [0.1, 0.1], next: [sunny]}\n"
        "  - {name: never, weight: 0, duration: [0.1, 0.1]}\n"));
    if (!extra.empty()) {   // overrides win over the base keys
        const fkyaml::node over = fkyaml::node::deserialize(extra);
        for (auto item : over.map_items()) n[item.key().get_value<std::string>()] = item.value();
    }
    return n;
}
inline void weather_step(toy::weather::WeatherSystem& w, coopa::scene::Scene& scene, float seconds, float dt = 0.1f) {
    for (float t = 0.0f; t < seconds - 1e-4f; t += dt) {
        coopa::scene::FrameContext ctx;
        ctx.delta_time = dt;
        w.execute(scene, ctx);
    }
}

/// Where weather_demo's snow field is centred (its objects are laid out around this point).
inline const glm::vec2 k_snow_field{80.0f, 0.0f};

/**
 * @brief weather_demo set up for its snow field: snowing on a fixed schedule with the clock
 *        stopped at 11:00, and the orbit camera (auto-rotate off) looking at the field -- the
 *        snow trench window centres on the camera's look point.
 */
inline std::string snow_field_demo() {
    return scene_variant("assets/scenes/effects/weather_demo/scene.yaml", "snow_field", [](fkyaml::node& doc) {
        fkyaml::node& w = doc["scene"]["settings"]["weather"];
        w["condition"] = fkyaml::node(std::string("snow"));
        w["schedule"] = fkyaml::node(std::string("fixed"));
        w["day_length_minutes"] = fkyaml::node(0);
        w["time_of_day"] = fkyaml::node(11.0);
        auto v3 = [](float x, float y, float z) {
            fkyaml::node n = fkyaml::node::mapping();
            n["x"] = fkyaml::node(x);
            n["y"] = fkyaml::node(y);
            n["z"] = fkyaml::node(z);
            return n;
        };
        if (fkyaml::node* tf = scene_component(doc, "camera", "Transform")) {
            (*tf)["position"] = v3(k_snow_field.x, k_snow_field.y - 11.0f, 5.5f);
            (*tf)["rotation"] = v3(66.0f, 0.0f, 0.0f);
        }
        if (fkyaml::node* cc = scene_component(doc, "camera", "CameraController")) {
            (*cc)["target"] = v3(k_snow_field.x, k_snow_field.y + 0.5f, 0.0f);
            (*cc)["auto_rotate_deg_per_sec"] = fkyaml::node(0.0);
        }
    });
}

} // namespace toy::test
