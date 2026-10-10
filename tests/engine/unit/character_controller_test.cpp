/**
 * @file character_controller_test.cpp
 * @brief CharacterController on real colliders (support/character_rig.h): stairs vs a too-tall
 *        step, steep slopes, ramps, moving platforms and lifts, pushing crates, jump height and
 *        buffering, and the no-penetration invariant under random input.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <string>

#include <glm/glm.hpp>
#include <toyengine/scene/kinematic_mover.h>

#include "engine/support/checks.h"
#include "engine/support/character_rig.h"

COOPA_TEST_SUITE("character_controller");

using namespace toy::test;

/**
 * @brief Stairs with 0.3 m risers (under step_height 0.35) are walked up to the top; a single
 *        0.5 m block stops the character at its face, on the ground.
 */
COOPA_TEST(climbs_stairs_but_not_a_tall_step) {
    {
        CharacterRig rig;
        character_ground(rig);
        for (int i = 0; i < 5; ++i) {
            const float h = 0.3f * (i + 1);
            rig.box("stair", glm::vec3(0.0f, 2.0f + 0.5f * i + 0.25f, 0.5f * h), glm::vec3(2.0f, 0.5f, h));
        }
        rig.box("deck", glm::vec3(0.0f, 2.0f + 2.5f + 2.0f, 0.75f), glm::vec3(2.0f, 4.0f, 1.5f));
        rig.spawn(glm::vec3(0.0f));
        rig.start();
        rig.step(30);
        expect(rig.cc->is_grounded(), "stairs: grounded at the start");
        rig.step(100, glm::vec2(0.0f, 1.0f)); // ~3.5 m of stairs at up to 4 m/s, stopping on the 4 m deck
        expect(rig.feet().y > 5.0f, "stairs: walked to the deck (y = " + std::to_string(rig.feet().y) + ")");
        expect_near(rig.feet().z, 1.5f + rig.cc->skin, 0.02f, "stairs: standing on the deck at 1.5 m");
        expect(rig.cc->is_grounded(), "stairs: grounded on the deck");
    }
    {
        CharacterRig rig;
        character_ground(rig);
        rig.box("tall_step", glm::vec3(0.0f, 3.0f, 0.25f), glm::vec3(2.0f, 2.0f, 0.5f));
        rig.spawn(glm::vec3(0.0f));
        rig.start();
        rig.step(150, glm::vec2(0.0f, 1.0f));
        expect_near(rig.feet().y, 2.0f - rig.cc->radius - rig.cc->skin, 0.02f, "tall step: stopped at its face");
        expect_near(rig.feet().z, rig.cc->skin, 0.01f, "tall step: still on the ground");
        expect(rig.cc->is_grounded(), "tall step: grounded");
    }
}

/**
 * @brief A 60 degree slope (over slope_limit 45): a character dropped onto it is never grounded
 *        on it and slides down to the floor; walking into it gains no more than a step's height.
 */
COOPA_TEST(slides_off_a_steep_slope) {
    CharacterRig rig;
    character_ground(rig);
    // Slab rising toward +Y at 60 degrees; its top surface passes through (0, 0, 0) at y = 0.
    const float a = glm::radians(60.0f);
    rig.box("steep", glm::vec3(0.0f, 2.5f * std::cos(a) + 0.5f * std::sin(a), 2.5f * std::sin(a) - 0.5f * std::cos(a)),
            glm::vec3(3.0f, 5.0f, 1.0f), glm::vec3(60.0f, 0.0f, 0.0f));
    // Over the slope at y = 2 (surface z = 3.46), clear of it.
    rig.spawn(glm::vec3(0.0f, 2.0f, 4.0f));
    rig.start();
    bool grounded_on_slope = false;
    for (int i = 0; i < 20; ++i) {
        rig.step(1);
        if (rig.cc->is_grounded() && rig.feet().z > 0.5f) grounded_on_slope = true;
    }
    rig.step(160);
    expect(!grounded_on_slope, "steep slope: never grounded on it");
    expect(rig.feet().y < 0.0f, "steep slope: slid down off it (y = " + std::to_string(rig.feet().y) + ")");
    expect_near(rig.feet().z, rig.cc->skin, 0.02f, "steep slope: ended on the floor");
    expect(rig.cc->is_grounded(), "steep slope: grounded on the floor");

    // Walking into it from the floor stays at floor level (plus at most a step onto the toe).
    rig.step(120, glm::vec2(0.0f, 1.0f));
    expect(rig.feet().z < rig.cc->step_height + 0.05f, "steep slope: can't be walked up (z = " + std::to_string(rig.feet().z) + ")");
}

