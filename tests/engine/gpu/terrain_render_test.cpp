/**
 * @file terrain_render_test.cpp
 * @brief The half of the terrain system the unit suites cannot reach: generation on the job engine,
 *        GPU upload and chunk objects, streaming around a moving camera (with release hysteresis),
 *        the styled scene's styles and shader, and the focus shadow fit's view probe. Worlds are
 *        shrunk before the first tick so generation takes a second, not a minute.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/free_mover.h>
#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("terrain_render");

using namespace toy::test;

/**
 * @brief Generates a small world, builds its chunks, then flies the camera and watches the
 *        loaded region follow it.
 *
 * The scene is the shipped demo in its voxel read (voxel_terrain_demo()), but its world is shrunk
 * here before the first tick -- generation does not start until TerrainSystem's first
 * execute(), so the component is still unconfigured at this point. That keeps this test to a
 * 32-cell map and nine 8x8 chunks instead of the demo's ~10k cells and forty-nine 32x32 ones,
 * which is the difference between a second and most of a minute.
 *
 * Everything is reached through CameraComponent::main()->scene rather than through an Engine
 * accessor: the main camera is a registered singleton and every Component carries its Scene
 * back-pointer, so the test needs no new engine API to see what it is testing.
 *
 * What gets DRIVEN is the scene's `focus_marker`, not the camera. The camera orbits that marker
 * (`tracker: focus_marker`), so CameraController::update_orbit_() recomputes its pose from the
 * marker every frame and a write straight to the camera's Transform would simply be overwritten
 * the same frame. Driving the marker is also what a player does, so this exercises the real
 * control path rather than a back door into it. The orbit rig is collapsed to a short, unsmoothed
 * arm first, so "the camera's chunk" and "the marker's chunk" stay the same chunk and the
 * assertions below can name one coordinate.
 */
