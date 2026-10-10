/**
 * @file engine_host_test.cpp
 * @brief The Engine embedding surface the editor builds on: scene settings apply (and rebuild the
 *        renderer) per scene, edit mode freezes simulation yet shows water, push/remove scenes leave
 *        the edit scene untouched, display regions move the image and viewport rays, and the Engine
 *        owns audio without opening a sound card.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <limits>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/yaml/document.h>
#include <physxcoopa/system/physics_system.h>
#include <toyengine/water/water_body.h>
#include <toyengine/water/water_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("engine_host");

using namespace toy::test;

namespace {

/** @brief Local position of a named root object (its world position, being a root), or NaN if missing. */
glm::vec3 object_position(coopa::scene::Scene& scene, const std::string& name) {
    coopa::scene::SceneObject* obj = scene.find_object(name);
    if (!obj || !obj->get_transform()) return glm::vec3(std::numeric_limits<float>::quiet_NaN());
    return obj->get_transform()->transform().position();
}

} // namespace

/**
 * @brief A scene file's `scene.settings` reach the running engine: render overrides apply live
 *        when it loads, physics overrides configure its physics, and loading a scene without
 *        overrides puts the project's settings back.
 */
COOPA_TEST(scene_settings_apply_per_scene_and_rebuild_the_renderer) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    const std::filesystem::path dir = coopa::test::scratch_dir("scene_settings");
    toy::core::Engine engine(make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 320, 180, 160, 90));
    tick_frames(engine, 2);
    // A switch fixed at pipeline construction: the scene flips it, so the renderer rebuilds.
    const bool project_ssr = engine.pipeline().render_config().ssr_enabled;
    const int rebuilds = engine.pipeline_rebuild_count();
    {
        std::ofstream out(dir / "scene.yaml");
        out << "format: blender\n"
               "scene:\n"
               "  scene_name: settings_test\n"
               "  settings:\n"
               "    render: { exposure: 2.5, fog_density: 0.09, ssr_enabled: " << (project_ssr ? "false" : "true") << " }\n"
               "    physics: { gravity: { x: 0.0, y: 0.0, z: -2.0 } }\n"
               "  root_objects:\n"
               "    - name: camera\n"
               "      components:\n"
               "        - type: Transform\n"
               "          position: { x: 0.0, y: -6.0, z: 3.0 }\n"
               "        - type: Camera\n"
               "          main: true\n";
    }
    const float project_exposure = engine.render_config().exposure;
    const float project_fog = engine.render_config().fog_density;
    expect(std::abs(project_exposure - 2.5f) > 1e-3f, "scene settings: the project's exposure differs from the override");

    engine.load_scene((dir / "scene.yaml").string());
    tick_frames(engine, 2);
    expect(std::abs(engine.render_config().exposure - 2.5f) < 1e-5f &&
           std::abs(engine.render_config().fog_density - 0.09f) < 1e-5f,
           "scene settings: the scene's render overrides apply when it loads");
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(engine.scene().find_system("Physics"));
    expect(physics && std::abs(physics->world().gravity().z + 2.0f) < 1e-5f,
           "scene settings: the scene's physics overrides configure its physics");
    expect(engine.pipeline().render_config().ssr_enabled == !project_ssr && engine.pipeline_rebuild_count() == rebuilds + 1,
           "scene settings: a startup-fixed override rebuilds the renderer in place (" +
               std::to_string(engine.pipeline_rebuild_count() - rebuilds) + " rebuilds)");

    engine.load_scene("assets/scenes/pixel_demo/scene.yaml");
    tick_frames(engine, 2);
    expect(std::abs(engine.render_config().exposure - project_exposure) < 1e-5f &&
           std::abs(engine.render_config().fog_density - project_fog) < 1e-5f,
           "scene settings: a scene without overrides runs with the project's settings again");
    expect(engine.pipeline().render_config().ssr_enabled == project_ssr && engine.pipeline_rebuild_count() == rebuilds + 2,
           "scene settings: ...rebuilding back to the project's switch");
    std::filesystem::remove_all(dir);
}

/** @brief Edit mode freezes physics; play mode simulates; load_scene() is re-entrant -- and the
 *         Engine owns audio without opening a sound card. */
