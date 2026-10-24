/**
 * @file dir_shadow_clipmap_test.cpp
 * @brief The clipmap fit and its static-caster cache: caching must be INVISIBLE (the same
 *        image as drawing every caster live), must invalidate when the camera or the scene
 *        moves, and must stop re-rendering once the camera rests.
 *
 * `shadow_fit` and `shadow_cache_enabled` are startup-fixed (the cache image's existence
 * depends on them), so each configuration needs its own Engine rather than a live toggle --
 * see render_fixture.h's rules.
 */

#include <coopa/testing/test.h>

#include <string>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <coopa/scene/scene_object.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("dir_shadow_clipmap");

using namespace toy::test;

namespace {

/// A kitchen_sink engine on the clipmap fit, with the cache on or off. Rings are kept small
/// so the fixture's few metres of geometry actually span several levels.
toy::core::AppConfig clipmap_config(bool cache) {
    toy::core::AppConfig config =
        make_test_config("tests/fixtures/scenes/kitchen_sink/scene.yaml", 640, 360, 320, 180);
    config.render.shadow_fit             = "clipmap";
    config.render.shadow_cascades        = 4;
    config.render.shadow_map_resolution  = 1024;
    config.render.shadow_clip_radius     = 3.0f;
    config.render.shadow_clip_ratio      = 2.0f;
    config.render.shadow_cache_enabled   = cache;
    config.render.debug_view             = "direct";
    return config;
}

} // namespace

/**
 * @brief The cache changes the cost, never the picture: a cached frame and a fully live frame
 *        of the same resting scene must agree within the fixture's drift budget.
 *
 * The one failure this is really hunting is a stale level -- a tile whose depth was drawn with
 * one matrix while the shader samples it with another, which shears shadows away from their
 * casters. It would show here as a large difference that no amount of settling removes.
 */
COOPA_TEST(cached_static_depth_renders_the_same_image_as_drawing_it_live) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    Frame cached, live;
    {
        toy::core::Engine engine(clipmap_config(true));
        // Well past kStaticCasterFrames, and past the round-robin budget's reach, so every
        // level has had its static depth drawn and is now resting on the cache.
        tick_frames(engine, 6 * kNoiseCycle);
        cached = engine.capture_image(true);
        const auto& stats = engine.pipeline().last_frame_stats();
        expect(stats.dir_levels_rendered == 0,
               "clipmap cache: a resting frame re-renders no level's static depth (" +
                   std::to_string(stats.dir_levels_rendered) + ")");
    }
    {
        toy::core::Engine engine(clipmap_config(false));
        tick_frames(engine, 6 * kNoiseCycle);
        live = engine.capture_image(true);
    }
    expect(same_extent(cached, live), "clipmap cache: captures share an extent");
    const long long diff = count_diff(cached, live, 2);
    expect(diff <= kDriftBudget,
           "clipmap cache: cached and live static depth agree (" + std::to_string(diff) +
               " px differ)");
}

/**
 * @brief Moving the camera far enough to step the levels' snap cells invalidates their cached
 *        depth, and the settled result matches a pipeline that never cached at all.
 */
COOPA_TEST(camera_motion_invalidates_cached_levels) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    auto move_camera_and_capture = [](bool cache) {
        toy::core::Engine engine(clipmap_config(cache));
        tick_frames(engine, 4 * kNoiseCycle);
        auto* cam = engine.scene().find_object("camera");
        expect(cam != nullptr, "clipmap cache: kitchen_sink has its camera");
        if (!cam) return Frame{};
        coopa::util::Transform& t = cam->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(4.0f, 1.0f, 0.0f));
        // Long enough for every dirtied level to come back through the round-robin budget.
        tick_frames(engine, 8 * kNoiseCycle);
        return engine.capture_image(true);
    };

    const Frame cached = move_camera_and_capture(true);
    const Frame live   = move_camera_and_capture(false);
    expect(same_extent(cached, live), "clipmap cache: moved captures share an extent");
    const long long diff = count_diff(cached, live, 2);
    expect(diff <= kDriftBudget,
           "clipmap cache: after a camera move the cached image catches up to the live one (" +
               std::to_string(diff) + " px differ)");
}