COOPA_TEST(chunks_stream_around_the_camera) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config(voxel_terrain_demo(), 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr, "terrain: the demo scene registers a main camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;

    coopa::scene::Scene& scene = *camera->scene;
    auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
    expect(terrain != nullptr, "terrain: the demo scene carries a Terrain component");
    if (terrain == nullptr) return;

    // Shrink the world. 32 cells x 2 tiles = 64 tiles per axis, in 8 chunks of 8 tiles.
    terrain->grid_size                  = 32;
    terrain->params.tiles_per_grid_unit = 2;
    terrain->params.chunk_size          = 8;
    terrain->params.view_radius         = 1;

    const float chunk_world = terrain->params.chunk_world_size();

    // The DOF focus target and the orbit pivot are the same object, and its name is what
    // resolve_dof_focus_() will look up every frame -- so resolve it exactly the way that
    // function does. A typo in either YAML key shows up here rather than as a silently
    // mis-focused frame nobody asserts on.
    expect(!camera->focus_object.empty(), "terrain: the camera names a DOF focus object");
    coopa::scene::SceneObject* marker = scene.find_object_by_path(camera->focus_object);
    expect(marker != nullptr, "terrain: the camera's focus_object resolves to a real object");
    if (marker == nullptr) return;
    expect(marker->get_component<toy::scene::FreeMover>() != nullptr,
           "terrain: the focus marker is driveable (carries a FreeMover)");
    // An invisible marker is the whole point: resolve_dof_focus_() falls back to the transform
    // origin precisely because there are no renderer bounds to centre on.
    expect(marker->get_component<coopa::gfx::engine::components::MeshRenderer>() == nullptr,
           "terrain: the focus marker draws nothing");

    auto* marker_transform = marker->get_transform();
    expect(marker_transform != nullptr, "terrain: the focus marker has a Transform to drive");
    if (marker_transform == nullptr) return;

    // Collapse the orbit arm so the camera rides along with the marker: the streamer centres on
    // the CAMERA, and the shipped 42-unit arm would put it several chunks away from the marker at
    // this test's 8-unit chunk size. Zeroing both smoothing rates makes the follow instant, so a
    // move converges in ticks rather than in however long an exponential chase takes.
    if (auto* controller = camera->owner->get_component<toy::scene::CameraController>()) {
        controller->distance           = 2.0f;
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
    }

    // Park the marker over chunk (4, 4), well inside the world on every side.
    const toy::world::ChunkCoord start{4, 4};
    marker_transform->transform().set_position(
        glm::vec3((static_cast<float>(start.x) + 0.5f) * chunk_world,
                  (static_cast<float>(start.y) + 0.5f) * chunk_world, 48.0f));

    // Generation runs on the job engine and takes a while; the budget is generous because what
    // is being asserted is "this converges", not "this converges in N frames".
    const int wanted = 9; // (2 * view_radius + 1)^2
    const int frames_to_ready =
        tick_until(engine, 600, [&] { return count_live_chunks(*terrain) >= wanted; });
    expect(count_live_chunks(*terrain) >= wanted,
           "terrain: every chunk in the view radius becomes live");
    expect(terrain->is_ready(), "terrain: the sampler and the side library both come up");
    if (coopa::test::verbose()) std::cout << "         converged after " << frames_to_ready << " frames\n";

    // Each live chunk is a real drawable: a child object with a loaded mesh on it.
    coopa::scene::SceneObject* terrain_object = scene.find_object("terrain");
    expect(terrain_object != nullptr, "terrain: the terrain object is in the scene");
    int drawable = 0;
    if (terrain_object != nullptr) {
        for (const auto& child : terrain_object->children()) {
            auto* renderer =
                child->get_component<coopa::gfx::engine::components::MeshRenderer>();
            if (renderer != nullptr && renderer->is_ready()) ++drawable;
        }
    }
    expect(drawable >= wanted, "terrain: every live chunk hung a ready MeshRenderer on the scene");

    // The chunk under the marker (and so under the camera riding beside it) must be one of them
    // -- a world that built only its fringe would still pass a bare count.
    expect(chunk_is_live(*terrain, start), "terrain: the chunk under the marker is live");

    // --- Streaming: fly the marker two chunks along +X and watch the region follow. ---
    const toy::world::ChunkCoord moved{start.x + 2, start.y};
    marker_transform->transform().set_position(
        glm::vec3((static_cast<float>(moved.x) + 0.5f) * chunk_world,
                  (static_cast<float>(moved.y) + 0.5f) * chunk_world, 48.0f));

    tick_until(engine, 600, [&] {
        return chunk_is_live(*terrain, toy::world::ChunkCoord{moved.x + 1, moved.y}) &&
               terrain->chunks().find(toy::world::ChunkCoord{start.x - 1, start.y}) ==
                   terrain->chunks().end();
    });

    expect(chunk_is_live(*terrain, toy::world::ChunkCoord{moved.x + 1, moved.y}),
           "terrain: ground ahead of the marker is built as it advances");
    expect(terrain->chunks().find(toy::world::ChunkCoord{start.x - 1, start.y}) ==
               terrain->chunks().end(),
           "terrain: ground left behind is released");
    // Hysteresis: the retirement threshold is view_radius + 1, so the chunk exactly two behind
    // is deliberately still loaded. Asserting it explicitly is what stops a future "tidy-up"
    // from dropping the margin and reintroducing boundary thrash.
    expect(terrain->chunks().find(start) != terrain->chunks().end(),
           "terrain: the chunk one past the view radius is kept, not thrashed");

    // Nothing leaked: the live set is still a bounded neighbourhood, not everything ever seen.
    const int live_after = count_live_chunks(*terrain);
    expect(live_after <= 25, "terrain: the live chunk count stays bounded while streaming");
    if (live_after > 25) std::cerr << "         " << live_after << " chunks still live\n";

    // --- And it reaches the screen. ---
    tick_frames(engine, kNoiseCycle);
    const Frame frame = engine.capture_image(/*low_res=*/true);
    expect(frame.width == 160 && frame.height == 90, "terrain: the capture has the render size");

    // The camera looks down at the ground from 48 units up, so the lower half of the frame
    // should be terrain, not sky. Sky here is the EnvironmentLight gradient -- strongly
    // blue-dominant -- which makes "blue beats both other channels by a clear margin" a
    // reliable test for it without pinning an exact colour.
    long long ground = 0;
    long long total = 0;
    for (uint32_t y = frame.height / 2; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * frame.width + x) * frame.channels;
            const int r = frame.pixels[i], g = frame.pixels[i + 1], b = frame.pixels[i + 2];
            if (!(b > r + 20 && b > g + 10)) ++ground;
            ++total;
        }
    }
    expect(total > 0 && ground * 2 > total,
           "terrain: the lower half of the frame is terrain rather than sky");
    if (total > 0 && ground * 2 <= total) {
        std::cerr << "         only " << ground << " of " << total << " lower-half pixels\n";
        dump_frame(frame, "terrain_stream");
    }
}

/**
 * @brief The styled scene end to end: assets/scenes/terrain/terrain_demo parses its styles, bakes
 *        every piece, streams chunks through the styled mesher, and puts terrain on screen.
 *
 * World shrunk before the first tick exactly as test_terrain_streams_chunks_around_the_camera
 * does; what this adds is the styled half -- that `styles:`/`kind_styles:` reach the library,
 * that every style resolved all nine of its piece files, and that styled chunks draw with the
 * `terrain_styled` surface shader.
 */
