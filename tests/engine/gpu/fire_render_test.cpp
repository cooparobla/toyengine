/**
 * @file fire_render_test.cpp
 * @brief fire_demo end to end: a click lights the chair it hits AT the point it hit, the front
 *        grows across that object alone, and reset() puts the scene back.
 *
 * Note there is no component here for a purely visual fire: the demo's campfire is a particle
 * prefab, so nothing in toyengine/fire/ is involved in drawing it.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <functional>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <toyengine/core/engine.h>
#include <toyengine/fire/fire.h>
#include <toyengine/fire/fire_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("fire_render");

using namespace toy::test;

namespace {

const char* k_fire_demo = "assets/scenes/effects/fire_demo/scene.yaml";

/** @brief Ticks until `pred` holds or `frames` pass. @return Frames actually ticked. */
int run_until(toy::core::Engine& engine, int frames, const std::function<bool()>& pred) {
    for (int i = 0; i < frames; ++i) {
        tick_frames(engine, 1);
        if (pred()) return i + 1;
    }
    return frames;
}

/** @brief Whether any MeshRenderer in `obj`'s subtree is flagged burning. */
bool any_material_burning(coopa::scene::SceneObject* obj) {
    if (!obj) return false;
    if (auto* mr = obj->get_component<coopa::gfx::engine::components::MeshRenderer>()) {
        if (mr->material.burning) return true;
    }
    for (auto& c : obj->children()) {
        if (any_material_burning(c.get())) return true;
    }
    return false;
}

} // namespace

COOPA_TEST(a_click_lights_the_chair_it_hits_at_the_point_it_hits) {
    // The headline. A ray at the chair's BACK must light that chair, and the burn must start at
    // the back rather than at the object's origin -- which is the whole difference between
    // "this object is on fire" and "it caught where you touched it".
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(k_fire_demo, 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);

    toy::fire::FireSystem* fs = toy::fire::find_fire(engine.scene());
    coopa::scene::SceneObject* a = engine.scene().find_object("chair_a");
    coopa::scene::SceneObject* b = engine.scene().find_object("chair_b");
    require(fs != nullptr && a != nullptr && b != nullptr, "fire: system and chairs are present");
    toy::fire::Burnable* ba = a->get_component<toy::fire::Burnable>();
    toy::fire::Burnable* bb = b->get_component<toy::fire::Burnable>();
    require(ba != nullptr && bb != nullptr, "fire: both chairs are Burnable");
    expect(!ba->burning() && !bb->burning(), "fire: nothing is alight to begin with");

    // Straight down onto chair_a's seat, from well above it.
    const glm::vec3 seat(-1.4f, 0.0f, 0.45f);
    toy::fire::Burnable* hit = fs->ignite_ray(engine.scene(), seat + glm::vec3(0.0f, 0.0f, 6.0f),
                                              glm::vec3(0.0f, 0.0f, -1.0f));
    require(hit == ba, "fire: the ray lit the chair it pointed at");
    expect(ba->burning(), "fire: and that chair is alight");
    expect(!bb->burning(),
           "fire: the OTHER chair is untouched -- fire is per object here, and nothing spreads "
           "between them");

    const glm::vec3 origin = ba->origin_world();
    expect(std::fabs(origin.z - 0.45f) < 0.35f,
           "fire: it caught near the seat, where the ray hit -- not at the object's origin on "
           "the floor");
    expect(glm::length(glm::vec2(origin) - glm::vec2(seat)) < 0.6f,
           "fire: and under the ray in plan");
}

COOPA_TEST(the_front_grows_across_the_object_and_reaches_its_materials) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(k_fire_demo, 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);

    toy::fire::FireSystem* fs = toy::fire::find_fire(engine.scene());
    coopa::scene::SceneObject* a = engine.scene().find_object("chair_a");
    require(fs != nullptr && a != nullptr, "fire: system and chair are present");
    toy::fire::Burnable* ba = a->get_component<toy::fire::Burnable>();
    require(ba != nullptr, "fire: the chair is Burnable");

    fs->ignite_ray(engine.scene(), glm::vec3(-1.4f, 0.0f, 6.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    tick_frames(engine, 10);
    const float early = ba->front();
    expect(ba->burning(), "fire: it is alight");
    expect(fs->burning_count() > 0, "fire: the system counts it");

    // The burn flag and the front must reach the materials -- that is the only route the
    // burn-front shader has, so a front that never lands there draws nothing at all.
    expect(any_material_burning(a),
           "fire: the chair's materials are flagged burning, which is what switches the "
           "burn-front layer on");

    tick_frames(engine, 120);
    expect(ba->front() > early, "fire: the front travelled outward");
    expect(ba->front() > 0.1f, "fire: far enough to cover part of the chair");
    expect(ba->progress() > 0.0f && ba->progress() < 1.0f, "fire: and it is partway through");

    // It burns out on its own.
    run_until(engine, 2400, [&] { return ba->spent(); });
    expect(ba->spent(), "fire: it was consumed once burn_time was up");
    expect(!ba->burning(), "fire: and is no longer alight");
}

COOPA_TEST(reset_puts_every_chair_back) {
    // Fire consumes world state -- a burnt chair stays burnt -- so without this the scene is
    // spent after one run, and so is any test that wants to run it twice.
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(k_fire_demo, 640, 360, 640, 360);
    config.render.transparency_enabled = true;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);

    toy::fire::FireSystem* fs = toy::fire::find_fire(engine.scene());
    coopa::scene::SceneObject* a = engine.scene().find_object("chair_a");
    require(fs != nullptr && a != nullptr, "fire: system and chair are present");
    toy::fire::Burnable* ba = a->get_component<toy::fire::Burnable>();

    fs->ignite_ray(engine.scene(), glm::vec3(-1.4f, 0.0f, 6.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    tick_frames(engine, 60);
    require(ba->burning() && ba->front() > 0.0f, "fire: it is burning before the reset");

    fs->reset(engine.scene());
    tick_frames(engine, 2);
    expect(!ba->burning(), "fire: reset put it out");
    expect(!ba->spent(), "fire: and it is not spent");
    expect(ba->front() <= 0.0f, "fire: the front is back to nothing");
    expect(!any_material_burning(a),
           "fire: and the materials are no longer flagged, so the chair draws unburnt again");

    // ...and it can be lit a second time, exactly as it was the first.
    fs->ignite_ray(engine.scene(), glm::vec3(-1.4f, 0.0f, 6.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    tick_frames(engine, 30);
    expect(ba->burning(), "fire: it lights again");
}

COOPA_TEST(a_ray_that_hits_nothing_burnable_lights_nothing) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(k_fire_demo, 640, 360, 640, 360);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 30);

    toy::fire::FireSystem* fs = toy::fire::find_fire(engine.scene());
    require(fs != nullptr, "fire: the system is installed");

    // Straight up, into the sky.
    toy::fire::Burnable* hit = fs->ignite_ray(engine.scene(), glm::vec3(0.0f, 0.0f, 1.0f),
                                              glm::vec3(0.0f, 0.0f, 1.0f));
    expect(hit == nullptr, "fire: a ray into the sky lights nothing");
    for (toy::fire::Burnable* b : engine.scene().get_components<toy::fire::Burnable>()) {
        expect(!b->burning(), "fire: and no chair caught");
    }
}