/**
 * @brief Walking down a 25 degree ramp, ground snap keeps the character grounded on every frame
 *        (no hopping) until it reaches the floor.
 */
COOPA_TEST(stays_grounded_walking_down_a_ramp) {
    CharacterRig rig;
    character_ground(rig);
    // A deck at 2 m with a 25-degree ramp down toward +Y from its edge at y = 2.
    rig.box("deck", glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(3.0f, 4.0f, 2.0f));
    const float a = glm::radians(25.0f);
    const float len = 2.0f / std::sin(a);
    // Rotation -25 about X: rises toward -Y. Its top passes through (y = 2, z = 2).
    rig.box("ramp", glm::vec3(0.0f, 2.0f + 0.5f * len * std::cos(a) - 0.1f * std::sin(a), 2.0f - 0.5f * len * std::sin(a) - 0.1f * std::cos(a)),
            glm::vec3(3.0f, len, 0.2f), glm::vec3(-25.0f, 0.0f, 0.0f));
    rig.spawn(glm::vec3(0.0f, 0.0f, 2.0f));
    rig.cc->move_speed = 5.0f;
    rig.start();
    rig.step(30);
    expect(rig.cc->is_grounded(), "ramp: grounded on the deck");
    int airborne = 0;
    for (int i = 0; i < 150 && rig.feet().y < 2.0f + len * std::cos(a) + 1.0f; ++i) {
        rig.step(1, glm::vec2(0.0f, 1.0f));
        if (!rig.cc->is_grounded()) ++airborne;
    }
    expect(rig.feet().y > 2.0f + len * std::cos(a), "ramp: walked off the bottom");
    expect(airborne == 0, "ramp: grounded every frame walking down (" + std::to_string(airborne) + " airborne frames)");
}

/**
 * @brief A kinematic platform moving along X at 1.5 m/s carries a character standing still on
 *        it: the character keeps its offset from the platform centre and stays grounded.
 */
COOPA_TEST(rides_moving_platforms_and_lifts) {
    CharacterRig rig;
    character_ground(rig);
    SceneObject* deck = rig.box("platform", glm::vec3(0.0f, 0.0f, 0.15f), glm::vec3(3.0f, 3.0f, 0.3f), glm::vec3(0.0f), 1);
    auto* mover = deck->add_component<toy::scene::KinematicMover>();
    mover->mode = toy::scene::KinematicMoverMode::PingPong;
    mover->axis = glm::vec3(1.0f, 0.0f, 0.0f);
    mover->distance = 6.0f;
    mover->speed = 0.08f; // peak speed = pi * distance * speed ~ 1.5 m/s
    rig.spawn(glm::vec3(0.5f, 0.0f, 0.35f));
    rig.start();
    rig.step(20);
    expect(rig.cc->is_grounded(), "platform: grounded on it");
    const float offset0 = rig.feet().x - deck->get_transform()->transform().position().x;
    float worst = 0.0f;
    bool always_grounded = true;
    for (int i = 0; i < 240; ++i) {
        rig.step(1);
        worst = std::max(worst, std::abs(rig.feet().x - deck->get_transform()->transform().position().x - offset0));
        always_grounded = always_grounded && rig.cc->is_grounded();
    }
    expect(std::abs(deck->get_transform()->transform().position().x) > 1.0f, "platform: the platform moved");
    expect(worst < 0.01f, "platform: rode along (worst drift " + std::to_string(worst) + " m)");
    expect(always_grounded, "platform: stayed grounded while riding");
    expect(rig.penetration() < 0.01f, "platform: not sunk into it");

    // A lift: up and down 2 m at up to ~1 m/s. Physics still holds last frame's lift pose when
    // the character moves, so allow a frame of travel (~1.7 cm) between feet and deck top.
    CharacterRig lift_rig;
    character_ground(lift_rig);
    SceneObject* lift = lift_rig.box("lift", glm::vec3(0.0f, 0.0f, 1.5f), glm::vec3(3.0f, 3.0f, 0.3f), glm::vec3(0.0f), 1);
    auto* lift_mover = lift->add_component<toy::scene::KinematicMover>();
    lift_mover->axis = glm::vec3(0.0f, 0.0f, 1.0f);
    lift_mover->distance = 2.0f;
    lift_mover->speed = 0.15f;
    lift_rig.spawn(glm::vec3(0.0f, 0.0f, 1.7f));
    lift_rig.start();
    lift_rig.step(10);
    float worst_gap = 0.0f;
    bool lift_grounded = true;
    for (int i = 0; i < 400; ++i) {
        lift_rig.step(1);
        const float top = lift->get_transform()->transform().position().z + 0.15f;
        worst_gap = std::max(worst_gap, std::abs(lift_rig.feet().z - top - lift_rig.cc->skin));
        lift_grounded = lift_grounded && lift_rig.cc->is_grounded();
    }
    expect(worst_gap < 0.03f, "lift: feet follow the deck up and down (worst gap " + std::to_string(worst_gap) + " m)");
    expect(lift_grounded, "lift: stayed grounded riding up and down");
}

