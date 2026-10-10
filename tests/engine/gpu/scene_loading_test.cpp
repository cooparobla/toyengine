/**
 * @file scene_loading_test.cpp
 * @brief Scene loading through the Engine: load_scene_async() builds over frames without touching
 *        the running scene, fades, swaps, fails cleanly and is cancelled by load_scene(); saves
 *        cross scenes; and SceneLink shows a loading screen and lands the player on the spawn point.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <coopa/animation/animation_system.h>
#include <coopa/scene/scene_loader.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/scene_link.h>
#include <toyengine/weather/weather_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/scratch_files.h"

COOPA_TEST_SUITE("scene_loading");

using namespace toy::test;

namespace scene_load_test {

/** @brief Counts constructions and start() calls, to see when an async load starts its scene. */
class StartProbe : public coopa::scene::Component {
public:
    static inline int constructed = 0;
    static inline int started = 0;
    StartProbe() { ++constructed; }
    std::string type_name() const override { return "StartProbe"; }
    void start() override { ++started; }
};

void register_probe() {
    coopa::scene::SceneLoader::register_component_parser("StartProbe",
        [](const fkyaml::node&, coopa::scene::SceneObject& obj, const coopa::scene::SceneLoader::ParseContext&) {
            obj.add_component<StartProbe>();
        });
}

/** @brief A scene of `count` probed cubes plus a camera and a sun, written under coopa::test::scratch_dir(). */
std::string write_probe_scene(int count) {
    const std::filesystem::path dir = coopa::test::scratch_dir() / "async_probe_scene";
    std::filesystem::create_directories(dir);
    std::string y =
        "scene:\n"
        "  scene_name: AsyncProbe\n"
        "  root_objects:\n"
        "    - name: camera\n"
        "      components:\n"
        "        - type: Transform\n"
        "          position: { x: 0, y: -14, z: 7 }\n"
        "          rotation: { x: 62, y: 0, z: 0 }\n"
        "        - type: Camera\n"
        "          main: true\n"
        "          fov: 60.0\n"
        "    - name: sun\n"
        "      components:\n"
        "        - type: DirectionalLight\n"
        "          direction: { x: -0.4, y: 0.5, z: -0.75 }\n"
        "          intensity: 1.0\n";
    for (int i = 0; i < count; ++i) {
        const int gx = i % 8, gy = i / 8;
        y += "    - name: probe_" + std::to_string(i) + "\n"
             "      components:\n"
             "        - type: Transform\n"
             "          position: { x: " + std::to_string(gx * 1.5f - 5.25f) + ", y: " + std::to_string(gy * 1.5f) + ", z: 0.5 }\n"
             "        - type: MeshRenderer\n"
             "          mesh_path: cube\n"
             "          material: { albedo: { r: 0.9, g: 0.5, b: 0.2 } }\n"
             "        - type: StartProbe\n";
    }
    const std::filesystem::path path = dir / "scene.yaml";
    write_text_file(path, y);
    return path.string();
}

/** @brief Pixels whose RGB sum is under `dark`. */
long long count_dark(const Frame& f, int dark = 30) {
    long long n = 0;
    for (size_t i = 0; i < static_cast<size_t>(f.width) * f.height; ++i) {
        const uint8_t* px = &f.pixels[i * f.channels];
        if (int(px[0]) + int(px[1]) + int(px[2]) < dark) ++n;
    }
    return n;
}

}  // namespace scene_load_test

/**
 * @brief load_scene_async() builds the next scene over many frames while the current one keeps
 *        ticking untouched, never starts it before activation, reports monotonic progress,
 *        fades out -> swaps -> fades in, and a failed load leaves the current scene running.
 */
