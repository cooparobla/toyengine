/**
 * @file wind_field_test.cpp
 * @brief The weather's wind field (toyengine/weather/wind.h) and what reads it: the gust field
 *        averages to the mean wind and rolls downwind; particles with wind_influence are carried
 *        downwind (and untouched without it); water waves rise and turn with the wind (and stay as
 *        authored with wind_response 0); WindSystem pushes wind-influenced rigidbodies (a heavy
 *        one less, a sleeping one in calm air not at all) and drives cloth wind.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <physxcoopa/components/cloth.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/particles/particle_system_runner.h>
#include <toyengine/water/water_waves.h>
#include <toyengine/weather/weather_system.h>
#include <toyengine/weather/wind.h>
#include <toyengine/weather/wind_system.h>

#include "engine/support/checks.h"
#include "engine/support/particle_fixtures.h"
#include "engine/support/weather_fixtures.h"

COOPA_TEST_SUITE("wind_field");

using namespace toy::test;

namespace {

/** @brief A weather block holding one fixed condition with this wind. */
fkyaml::node windy_block(float strength, float gust, float heading = 0.0f) {
    return weather_test_block("schedule: fixed\ncondition: gale\nconditions:\n  - {name: gale, wind_strength: " + std::to_string(strength) +
                              ", wind_gust: " + std::to_string(gust) + ", wind_heading: " + std::to_string(heading) +
                              ", duration: [100, 100]}\n");
}

} // namespace

COOPA_TEST(field_averages_to_the_mean_and_gusts_roll_downwind) {
    using toy::weather::wind_at;
    const glm::vec3 wind(6.0f, 0.0f, 0.0f);
    expect(wind_at(glm::vec3(0.0f), 0.8f, 3.0f, glm::vec3(10, 20, 0)) == glm::vec3(0.0f), "wind: calm air is calm everywhere");
    expect(wind_at(wind, 0.0f, 3.0f, glm::vec3(10, 20, 0)) == wind, "wind: no gusts = the mean everywhere");
    expect(wind_at(wind, 0.5f, 7.0f, glm::vec3(3, 4, 0)) == wind_at(wind, 0.5f, 7.0f, glm::vec3(3, 4, 0)), "wind: deterministic");

    // Mean over a big patch of ground and a stretch of time ~ the mean wind; it does vary.
    glm::vec3 sum(0.0f);
    float lo = 1e9f, hi = -1e9f;
    int n = 0;
    for (int t = 0; t < 40; ++t) {
        for (int y = 0; y < 20; ++y) {
            for (int x = 0; x < 20; ++x) {
                const glm::vec3 w = wind_at(wind, 0.6f, t * 1.7f, glm::vec3(x * 23.0f, y * 19.0f, 0.0f));
                sum += w;
                lo = std::min(lo, w.x);
                hi = std::max(hi, w.x);
                ++n;
            }
        }
    }
    const glm::vec3 mean = sum / static_cast<float>(n);
    expect_near(mean.x, 6.0f, 0.6f, "wind: the gust field averages to the mean wind");
    expect_near(mean.y, 0.0f, 1e-4f, "wind: gusts never turn it");
    expect(hi - lo > 3.0f, "wind: gusts vary the speed across space and time (" + std::to_string(lo) + " .. " + std::to_string(hi) + ")");

    // The spatial pattern travels downwind at the wind's speed (the global pulse aside).
    const float t0 = 4.0f, dt = 2.5f;
    const glm::vec2 xy(30.0f, 12.0f);
    const float g0 = toy::weather::wind_gust_term(glm::vec2(wind), xy, t0) - 0.3f * (0.6f * std::sin(0.7f * t0) + 0.4f * std::sin(1.9f * t0 + 1.3f));
    const float t1 = t0 + dt;
    const float g1 = toy::weather::wind_gust_term(glm::vec2(wind), xy + glm::vec2(6.0f * dt, 0.0f), t1) -
                     0.3f * (0.6f * std::sin(0.7f * t1) + 0.4f * std::sin(1.9f * t1 + 1.3f));
    expect_near(g0, g1, 1e-3f, "wind: a gust front is found downwind, wind speed x time later");
}