COOPA_TEST(styled_scene_streams_styled_chunks) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/terrain/terrain_demo/scene.yaml", 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr, "smooth terrain: the scene registers a main camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;
    coopa::scene::Scene& scene = *camera->scene;
    auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
    expect(terrain != nullptr, "smooth terrain: the scene carries a Terrain component");
    if (terrain == nullptr) return;

    expect(terrain->styles.size() == 4, "smooth terrain: all four styles parse");
    expect(terrain->kind_styles[static_cast<std::size_t>(toy::world::TileKind::Stone)] !=
               terrain->kind_styles[static_cast<std::size_t>(toy::world::TileKind::Grass)],
           "smooth terrain: stone and grass are shaped by different styles");

    terrain->grid_size                  = 32;
    terrain->params.tiles_per_grid_unit = 2;
    terrain->params.chunk_size          = 8;
    terrain->params.view_radius         = 1;
    if (auto* controller = camera->owner->get_component<toy::scene::CameraController>()) {
        controller->distance           = 12.0f;
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
    }
    coopa::scene::SceneObject* marker = scene.find_object_by_path(camera->focus_object);
    expect(marker != nullptr && marker->get_transform() != nullptr, "smooth terrain: the focus marker exists");
    if (marker == nullptr || marker->get_transform() == nullptr) return;
    const float chunk_world = terrain->params.chunk_world_size();
    marker->get_transform()->transform().set_position(glm::vec3(4.5f * chunk_world, 4.5f * chunk_world, 12.0f));

    tick_until(engine, 600, [&] { return count_live_chunks(*terrain) >= 9; });
    expect(count_live_chunks(*terrain) >= 9, "smooth terrain: every chunk in the view radius becomes live");
    expect(terrain->is_ready() && terrain->library().has_styles(),
           "smooth terrain: the library comes up styled");
    bool every_piece = terrain->library().style_count() == terrain->styles.size();
    for (std::uint8_t s = 0; s < terrain->library().style_count(); ++s) {
        for (std::size_t p = 0; p < toy::world::k_tile_piece_count; ++p) {
            every_piece = every_piece && terrain->library().has_piece(s, static_cast<toy::world::TilePiece>(p));
        }
    }
    expect(every_piece, "smooth terrain: every style resolved all eleven of its pieces");

    int drawable = 0, styled_shader = 0;
    if (coopa::scene::SceneObject* terrain_object = scene.find_object("terrain")) {
        for (const auto& child : terrain_object->children()) {
            auto* renderer = child->get_component<coopa::gfx::engine::components::MeshRenderer>();
            if (renderer == nullptr || !renderer->is_ready()) continue;
            ++drawable;
            if (renderer->material.shader == "terrain_styled") ++styled_shader;
        }
    }
    expect(drawable >= 9, "smooth terrain: every live chunk hung a ready MeshRenderer");
    expect(styled_shader == drawable, "smooth terrain: styled chunks draw with the terrain_styled shader");

    tick_frames(engine, kNoiseCycle);
    const Frame frame = engine.capture_image(/*low_res=*/true);
    long long ground = 0, total = 0;
    for (uint32_t y = frame.height / 2; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * frame.width + x) * frame.channels;
            const int r = frame.pixels[i], g = frame.pixels[i + 1], b = frame.pixels[i + 2];
            if (!(b > r + 20 && b > g + 10)) ++ground;
            ++total;
        }
    }
    expect(total > 0 && ground * 2 > total, "smooth terrain: the lower half of the frame is terrain rather than sky");
    if (total > 0 && ground * 2 <= total) dump_frame(frame, "terrain_smooth");
}

/**
 * @brief The "focus" shadow fit finds what the camera is looking at when the scene names no
 *        focus: the screen-centre G-buffer probe measures a real distance, and stays idle
 *        while the scene's own focus_object answers instead.
 */
COOPA_TEST(focus_shadow_probe_measures_the_viewed_surface) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config(voxel_terrain_demo(), 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));
    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr, "focus probe: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr) return;
    auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>();
    if (terrain == nullptr) return;
    terrain->grid_size                  = 32;
    terrain->params.tiles_per_grid_unit = 2;
    terrain->params.chunk_size          = 8;
    terrain->params.view_radius         = 2;
    expect(engine.pipeline().render_config_mut().shadow_fit == "focus",
           "focus probe: terrain_demo runs the focus shadow fit");

    // Park the orbit pivot over the middle of the shrunken 64-tile island, at about its plateau
    // height (the demo's height_scale is 10 m), so the camera frames terrain rather than the
    // empty sky past its edge.
    coopa::scene::SceneObject* marker = camera->scene->find_object_by_path(camera->focus_object);
    if (marker == nullptr || marker->get_transform() == nullptr) return;
    marker->get_transform()->transform().set_position(glm::vec3(32.0f, 32.0f, 6.0f));
    if (auto* controller = camera->owner->get_component<toy::scene::CameraController>()) {
        controller->distance           = 30.0f;
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
    }

    // With the scene's focus_object the probe has nothing to do.
    tick_until(engine, 600, [&] { return count_live_chunks(*terrain) >= 25; });
    tick_frames(engine, 8);
    expect(engine.pipeline().shadow_focus_probe_distance() == 0.0f,
           "focus probe: idle while the camera names its own focus_object");

    // Without one, it measures what the camera looks at: the ground around the orbit pivot.
    camera->focus_object.clear();
    tick_frames(engine, 30);
    const float probed = engine.pipeline().shadow_focus_probe_distance();
    expect(probed > 5.0f && probed < 150.0f, "focus probe: measures a real distance to the viewed surface");
    if (!(probed > 5.0f && probed < 150.0f)) std::cerr << "         probed " << probed << "\n";

}
