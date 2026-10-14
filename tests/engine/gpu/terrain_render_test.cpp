/**
 * @file terrain_render_test.cpp
 * @brief The half of the terrain system the unit suites cannot reach: generation on the job engine,
 *        GPU upload and chunk objects, streaming around a moving camera (with release hysteresis),
 *        the demo's tile sets and shader, chunk colliders matching the drawn surface, editing the
 *        stored tiles re-meshing only the chunks they touch, and the focus shadow fit's view probe. Worlds are shrunk before the first tick so generation
 *        takes a second, not a minute.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/free_mover.h>
#include <physxcoopa/components/mesh_collider.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>

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
    terrain->generator.grid_size        = 32;
    terrain->generator.shape.shape      = coopa::maps::MapShape::Circle;   // a centred island, as these frame it
    terrain->generator.shape.diameter_m = 0.0;                             // ...fitted to the map
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
        // Looking straight down at the marker: the streamer centres on what the camera looks at,
        // and the demo's own (shallow) orbit pitch would aim that chunks past the marker.
        controller->pitch_deg          = controller->max_pitch_deg;
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

    const toy::world::ChunkCoord behind{start.x - 1, start.y};
    tick_until(engine, 600, [&] {
        return chunk_is_live(*terrain, toy::world::ChunkCoord{moved.x + 1, moved.y}) &&
               !chunk_is_live(*terrain, behind);
    });

    expect(chunk_is_live(*terrain, toy::world::ChunkCoord{moved.x + 1, moved.y}),
           "terrain: ground ahead of the marker is built as it advances");
    // Released means off the scene tree, not necessarily destroyed: a chunk the camera may come
    // straight back to is parked whole instead (TerrainChunk::parked, `dormant_chunks`), so it
    // can stay in the chunk table while drawing nothing.
    expect(!chunk_is_live(*terrain, behind), "terrain: ground left behind stops drawing");
    const auto left = terrain->chunks().find(behind);
    expect(left == terrain->chunks().end() || left->second.object == nullptr,
           "terrain: ...and is off the scene tree, parked or gone");
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
 * does; what this adds is the tile-set half -- that `tile_set:`/`kind_tile_sets:` reach the
 * library, that every set resolved all eleven of its pieces, and that chunks draw with the
 * `terrain_styled` surface shader.
 */
COOPA_TEST(styled_scene_streams_styled_chunks) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::world::TileMapStore::instance().clear();
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

    expect(terrain->styles.size() >= 2, "smooth terrain: the main tile set and its per-kind sets parse (" +
                                            std::to_string(terrain->styles.size()) + " distinct)");
    expect(terrain->kind_styles[static_cast<std::size_t>(toy::world::TileKind::Stone)] !=
               terrain->kind_styles[static_cast<std::size_t>(toy::world::TileKind::Grass)],
           "smooth terrain: stone and grass are shaped by different styles");

    terrain->generator.grid_size        = 32;
    terrain->generator.shape.shape      = coopa::maps::MapShape::Circle;   // a centred island, as these frame it
    terrain->generator.shape.diameter_m = 0.0;                             // ...fitted to the map
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
    // The demo loads its stored tiles (the whole island): look at the island's middle.
    (void)chunk_world;
    const float mid = 0.5f * static_cast<float>(terrain->tiles ? terrain->tiles->width() : 64) * terrain->params.tile_size;
    marker->get_transform()->transform().set_position(glm::vec3(mid, mid, 12.0f));

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
    terrain->generator.grid_size        = 32;
    terrain->generator.shape.shape      = coopa::maps::MapShape::Circle;   // a centred island, as these frame it
    terrain->generator.shape.diameter_m = 0.0;                             // ...fitted to the map
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

/**
 * @brief Every live chunk carries a static collider of the surface it draws: a physics ray
 *        straight down lands exactly on the column top the sampler reports (the voxel set's tops
 *        are flat at `steps * height_step`, so there is no rounding to allow for).
 */