COOPA_TEST(particles_with_wind_influence_are_carried_downwind) {
    using namespace toy::particles;
    PScene ps;
    auto setup = [](float influence) {
        return [influence](ParticleSystem& s) {
            s.settings.shape.type = EmitShape::Point;
            s.settings.start_speed = 0.0f;
            s.settings.gravity = 0.0f;
            s.settings.rate = 0.0f;
            s.settings.start_lifetime = 100.0f;
            s.settings.wind_influence = influence;
            s.settings.looping = false;
            s.settings.duration = 100.0f;
            s.settings.bursts.push_back({0.0f, Range(10.0f), 1, 1.0f, 1.0f});
        };
    };
    auto* blown = add_system(ps, "smoke", glm::vec3(0, 0, 2), setup(1.0f));
    auto* half = add_system(ps, "embers", glm::vec3(0, 5, 2), setup(0.5f));
    auto* still = add_system(ps, "sparks", glm::vec3(0, 10, 2), setup(0.0f));
    toy::weather::install_weather_system(*ps.scene, windy_block(8.0f, 0.0f));
    start(ps);
    run(ps, 6.0f);

    auto mean_vx = [](const ParticleSystem* s) {
        float v = 0.0f;
        for (std::size_t i = 0; i < s->pool().size(); ++i) v += s->pool().vel[i].x;
        return s->pool().size() ? v / static_cast<float>(s->pool().size()) : 0.0f;
    };
    expect(blown->particle_count() == 10, "wind particles: the burst is alive");
    expect_near(mean_vx(blown), 8.0f, 0.5f, "wind particles: influence 1 moves at the wind's speed");
    expect_near(mean_vx(half), 4.0f, 0.4f, "wind particles: influence 0.5 at half of it");
    expect_near(mean_vx(still), 0.0f, 1e-6f, "wind particles: influence 0 ignores the wind");
    expect(blown->pool().pos[0].x > 30.0f, "wind particles: blown well downwind");
}

COOPA_TEST(water_waves_rise_and_turn_with_the_wind) {
    using namespace toy::water;
    WaveParams authored;
    authored.amplitude = 0.4f;
    authored.wavelength = 10.0f;
    authored.direction = 0.0f;
    authored.steepness = 0.4f;

    const WaveParams gale = wind_waves(authored, 1.0f, glm::vec2(0.0f, 14.0f));   // blowing toward +Y
    const WaveParams calm = wind_waves(authored, 1.0f, glm::vec2(0.0f, 0.5f));
    const WaveParams pinned = wind_waves(authored, 0.0f, glm::vec2(0.0f, 14.0f));
    expect(gale.amplitude > authored.amplitude * 2.0f, "water wind: a gale raises the waves");
    expect(gale.steepness > authored.steepness, "water wind: and sharpens them");
    expect_near(gale.direction, 1.5707963f, 1e-3f, "water wind: and turns them downwind");
    expect_near(gale.wavelength, authored.wavelength, 1e-6f, "water wind: never the wavelength (phase)");
    expect(calm.amplitude < authored.amplitude * 0.3f, "water wind: calm air flattens the water");
    expect(pinned.pack() == authored.pack(), "water wind: wind_response 0 keeps the authored waves");
    expect(wind_waves_max(authored, 1.0f).amplitude >= gale.amplitude, "water wind: the bounds wave covers the roughest sea");
    expect(wind_ripple_scale(1.0f, glm::vec2(14.0f, 0.0f)) > 1.5f && wind_ripple_scale(1.0f, glm::vec2(0.0f)) < 0.5f,
           "water wind: ripples follow the wind");
}

