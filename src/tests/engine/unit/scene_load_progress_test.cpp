/**
 * @file scene_load_progress_test.cpp
 * @brief The loading screen's bar: fold_warmup_states(), load_bar_target() and advance_load_bar().
 *
 * What a bar has to do beyond being roughly right is never move backwards and never jump, and
 * neither of those is visible in a figure read one frame at a time -- which is why the three
 * steps behind it are free functions over SceneLoadState (scene_loading.h) rather than Engine
 * internals. The replay at the bottom drives them with the readings a streamed world actually
 * produces: systems that join the warm-up late, and systems whose own progress falls back when
 * they find more to do.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <string>
#include <vector>

#include <toyengine/core/scene_loading.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("scene_load_progress");

namespace {

using coopa::scene::WarmupState;
using toy::core::SceneLoadStage;
using toy::core::SceneTransition;
using toy::core::detail::SceneLoadState;

/** @brief One system's report, as Scene::warm_up_states() hands it over (name filled in). */
WarmupState report(const char* name, float progress, int remaining, bool warm) {
    WarmupState ws;
    ws.participates = true;
    ws.warm = warm;
    ws.progress = progress;
    ws.remaining = remaining;
    ws.what = name;
    ws.name = name;
    return ws;
}

/** @brief A state mid-warm-up, as Engine::begin_scene_warmup() leaves it. */
SceneLoadState warming_state() {
    SceneLoadState st;
    st.stage = SceneLoadStage::Warming;
    st.progress = 1.0f;
    st.warm_only = true;
    st.options.transition = SceneTransition::loading_screen_ui("ui/loading_screen");
    return st;
}

/// The bar's value over a run of frames, and what it did between them.
struct BarRun {
    std::vector<float> shown;
    float worst_step_back = 0.0f;   ///< Most it ever fell in one frame (0 = never fell).
    float largest_step = 0.0f;      ///< Most it ever moved in one frame.

    void push(float v) {
        if (!shown.empty()) {
            const float d = v - shown.back();
            worst_step_back = std::min(worst_step_back, d);
            largest_step = std::max(largest_step, std::abs(d));
        }
        shown.push_back(v);
    }
    float final_value() const { return shown.empty() ? 0.0f : shown.back(); }
};

/** @brief Folds `frames` of reports in order and eases the bar over them at `dt` per frame. */
BarRun replay(const std::vector<std::vector<WarmupState>>& frames, float dt = 1.0f / 60.0f) {
    SceneLoadState st = warming_state();
    BarRun run;
    float shown = 0.0f;
    for (const std::vector<WarmupState>& frame : frames) {
        // The engine's order: the bar is drawn from the previous frame's fold, then the fold runs.
        shown = toy::core::advance_load_bar(shown, toy::core::load_bar_target(st), dt);
        run.push(shown);
        toy::core::fold_warmup_states(st, frame);
    }
    return run;
}

}  // namespace

COOPA_TEST(a_system_that_joins_the_warm_up_late_does_not_rescale_the_bar) {
    SceneLoadState st = warming_state();

    // Terrain alone, most of the way through its chunks.
    toy::core::fold_warmup_states(st, {report("Terrain", 0.8f, 88, false)});
    const float terrain_only = st.warm_progress;
    expect(terrain_only > 0.7f && terrain_only < 0.9f,
           "one system: progress is that system's own");

    // Water joins with nothing done yet -- a flat mean would halve the figure here.
    toy::core::fold_warmup_states(st,
                                  {report("Terrain", 0.8f, 88, false), report("Water", 0.0f, 2, false)});
    expect(st.warm_progress >= terrain_only - 0.02f,
           "a system joining the warm-up does not pull the figure back");
    expect(st.warm_participants.size() == 2, "both systems are tracked");
}

