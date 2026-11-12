/**
 * @file kinematic_mover_test.cpp
 * @brief KinematicMover's scripted modes (moving platforms): PingPong is a sinusoid centred on the
 *        seed, Orbit holds its radius and height with no drift, Spin rotates in place.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>

#include <glm/glm.hpp>
#include <coopa/scene/scene.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/kinematic_mover.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("kinematic_mover");

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace {

/** @brief Builds a one-object scene with a KinematicMover seeded at `seed_pos`, un-started. */
std::unique_ptr<Scene> make_mover_scene(toy::scene::KinematicMover** out_km,
                                        const glm::vec3& seed_pos) {
    auto scene = std::make_unique<Scene>("kinematic_mover_test");
    auto obj = std::make_unique<SceneObject>("platform");
    obj->add_component<TransformComponent>()->transform().set_position(seed_pos);
    *out_km = obj->add_component<toy::scene::KinematicMover>();
    scene->add_root_object(std::move(obj));
    // Same reason as make_controller_scene(): the motion is in advance(), which only the
    // order-50 system calls. Without this the object never moves and every assertion below
    // would pass or fail for the wrong reason.
    toy::scene::install_kinematic_control_system(*scene);
    return scene;
}

} // namespace

/**
 * @brief PingPong oscillates about the seed position and comes back to it.
 *
 * The scripted counterpart to KinematicController, driven by the same order-50 system and used
 * by assets/scenes/physics/physics_demo/scene.yaml's moving platforms. The sinusoid is the point: a
 * linear back-and-forth would reverse instantaneously, and PhysicsSystem derives the body's
 * velocity from this Transform delta, so the discontinuity would kick anything standing on it.
 */
COOPA_TEST(pingpong_oscillates_symmetrically_about_the_seed) {
    toy::scene::KinematicMover* km = nullptr;
    const glm::vec3 seed(1.0f, 2.0f, 3.0f);
    auto scene = make_mover_scene(&km, seed);
    km->mode     = toy::scene::KinematicMoverMode::PingPong;
    km->axis     = glm::vec3(1.0f, 0.0f, 0.0f);
    km->distance = 4.0f;   // peak-to-peak, so +-2 about the seed
    km->speed    = 1.0f;   // 1 Hz: peak at t = 0.25 s, back to the seed at t = 0.5 s
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    const float dt = 1.0f / 240.0f;

    // Quarter period -> one full peak in +axis.
    for (int i = 0; i < 60; ++i) scene->update(dt);
    glm::vec3 peak = tc->transform().position();
    expect_near(peak.x, seed.x + 2.0f, 0.05f, "kinematic_mover: pingpong reaches +distance/2 at the quarter period");
    expect(std::fabs(peak.y - seed.y) < 1e-5f && std::fabs(peak.z - seed.z) < 1e-5f,
           "kinematic_mover: pingpong only moves along `axis`");

    // Half period -> back through the seed position.
    for (int i = 0; i < 60; ++i) scene->update(dt);
    expect_near(tc->transform().position().x, seed.x, 0.05f,
                "kinematic_mover: pingpong returns to the seed position at the half period");

    // Three quarters -> the opposite peak. Motion is centred on the seed, not offset from it.
    for (int i = 0; i < 60; ++i) scene->update(dt);
    expect_near(tc->transform().position().x, seed.x - 2.0f, 0.05f,
                "kinematic_mover: pingpong swings symmetrically to -distance/2");
}

/** @brief Orbit circles orbit_center at orbit_radius in XY, holding the seed's Z height. */
COOPA_TEST(orbit_holds_radius_and_height_without_drift) {
    toy::scene::KinematicMover* km = nullptr;
    auto scene = make_mover_scene(&km, glm::vec3(0.0f, 0.0f, 1.75f));
    km->mode         = toy::scene::KinematicMoverMode::Orbit;
    km->orbit_center = glm::vec3(2.0f, -1.0f, 0.0f);
    km->orbit_radius = 3.0f;
    km->speed        = 90.0f; // degrees/sec
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    const float dt = 1.0f / 120.0f;

    float worst_radius_error = 0.0f;
    float worst_height_error = 0.0f;
    for (int i = 0; i < 480; ++i) { // four seconds = one full revolution at 90 deg/s
        scene->update(dt);
        const glm::vec3 p = tc->transform().position();
        const float radius = glm::length(glm::vec2(p.x, p.y) - glm::vec2(km->orbit_center));
        worst_radius_error = std::max(worst_radius_error, std::fabs(radius - km->orbit_radius));
        worst_height_error = std::max(worst_height_error, std::fabs(p.z - 1.75f));
    }
    expect(worst_radius_error < 1e-3f,
           "kinematic_mover: orbit stays on orbit_radius for a whole revolution");
    expect(worst_height_error < 1e-5f,
           "kinematic_mover: orbit holds the seed transform's Z height (the engine is Z-up)");

    // A full revolution lands back where it started, rather than accumulating drift.
    const glm::vec3 after_one_revolution = tc->transform().position();
    for (int i = 0; i < 480; ++i) scene->update(dt);
    expect(glm::length(tc->transform().position() - after_one_revolution) < 1e-3f,
           "kinematic_mover: orbit is periodic, with no per-frame drift");
}

/** @brief Spin rotates in place: the rotation changes, the position does not. */
COOPA_TEST(spin_rotates_in_place) {
    toy::scene::KinematicMover* km = nullptr;
    const glm::vec3 seed(0.5f, -0.5f, 2.0f);
    auto scene = make_mover_scene(&km, seed);
    km->mode       = toy::scene::KinematicMoverMode::Spin;
    km->spin_axis  = glm::vec3(0.0f, 0.0f, 1.0f);
    km->spin_speed = 90.0f; // degrees/sec
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    const glm::quat seed_rotation = tc->transform().rotation_quat();

    for (int i = 0; i < 120; ++i) scene->update(1.0f / 120.0f); // one second = 90 degrees

    expect(glm::length(tc->transform().position() - seed) < 1e-6f,
           "kinematic_mover: spin never moves the object");

    // A 90-degree Z rotation takes local +X onto +Y.
    const glm::vec3 local_x = tc->transform().rotation_quat() * glm::vec3(1.0f, 0.0f, 0.0f);
    expect(glm::length(local_x - glm::vec3(0.0f, 1.0f, 0.0f)) < 0.02f,
           "kinematic_mover: spin_speed=90 turns the object a quarter turn in one second");
    expect(glm::length(tc->transform().rotation_quat() - seed_rotation) > 1e-3f,
           "kinematic_mover: spin actually changes the rotation");
}