COOPA_TEST(async_load_builds_over_frames_and_swaps) {
    using namespace scene_load_test;
    ScopedEnv fixed_dt("FIXED_DT", "0.016666");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("tests/fixtures/scenes/kitchen_sink/scene.yaml", 320, 180, 160, 90));
    register_probe();
    const int kProbes = 24;
    const std::string path = write_probe_scene(kProbes);
    tick_frames(engine, 2);

    coopa::scene::Scene* old = &engine.scene_manager().get_active_scene();
    const size_t old_roots = old->root_objects().size();
    StartProbe::constructed = StartProbe::started = 0;

    toy::core::SceneLoadOptions opts;
    opts.transition = toy::core::SceneTransition::fade(0.1f, glm::vec3(0.0f));
    opts.build_budget_ms = 0.0f;   // one root object per frame: the build must span frames
    toy::core::SceneLoadHandle load = engine.load_scene_async(path, opts);
    expect(load.valid() && !load.is_ready(), "async load: the request returns at once, not ready");
    expect(engine.overlay_layers().size() == 1, "async load: the fade is an engine overlay layer");
    int completes = 0;
    int started_at_complete = -1;
    load.on_complete([&](coopa::scene::Scene& s) {
        ++completes;
        started_at_complete = StartProbe::started;
        expect(s.name() == "AsyncProbe", "async load: on_complete gets the new scene");
    });

    int frames = 0;
    float last_progress = 0.0f;
    bool monotonic = true, intact = true, unstarted = true, covered = false, ticked = true;
    while (!load.is_ready() && frames < 1000) {
        const uint64_t before = old->frame_index();
        engine.tick();
        ++frames;
        if (load.progress() < last_progress) monotonic = false;
        last_progress = load.progress();
        if (load.transition_alpha() >= 1.0f) covered = true;
        if (load.is_ready()) break;
        if (&engine.scene_manager().get_active_scene() != old || engine.scene_manager().scenes().size() != 1 ||
            old->root_objects().size() != old_roots) {
            intact = false;
        }
        if (old->frame_index() == before) ticked = false;
        if (StartProbe::started != 0) unstarted = false;
    }
    expect(load.is_ready(), "async load: the scene activates (" + std::to_string(frames) + " frames)");
    expect_at_least(frames, kProbes, "async load: frames keep ticking between the request and activation");
    expect(ticked, "async load: the current scene keeps updating while the next one builds");
    expect(intact, "async load: the current scene stays intact (and active) until activation");
    expect(unstarted && StartProbe::constructed == kProbes, "async load: objects are built but not started before activation");
    expect(monotonic, "async load: progress never goes back");
    expect(covered, "async load: the fade covers the screen before the swap");
    expect(completes == 1 && started_at_complete == kProbes, "async load: on_complete fires once, after start()");
    expect(engine.scene_manager().scenes().size() == 1 && engine.scene_manager().get_active_scene().name() == "AsyncProbe",
           "async load: the new scene replaced the old one");
    expect(load.progress() == 1.0f, "async load: progress is 1 once ready");

    int more = 0;
    while (!load.is_done() && more < 300) { engine.tick(); ++more; }
    expect(load.is_done() && !load.failed() && load.stage() == toy::core::SceneLoadStage::Done,
           "async load: the fade-in completes");
    expect(engine.overlay_layers().empty() && load.transition_alpha() == 0.0f, "async load: the transition layer is gone");
    int late = 0;
    load.on_complete([&](coopa::scene::Scene&) { ++late; });
    expect(late == 1, "async load: on_complete after activation runs immediately");
    tick_frames(engine, 2);
    const Frame f = engine.capture_image(true);
    expect(count_dark(f) < static_cast<long long>(f.width) * f.height / 2, "async load: the new scene renders, uncovered");

    // A load that fails leaves the running scene alone and reports why.
    coopa::scene::Scene* current = &engine.scene_manager().get_active_scene();
    std::string why;
    toy::core::SceneLoadOptions fade_opts;
    fade_opts.transition = toy::core::SceneTransition::fade(0.1f);
    toy::core::SceneLoadHandle bad = engine.load_scene_async((coopa::test::scratch_dir() / "async_probe_scene" / "missing.yaml").string(), fade_opts);
    bad.on_failed([&](const std::string& e) { why = e; });
    for (int i = 0; i < 200 && !bad.is_done(); ++i) engine.tick();
    expect(bad.failed() && !why.empty(), "async load: a missing file fails with an error (" + why + ")");
    expect(&engine.scene_manager().get_active_scene() == current, "async load: a failed load keeps the current scene");
    for (int i = 0; i < 60 && !engine.overlay_layers().empty(); ++i) engine.tick();
    expect(engine.overlay_layers().empty() && bad.transition_alpha() == 0.0f, "async load: a failed load's fade clears");

    // load_scene() cancels an async load still in flight.
    toy::core::SceneLoadHandle cancelled = engine.load_scene_async(path, opts);
    engine.tick();
    engine.load_scene("tests/fixtures/scenes/kitchen_sink/scene.yaml");
    expect(cancelled.failed() && engine.overlay_layers().empty(), "async load: load_scene() cancels an unfinished async load");
    tick_frames(engine, 2);
}