COOPA_TEST(a_system_whose_own_progress_falls_back_does_not_pull_the_bar_back) {
    SceneLoadState st = warming_state();
    toy::core::fold_warmup_states(st, {report("Water", 1.0f, 0, true)});
    const float was = st.warm_progress;
    expect_near(was, 1.0f, 1e-5f, "one warm system: the figure is full");

    // More bodies come into range: the system un-warms and reports a worse fraction.
    toy::core::fold_warmup_states(st, {report("Water", 0.4f, 6, false)});
    expect(st.warm_progress >= was - 1e-5f, "a system's own fall-back is held at its high-water mark");
}

COOPA_TEST(small_late_work_cannot_swing_a_bar_a_large_system_owns) {
    SceneLoadState st = warming_state();
    // 400 chunks of terrain against 4 dirty navmesh tiles: shares of the bar, not halves of it.
    const std::vector<WarmupState> both = {report("Terrain", 0.5f, 200, false),
                                           report("Navigation", 1.0f, 4, true)};
    toy::core::fold_warmup_states(st, both);
    toy::core::fold_warmup_states(st, {report("Terrain", 0.5f, 400, false),
                                       report("Navigation", 1.0f, 4, true)});
    const float with_nav_warm = st.warm_progress;

    // The baker drops back to half with a tile to redo. Terrain's 400 outweighs its 4.
    toy::core::fold_warmup_states(st, {report("Terrain", 0.5f, 400, false),
                                       report("Navigation", 0.5f, 4, false)});
    expect(std::abs(st.warm_progress - with_nav_warm) < 0.05f,
           "a four-item system moves the bar by its share of the work, not by a half of it");
}

COOPA_TEST(nothing_streams_reads_as_warm_but_the_opening_frames_do_not) {
    SceneLoadState st = warming_state();
    expect(toy::core::fold_warmup_states(st, {}), "no participating system: nothing to wait for");

    WarmupState silent;   // participates == false: a system that streams nothing
    silent.name = "Behaviour";
    expect(toy::core::fold_warmup_states(st, {silent}), "a system that streams nothing is skipped");

    expect(!toy::core::fold_warmup_states(st, {report("Terrain", 0.0f, 0, false)}),
           "a system that has not found its work yet is not warm");
    expect(toy::core::fold_warmup_states(st, {report("Terrain", 1.0f, 0, true)}),
           "every participant warm: the scene is worth showing");
    expect_near(st.warm_progress, 1.0f, 1e-5f, "warm: the figure is exactly full");
}

COOPA_TEST(the_status_line_names_whichever_system_has_the_most_left) {
    SceneLoadState st = warming_state();
    toy::core::fold_warmup_states(st, {report("terrain", 0.1f, 300, false),
                                       report("water", 0.9f, 3, false)});
    expect(st.warm_status == "Building terrain...", "the long pole names the status line");
    toy::core::fold_warmup_states(st, {report("terrain", 0.99f, 2, false),
                                       report("water", 0.5f, 40, false)});
    expect(st.warm_status == "Building water...", "it moves on when the long pole does");
}

COOPA_TEST(the_bar_travels_one_way_at_a_bounded_speed) {
    const float dt = 1.0f / 60.0f;
    expect_near(toy::core::advance_load_bar(0.4f, 0.4f, dt), 0.4f, 1e-6f, "a still target holds");
    expect_near(toy::core::advance_load_bar(0.9f, 0.1f, dt), 0.9f, 1e-6f,
                "a target below the bar leaves it where it is");

    // A step change becomes travel rather than being drawn as it arrives.
    const float one_frame = toy::core::advance_load_bar(0.0f, 1.0f, dt);
    expect(one_frame > 0.0f && one_frame < 0.1f, "a jump in the target becomes a glide");

    // Same motion whatever the frame took: two half-frames land where one whole one does.
    const float split = toy::core::advance_load_bar(
        toy::core::advance_load_bar(0.0f, 1.0f, dt * 0.5f), 1.0f, dt * 0.5f);
    expect_near(split, one_frame, 1e-5f, "the speed is per second, not per frame");

    // A target that rises slower than the limit is tracked exactly, not trailed.
    float v = 0.0f;
    for (int i = 1; i <= 60; ++i) v = toy::core::advance_load_bar(v, static_cast<float>(i) / 120.0f, dt);
    expect_near(v, 0.5f, 1e-4f, "a target inside the speed limit is followed exactly");

    // And it closes on one outside it, rather than approaching it forever.
    v = 0.0f;
    for (int i = 0; i < 120; ++i) v = toy::core::advance_load_bar(v, 1.0f, dt);
    expect_near(v, 1.0f, 1e-4f, "the bar reaches a target that stops moving");
}