/** @brief Walking into a 20 kg crate pushes it along; the character follows it. */
COOPA_TEST(pushes_dynamic_crates_only_when_enabled) {
    CharacterRig rig;
    character_ground(rig);
    SceneObject* crate = rig.box("crate", glm::vec3(0.0f, 2.0f, 0.4f), glm::vec3(0.8f), glm::vec3(0.0f), 2, 20.0f);
    rig.spawn(glm::vec3(0.0f));
    rig.start();
    rig.step(30);
    const float y0 = crate->get_transform()->transform().position().y;
    rig.step(180, glm::vec2(0.0f, 1.0f));
    const float y1 = crate->get_transform()->transform().position().y;
    expect(y1 - y0 > 2.0f, "crate: pushed " + std::to_string(y1 - y0) + " m");
    expect(rig.feet().y > 2.0f, "crate: the character followed it");
    expect(rig.penetration() < 0.03f, "crate: the character isn't inside it");

    // ...and with pushing off, the crate is just a wall.
    CharacterRig still;
    character_ground(still);
    SceneObject* crate2 = still.box("crate", glm::vec3(0.0f, 2.0f, 0.4f), glm::vec3(0.8f), glm::vec3(0.0f), 2, 20.0f);
    still.spawn(glm::vec3(0.0f));
    still.cc->push_dynamic_bodies = false;
    still.start();
    still.step(30);
    const float z0 = crate2->get_transform()->transform().position().y;
    still.step(120, glm::vec2(0.0f, 1.0f));
    expect(crate2->get_transform()->transform().position().y - z0 < 0.3f, "crate: not pushed with push_dynamic_bodies off");
}

/**
 * @brief A jump from flat ground peaks at jump_height (velocity-Verlet integration is exact for
 *        constant gravity), fires on_jumped once, and lands with on_landed reporting the
 *        take-off speed; a press 0.1 s before landing (jump_buffer) jumps again on touch-down.
 */
COOPA_TEST(jump_peaks_at_jump_height_and_buffers_early_presses) {
    CharacterRig rig;
    character_ground(rig);
    rig.spawn(glm::vec3(0.0f));
    rig.cc->jump_height = 1.5f;
    rig.start();
    int jumps = 0;
    float landed_speed = -1.0f;
    rig.cc->on_jumped.connect([&] { ++jumps; });
    rig.cc->on_landed.connect([&](float v) { landed_speed = v; });
    rig.step(20);
    const float z0 = rig.feet().z;
    rig.cc->jump = true;
    float apex = z0;
    int frames = 0;
    do {
        rig.step(1);
        apex = std::max(apex, rig.feet().z);
        ++frames;
    } while (!rig.cc->is_grounded() && frames < 200);
    expect(jumps == 1, "jump: on_jumped fired once");
    expect_near(apex - z0, 1.5f, 0.02f, "jump: apex at jump_height");
    expect(rig.cc->is_grounded() && frames < 200, "jump: landed");
    expect_near(landed_speed, std::sqrt(2.0f * 9.81f * 1.5f), 0.4f, "jump: on_landed reports the impact speed");

    // Buffered: press while still falling, a few frames before touch-down.
    rig.cc->jump = true;
    rig.step(1);
    expect(jumps == 2, "jump: grounded press jumps");
    int f = 0;
    while (rig.feet().z > z0 + 0.15f || rig.cc->velocity().z > 0.0f) { rig.step(1); if (++f > 200) break; }
    rig.cc->jump = true; // ~0.15 m above the ground, falling: inside the buffer window
    rig.step(30);
    expect(jumps == 3, "jump: a press just before landing is buffered into a jump");
}

