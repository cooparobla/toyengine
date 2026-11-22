/**
 * @file sub_scene_test.cpp
 * @brief A `SubScene` component: a second scene, simulated and rendered into a texture the
 *        host's UI draws.
 *
 * The design this covers: one ToyRenderPipeline INSTANCE per view, rather than one pipeline
 * time-slicing several views. Every target, camera UBO and piece of temporal state in
 * ToyRenderPipeline is already an instance member, so two instances share none of it -- which is
 * why a sub-view can run the full deferred chain instead of a cut-down forward pass.
 *
 * Both views record into ONE command buffer: the engine prepares each sub-view before
 * `Renderer::begin_frame()` and hands it to the host pipeline through `set_sub_views()`, which
 * records them ahead of its own chain. One submit, no semaphore, and the host's UI samples the
 * result later in that same buffer -- legal with no manual barrier because an OffscreenTarget's
 * render pass leaves its colour attachment in SHADER_READ_ONLY_OPTIMAL.
 *
 * Asserted through the HOST's frame rather than by reading the sub-view's image directly: that
 * is the path a game actually uses, and it covers the whole chain -- load, simulate, render,
 * publish, register the texture, draw it -- rather than just the render.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include <coopa/scene/scene_loader.h>
#include <fkYAML/node.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/sub_scene.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/scene_variant.h"

using namespace toy::test;

COOPA_TEST_SUITE("sub_scene");

namespace {

constexpr uint32_t kWinW = 1920;
constexpr uint32_t kWinH = 1080;

/// Well inside the Map panel's body when the dock opens on the World group at 1920x1080 --
/// clear of the panel's title row, its zoom segments and its edges.
constexpr uint32_t kMapX0 = 120, kMapY0 = 260, kMapX1 = 780, kMapY1 = 900;

/**
 * @brief How many DISTINCT quantized colours a region holds.
 *
 * Used only to rule out a flat fill -- a target that was cleared but never drawn reads as one
 * colour whatever that colour is, which a difference count alone cannot catch. It is NOT a
 * measure of "rendered vs drawn": the panel without a sub-scene is transparent glass over the
 * blurred world behind it, which is a richer gradient than a top-down town.
 */
size_t distinct_colors(const Frame& f, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    std::vector<uint32_t> seen;
    for (uint32_t y = y0; y < std::min(y1, f.height); ++y) {
        for (uint32_t x = x0; x < std::min(x1, f.width); ++x) {
            const uint8_t* p = f.pixels.data() + (static_cast<size_t>(y) * f.width + x) * f.channels;
            seen.push_back(static_cast<uint32_t>(p[0] >> 3) << 10 |
                           static_cast<uint32_t>(p[1] >> 3) << 5 |
                           static_cast<uint32_t>(p[2] >> 3));
        }
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    return seen.size();
}

/// Pixels in a region that differ between two frames by more than `tolerance` per channel.
long long region_diff(const Frame& a, const Frame& b, uint32_t x0, uint32_t y0,
                      uint32_t x1, uint32_t y1, int tolerance) {
    long long n = 0;
    for (uint32_t y = y0; y < std::min({y1, a.height, b.height}); ++y) {
        for (uint32_t x = x0; x < std::min({x1, a.width, b.width}); ++x) {
            const size_t i = (static_cast<size_t>(y) * a.width + x) * a.channels;
            const uint8_t* pa = a.pixels.data() + i;
            const uint8_t* pb = b.pixels.data() + i;
            if (std::abs(pa[0] - pb[0]) > tolerance || std::abs(pa[1] - pb[1]) > tolerance ||
                std::abs(pa[2] - pb[2]) > tolerance) {
                ++n;
            }
        }
    }
    return n;
}

/// glass_ui_demo, with the dock opened on the World group so the Map panel is the one on screen.
/// `drop_sub_scene` removes the SubScene component, which is the control: the panel then falls
/// back to its vector grid.
std::string map_scene(const std::string& tag, bool drop_sub_scene) {
    return scene_variant("assets/scenes/ui/glass_ui_demo/scene.yaml", tag, [&](fkyaml::node& doc) {
        fkyaml::node* dock = scene_component(doc, "dock", "QuadrantDock");
        require(dock != nullptr, "the demo still has a QuadrantDock on `dock`");
        (*dock)["open_inventory"] = false;
        (*dock)["open_character"] = false;
        (*dock)["open_skills"]    = false;
        // reduced_motion pins the reveal, so one settled frame is enough rather than the
        // assertion depending on how far the open transition has eased.
        (*dock)["reduced_motion"] = true;
        if (drop_sub_scene) {
            require(remove_scene_component(doc, "dock", "SubScene"),
                    "the demo declares a SubScene to remove");
        }
    });
}

}  // namespace