COOPA_TEST(the_warm_up_owns_the_whole_bar_only_when_no_load_stages_ran) {
    SceneLoadState st = warming_state();   // begin_scene_warmup(): the scene is already built
    st.warm_progress = 0.5f;
    expect_near(toy::core::load_bar_target(st), 0.5f, 1e-5f,
                "a scene that is only being warmed fills the bar with its warm-up");

    SceneLoadState loaded = warming_state();   // a load that read, built and warmed
    loaded.warm_only = false;
    loaded.warm_progress = 0.0f;
    const float handover = toy::core::load_bar_target(loaded);
    expect(handover > 0.0f && handover < 1.0f,
           "a full load hands the bar to the warm-up part-filled, not full");
    loaded.warm_progress = 1.0f;
    expect_near(toy::core::load_bar_target(loaded), 1.0f, 1e-5f, "and the warm-up fills the rest");

    // The load stages themselves must stop short of full when a warm-up is still to come.
    SceneLoadState building;
    building.stage = SceneLoadStage::LoadingAssets;
    building.options.transition = SceneTransition::loading_screen_ui("ui/loading_screen");
    building.options.warm_up = true;
    building.progress = 1.0f;
    expect_near(toy::core::load_bar_target(building), handover, 1e-5f,
                "the load stages fill the bar to exactly where the warm-up picks it up");
    building.options.warm_up = false;
    expect_near(toy::core::load_bar_target(building), 1.0f, 1e-5f,
                "with no warm-up to come, the load stages fill all of it");
}

COOPA_TEST(a_streamed_worlds_readings_draw_a_smooth_bar) {
    // The shape a real warm-up reports: terrain is the long pole and climbs steadily, water
    // flaps between warm and not as bodies come into range, and the navmesh baker halves
    // whenever a tile goes dirty. Fed straight to a bar, the last two are what made it jitter.
    std::vector<std::vector<WarmupState>> frames;
    frames.push_back({report("Navigation", 0.0f, 0, false)});               // nav alone, nothing found
    frames.push_back({report("Terrain", 0.0f, 441, false),                  // terrain joins...
                      report("Navigation", 1.0f, 0, true)});
    for (int i = 0; i < 80; ++i) {                                          // ...water too, late
        const float t = static_cast<float>(i) / 79.0f;
        const bool water_warm = (i % 7) < 4;                                // flapping
        const bool nav_warm = (i % 5) == 0;                                 // flapping
        frames.push_back({report("Terrain", t, 441 - static_cast<int>(t * 441.0f), t >= 1.0f),
                          report("Water", water_warm ? 1.0f : 0.5f, water_warm ? 0 : 8, water_warm),
                          report("Navigation", nav_warm ? 1.0f : 0.5f, nav_warm ? 0 : 4, nav_warm)});
    }
    frames.push_back({report("Terrain", 1.0f, 0, true), report("Water", 1.0f, 0, true),
                      report("Navigation", 1.0f, 0, true)});

    const BarRun run = replay(frames);
    expect(run.worst_step_back >= -1e-6f, "the bar never moves backwards");
    expect(run.largest_step < 0.1f, "and never jumps: no frame moves it a tenth of its width");
    expect(run.final_value() > 0.95f, "it is full, or near it, by the time the screen goes");
    expect(run.shown[run.shown.size() / 2] > 0.2f && run.shown[run.shown.size() / 2] < 0.9f,
           "and it is somewhere in the middle halfway through, not pinned at an end");
}