/**
 * @brief A mover's shadow is drawn live over the copied static depth every frame, so it
 *        tracks the caster -- and leaves no trail of itself in the cached tiles.
 */
COOPA_TEST(movers_draw_over_cached_static_depth_without_ghosting) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(clipmap_config(true));
    tick_frames(engine, 6 * kNoiseCycle);
    const Frame before = engine.capture_image(true);

    auto* pillar = engine.scene().find_object("pillar");
    expect(pillar != nullptr, "clipmap cache: kitchen_sink has its pillar");
    if (!pillar) return;
    coopa::util::Transform& t = pillar->get_transform()->transform();
    const glm::vec3 home = t.position();

    t.set_position(home + glm::vec3(1.5f, 0.0f, 0.0f));
    tick_frames(engine, kNoiseCycle);
    const Frame moved = engine.capture_image(true);
    expect_at_least(count_diff(before, moved, 2), 50,
                    "clipmap cache: a mover's shadow follows it");

    // Back home and settled: the image must return to what it was. A ghost left behind in a
    // cached tile -- the copy failing to reset a tile the mover was drawn into -- would show
    // up here as a permanent difference.
    t.set_position(home);
    tick_frames(engine, 6 * kNoiseCycle);
    const Frame returned = engine.capture_image(true);
    const long long ghost = count_diff(before, returned, 2);
    expect(ghost <= kDriftBudget,
           "clipmap cache: a mover leaves no ghost in the cached tiles (" +
               std::to_string(ghost) + " px differ)");
}

/**
 * @brief A caster well past `shadow_distance` still casts a VISIBLE shadow.
 *
 * The clipmap's reach is its outermost ring, not `shadow_distance` -- but the distance fade is
 * a separate mechanism, and keying it to `shadow_distance` would render every outer ring and
 * then fade its shadows to nothing before they were sampled, which looks exactly like the fit
 * never reaching. So this moves a real caster out past that distance and asserts the image
 * changes when it starts casting: the only thing that can produce that difference is a shadow
 * from a level the fade did not erase.
 */
COOPA_TEST(a_caster_past_shadow_distance_still_casts_a_visible_shadow) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config = clipmap_config(true);
    config.render.shadow_clip_radius   = 12.0f;  // rings 12 / 24 / 48 / 96 m
    config.render.shadow_distance      = 8.0f;   // deliberately far shorter than the rings
    config.render.shadow_fade_fraction = 0.1f;   // fade on, so a mis-keyed fade would bite
    toy::core::Engine engine(std::move(config));

    // Pull the CAMERA back instead of pushing the caster away: that puts the whole fixture
    // tens of metres from the fade origin while keeping caster and receiver on screen. (Moving
    // the caster instead just walks it, and its shadow, out of frame.)
    auto* cam = engine.scene().find_object("camera");
    auto* pillar = engine.scene().find_object("pillar");
    expect(cam != nullptr && pillar != nullptr, "clipmap reach: kitchen_sink has its camera and pillar");
    if (!cam || !pillar) return;
    auto* mr = pillar->get_component<coopa::gfx::engine::components::MeshRenderer>();
    expect(mr != nullptr, "clipmap reach: the pillar has a MeshRenderer");
    if (!mr) return;

    coopa::util::Transform& ct = cam->get_transform()->transform();
    const glm::vec3 eye = ct.position();
    // Straight back from the origin it looks at, to ~30 m -- well past the 8 m shadow_distance.
    const glm::vec3 away = glm::normalize(eye) * 30.0f;
    ct.set_position(away);

    mr->cast_shadows = false;
    tick_frames(engine, 6 * kNoiseCycle);
    const Frame no_cast = engine.capture_image(true);

    mr->cast_shadows = true;
    tick_frames(engine, 6 * kNoiseCycle);
    const Frame casting = engine.capture_image(true);

    const long long px = count_diff(no_cast, casting, 2);
    expect_at_least(px, 10,
                    "clipmap reach: a caster ~30 m from the fade origin still shadows, past an "
                    "8 m shadow_distance (" + std::to_string(px) + " px)");
}