/**
 * @brief The Map panel holds a live render of the sub-scene rather than drawn vector art.
 *
 * Compared against the SAME scene with the SubScene component removed, because that is the
 * only control that isolates the feature: everything else on screen -- the glass, the dock, the
 * POI markers drawn OVER the view -- is identical between the two frames.
 */
COOPA_TEST(a_sub_scene_renders_a_live_world_into_the_map_panel) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    Frame with, without;
    {
        toy::core::Engine engine(make_test_config(map_scene("sub_scene_on", false),
                                                  kWinW, kWinH, 1440, 960));
        tick_frames(engine, 24);
        with = engine.capture_image(/*low_res=*/false);
    }
    {
        toy::core::Engine engine(make_test_config(map_scene("sub_scene_off", true),
                                                  kWinW, kWinH, 1440, 960));
        tick_frames(engine, 24);
        without = engine.capture_image(/*low_res=*/false);
    }

    // The panel body is replaced by the sub-view's pixels, so nearly all of it differs from the
    // control. This is the assertion that fails if the view stops being recorded, stops being
    // registered as a UI texture, or is simply never drawn.
    const long long area = static_cast<long long>(kMapX1 - kMapX0) * (kMapY1 - kMapY0);
    const long long differing = region_diff(with, without, kMapX0, kMapY0, kMapX1, kMapY1, 8);
    const bool live_ok = expect(differing > area / 2,
        "map panel: its body is the sub-view's pixels, not the control's (" +
            std::to_string(differing) + "/" + std::to_string(area) + " differ)");

    // ...and what replaced it is an image rather than a flat clear, which a difference count
    // on its own would happily accept.
    const size_t rendered = distinct_colors(with, kMapX0, kMapY0, kMapX1, kMapY1);
    const bool differs_ok = expect(rendered > 24,
        "map panel: it holds a rendered image rather than a flat fill (" +
            std::to_string(rendered) + " distinct colours)");

    if (!live_ok || !differs_ok) {
        std::cerr << "[sub_scene] differing=" << differing << "/" << area
                  << " colours=" << rendered << "\n";
        dump_frame(with, "map_with_sub_scene");
        dump_frame(without, "map_without_sub_scene");
    }
}

/**
 * @brief The sub-scene is loaded, kept active so it simulates, and publishes a texture.
 *
 * Checks the lifecycle the frame assertion above cannot see: that the second scene is actually
 * in the manager and ACTIVE (an inactive scene would render a frozen first frame forever), and
 * that the component's published handle is valid for a UI to draw.
 */
COOPA_TEST(the_sub_scene_is_loaded_active_and_publishes_its_texture) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config(map_scene("sub_scene_live", false),
                                              kWinW, kWinH, 1440, 960));
    tick_frames(engine, 16);

    auto views = engine.scene().get_components<toy::render::SubScene>();
    require(views.size() == 1, "the host scene declares exactly one SubScene");
    toy::render::SubScene* view = views[0];

    expect(view->ready, "the sub-view reports ready once its scene has been rendered");
    expect(view->texture.valid(), "it publishes a valid texture handle for a UI to draw");

    // Two scenes in the manager, and the sub-scene among them -- ACTIVE, so it ticks.
    const std::vector<coopa::scene::Scene*> scenes = engine.scene_manager().scenes();
    expect(scenes.size() == 2, "the sub-scene was added alongside the host (" +
                                   std::to_string(scenes.size()) + " scenes)");
    size_t active = 0;
    for (coopa::scene::Scene* s : scenes) {
        if (engine.scene_manager().is_scene_active(s)) ++active;
    }
    expect(active == 2, "both the host and the sub-scene are active, so both simulate (" +
                            std::to_string(active) + " active)");
}

/**
 * @brief Switching the active scene does not freeze a sub-scene.
 *
 * activate_scene() otherwise forces exclusivity over every scene, which the editor triggers on
 * every play-mode toggle and asset-preview entry -- so without the pinned-active exemption the
 * symptom is "works in the game, frozen in the editor", which no game-side test would catch.
 */