COOPA_TEST(wind_system_pushes_rigidbodies_by_influence_and_mass) {
    using coopa::physx::components::RigidbodyComponent;
    using coopa::physx::components::SphereCollider;
    using coopa::scene::Scene;
    using coopa::scene::SceneObject;
    using coopa::scene::TransformComponent;

    Scene scene("wind_bodies");
    auto add_ball = [&](const std::string& name, glm::vec3 pos, float mass, float influence) {
        auto obj = std::make_unique<SceneObject>(name);
        obj->add_component<TransformComponent>()->transform().set_position(pos);
        obj->add_component<SphereCollider>()->set_radius(0.3f);
        auto* rb = obj->add_component<RigidbodyComponent>();
        rb->mass = mass;
        rb->use_gravity = false;
        rb->wind_influence = influence;
        scene.add_root_object(std::move(obj));
        return rb;
    };
    auto* light = add_ball("light", glm::vec3(0, 0, 5), 1.0f, 0.2f);
    auto* heavy = add_ball("heavy", glm::vec3(0, 10, 5), 50.0f, 0.2f);
    auto* deaf = add_ball("deaf", glm::vec3(0, 20, 5), 1.0f, 0.0f);
    coopa::physx::components::ClothComponent* flag = nullptr;
    {
        auto obj = std::make_unique<SceneObject>("flag");
        obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0, -10, 5));
        flag = obj->add_component<coopa::physx::components::ClothComponent>();
        flag->columns = flag->rows = 5;
        flag->params.wind = glm::vec3(0.0f, 1.0f, 0.0f);
        scene.add_root_object(std::move(obj));
    }
    toy::weather::install_weather_system(scene, windy_block(8.0f, 0.0f));
    scene.start();
    auto* phys = coopa::physx::system::install_physics_system(scene);
    auto* wind = toy::weather::install_wind_system(scene);

    for (int i = 0; i < 60; ++i) { scene.update(1.0f / 60.0f); scene.late_update(1.0f / 60.0f); }
    auto vx = [&](RigidbodyComponent* rb) {
        const auto* b = phys->world().get_body(rb->body_id());
        return b ? b->linear_velocity.x : -1.0f;
    };
    expect(wind->pushed_bodies() == 2, "wind bodies: the two with an influence are pushed");
    expect(vx(light) > 3.0f && vx(light) < 8.0f, "wind bodies: a light body blows downwind, toward the wind's speed (" +
                                                      std::to_string(vx(light)) + ")");
    expect(vx(heavy) > 0.05f && vx(heavy) < vx(light) * 0.25f, "wind bodies: a heavy one barely (" + std::to_string(vx(heavy)) + ")");
    expect_near(vx(deaf), 0.0f, 1e-5f, "wind bodies: influence 0 ignores the wind");
    expect(flag->cloth() != nullptr, "wind cloth: the flag bound");
    if (flag->cloth()) {
        expect_near(flag->cloth()->params.wind.x, 8.0f, 1e-3f, "wind cloth: the weather's wind blows the flag");
        expect_near(flag->cloth()->params.wind.y, 1.0f, 1e-3f, "wind cloth: on top of its own authored wind");
    }
    expect_near(toy::weather::wind_drag_force(1.0f, glm::vec3(4, 0, 0), glm::vec3(4, 0, -9)).x, 0.0f, 1e-6f,
                "wind bodies: moving with the air, no push (falling does not count)");
}

COOPA_TEST(calm_weather_leaves_everything_as_authored) {
    using namespace toy::particles;
    PScene ps;
    auto* smoke = add_system(ps, "smoke", glm::vec3(0, 0, 2), [](ParticleSystem& s) {
        s.settings.shape.type = EmitShape::Point;
        s.settings.start_speed = 0.0f;
        s.settings.gravity = 0.0f;
        s.settings.rate = 0.0f;
        s.settings.start_lifetime = 100.0f;
        s.settings.wind_influence = 1.0f;
        s.settings.looping = false;
        s.settings.duration = 100.0f;
        s.settings.bursts.push_back({0.0f, Range(4.0f), 1, 1.0f, 1.0f});
    });
    toy::weather::install_weather_system(*ps.scene, windy_block(0.0f, 0.5f));
    start(ps);
    run(ps, 2.0f);
    expect(smoke->particle_count() == 4, "calm: the burst is alive");
    expect_near(glm::length(smoke->pool().vel[0]), 0.0f, 1e-6f, "calm: wind_influence pushes nothing in calm air");
}
