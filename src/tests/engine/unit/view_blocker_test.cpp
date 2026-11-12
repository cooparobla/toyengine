/**
 * @file view_blocker_test.cpp
 * @brief ViewBlockerFade's dissolve curve: it reaches the target in the time asked for, comes
 *        back in its own time, never leaves [0, target], and quantizes to the step count that
 *        keeps the shadow caches from being rebuilt every frame.
 *
 * The curve is a pure function precisely so this can be pinned without a device or a physics
 * world -- the system that decides WHEN a wall is blocking is a separate concern from how fast
 * it then goes.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <memory>
#include <set>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <toyengine/scene/register.h>
#include <toyengine/scene/view_blocker.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("view_blocker");

using toy::scene::advance_blocker_cutoff;
using toy::scene::quantize_cutoff;
using toy::scene::ViewBlockerFade;

namespace {

/// Runs the curve for `seconds` at 60 Hz and returns where it ended up.
float run(float start, bool blocking, float target, float in_time, float out_time, float seconds) {
    const float dt = 1.0f / 60.0f;
    float v = start;
    for (int i = 0; i < static_cast<int>(std::round(seconds / dt)); ++i) {
        v = advance_blocker_cutoff(v, blocking, target, in_time, out_time, dt);
    }
    return v;
}

} // namespace

/** @brief A blocked wall reaches full dissolve in about fade_in_time, not sooner or much later. */
COOPA_TEST(a_blocked_wall_dissolves_in_the_time_asked_for) {
    const float half = run(0.0f, true, 1.0f, 0.2f, 0.4f, 0.1f);
    expect(half > 0.3f && half < 0.7f, "view_blocker: halfway through fade_in_time it is about half gone");

    const float done = run(0.0f, true, 1.0f, 0.2f, 0.4f, 0.25f);
    expect_near(done, 1.0f, 1e-3f, "view_blocker: past fade_in_time it is fully dissolved");
}

/**
 * @brief The dissolve ARRIVES rather than merely approaching.
 *
 * An exponential ease would asymptote: a wall at 0.99 still costs a draw and still casts a
 * ragged shadow, so "almost gone" is not a state this may rest in.
 */
COOPA_TEST(the_dissolve_reaches_its_endpoints_exactly) {
    expect_near(run(0.0f, true, 1.0f, 0.2f, 0.4f, 2.0f), 1.0f, 0.0f,
                "view_blocker: a long block ends exactly at the target");
    expect_near(run(1.0f, false, 1.0f, 0.2f, 0.4f, 2.0f), 0.0f, 0.0f,
                "view_blocker: a long clear ends exactly at solid");
}

/** @brief Unblocking restores the wall over fade_out_time, independently of fade_in_time. */
COOPA_TEST(an_unblocked_wall_restores_over_its_own_time) {
    // Same elapsed time, two different out-times: the slower one must be further from solid.
    const float fast = run(1.0f, false, 1.0f, 0.2f, 0.2f, 0.1f);
    const float slow = run(1.0f, false, 1.0f, 0.2f, 0.8f, 0.1f);
    expect(slow > fast, "view_blocker: a longer fade_out_time restores more slowly");
    expect(fast < 1.0f, "view_blocker: clearing moves it back toward solid");
}

/** @brief The level never leaves [0, fade_target], whichever way it is driven. */
COOPA_TEST(the_level_stays_within_zero_and_the_target) {
    expect_near(run(0.0f, true, 0.6f, 0.2f, 0.4f, 2.0f), 0.6f, 1e-6f,
                "view_blocker: a partial fade_target is the ceiling");
    // Driven from above its own ceiling (a target lowered at runtime), it must come down to it.
    const float clamped = advance_blocker_cutoff(0.9f, true, 0.6f, 0.2f, 0.4f, 1.0f / 60.0f);
    expect(clamped <= 0.6f + 1e-6f, "view_blocker: a level above the target is clamped to it");
    expect(clamped >= 0.0f, "view_blocker: never negative");
}

/**
 * @brief A zero (or negative) time snaps, rather than freezing.
 *
 * `fade_in_time: 0` has to mean "instantly"; dividing by it would otherwise produce an
 * infinite step or a NaN, and a NaN cutoff makes the whole wall vanish permanently.
 */