COOPA_TEST(chunks_collide_with_what_they_draw) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config(voxel_terrain_demo(), 320, 180, 160, 90));
    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    require(camera != nullptr && camera->scene != nullptr, "terrain collision: the scene has a main camera");
    coopa::scene::Scene& scene = *camera->scene;
    auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
    require(terrain != nullptr, "terrain collision: the scene carries a TileTerrain");
    const glm::vec3 centre = shrink_terrain_and_centre(*terrain, scene);

    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
    tick_frames(engine, 4);   // the physics system picks the new colliders up on its next refresh

    int live = 0, collidable = 0;
    for (const auto& [coord, chunk] : terrain->chunks()) {
        if (chunk.state != toy::world::ChunkState::Live || chunk.object == nullptr) continue;
        ++live;
        if (chunk.object->get_component<coopa::physx::components::MeshCollider>() != nullptr) ++collidable;
    }
    expect(live >= 25 && collidable == live, "terrain collision: every drawn chunk has a collider (" +
           std::to_string(collidable) + " of " + std::to_string(live) + ")");

    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
    require(physics != nullptr, "terrain collision: the scene has a physics system");
    int hits = 0, exact = 0;
    for (int dy = -12; dy <= 12; dy += 4) {
        for (int dx = -12; dx <= 12; dx += 4) {
            const int tx = static_cast<int>(centre.x / terrain->params.tile_size) + dx;
            const int ty = static_cast<int>(centre.y / terrain->params.tile_size) + dy;
            const toy::world::TileColumn col = terrain->tiles->get(tx, ty).column();
            coopa::physx::geometry::Ray ray;
            ray.origin = glm::vec3((static_cast<float>(tx) + 0.5f) * terrain->params.tile_size,
                                   (static_cast<float>(ty) + 0.5f) * terrain->params.tile_size, 200.0f);
            ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
            ray.max_distance = 400.0f;
            coopa::physx::system::PhysicsSystem::RaycastHit hit;
            if (!physics->raycast(ray, hit)) continue;
            ++hits;
            if (std::abs(hit.point.z - static_cast<float>(col.steps) * terrain->params.height_step) < 0.01f) ++exact;
        }
    }
    expect(hits == 49, "terrain collision: every ray down over the loaded area hits the terrain (" +
           std::to_string(hits) + " of 49)");
    expect(exact == hits, "terrain collision: ...exactly on the column top it draws (" + std::to_string(exact) + " of " +
           std::to_string(hits) + ")");
}

/**
 * @brief Editing the stored tiles re-meshes exactly the chunks they touch -- the rest keep their
 *        meshes -- and the chunk's collider follows the new surface.
 */
COOPA_TEST(editing_tiles_remeshes_only_their_chunks) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config(voxel_terrain_demo(), 320, 180, 160, 90));
    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    require(camera != nullptr && camera->scene != nullptr, "tile edit: the scene has a main camera");
    coopa::scene::Scene& scene = *camera->scene;
    auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
    require(terrain != nullptr, "tile edit: the scene carries a TileTerrain");
    const glm::vec3 centre = shrink_terrain_and_centre(*terrain, scene);
    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
    require(terrain->tiles != nullptr && !terrain->tiles->empty(), "tile edit: the world baked into stored tiles");
    if (coopa::test::verbose()) std::cout << "    tiles " << terrain->tiles->width() << " target chunk " << terrain->params.chunk_at(centre).x << "," << terrain->params.chunk_at(centre).y << "\n";

    // A 3 x 3 patch in the middle of the camera's chunk, clear of its borders.
    const int size = terrain->params.chunk_size;
    const toy::world::ChunkCoord target = terrain->params.chunk_at(centre);
    const int tx = target.x * size + size / 2, ty = target.y * size + size / 2;
    std::map<std::pair<int, int>, std::uint32_t> serials;
    for (const auto& [coord, chunk] : terrain->chunks()) serials[{coord.x, coord.y}] = chunk.mesh_serial;
    const int raised = terrain->tiles->at(tx, ty).steps + 5;
    for (int y = ty - 1; y <= ty + 1; ++y)
        for (int x = tx - 1; x <= tx + 1; ++x) terrain->tiles->at(x, y).steps = static_cast<std::int16_t>(raised);
    terrain->tiles->mark_dirty(toy::world::IRect{tx - 1, ty - 1, tx + 2, ty + 2});

    tick_until(engine, 120, [&] {
        auto it = terrain->chunks().find(target);
        return it != terrain->chunks().end() && it->second.state == toy::world::ChunkState::Live &&
               it->second.mesh_serial != serials[{target.x, target.y}];
    });
    tick_frames(engine, 4);
    int rebuilt = 0, other_rebuilt = 0;
    for (const auto& [coord, chunk] : terrain->chunks()) {
        const auto it = serials.find({coord.x, coord.y});
        if (it == serials.end() || it->second == chunk.mesh_serial) continue;
        if (coord.x == target.x && coord.y == target.y) ++rebuilt; else ++other_rebuilt;
        if (coopa::test::verbose()) std::cout << "    rebuilt " << coord.x << "," << coord.y << " serial " << it->second << " -> " << chunk.mesh_serial << "\n";
    }
    expect(rebuilt == 1, "tile edit: the edited chunk re-meshed");
    expect(other_rebuilt == 0, "tile edit: ...and no other chunk did (" + std::to_string(other_rebuilt) + ")");

    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
    require(physics != nullptr, "tile edit: the scene has physics");
    coopa::physx::geometry::Ray ray;
    ray.origin = glm::vec3((tx + 0.5f) * terrain->params.tile_size, (ty + 0.5f) * terrain->params.tile_size, 300.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    ray.max_distance = 600.0f;
    coopa::physx::system::PhysicsSystem::RaycastHit hit;
    const bool found = physics->raycast(ray, hit);
    expect(found && std::abs(hit.point.z - raised * terrain->params.height_step) < 0.01f,
           "tile edit: the collider has the raised surface (" + std::to_string(found ? hit.point.z : -1.0f) + ")");

    // Regenerate: the world is baked again from the generator settings, edits and all.
    const std::uint64_t bakes = terrain->tiles->bakes_done();
    terrain->tiles->request_bake();
    tick_until(engine, 600, [&] { return terrain->tiles->bakes_done() > bakes; });
    expect(terrain->tiles->bakes_done() > bakes, "regenerate: the world re-bakes on request");
    expect(terrain->tiles->at(tx, ty).steps == raised - 5, "regenerate: the edit is replaced by the generated tile");
}