/**
 * @brief 600 frames of random walking, sprinting and jumping across stairs, a tall block, a
 *        steep slope, a walkable ramp and a wall corner: the capsule never ends a frame
 *        overlapping anything.
 */
COOPA_TEST(never_penetrates_under_random_input) {
    CharacterRig rig;
    character_ground(rig);
    for (int i = 0; i < 4; ++i) {
        const float h = 0.25f * (i + 1);
        rig.box("stair", glm::vec3(-3.0f, 1.5f + 0.4f * i, 0.5f * h), glm::vec3(2.0f, 0.4f, h));
    }
    rig.box("block", glm::vec3(3.0f, 2.0f, 0.4f), glm::vec3(1.5f, 1.5f, 0.8f));
    rig.box("steep", glm::vec3(0.0f, 4.5f, 0.8f), glm::vec3(3.0f, 3.0f, 0.4f), glm::vec3(55.0f, 0.0f, 0.0f));
    rig.box("ramp", glm::vec3(-4.0f, -3.0f, 0.4f), glm::vec3(3.0f, 4.0f, 0.2f), glm::vec3(-20.0f, 0.0f, 0.0f));
    rig.box("wall_a", glm::vec3(5.0f, -2.0f, 1.0f), glm::vec3(0.4f, 6.0f, 2.0f));
    rig.box("wall_b", glm::vec3(2.0f, -5.0f, 1.0f), glm::vec3(6.0f, 0.4f, 2.0f), glm::vec3(0.0f, 0.0f, 20.0f));
    rig.box("ceiling", glm::vec3(0.0f, -2.5f, 2.4f), glm::vec3(2.0f, 2.0f, 0.2f));
    rig.spawn(glm::vec3(0.0f));
    rig.start();

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    glm::vec2 move(0.0f);
    float worst = 0.0f;
    for (int i = 0; i < 600; ++i) {
        if (i % 20 == 0) move = glm::vec2(u(rng), u(rng));
        rig.cc->sprint = u(rng) > 0.3f;
        if (u(rng) > 0.93f) rig.cc->jump = true;
        rig.cc->move_basis_yaw_deg = 30.0f * u(rng);
        rig.step(1, move);
        worst = std::max(worst, rig.penetration());
        const glm::vec3 p = rig.feet();
        if (std::abs(p.x) > 8.0f || std::abs(p.y) > 8.0f) {  // keep it in the obstacle course
            rig.cc->teleport(glm::vec3(0.0f));
        }
    }
    expect(worst < 0.005f, "random input: never penetrating (worst " + std::to_string(worst) + " m)");
    expect(rig.feet().z > -0.01f, "random input: never fell through the floor");
}

/**
 * @brief A root-motion character in the air moves physically: dropped against the foot of a
 *        55 degree slope -- the V between it and the floor, where the ground probe finds only the
 *        steep face -- it slides out and lands, though its (airborne) clip hands it no travel.
 *        Following root motion in the air left it wedged there, ungrounded forever.
 */
COOPA_TEST(root_motion_character_slides_off_a_steep_slopes_foot) {
    for (float y : {4.1f, 4.3f, 4.6f}) {
        CharacterRig rig;
        character_ground(rig);
        // character_demo's steep ramp: a 0.2 m slab at 55 degrees, its foot at y = 4.
        rig.box("ramp_steep", glm::vec3(2.0f, 5.22907f, 1.58095f), glm::vec3(2.4f, 4.0f, 0.2f), glm::vec3(55.0f, 0.0f, 0.0f));
        rig.spawn(glm::vec3(2.0f, y, 0.6f));
        rig.cc->use_root_motion = true;   // and no Animator: the jump clip's travel, i.e. none
        rig.start();
        rig.step(60);
        expect(rig.cc->is_grounded(), "root motion: dropped at y " + std::to_string(y) + " against the steep foot, it lands (pos " +
                                          std::to_string(rig.feet().y) + ", " + std::to_string(rig.feet().z) + ")");
        expect(rig.feet().z < 0.05f, "root motion: ...on the floor, not perched on the slope");
    }
}
