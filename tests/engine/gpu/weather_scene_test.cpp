/**
 * @file weather_scene_test.cpp
 * @brief weather_test end to end: rain with TAA's reactive mask, the atmosphere driving the render
 *        config, splashes only on WeatherSurface ground and none under the awning, reactors switching
 *        lamps and the campfire, and disabling the weather restoring the config.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/runtime_object.h>
#include <toyengine/weather/weather_reactor.h>
#include <toyengine/weather/weather_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("weather_scene");

using namespace toy::test;

COOPA_TEST(rain_reactors_and_atmosphere_reach_the_scene) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/tests/effects/weather_test/scene.yaml", 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    config.render.aa_mode = "taa";              // the reactive mask is TAA's
    config.render.volumetrics_enabled = true;
    toy::core::Engine engine(std::move(config));
    // Orbit close by the market awning, so it is inside the precipitation map around the camera.
    if (auto* cc = engine.scene().find_first_component<toy::scene::CameraController>()) {
        cc->target = glm::vec3(3.2f, -1.0f, 1.0f);
        cc->distance = 8.0f;
    }
    tick_frames(engine, 90);

    toy::weather::WeatherSystem* w = engine.weather();
    expect(w && w->enabled() && w->state().enabled, "weather_test: the scene's weather is on");
    if (!w) return;
    expect(w->state().condition == "rain", "weather_test: starts in rain");
    expect(engine.pipeline().has_reactive_mask(), "weather_test: TAA runs with the particles' reactive mask");
    const auto& cfg = engine.pipeline().render_config();
    expect_near(cfg.fog_density, w->atmosphere().fog_density, 1e-6f, "weather_test: the atmosphere reaches the render config");
    expect(glm::length(cfg.indirect.sky_zenith - w->atmosphere().sky_zenith) < 1e-5f, "weather_test: ...sky included");

    coopa::scene::SceneObject* root = engine.scene().find_object("Weather");
    expect(root && toy::scene::is_runtime_object(*root), "weather_test: the effects hang under a runtime root");
    coopa::scene::SceneObject* drops = engine.scene().find_object("weather_rain_drops");
    auto* ps = drops ? drops->get_component<toy::particles::ParticleSystem>() : nullptr;
    expect(ps && ps->particle_count() > 500u, "weather_test: it is raining (" + std::to_string(ps ? ps->particle_count() : 0) + " drops)");
    expect(drops && toy::scene::is_runtime_object(*drops), "weather_test: the rain is a runtime object");
    // Ground interaction: splashes where drops land, the precipitation map sees the awning
    // (2.4 m up over x 1.7..4.7, y -3..1), and no drop is under it.
    coopa::scene::SceneObject* splash_obj = engine.scene().find_object("weather_rain_splash");
    auto* splash = splash_obj ? splash_obj->get_component<toy::particles::ParticleSystem>() : nullptr;
    expect(splash && splash->particle_count() > 20u, "weather_test: rain splashes where it lands (" +
                                                        std::to_string(splash ? splash->particle_count() : 0) + ")");
    const auto& field = w->ground_probe().field();
    expect(field && std::abs(field->sample(3.2f, -1.0f) - 2.46f) < 0.1f, "weather_test: the precipitation map finds the awning (" +
                                                                         std::to_string(field ? field->sample(3.2f, -1.0f) : -1.0f) + ")");
    expect(field && std::abs(field->sample(-12.0f, 3.0f)) < 0.05f, "weather_test: ...and the ground beside it");
    // Only the ground takes splashes (its WeatherSurface): none on roofs, the well or the awning;
    // and they reach past the drops' own box (WeatherDistantLandings).
    if (splash) {
        bool ground_only = true, far = false;
        glm::vec3 eye(0.0f);
        if (auto* cam = coopa::gfx::engine::components::CameraComponent::main(); cam && cam->owner && cam->owner->get_transform())
            eye = glm::vec3(cam->owner->get_transform()->transform().get_world_matrix()[3]);
        for (const auto& p : splash->pool().pos) {
            ground_only &= p.z < 0.3f;
            far |= glm::length(glm::vec2(p) - glm::vec2(eye)) > 25.0f;
        }
        expect(ground_only, "weather_test: splashes only on the ground (WeatherSurface), never on roofs");
        expect(far, "weather_test: distant landings splash past the drops' box (> 25 m)");
    }
    if (ps) {
        bool dry = true;
        for (const auto& p : ps->pool().pos) dry &= !(p.x > 2.2f && p.x < 4.2f && p.y > -2.5f && p.y < 0.5f && p.z < 2.3f);
        expect(dry, "weather_test: no rain under the awning");
    }

    // The campfire is out in the rain; the lamps are off at 16:30.
    coopa::scene::SceneObject* fire = engine.scene().find_object("campfire_site");
    auto* reactor = fire ? fire->get_component<toy::weather::WeatherReactor>() : nullptr;
    expect(reactor && !reactor->on(), "weather_test: the campfire's reactor is off in the rain");
    auto* lamp = engine.scene().find_object("lamp_0_light");
    auto* lamp_r = lamp ? lamp->get_component<toy::weather::WeatherReactor>() : nullptr;
    expect(lamp_r && !lamp_r->on(), "weather_test: lamps are off in the afternoon");

    // Night, clear: lamps on, fire lit, rain fading out.
    w->set_time(22.0f);
    w->set_condition("clear", 0.0f);
    tick_frames(engine, 150);
    expect(lamp_r && lamp_r->on() && lamp_r->level() > 0.6f, "weather_test: lamps fade in at night");
    expect(reactor && reactor->on(), "weather_test: the campfire relights when the rain stops");
    expect(w->state().is_night(), "weather_test: 22:00 is night");

    // Switching the weather off puts the config's own values back.
    fkyaml::node settings = fkyaml::node::mapping();
    settings["weather"] = fkyaml::node::deserialize(std::string("enabled: false\n"));
    engine.set_scene_settings(engine.scene(), settings);
    tick_frames(engine, 2);
    expect(engine.scene().find_object("Weather") == nullptr, "weather_test: disabled, the runtime objects are gone");
    expect(engine.pipeline().render_config().fog_density != w->atmosphere().fog_density ||
           engine.pipeline().render_config().fog_density == engine.scene_config(engine.scene()).render.fog_density,
           "weather_test: disabled, the render config is the config's again");
    const Frame f = engine.capture_image(true);
    long long black = 0;
    for (size_t i = 0; i < static_cast<size_t>(f.width) * f.height; ++i) {
        const uint8_t* px = &f.pixels[i * f.channels];
        if (px[0] < 2 && px[1] < 2 && px[2] < 2) ++black;
    }
    expect(black < static_cast<long long>(f.width) * f.height / 200, "weather_test: no NaN black blocks");
}