/**
 * @brief The water over a baked world: it never stands above the dry ground beside it, chunks
 *        draw it as WaterBody surfaces over sand beds (not as blocks), and a buoyant crate dropped
 *        into the sea floats at the surface rather than resting on it or sinking to the bed.
 */
COOPA_TEST(water_lies_over_the_beds_and_floats_a_crate) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config(voxel_terrain_demo(), 320, 180, 160, 90));
    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    require(camera != nullptr && camera->scene != nullptr, "water: the scene has a main camera");
    coopa::scene::Scene& scene = *camera->scene;
    auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
    require(terrain != nullptr, "water: the scene carries a TileTerrain");
    const glm::vec3 centre = shrink_terrain_and_centre(*terrain, scene);
    tick_until(engine, 900, [&] { return terrain->tiles && !terrain->tiles->empty() && !terrain->baking(); });
    require(terrain->tiles != nullptr && !terrain->tiles->empty(), "water: the world baked");
    const toy::world::TileMapData& tiles = *terrain->tiles;
    const toy::world::TerrainParams& p = terrain->params;

    // The invariant the bake keeps: no dry tile beside water lies under that water's terrace.
    int wet = 0, over = 0;
    glm::ivec2 sea{-1};
    float best = 1e9f;
    for (int y = 0; y < tiles.height(); ++y) {
        for (int x = 0; x < tiles.width(); ++x) {
            const toy::world::TileRecord& r = tiles.at(x, y);
            if (!r.water()) continue;
            ++wet;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (tiles.contains(x + dx, y + dy) && !tiles.at(x + dx, y + dy).water() &&
                        tiles.at(x + dx, y + dy).steps < r.water_level())
                        ++over;
            // Deep sea nearest the island's middle: where the crate goes.
            const float d = glm::length(glm::vec2(x + 0.5f, y + 0.5f) * p.tile_size - glm::vec2(centre));
            if (r.has(toy::world::TileRecord::Ocean) && r.water_depth >= 2 && d < best) {
                // Still water all round, so the crate cannot ground on a bank and the tile's corners
                // are the sea's alone (a river mouth's corners blend toward the river's surface).
                bool open = true;
                for (int dy = -1; dy <= 1 && open; ++dy)
                    for (int dx = -1; dx <= 1 && open; ++dx)
                        open = tiles.contains(x + dx, y + dy) && tiles.at(x + dx, y + dy).water() &&
                               !tiles.at(x + dx, y + dy).has(toy::world::TileRecord::Flowing);
                if (open) { best = d; sea = {x, y}; }
            }
        }
    }
    expect(wet > 0, "water: the world has water");
    expect(over == 0, "water: no dry tile lies under the water beside it (" + std::to_string(over) + ")");
    if (sea.x < 0 && coopa::test::verbose()) {
        int deepest = 0, ocean = 0, hist[8] = {0};
        for (const auto& t : tiles.records()) {
            deepest = std::max<int>(deepest, t.water_depth);
            ocean += t.has(toy::world::TileRecord::Ocean) ? 1 : 0;
            hist[std::min<int>(7, t.water_depth)]++;
        }
        std::cout << "    deepest water " << deepest << " steps, ocean " << ocean << ", depth histogram";
        for (int h : hist) std::cout << " " << h;
        std::cout << "\n";
    }
    require(sea.x >= 0, "water: there is open sea to drop the crate in");

    const toy::world::TileRecord& r = tiles.at(sea.x, sea.y);
    const float surface = (static_cast<float>(r.water_level()) - p.water_inset) * p.height_step;
    const float bed = static_cast<float>(r.steps) * p.height_step;
    const glm::vec3 above((sea.x + 0.5f) * p.tile_size, (sea.y + 0.5f) * p.tile_size, surface);
    if (auto* marker = scene.find_object("focus_marker")) marker->get_transform()->transform().set_position(above);
    const toy::world::ChunkCoord chunk = p.chunk_at(above);
    tick_until(engine, 900, [&] { return chunk_is_live(*terrain, chunk); });
    tick_frames(engine, 8);

    // The sea is a WaterBody over the chunk's beds, not blocks in its mesh.
    const auto it = terrain->chunks().find(chunk);
    require(it != terrain->chunks().end() && it->second.object != nullptr, "water: the sea's chunk is live");
    toy::water::WaterBody* sea_body = nullptr;
    for (const auto& child : it->second.object->children()) {
        if (auto* body = child->get_component<toy::water::WaterBody>(); body && body->mode == toy::water::WaterMode::Planar)
            sea_body = body;
    }
    require(sea_body != nullptr, "water: the chunk carries a still WaterBody");
    // Over the open sea (the chosen tile's own corners). Elsewhere in the chunk the still body also
    // carries skirts under its banks, which stand at the highest water beside them -- a river's.
    const glm::vec2 tile0 = glm::vec2(sea.x, sea.y) * p.tile_size - glm::vec2(chunk.x, chunk.y) * p.chunk_world_size();
    float lo = 1e9f, hi = -1e9f;
    for (const glm::vec3& v : sea_body->geometry_positions) {
        const glm::vec2 d = glm::vec2(v) - tile0;
        if (d.x < -0.01f || d.y < -0.01f || d.x > p.tile_size + 0.01f || d.y > p.tile_size + 0.01f) continue;
        lo = std::min(lo, v.z);
        hi = std::max(hi, v.z);
    }
    expect(std::abs(lo - surface) < 0.01f && std::abs(hi - surface) < 0.01f,
           "water: the sea is one flat surface at its level (" + std::to_string(lo) + ".." + std::to_string(hi) + ")");

    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
    require(physics != nullptr, "water: the scene has physics");
    coopa::physx::geometry::Ray ray;
    ray.origin = glm::vec3(above.x, above.y, surface + 20.0f);
    ray.direction = glm::vec3(0.0f, 0.0f, -1.0f);
    ray.max_distance = 100.0f;
    coopa::physx::system::PhysicsSystem::RaycastHit hit;
    expect(physics->raycast(ray, hit) && std::abs(hit.point.z - bed) < 0.05f,
           "water: things collide with the bed, not the water (" + std::to_string(hit.point.z) + ")");

    // A buoyant crate, dropped in from above, floats.
    auto crate = std::make_unique<coopa::scene::SceneObject>("float_crate");
    crate->add_component<coopa::scene::TransformComponent>()->transform().set_position(above + glm::vec3(0.0f, 0.0f, 2.0f));
    crate->add_component<coopa::physx::components::BoxCollider>()->set_size(glm::vec3(0.8f));
    crate->add_component<coopa::physx::components::RigidbodyComponent>()->mass = 150.0f;
    crate->add_component<toy::water::Buoyancy>();
    coopa::scene::SceneObject* raw = crate.get();
    scene.add_root_object(std::move(crate));
    scene.adopt(*raw);
    physics->refresh();
    tick_frames(engine, 420);
    const float z = raw->get_transform()->transform().get_world_matrix()[3].z;
    expect(z > bed + 0.6f && std::abs(z - surface) < 0.6f,
           "water: the crate floats at the surface (z " + std::to_string(z) + ", surface " + std::to_string(surface) +
               ", bed " + std::to_string(bed) + ")");
}