COOPA_TEST(a_zero_time_snaps_instead_of_producing_a_nan) {
    const float v = advance_blocker_cutoff(0.0f, true, 1.0f, 0.0f, 0.0f, 1.0f / 60.0f);
    expect_near(v, 1.0f, 1e-6f, "view_blocker: fade_in_time 0 snaps to the target");
    expect(std::isfinite(v), "view_blocker: and stays finite");

    const float back = advance_blocker_cutoff(1.0f, false, 1.0f, 0.0f, 0.0f, 1.0f / 60.0f);
    expect_near(back, 0.0f, 1e-6f, "view_blocker: fade_out_time 0 snaps back to solid");
}

/**
 * @brief A zero delta holds the level where it is.
 *
 * Deliberately NOT the same case as a zero fade time: a paused frame (or the editor sitting
 * still) would otherwise complete every wall's dissolve instantly the moment it was blocked.
 */
COOPA_TEST(a_zero_delta_holds_the_dissolve) {
    expect_near(advance_blocker_cutoff(0.4f, true, 1.0f, 0.2f, 0.4f, 0.0f), 0.4f, 1e-6f,
                "view_blocker: dt 0 leaves the level untouched");
    expect_near(advance_blocker_cutoff(0.4f, false, 1.0f, 0.2f, 0.4f, 0.0f), 0.4f, 1e-6f,
                "view_blocker: and that holds while clearing too");
}

/**
 * @brief Quantization yields at most `steps` + 1 distinct values over a whole sweep.
 *
 * This is the performance property, not a look: `alpha_cutoff` is part of the mesh batch key
 * and is hashed into the directional and local shadow caches, so one distinct value per frame
 * means re-rendering this caster's cached static shadow tiles every frame of the fade.
 */
COOPA_TEST(quantization_bounds_how_often_the_cutoff_changes) {
    std::set<float> seen;
    const float dt = 1.0f / 60.0f;
    float v = 0.0f;
    for (int i = 0; i < 240; ++i) {   // 4 s of blocking at 60 Hz: 240 frames
        v = advance_blocker_cutoff(v, true, 1.0f, 2.0f, 0.4f, dt);
        seen.insert(quantize_cutoff(v, 32));
    }
    expect(seen.size() <= 33, "view_blocker: 32 steps give at most 33 distinct values over a sweep");
    expect(seen.size() > 8, "view_blocker: and still enough of them to read as a smooth dissolve");

    // steps <= 1 is the documented escape hatch: a continuous value, cache cost accepted.
    expect_near(quantize_cutoff(0.123456f, 0), 0.123456f, 1e-6f, "view_blocker: 0 steps is continuous");
    expect_near(quantize_cutoff(0.123456f, 1), 0.123456f, 1e-6f, "view_blocker: 1 step is continuous too");
    expect_near(quantize_cutoff(0.51f, 2), 0.5f, 1e-6f, "view_blocker: 2 steps snap to halves");
}

/** @brief A fresh component is solid and not blocking -- the feature costs nothing until driven. */
COOPA_TEST(a_fresh_component_starts_solid) {
    ViewBlockerFade f;
    expect_near(f.cutoff(), 0.0f, 1e-6f, "view_blocker: starts fully solid");
    expect(!f.is_blocking(), "view_blocker: starts unblocked");
}

/** @brief Every key round-trips through the scene parser. */
COOPA_TEST(the_component_round_trips_through_scene_yaml) {
    toy::scene::register_scene_components();
    coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(std::string(
        "scene:\n"
        "  scene_name: view_blocker_yaml\n"
        "  root_objects:\n"
        "    - name: wall\n"
        "      components:\n"
        "        - { type: Transform }\n"
        "        - type: ViewBlockerFade\n"
        "          fade_target: 0.8\n"
        "          fade_in_time: 0.25\n"
        "          fade_out_time: 0.5\n"
        "          cutoff_steps: 16\n")), "view_blocker_test.yaml");

    auto* f = scene.find_first_component<ViewBlockerFade>();
    require(f != nullptr, "view_blocker: the scene parser built one");
    expect_near(f->fade_target, 0.8f, 1e-6f, "view_blocker: fade_target parsed");
    expect_near(f->fade_in_time, 0.25f, 1e-6f, "view_blocker: fade_in_time parsed");
    expect_near(f->fade_out_time, 0.5f, 1e-6f, "view_blocker: fade_out_time parsed");
    EXPECT_EQ(f->cutoff_steps, 16);
}
