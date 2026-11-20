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

/**
 * @brief The DRAWN SURFACE stays out of the ball, not merely the particles.
 *
 * The distinction is the whole point. Collision projects PARTICLES, which are points, so a
 * sheet whose every particle sits exactly at the standoff still draws triangles whose interiors
 * chord across a convex collider and dip inside it -- visible as the ball bleeding through the
 * cloth. Sampling each triangle's interior is the only way to see that from a test; a
 * particle-only check (the one above) passes straight through it.
 *
 * The bound is the standoff the solver promises minus the depth a chord may legitimately cut:
 * for a chord of length `c` on a sphere of radius `R` the midpoint sags `R - sqrt(R^2 - c^2/4)`
 * below the arc. Anything deeper than that is the solver failing to hold the surface out, not
 * geometry being discrete.
 */
COOPA_TEST(the_drawn_surface_never_enters_the_ball) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(
        make_test_config("assets/scenes/physics/cloth_demo/scene.yaml", 640, 360, 320, 180));
    tick_frames(engine, 2);

    auto* cc = engine.scene().find_first_component<coopa::physx::components::ClothComponent>();
    require(cc != nullptr, "cloth surface: the scene's Cloth parsed");
    auto* ball = engine.scene().find_object("ball");
    require(ball != nullptr, "cloth surface: the scene has its ball");
    constexpr float k_ball_radius = 1.0f;

    tick_frames(engine, 220);   // settled drape

    const coopa::physx::cloth::Cloth* sim = cc->cloth();
    require(sim != nullptr && !sim->triangles.empty(), "cloth surface: the sheet is simulating");
    const glm::vec3 centre(ball->get_transform()->transform().get_world_matrix()[3]);

    // Sample each triangle's interior on a barycentric lattice -- the corners are the particles
    // the solver already projected, so the interior is where a chord sag would hide.
    constexpr int k_steps = 4;   // 15 samples per triangle
    float worst_inside = 0.0f;   // metres the surface reaches inside the ball's own radius
    float worst_chord = 0.0f;
    for (std::size_t t = 0; t + 2 < sim->triangles.size(); t += 3) {
        const glm::vec3& a = sim->particles[sim->triangles[t]].position;
        const glm::vec3& b = sim->particles[sim->triangles[t + 1]].position;
        const glm::vec3& c = sim->particles[sim->triangles[t + 2]].position;
        worst_chord = std::max({worst_chord, glm::length(b - a), glm::length(c - b), glm::length(a - c)});
        for (int i = 0; i <= k_steps; ++i) {
            for (int j = 0; i + j <= k_steps; ++j) {
                const float u = static_cast<float>(i) / k_steps;
                const float v = static_cast<float>(j) / k_steps;
                const glm::vec3 p = a * (1.0f - u - v) + b * u + c * v;
                worst_inside = std::max(worst_inside, k_ball_radius - glm::length(p - centre));
            }
        }
    }

    // How far a chord of the sheet's own spacing may sag below the sphere's arc: unavoidable
    // with flat triangles, and what the standoff has to cover.
    const float sag = k_ball_radius - std::sqrt(std::max(0.0f, k_ball_radius * k_ball_radius -
                                                                   0.25f * worst_chord * worst_chord));
    std::cout << "         settled: worst surface depth " << worst_inside << " m, longest chord "
              << worst_chord << " m, chord sag " << sag << " m, thickness " << sim->params.thickness
              << " m\n";
    expect(worst_inside <= 0.0f,
           "cloth surface: no part of the drawn sheet is inside the settled ball (worst " +
               std::to_string(worst_inside) + " m in; chord " + std::to_string(worst_chord) +
               " m sags " + std::to_string(sag) + " m)");

    // --- The case a player actually sees: the ball DRIVEN under the draped sheet. The scene's
    // KinematicController re-reads input every tick, so drive the Transform directly (what the
    // moving-ball regression above does) and sample the surface against the pose each frame is
    // DRAWN at -- a collider the solver only sees one snapshot of per frame can pass through
    // cloth between frames.
    float worst_moving = 0.0f;
    float worst_moving_chord = 0.0f;
    for (int i = 0; i < 90; ++i) {
        coopa::util::Transform& t = ball->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(4.0f / 60.0f, 0.0f, 0.0f));
        tick_frames(engine, 1);
        const glm::vec3 drawn(ball->get_transform()->transform().get_world_matrix()[3]);
        const coopa::physx::cloth::Cloth* live = cc->cloth();
        if (live == nullptr) break;
        for (std::size_t t2 = 0; t2 + 2 < live->triangles.size(); t2 += 3) {
            const glm::vec3& a = live->particles[live->triangles[t2]].position;
            const glm::vec3& b = live->particles[live->triangles[t2 + 1]].position;
            const glm::vec3& c = live->particles[live->triangles[t2 + 2]].position;
            worst_moving_chord = std::max({worst_moving_chord, glm::length(b - a),
                                           glm::length(c - b), glm::length(a - c)});
            for (int x = 0; x <= k_steps; ++x) {
                for (int y = 0; x + y <= k_steps; ++y) {
                    const float u = static_cast<float>(x) / k_steps;
                    const float v = static_cast<float>(y) / k_steps;
                    const glm::vec3 p = a * (1.0f - u - v) + b * u + c * v;
                    worst_moving = std::max(worst_moving, k_ball_radius - glm::length(p - drawn));
                }
            }
        }
    }
    std::cout << "         moving: worst surface depth " << worst_moving << " m, longest chord "
              << worst_moving_chord << " m\n";
    expect(worst_moving <= 0.0f,
           "cloth surface: no part of the drawn sheet enters the MOVING ball (worst " +
               std::to_string(worst_moving) + " m in)");
}