COOPA_TEST(edit_mode_freezes_simulation_and_reloads_cleanly) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666");
    ScopedEnv no_input("NO_INPUT", "1");
    ScopedEnv home("HOME", (coopa::test::scratch_dir() / "home").string());   // user settings land here
    toy::core::EngineOptions opts;
    opts.edit_mode = true;
    toy::core::AppConfig cfg = make_test_config("assets/scenes/physics_test/scene.yaml", 320, 180, 160, 90);
    cfg.audio.music = 0.3f;
    toy::core::Engine engine(cfg, opts);

    // The Engine owns audio: config.yaml's bus defaults apply, one-shots play (null device).
    expect_near(engine.audio().bus_volume(toy::audio::k_bus_music), 0.3f, 1e-4f, "config.yaml's audio.music sets the Music bus");
    expect(!engine.audio().has_output(), "a hidden-window engine never opens the real sound card");
    expect(engine.audio().play_oneshot(std::string(PROJ_DIR) + "/sfxcoopa/ex/SEFE_Whoosh03.wav").is_valid(),
           "Engine::audio().play_oneshot plays");

    expect(engine.edit_mode() && !engine.scene().is_simulating(), "edit-mode engine loads a non-simulating scene");
    const glm::vec3 start = object_position(engine.scene(), "bounce_clay");
    tick_frames(engine, 20);
    const glm::vec3 frozen = object_position(engine.scene(), "bounce_clay");
    expect(glm::distance(start, frozen) < 1e-5f, "a dynamic body does not move in edit mode (" +
           std::to_string(start.z) + " -> " + std::to_string(frozen.z) + ")");

    engine.set_edit_mode(false);
    tick_frames(engine, 20);
    const glm::vec3 moved = object_position(engine.scene(), "bounce_clay");
    expect(glm::distance(start, moved) > 1e-3f, "the same body falls once simulation is on");

    // Re-entrant load: same scene again, nothing accumulates.
    engine.set_edit_mode(true);
    engine.load_scene("assets/scenes/physics_test/scene.yaml");
    expect(engine.scene_manager().scenes().size() == 1, "load_scene() replaces, never accumulates, scenes");
    expect(engine.scene().find_system("Physics") != nullptr, "load_scene() installs the per-scene systems");
    expect(glm::distance(object_position(engine.scene(), "bounce_clay"), start) < 1e-5f,
           "a reloaded scene starts from its authored state");
}

/** @brief Water is visible in the editor: bodies bake (and publish their mesh) in edit mode
 *         without simulating -- nothing floats, nothing ripples -- in both the rendered view and
 *         the editor's material-preview shading; entering play mode re-bakes with physics. */
COOPA_TEST(edit_mode_bakes_and_shows_water) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::EngineOptions opts;
    opts.edit_mode = true;
    toy::core::Engine engine(make_test_config("assets/scenes/water_test/scene.yaml", 640, 360, 320, 180), opts);
    tick_frames(engine, 5);

    auto bodies = engine.scene().get_components<toy::water::WaterBody>();
    expect(bodies.size() == 2u, "editor water: both water bodies loaded");
    bool all_visible = !bodies.empty();
    for (auto* b : bodies) {
        // On the owner's MeshRenderer, or (a body larger than one render tile) on its tiles.
        all_visible = all_visible && b->bake_stage == 1 && toy::water::WaterSystem::is_published(*b);
    }
    expect(all_visible, "editor water: every body baked and published its mesh in edit mode");
    auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
    expect(water && water->ripples().empty(), "editor water: no ripples while editing");
    const glm::vec3 crate = object_position(engine.scene(), "crate_light");

    // `margin`: how much bluer than red a pixel must be -- the preview modes' studio shading is
    // far less saturated than the stylized frame.
    auto count_blue = [](const Frame& f, int margin = 40) {
        long long n = 0;
        const size_t pixels = static_cast<size_t>(f.width) * f.height;
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* px = &f.pixels[i * f.channels];
            if (px[2] > px[0] + margin && px[2] > 70) ++n;
        }
        return n;
    };
    const Frame rendered = engine.capture_image(/*low_res=*/true);
    const long long px = static_cast<long long>(rendered.width) * rendered.height;
    expect(count_blue(rendered) > px / 20, "editor water: the lake is visible in the rendered view");

    engine.pipeline().render_config_mut().debug_view = "material_preview";
    tick_frames(engine, 2);
    const Frame preview = engine.capture_image(true);
    expect(count_blue(preview, 20) > px / 20, "editor water: ...and in material-preview shading");
    if (count_blue(preview, 20) <= px / 20) dump_frame(preview, "editor_water_preview");
    engine.pipeline().render_config_mut().debug_view = "off";
    expect(glm::distance(object_position(engine.scene(), "crate_light"), crate) < 1e-5f,
           "editor water: a buoyant crate stays put while editing");

    engine.set_edit_mode(false);
    tick_frames(engine, 3);
    bool stage2 = true;
    for (auto* b : engine.scene().get_components<toy::water::WaterBody>()) stage2 = stage2 && b->bake_stage == 2;
    expect(stage2, "editor water: entering play mode re-bakes with physics (depth, obstacles)");
}