/**
 * @brief Saves through the Engine, on the character_demo demo: the player walks onto a coin
 *        (SaveDemo picks it up), the clock is set, and the slot is saved; after switching to
 *        another scene, load() brings character_demo back through load_scene_async() and
 *        restores the player's pose (SaveId + CharacterController), the collected coin and the
 *        clock before the scene's first update. A load in the same scene applies at once.
 */
COOPA_TEST(save_load_crosses_scenes) {
    using namespace scene_load_test;
    ScopedEnv fixed_dt("FIXED_DT", "0.016666");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/gameplay/character_demo/scene.yaml", 320, 180, 160, 90));
    engine.saves().set_root(coopa::test::scratch_dir("engine_saves"));
    tick_frames(engine, 3);
    auto player_of = [&]() { return engine.scene().find_first_component<toy::scene::CharacterController>(); };
    auto coin_active = [&](const char* name) {
        coopa::scene::SceneObject* o = engine.scene().find_object(name);
        return o && o->active();
    };
    auto* cc = player_of();
    expect(cc != nullptr && engine.weather() != nullptr, "save e2e: character_demo has a player and a weather clock");
    if (!cc || !engine.weather()) return;
    cc->teleport(glm::vec3(3.0f, -4.0f, 0.0f));   // onto coin_4
    tick_frames(engine, 3);
    expect(!coin_active("coin_4") && coin_active("coin_3"), "save e2e: the player picked up coin_4");
    const glm::vec3 saved_pos = cc->owner->get_transform()->transform().position();
    engine.weather()->set_time(15.0f);
    expect(engine.saves().save("e2e"), "save e2e: saved (" + engine.saves().last_error() + ")");
    const auto info = engine.saves().slot_info("e2e");
    expect(info && info->scene == "assets/scenes/gameplay/character_demo/scene.yaml",
           "save e2e: the slot names the scene relative to the project (" + (info ? info->scene : std::string("-")) + ")");
    expect(info && info->summary_node().get("coins", 0) == 1, "save e2e: SaveDemo put the coin count in the summary");

    // Same scene: applied at once.
    cc->teleport(glm::vec3(-8.0f, 2.0f, 0.0f));
    expect(engine.saves().load("e2e") && !engine.saves().loading(), "save e2e: a same-scene load applies immediately");
    expect(glm::length(cc->owner->get_transform()->transform().position() - saved_pos) < 1e-3f, "save e2e: same-scene load restores the pose");

    // Another scene, then back.
    toy::core::SceneLoadOptions none;
    none.transition = toy::core::SceneTransition::none();
    toy::core::SceneLoadHandle away = engine.load_scene_async(write_probe_scene(4), none);
    for (int i = 0; i < 600 && !away.is_ready(); ++i) engine.tick();
    expect(away.is_ready() && engine.scene().name() == "AsyncProbe", "save e2e: switched to another scene");
    int loaded_signals = 0;
    auto conn = engine.saves().on_loaded.connect_scoped([&](const std::string&) { ++loaded_signals; });
    expect(engine.saves().load("e2e") && engine.saves().loading(), "save e2e: a load from another scene starts a scene load");
    int frames = 0;
    while (engine.saves().loading() && frames < 1200) { engine.tick(); ++frames; }
    expect(!engine.saves().loading() && loaded_signals == 1, "save e2e: the load finished (" + std::to_string(frames) + " frames)");
    expect(engine.scene().name() == "character_demo", "save e2e: character_demo is back");
    cc = player_of();
    expect(cc != nullptr, "save e2e: the reloaded scene has its player");
    if (!cc) return;
    expect(glm::length(cc->owner->get_transform()->transform().position() - saved_pos) < 0.05f,
           "save e2e: the player's position is restored");
    expect(!coin_active("coin_4") && coin_active("coin_1"), "save e2e: the collected coin stays collected");
    expect(engine.weather() && std::abs(engine.weather()->time_of_day() - 15.0f) < 0.1f, "save e2e: the clock is restored");
}

/**
 * @brief loading_demo end to end: walking into the hub's SceneLink shows the loading screen,
 *        the heavy scene's animators run without a refresh, and the way back lands the player
 *        on its spawn point.
 */
