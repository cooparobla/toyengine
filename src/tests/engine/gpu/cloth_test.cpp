/**
 * @file cloth_test.cpp
 * @brief The cloth scene end to end: the sheet drapes and stays outside the ball, travels with the
 *        moving ball without ever entering it at its rendered pose (the clipping regression), and its
 *        dynamic vertex buffer reaches the screen.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <physxcoopa/components/cloth.h>
#include <toyengine/scene/cloth_renderer.h>
#include <toyengine/scene/kinematic_controller.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("cloth");

using namespace toy::test;

/**
 * @brief Full headless render of the cloth scene: the sheet must actually simulate (its
 * particles move and end up outside the ball) AND that simulation must reach the screen (two
 * frames far apart differ).
 *
 * The rendered-difference half is the part that matters most: everything else about cloth is
 * covered by physxcoopa's own headless suite, but nothing there can catch a broken dynamic
 * vertex buffer -- a mesh uploaded once and never again would still pass every physics
 * assertion while drawing a frozen flat sheet.
 *
 * Alone among the render tests, this one pins dt to a real 1/60 rather than 0: a drape is a
 * function of elapsed simulated time. Without the pin the Engine would run on wall-clock dt,
 * which headless is a few milliseconds -- so the fixed per-frame displacement below would
 * describe a ~22 m/s ball rather than the 4 m/s the scene is authored for, and most frames
 * would run no physics substep at all. Both make the drape unreproducible.
 */
COOPA_TEST(cloth_drapes_follows_the_ball_and_reaches_the_screen) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/physics/cloth_demo/scene.yaml", 640, 360, 320, 180);

    toy::core::Engine engine(std::move(config));

    tick_frames(engine, 4);
    const Frame early = engine.capture_image(/*low_res=*/true);

    auto* cc = engine.scene().find_first_component<coopa::physx::components::ClothComponent>();
    expect(cc != nullptr, "cloth scene: the Cloth component parsed from YAML");
    const coopa::physx::cloth::Cloth* sim = cc ? cc->cloth() : nullptr;
    expect(sim != nullptr, "cloth scene: PhysicsSystem bound a simulated cloth to it");
    expect(sim && sim->particles.size() == 25u * 25u, "cloth scene: resolution 25x25 round-trips from YAML");
    expect(sim && !sim->anchors.empty(), "cloth scene: the ball anchor resolved by name");

    auto* cr = engine.scene().find_first_component<toy::scene::ClothRenderer>();
    expect(cr != nullptr && cr->is_ready(),
          "cloth scene: ClothRenderer built and published its dynamic GPU mesh");

    const float start_min_z = sim ? sim->bounds.min.z : 0.0f;
    tick_frames(engine, 150);
    const Frame late = engine.capture_image(true);

    sim = cc ? cc->cloth() : nullptr;
    if (sim) {
        expect(sim->bounds.min.z < start_min_z - 0.5f,
              "cloth scene: the sheet drapes downward over the ball instead of staying flat");
        // The ball is a unit sphere at z = 2.6; no particle may be inside it.
        bool outside = true;
        for (const auto& p : sim->particles) {
            if (glm::length(p.position - glm::vec3(0.0f, 0.0f, 2.6f)) < 1.0f) { outside = false; break; }
        }
        expect(outside, "cloth scene: no particle ends up inside the ball's collider");
    }

    // Now the moving-ball half. The ball's Transform is written directly rather than through its
    // KinematicController: Engine::drive_kinematic_controllers_() re-reads the keyboard and
    // overwrites move_input at the top of every tick(), so a value poked in from outside can
    // never survive to Scene::update() -- and a headless test has no keyboard to press. The
    // input -> move_input -> Transform half is covered by the KinematicController tests in the
    // "scene" group; what only this test can cover is everything BELOW the Transform write:
    // Transform -> PhysicsSystem's derived kinematic velocity -> cloth anchors -> particles ->
    // the uploaded vertex buffer.
    expect(engine.scene().find_first_component<toy::scene::KinematicController>() != nullptr,
          "cloth scene: the ball has a KinematicController");
    auto* ball = engine.scene().find_object("ball");
    expect(ball != nullptr, "cloth scene: the ball object resolves by name");
    const glm::vec3 ball_before = ball ? ball->get_transform()->transform().position() : glm::vec3(0.0f);
    float cloth_x_before = 0.0f;
    if (sim) {
        for (const auto& p : sim->particles) cloth_x_before += p.position.x;
        cloth_x_before /= static_cast<float>(sim->particles.size());
    }

    // Worst penetration over EVERY moving frame, not just the final one: the clipping this
    // guards against is transient by nature (it appears while the ball travels and vanishes the
    // moment it stops), so sampling only the end state would miss it entirely.
    float worst_clearance = 1e9f;
    for (int i = 0; i < 60; ++i) {
        if (ball) {
            coopa::util::Transform& t = ball->get_transform()->transform();
            t.set_position(t.position() + glm::vec3(4.0f / 60.0f, 0.0f, 0.0f));
        }
        engine.tick();
        // Measured against the pose the ball was DRAWN at this frame -- the whole bug was that
        // this differs from the pose the cloth was solved against.
        const glm::vec3 drawn = ball ? ball->get_transform()->transform().position() : glm::vec3(0.0f);
        if (const auto* live = cc ? cc->cloth() : nullptr) {
            for (const auto& p : live->particles) {
                worst_clearance = std::min(worst_clearance, glm::length(p.position - drawn) - 1.0f);
            }
        }
    }
    expect(worst_clearance > 0.0f,
          "cloth scene: no particle ever enters the ball at the pose it is rendered at");
    if (worst_clearance <= 0.0f) {
        std::cerr << "         worst clearance was " << worst_clearance << " m\n";
    }

    const glm::vec3 ball_after = ball ? ball->get_transform()->transform().position() : glm::vec3(0.0f);
    expect(ball_after.x > ball_before.x + 0.5f, "cloth scene: the ball travels along +X");
    expect_near(ball_after.z, ball_before.z, 1e-4f,
                "cloth scene: the ball holds its hover height while moving");

    sim = cc ? cc->cloth() : nullptr;
    if (sim) {
        float cloth_x_after = 0.0f;
        for (const auto& p : sim->particles) cloth_x_after += p.position.x;
        cloth_x_after /= static_cast<float>(sim->particles.size());
        expect(cloth_x_after > cloth_x_before + 0.4f, "cloth scene: the sheet travels with the ball");
        expect(std::fabs(cloth_x_after - ball_after.x) < 1.0f,
              "cloth scene: the sheet trails the ball rather than being left behind");
        bool outside = true;
        for (const auto& p : sim->particles) {
            if (glm::length(p.position - ball_after) < 1.0f) { outside = false; break; }
        }
        expect(outside, "cloth scene: no particle penetrates the ball while it is moving");
    }

    // The rendered half: a dynamic vertex buffer that stopped being re-uploaded would draw the
    // same flat sheet in both captures and pass every assertion above.
    expect(same_extent(early, late), "cloth scene: both captures share one extent");
    if (same_extent(early, late)) {
        const long long pixels  = static_cast<long long>(early.width) * early.height;
        const long long changed = count_diff(early, late);
        expect_at_least(changed, pixels / 20,
                        "cloth scene: the dynamic vertex buffer reaches the screen (frames 4 and 154 differ substantially)");
        if (changed < pixels / 20) {
            dump_frame(early, "cloth_early");
            dump_frame(late, "cloth_late");
        }
    }
}