/** @brief push_scene() runs a simulating copy over the edit scene; removing it restores the original untouched. */
COOPA_TEST(pushed_play_scene_leaves_the_edit_scene_untouched) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::EngineOptions opts;
    opts.edit_mode = true;
    const std::string path = std::string(ROOT_DIR) + "/assets/scenes/tests/physics/physics_test/scene.yaml";
    toy::core::Engine engine(make_test_config(path, 320, 180, 160, 90), opts);
    coopa::scene::Scene* edit_scene = &engine.scene();
    const glm::vec3 start = object_position(*edit_scene, "bounce_clay");

    coopa::scene::Scene& play = engine.push_scene(
        coopa::scene::SceneLoader::load_from_node(coopa::yaml::load_document(path), path), /*simulating=*/true);
    expect(&engine.scene() == &play, "push_scene() makes the pushed scene active");
    tick_frames(engine, 20);
    expect(glm::distance(object_position(play, "bounce_clay"), start) > 1e-3f, "the pushed scene simulates");
    expect(glm::distance(object_position(*edit_scene, "bounce_clay"), start) < 1e-5f,
           "the edit scene underneath does not move while the pushed one plays");

    engine.remove_scene(&play);
    engine.activate_scene(edit_scene);
    expect(&engine.scene() == edit_scene, "removing the pushed scene and reactivating restores the edit scene");
    tick_frames(engine, 2);
    expect(glm::distance(object_position(*edit_scene, "bounce_clay"), start) < 1e-5f, "the restored edit scene is untouched");
}

/** @brief A display region moves the image without changing a single low-res pixel, and rays go through it. */
COOPA_TEST(display_region_moves_the_image_and_viewport_rays) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 640, 360, 160, 90));
    tick_frames(engine, kNoiseCycle);
    const Frame full = engine.capture_image(true);
    // This config (no palette quantization) carries some frame-to-frame temporal drift of its
    // own; the region must add nothing beyond it.
    tick_frames(engine, kNoiseCycle);
    const long long baseline = count_diff(full, engine.capture_image(true));

    engine.set_display_region(toy::render::LetterboxRect{200, 40, 320, 180});
    tick_frames(engine, kNoiseCycle);
    const Frame region = engine.capture_image(true);
    const long long region_diff = count_diff(full, region);
    expect(region_diff >= 0 && region_diff <= baseline + kDriftBudget,
           "the low-res image is unchanged by a display region (" + std::to_string(region_diff) + " px differ)");

    const toy::render::LetterboxRect box = engine.display_rect();
    expect(box.x == 200 && box.y == 40 && box.w == 320 && box.h == 180, "display_rect() reports the region (exact 2x fit)");

    const Frame window = engine.capture_image(false);
    auto pixel_lum = [&](int x, int y) {
        const size_t i = (static_cast<size_t>(y) * window.width + x) * window.channels;
        return int(window.pixels[i]) + window.pixels[i + 1] + window.pixels[i + 2];
    };
    expect(pixel_lum(20, 20) == 0 && pixel_lum(620, 340) == 0, "outside the region the window is black");

    glm::vec3 origin, dir;
    expect(engine.viewport_ray(glm::vec2(box.x + box.w * 0.5f, box.y + box.h * 0.5f), origin, dir),
           "viewport_ray() succeeds at the region centre");
    auto* cam = coopa::gfx::engine::components::CameraComponent::main();
    const glm::vec3 forward = -glm::vec3(glm::inverse(cam->get_view_matrix())[2]);
    expect(glm::dot(glm::normalize(forward), dir) > 0.999f, "the centre ray is the camera's forward axis");
    expect(!engine.viewport_ray(glm::vec2(10, 10), origin, dir), "no ray outside the display rect");

    glm::vec2 px;
    expect(engine.world_to_window(origin + dir * 10.0f, px) &&
           glm::distance(px, glm::vec2(box.x + box.w * 0.5f, box.y + box.h * 0.5f)) < 1.0f,
           "world_to_window() inverts viewport_ray()");
}