COOPA_TEST(scene_link_shows_loading_screen_and_lands_on_spawn) {
    using namespace scene_load_test;
    ScopedEnv fixed_dt("FIXED_DT", "0.016666");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/gameplay/loading_demo/scene.yaml", 640, 360, 320, 180));
    tick_frames(engine, 3);
    coopa::scene::Scene& hub = engine.scene_manager().get_active_scene();
    auto* player = hub.find_first_component<toy::scene::CharacterController>();
    expect(player != nullptr, "scene link: the hub has a player");
    if (!player) return;
    const Frame before = engine.capture_image(false);

    // Walk in: the north door's link fires on entry (a physics overlap, not a trigger pair).
    player->teleport(glm::vec3(0.0f, 8.0f, 0.0f));
    for (int i = 0; i < 5 && !engine.scene_loading(); ++i) engine.tick();
    expect(engine.scene_loading(), "scene link: entering the door starts an async load");
    toy::core::SceneLoadHandle load = engine.scene_load();
    for (int i = 0; i < 120 && load.transition_alpha() < 1.0f && !load.is_ready(); ++i) engine.tick();
    tick_frames(engine, 3);   // the loading screen spawns once covered, and lays out
    expect(!load.is_ready(), "scene link: min_display_time keeps the loading screen up");
    const Frame mid = engine.capture_image(false);
    const long long pixels = static_cast<long long>(mid.width) * mid.height;
    expect(count_dark(mid) > pixels / 2, "scene link: the loading screen covers the frame in the fade colour");
    expect_at_least(count_diff(mid, before, 40), pixels / 2, "scene link: the loading screen hides the hub");
    long long bright = 0;
    for (size_t i = 0; i < static_cast<size_t>(pixels); ++i) {
        const uint8_t* px = &mid.pixels[i * mid.channels];
        if (int(px[0]) + int(px[1]) + int(px[2]) > 200) ++bright;
    }
    expect_at_least(bright, 200, "scene link: the loading screen draws its title, bar and status");

    for (int i = 0; i < 2000 && !load.is_done(); ++i) engine.tick();
    expect(load.is_done() && !load.failed(), "scene link: the heavy scene loads (" + load.error() + ")");
    coopa::scene::Scene& heavy = engine.scene_manager().get_active_scene();
    expect(heavy.name() == "loading_demo_heavy" && engine.scene_manager().scenes().size() == 1,
           "scene link: the heavy scene replaced the hub");

    // Animators in a scene activated later run with no manual refresh: a dancer's pose changes.
    auto* anim = dynamic_cast<coopa::anim::AnimationSystem*>(heavy.find_system("Animation"));
    expect(anim && anim->animator_count() >= 13, "scene link: the heavy scene's animators are gathered (" +
           std::to_string(anim ? anim->animator_count() : 0) + ")");
    auto pose = [&]() {
        std::vector<glm::vec3> out;
        if (auto* d = heavy.find_object("dancer_0")) {
            d->for_each_recursive([&](coopa::scene::SceneObject& o) {
                if (auto* tc = o.get_transform()) out.push_back(tc->transform().rotation_degrees());
            });
        }
        return out;
    };
    const auto p0 = pose();
    tick_frames(engine, 20);
    expect(!p0.empty() && p0 != pose(), "scene link: an animated mannequin in the loaded scene moves");

    // Back through the heavy scene's door (fired from code): a fade, and the hub's spawn point.
    auto* back = heavy.find_object("back_door_link");
    auto* link = back ? back->get_component<toy::scene::SceneLink>() : nullptr;
    expect(link != nullptr, "scene link: the heavy scene has a door back");
    if (!link) return;
    link->trigger();
    engine.tick();
    toy::core::SceneLoadHandle back_load = engine.scene_load();
    glm::vec3 arrived(-100.0f);
    back_load.on_complete([&](coopa::scene::Scene& s) {
        if (auto* cc = s.find_first_component<toy::scene::CharacterController>()) {
            arrived = cc->owner->get_transform()->transform().position();
        }
    });
    for (int i = 0; i < 2000 && !back_load.is_done(); ++i) engine.tick();
    expect(back_load.is_done() && engine.scene_manager().get_active_scene().name() == "loading_demo",
           "scene link: the door back loads the hub");
    expect(glm::length(arrived - glm::vec3(0.0f, 5.5f, 0.0f)) < 0.01f,
           "scene link: the player arrives on spawn_point (" + std::to_string(arrived.x) + ", " +
           std::to_string(arrived.y) + ")");
    tick_frames(engine, 2);
}
