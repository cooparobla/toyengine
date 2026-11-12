/**
 * @file kinematic_controller_test.cpp
 * @brief KinematicController: input-driven motion at move_speed (clamped, not normalized
 *        diagonals), frame-rate independent smoothing, and -- the regression behind the cloth
 *        clipping bug -- its order-50 system runs BEFORE physics so the body sees this frame's pose.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>

#include <glm/glm.hpp>
#include <coopa/scene/scene.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/kinematic_controller.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("kinematic_controller");

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;

namespace {

/** @brief Builds a one-object scene with a KinematicController seeded at `seed_pos`. */
std::unique_ptr<Scene> make_controller_scene(toy::scene::KinematicController** out_kc,
                                             const glm::vec3& seed_pos) {
    auto scene = std::make_unique<Scene>("kinematic_controller_test");
    auto obj = std::make_unique<SceneObject>("ball");
    obj->add_component<TransformComponent>()->transform().set_position(seed_pos);
    *out_kc = obj->add_component<toy::scene::KinematicController>();
    scene->add_root_object(std::move(obj));
    // The controller moves nothing without this: its motion lives in advance(), driven here at
    // order 50 so it lands ahead of the physics phase. Installing it makes these tests exercise
    // the same path Engine uses (KinematicController's Behaviour-phase update() is a no-op).
    toy::scene::install_kinematic_control_system(*scene);
    return scene;
}

} // namespace

COOPA_TEST(moves_at_move_speed_and_holds_its_height) {
    toy::scene::KinematicController* kc = nullptr;
    auto scene = make_controller_scene(&kc, glm::vec3(0.0f, 0.0f, 2.6f));
    kc->move_speed = 4.0f;
    kc->smoothing = 0.0f; // instant, so the travelled distance is exactly speed * time
    scene->start();

    kc->move_input = glm::vec2(1.0f, 0.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);

    auto* tc = scene->root_objects()[0]->get_transform();
    const glm::vec3 p = tc->transform().position();
    expect_near(p.x, 4.0f, 0.05f, "kinematic_controller: +X input for 1 s travels move_speed metres");
    expect(std::fabs(p.y) < 1e-5f, "kinematic_controller: no Y drift from pure +X input");
    expect_near(p.z, 2.6f, 1e-5f, "kinematic_controller: holds the authored hover height");

    // Diagonal input is CLAMPED, not normalized: full deflection on both axes must not travel
    // faster than full deflection on one.
    kc->move_input = glm::vec2(1.0f, 1.0f);
    const glm::vec3 before = tc->transform().position();
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    const float diagonal = glm::length(tc->transform().position() - before);
    expect_near(diagonal, 4.0f, 0.05f,
                "kinematic_controller: diagonal input is clamped to move_speed, not sqrt(2) faster");
}

COOPA_TEST(smoothing_ramps_then_converges_and_decays) {
    toy::scene::KinematicController* kc = nullptr;
    auto scene = make_controller_scene(&kc, glm::vec3(0.0f));
    kc->move_speed = 4.0f;
    kc->smoothing = 8.0f;
    scene->start();

    kc->move_input = glm::vec2(1.0f, 0.0f);
    scene->update(1.0f / 60.0f);
    expect(kc->velocity().x > 0.0f && kc->velocity().x < 4.0f,
          "kinematic_controller: smoothing ramps velocity rather than snapping to move_speed");

    for (int i = 0; i < 300; ++i) scene->update(1.0f / 60.0f);
    expect_near(kc->velocity().x, 4.0f, 0.05f,
                "kinematic_controller: smoothed velocity converges to move_speed");

    // Releasing the key must decay back to rest, not stop dead -- PhysicsSystem derives the
    // kinematic body's velocity from this motion, so a discontinuity would jolt anything attached.
    kc->move_input = glm::vec2(0.0f);
    scene->update(1.0f / 60.0f);
    expect(kc->velocity().x > 0.0f && kc->velocity().x < 4.0f,
          "kinematic_controller: releasing input decays velocity instead of stopping instantly");
}

/**
 * @brief The regression test for the clipping bug: physics must see the pose written THIS frame.
 *
 * KinematicControlSystem runs at order 50 and PhysicsSystem at 100, so by the time Scene::update()
 * returns, the body's position must equal the Transform the controller just wrote. If the
 * controller ever drifts back to the Behaviour phase (200), physics spends each frame solving
 * against the PREVIOUS pose while the renderer draws the new one -- invisible for rigid contacts,
 * but it is exactly what made the ball clip through the cloth in cloth_demo.
 */
COOPA_TEST(control_runs_before_physics_in_the_same_frame) {
    using coopa::physx::components::SphereCollider;
    using coopa::physx::components::RigidbodyComponent;

    Scene scene("kinematic_order_test");
    auto obj = std::make_unique<SceneObject>("ball");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 2.0f));
    obj->add_component<SphereCollider>()->set_radius(1.0f);
    auto* rb = obj->add_component<RigidbodyComponent>();
    rb->is_kinematic = true;
    rb->use_gravity = false;
    auto* kc = obj->add_component<toy::scene::KinematicController>();
    kc->move_speed = 4.0f;
    kc->smoothing = 0.0f;
    SceneObject* ball = obj.get();
    scene.add_root_object(std::move(obj));

    scene.start();
    toy::scene::install_kinematic_control_system(scene);
    auto* phys = coopa::physx::system::install_physics_system(scene);

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 30; ++i) {
        kc->move_input = glm::vec2(1.0f, 0.0f); // re-assert; Engine would push this every frame
        scene.update(dt);
        scene.late_update(dt);
    }

    const float transform_x = ball->get_transform()->transform().position().x;
    const coopa::physx::dynamics::Body* body = phys->world().get_body(rb->body_id());
    expect(body != nullptr, "kinematic order: the ball bound to a physics body");
    expect(transform_x > 1.0f, "kinematic order: the controller actually moved the ball");
    if (body) {
        // One frame of lag would be move_speed * dt = 6.7 cm; require far tighter than that.
        expect_near(body->position.x, transform_x, 1e-4f,
                    "kinematic order: physics saw the pose written this frame, not the previous one");
    }
}