COOPA_TEST(activating_another_scene_leaves_the_sub_scene_running) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_test_config(map_scene("sub_scene_activate", false),
                                              kWinW, kWinH, 1440, 960));
    tick_frames(engine, 16);

    coopa::scene::Scene* host = &engine.scene();
    coopa::scene::Scene* sub = nullptr;
    for (coopa::scene::Scene* s : engine.scene_manager().scenes()) {
        if (s != host) sub = s;
    }
    require(sub != nullptr, "the sub-scene is in the manager");

    // Re-activating the host is exactly what the editor does when it switches documents.
    engine.activate_scene(host);
    expect(engine.scene_manager().is_scene_active(sub),
           "the sub-scene survives activate_scene() and keeps simulating");

    tick_frames(engine, 4);
    expect(engine.scene_manager().is_scene_active(sub),
           "and it is still active after further frames");
}

/**
 * @brief Removing the scene that declared a sub-view does not leave the engine holding a
 *        dangling pointer to it.
 *
 * This is the editor's Stop: EditorApp::stop() calls Engine::remove_scene(play_scene_), and the
 * play scene is the one that declared the SubScene. A view that kept a raw pointer to its host
 * would read freed memory on the very next frame, in get_components<SubScene>() -- where a
 * garbage length turns into an enormous allocation and the process is OOM-killed rather than
 * faulting, which is exactly how this presented.
 *
 * Pushing a second scene and removing it exercises the same path without the editor.
 */
COOPA_TEST(removing_the_declaring_scene_tears_its_sub_view_down) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    // Host with no sub-scene of its own, so the only view is the pushed scene's.
    toy::core::Engine engine(make_test_config(map_scene("sub_scene_pop_host", true),
                                              kWinW, kWinH, 1280, 720));
    tick_frames(engine, 8);
    const size_t base_scenes = engine.scene_manager().scenes().size();

    // Push a scene that DOES declare one -- the shape of entering play mode.
    coopa::scene::Scene played =
        coopa::scene::SceneLoader::load(map_scene("sub_scene_pop_play", false));
    coopa::scene::Scene* play = &engine.push_scene(std::move(played), true, fkyaml::node());
    tick_frames(engine, 12);
    require(engine.scene_manager().scenes().size() > base_scenes + 1,
            "the pushed scene brought a sub-scene with it");

    // ...and leaving it. Ticking after this is what used to read the freed host.
    engine.remove_scene(play);
    tick_frames(engine, 12);

    expect(engine.scene_manager().scenes().size() == base_scenes,
           "the sub-scene went with the scene that declared it (" +
               std::to_string(engine.scene_manager().scenes().size()) + " scenes, expected " +
               std::to_string(base_scenes) + ")");
}

/**
 * @brief A sub-scene that cannot load costs its own panel and nothing else.
 *
 * A sub-view is an ADDITION to a scene, so one bad path must not be fatal: before this was
 * handled, a single mistyped `scene:` key threw out of Engine::tick() and took the whole game
 * down, which makes a typo indistinguishable from an engine fault. The host must still render,
 * and the complaint must be made once rather than every frame.
 */
COOPA_TEST(a_sub_scene_that_cannot_load_does_not_take_the_host_down) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    const std::string scene = scene_variant("assets/scenes/ui/glass_ui_demo/scene.yaml",
                                            "sub_scene_missing", [](fkyaml::node& doc) {
        fkyaml::node* sub = scene_component(doc, "dock", "SubScene");
        coopa::test::require(sub != nullptr, "the demo declares a SubScene to break");
        (*sub)["scene"] = std::string("ui/there_is_no_such_scene");
    });

    toy::core::Engine engine(make_test_config(scene, 1280, 720, 960, 540));
    tick_frames(engine, 16);

    // The host rendered its own frame regardless.
    const Frame f = engine.capture_image(/*low_res=*/false);
    const long long total = static_cast<long long>(f.width) * f.height;
    expect(count_nonblack(f) > total / 2,
           "the host scene still renders with a broken sub-scene declaration");

    // The declaration is still there, simply never satisfied -- so UI drawing it falls back.
    auto views = engine.scene().get_components<toy::render::SubScene>();
    coopa::test::require(views.size() == 1, "the component survives a failed load");
    expect(!views[0]->ready, "it reports not-ready rather than publishing a stale texture");
    expect(engine.scene_manager().scenes().size() == 1,
           "no half-built sub-scene was left in the manager (" +
               std::to_string(engine.scene_manager().scenes().size()) + " scenes)");
}
