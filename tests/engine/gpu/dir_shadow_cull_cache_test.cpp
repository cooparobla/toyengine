/**
 * @file dir_shadow_cull_cache_test.cpp
 * @brief The directional cascades' cached static caster lists are membership-exact: reusing a
 *        list must never change the image, only skip the cull that would have rebuilt it, and
 *        any change to the static scene must invalidate it.
 */

#include <coopa/testing/test.h>

#include <string>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <coopa/scene/scene_object.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("dir_shadow_cull_cache");

using namespace toy::test;

/**
 * @brief With a resting camera the cascade lists hit their cache every frame, and the image is
 *        identical to culling from scratch (the cache key covers everything membership depends
 *        on). Moving a caster -- which drops it from the static set -- invalidates the lists
 *        and the shadow follows it, cache on or off.
 */
COOPA_TEST(cached_cascade_lists_are_invisible_and_invalidate_on_change) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("tests/fixtures/scenes/kitchen_sink/scene.yaml", 640, 360, 320, 180);
    toy::core::Engine engine(std::move(config));
    engine.render_config().debug_view = "direct";

    // Warm up past kStaticCasterFrames so resting casters are static-eligible, then long
    // enough for every cascade list to have been built and reused at least once.
    tick_frames(engine, 3 * kNoiseCycle);
    expect(engine.pipeline().last_frame_stats().dir_list_cache_hits > 0,
           "cull cache: a resting frame reuses every cascade's static list");
    const Frame cached = engine.capture_image(true);

    engine.render_config().shadow_cull_cache_enabled = false;
    tick_frames(engine, kNoiseCycle);
    expect(engine.pipeline().last_frame_stats().dir_list_cache_hits == 0,
           "cull cache: disabled means no reuse");
    const Frame fresh = engine.capture_image(true);
    expect(same_extent(cached, fresh), "cull cache: captures share an extent");
    const long long diff_px = count_diff(cached, fresh, 2);
    expect(diff_px <= kDriftBudget,
           "cull cache: reused lists render the identical image (" + std::to_string(diff_px) +
               " px differ)");

    // Invalidation: nudging a caster re-renders its shadow identically with the cache on and
    // off -- a stale list would leave the shadow at the old pose on one side only.
    engine.render_config().shadow_cull_cache_enabled = true;
    auto* pillar = engine.scene().find_object("pillar");
    expect(pillar != nullptr, "cull cache: kitchen_sink has its pillar");
    if (!pillar) return;
    coopa::util::Transform& t = pillar->get_transform()->transform();
    t.set_position(t.position() + glm::vec3(1.5f, 0.0f, 0.0f));
    tick_frames(engine, 3 * kNoiseCycle);   // move, then rest back into static eligibility
    const Frame moved_cached = engine.capture_image(true);
    const long long moved_px = count_diff(cached, moved_cached, 2);
    expect_at_least(moved_px, 50, "cull cache: the moved box changes the image");

    engine.render_config().shadow_cull_cache_enabled = false;
    tick_frames(engine, kNoiseCycle);
    const Frame moved_fresh = engine.capture_image(true);
    const long long stale_px = count_diff(moved_cached, moved_fresh, 2);
    expect(stale_px <= kDriftBudget,
           "cull cache: after a move the cached and fresh images agree (" +
               std::to_string(stale_px) + " px differ)");
}
