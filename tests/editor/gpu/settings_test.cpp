/**
 * @file settings_test.cpp
 * @brief Settings: a scene's overrides of config.yaml apply live, undo, reach Play and save as
 * `settings:`; the Render tab's feature headers and rows write overrides (a startup-only switch
 * rebuilds the renderer once) and its search narrows the tab; Project Settings writes config.yaml;
 * the World tab's weather drives render rows and runtime objects; Preferences and editor themes
 * apply at once and persist.
 */

#include <coopa/testing/test.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("settings");

namespace toy::editor::testing {

/**
 * @brief Settings rows edited in a scene become that scene's overrides of config.yaml: they
 *        apply live, leave config.yaml alone, undo like any scene edit, reach Play (physics
 *        included), revert to the project's value, and save into the scene as `settings:`.
 */
COOPA_TEST(scene_overrides_apply_live_undo_reach_play_and_save) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    const float project_exposure = engine.render_config().exposure;
    const Node config_render_before = app.config_document().section("render");

    app.set_prop_tab(PropTab::Render);
    const Node three = make_float(3.0);
    app.set_scene_setting("render", "exposure", &three);
    tick(engine, 2);
    expect(std::abs(engine.render_config().exposure - 3.0f) < 1e-5f, "a scene override applies live");
    expect(app.config_document().section("render") == config_render_before && !app.config_document().dirty(),
           "config.yaml is untouched by a scene override");
    expect(app.document().scene_setting("render", "exposure") != nullptr, "the scene document carries the override");

    app.undo();
    tick(engine, 4);
    expect(app.document().scene_setting("render", "exposure") == nullptr &&
           std::abs(engine.render_config().exposure - project_exposure) < 1e-5f, "undo removes the override");
    app.redo();
    tick(engine, 4);
    expect(std::abs(engine.render_config().exposure - 3.0f) < 1e-5f, "redo restores it");

    // A config.yaml edit still shows through wherever the scene doesn't override.
    {
        const Node before = app.config_document().node;
        app.config_document().section("render")["fog_density"] = make_float(0.07);
        app.config_document().commit("fog", before, {});
        app.apply_config_live();
        tick(engine, 2);
        expect(std::abs(engine.render_config().fog_density - 0.07f) < 1e-5f &&
               std::abs(engine.render_config().exposure - 3.0f) < 1e-5f,
               "a project setting applies under the scene's overrides");
    }

    const Node gravity = make_vec3({0.0f, 0.0f, -2.0f});
    app.set_scene_setting("physics", "gravity", &gravity);
    app.play();
    tick(engine, 4);
    auto* physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(engine.scene().find_system("Physics"));
    expect(app.playing() && physics && std::abs(physics->world().gravity().z + 2.0f) < 1e-5f,
           "Play runs with the scene's physics override");
    expect(std::abs(engine.render_config().exposure - 3.0f) < 1e-5f, "...and its render overrides");
    app.stop();
    tick(engine, 3);

    app.set_scene_setting("render", "exposure", nullptr);
    tick(engine, 2);
    expect(std::abs(engine.render_config().exposure - project_exposure) < 1e-5f, "reverting returns to the project's value");

    expect(app.save_scene(), "the scene saves");
    const Node saved = coopa::yaml::load_document(project.assets() / "scenes" / "main" / "scene.yaml");
    const Node& sc = saved.at("scene");
    expect(sc.contains("settings") && sc.at("settings").contains("physics") && !sc.at("settings").contains("render"),
           "the file keeps only the remaining overrides (" + coopa::yaml::emit(sc.contains("settings") ? sc.at("settings") : Node()) + ")");

    const Node off(false);
    app.set_scene_setting("render", "shadows_enabled", &off);   // a visible row, for the dump
    tick(engine, 2);
    expect(!engine.render_config().shadows_enabled, "a bool override applies");
    dump(engine, "21_scene_setting_override");
}

/**
 * @brief The Render tab, through real input. Features are sections with their switch in the
 *        header: in a scene, clicking it writes a scene override (live), opening a section leaves
 *        the switch alone, and setting a switch or row back to the project's value drops the
 *        override. A startup-only switch (SSAO) rebuilds the renderer in place exactly once when
 *        the edit settles; config.yaml is untouched; Revert All undoes every override in one
 *        step. The Project Settings modal edits config.yaml instead, and the search box narrows
 *        the tab to matching settings.
 */
COOPA_TEST(render_tab_headers_rows_and_search_write_scene_overrides) {
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    app.set_prop_tab(PropTab::Render);   // in the scene: edits to live settings override config.yaml for this scene
    app.clear_test_rects();
    tick(engine, 2);
    dump(engine, "render_settings_tab");
    for (const char* g : {"Resolution & Detail", "Lighting & Sky", "Anti-Aliasing", "Shadows", "Ambient Occlusion", "Reflections", "Fog"}) {
        expect(app.test_rect(std::string("setting_group:") + g).has_value(), std::string("the Render tab lists ") + g);
    }
    // The header's checkbox switches the feature (Shadows is on by default).
    auto shadows = app.test_rect("setting_group:Shadows");
    if (shadows) {
        const glm::vec2 check{shadows->x + shadows->h + 6.0f, shadows->center().y};
        in.click(check);
        tick(engine, 2);
        const Node* over = app.document().scene_setting("render", "shadows_enabled");
        expect(over && !over->get_value<bool>(), "the Shadows header checkbox turns shadows off (a scene override)");
        expect(!engine.render_config().shadows_enabled, "...live");
        // Clicking the title opens the section without touching the switch.
        shadows = app.test_rect("setting_group:Shadows");
        if (shadows) in.click({shadows->x + shadows->w * 0.6f, shadows->center().y});
        tick(engine, 2);
        over = app.document().scene_setting("render", "shadows_enabled");
        expect(over && !over->get_value<bool>(), "opening the section leaves the switch alone");
        dump(engine, "render_settings_shadows_open");
        // Switching it back to the project's value drops the override (and its tint).
        shadows = app.test_rect("setting_group:Shadows");
        if (shadows) in.click({shadows->x + shadows->h + 6.0f, shadows->center().y});
        tick(engine, 2);
        expect(app.document().scene_setting("render", "shadows_enabled") == nullptr && engine.render_config().shadows_enabled,
               "setting the switch back to the project's value removes the override");
        // Same for a row: override exposure, then type the project's value back in.
        const float project_exposure = engine.render_config().exposure;
        const Node two = make_float(project_exposure + 1.0);
        app.set_scene_setting("render", "exposure", &two);
        tick(engine, 2);
        expect(app.document().scene_setting("render", "exposure") != nullptr, "an exposure override exists");
        const Node back = make_float(project_exposure);
        app.set_scene_setting("render", "exposure", &back);
        tick(engine, 2);
        expect(app.document().scene_setting("render", "exposure") == nullptr, "setting a row back to the project's value removes the override");
        shadows = app.test_rect("setting_group:Shadows");
        if (shadows) in.click({shadows->x + shadows->w * 0.6f, shadows->center().y});   // fold the section again
        tick(engine, 2);
    }

    // A startup-only switch: SSAO. (Let anything the edits above queued settle first.)
    tick(engine, toy::core::Engine::kLiveRebuildDebounceFrames + 3);
    const bool project_ssao = engine.pipeline().render_config().ssao_enabled;
    const Node config_before = app.config_document().node;
    const int rebuilds = engine.pipeline_rebuild_count();

    app.clear_test_rects();
    tick(engine, 2);
    auto ao = app.test_rect("setting_group:Ambient Occlusion");
    expect(ao.has_value(), "the Render tab lists Ambient Occlusion");
    if (ao) {
        in.click({ao->x + ao->h + 6.0f, ao->center().y});
        tick(engine, 2);
    }
    const Node* over = app.document().scene_setting("render", "ssao_enabled");
    expect(over && over->get_value<bool>() == !project_ssao, "the header checkbox writes a scene override of a startup-only switch");
    expect(app.config_document().node == config_before && !app.config_document().dirty(), "config.yaml is untouched");
    tick(engine, toy::core::Engine::kLiveRebuildDebounceFrames + 3);
    expect(engine.pipeline().render_config().ssao_enabled == !project_ssao, "the renderer rebuilt with the scene's switch");
    expect(engine.pipeline_rebuild_count() == rebuilds + 1, "exactly one rebuild (" + std::to_string(engine.pipeline_rebuild_count() - rebuilds) + ")");
    dump(engine, "scene_feature_override");

    // A live (non-startup) override does not rebuild.
    const Node three = make_float(3.0);
    app.set_scene_setting("render", "exposure", &three);
    tick(engine, toy::core::Engine::kLiveRebuildDebounceFrames + 3);
    expect(engine.pipeline_rebuild_count() == rebuilds + 1, "a live setting applies without a rebuild");

    // Revert All: one undoable step back to the project's values.
    app.clear_test_rects();
    tick(engine, 2);
    const auto revert = app.test_rect("revert_all:render");
    expect(revert.has_value(), "the Render tab offers Revert All while the scene overrides settings");
    if (revert) in.click(revert->center());
    tick(engine, toy::core::Engine::kLiveRebuildDebounceFrames + 3);
    expect(app.document().scene_setting_count("render") == 0, "Revert All drops every render override");
    expect(engine.pipeline().render_config().ssao_enabled == project_ssao, "...and the renderer rebuilds back");
    app.undo();
    tick(engine, toy::core::Engine::kLiveRebuildDebounceFrames + 3);
    expect(app.document().scene_setting_count("render") == 2, "undo restores both overrides");

    // Project Settings: rows write config.yaml, not the scene.
    app.open_project_settings("Render Features");
    app.clear_test_rects();
    tick(engine, 3);
    dump(engine, "project_settings_modal");
    auto bloom = app.test_rect("setting_group:Bloom");
    expect(bloom.has_value() && app.test_rect("project_settings:Output").has_value(), "the Project Settings modal lists its categories and groups");
    if (bloom) {
        in.click({bloom->x + bloom->h + 6.0f, bloom->center().y});
        tick(engine, 2);
    }
    expect(app.config_document().dirty() && app.config_document().section("render").contains("bloom_enabled"),
           "a modal edit writes config.yaml");
    expect(app.document().scene_setting("render", "bloom_enabled") == nullptr, "...not a scene override");
    app.ui().close_all_popups();
    app.set_prop_tab(PropTab::Render);
    app.clear_test_rects();
    tick(engine, 2);

    // Search: only matching settings (and their groups) show.
    const auto filter = app.test_rect("render_settings_filter");
    expect(filter.has_value(), "the Render tab has a search box");
    if (filter) {
        in.click(filter->center());
        for (char ch : std::string("bloom")) {
            engine.queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
            tick(engine, 1);
        }
        app.clear_test_rects();
        tick(engine, 2);
        expect(app.test_rect("setting_group:Bloom").has_value() && !app.test_rect("setting_group:Shadows").has_value(),
               "searching 'bloom' shows the Bloom section and hides Shadows");
        dump(engine, "render_settings_search");
    }
}

/**
 * @brief The scene's weather in the World tab: the header switch writes the block and starts it
 *        live, the render rows it drives lock, its runtime objects show (locked) in the
 *        Hierarchy, Play runs it, disabling gives the render settings back, and it saves.
 */
COOPA_TEST(weather_drives_render_rows_runtime_objects_and_saves) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto& project = session.project;
    const fs::path root = session.project.root();
    const float project_fog = engine.render_config().fog_density;

    app.set_prop_tab(PropTab::World);
    app.clear_test_rects();
    tick(engine, 2);
    auto header = app.test_rect("weather_header");
    expect(header.has_value(), "the World tab has a Weather & Time of Day section");
    if (!header) return;
    in.click({header->x + header->h + 6.0f, header->center().y});   // its checkbox
    tick(engine, 4);
    const Node* enabled = app.document().scene_setting("weather", "enabled");
    expect(enabled && enabled->get_value<bool>(), "the header switch turns the scene's weather on");
    expect(app.document().scene_setting("weather", "conditions") != nullptr, "...writing out the stock conditions to edit");
    toy::weather::WeatherSystem* w = engine.weather();
    expect(w && w->enabled() && w->state().enabled, "the live scene's weather runs");
    if (!w) return;
    expect(std::abs(engine.render_config().fog_density - w->atmosphere().fog_density) < 1e-6f, "it drives the render fog");
    dump(engine, "weather_world_tab");
    expect(app.test_rect("weather_transition_speed").has_value(), "Preview has a transition fast-forward");
    expect(app.test_rect("weather_group:Time & Sun").has_value() && app.test_rect("weather_group:Sky Colours").has_value() &&
           app.test_rect("weather_group:Conditions").has_value(), "the settings are grouped into foldouts");
    if (auto strip = app.test_rect("weather_day_strip")) {   // dragging the 24-hour strip scrubs the live clock
        in.move({strip->x + strip->w * 0.25f, strip->center().y});
        in.drag({strip->x + strip->w * 0.75f, strip->center().y}, coopa::input::MouseButton::Left);
        expect(std::abs(w->time_of_day() - 18.0f) < 0.5f, "the day strip scrubs the clock (" + std::to_string(w->time_of_day()) + ")");
        expect(std::abs(as_float(*app.document().scene_setting("weather", "time_of_day"), 0.0f) - 10.0f) < 1e-3f, "...without saving it");
    } else {
        expect(false, "the Weather section has a 24-hour strip");
    }
    dump(engine, "weather_world_tab_groups");
    if (const char* dir = std::getenv("EDITOR_DUMP_DIR")) {   // the lower groups, for review
        (void)dir;
        if (auto h = app.test_rect("weather_header")) in.move({h->center().x, h->y + 300.0f});
        for (int k = 1; k <= 7; ++k) {
            for (int n = 0; n < 6; ++n) { engine.queue_input([](coopa::input::Input& i) { i.push_scroll(0.0, -1.0); }); tick(engine, 1); }
            tick(engine, 2);
            dump(engine, "weather_world_tab_scroll" + std::to_string(k));
        }
        for (int n = 0; n < 60; ++n) { engine.queue_input([](coopa::input::Input& i) { i.push_scroll(0.0, 1.0); }); tick(engine, 1); }
    }

    app.set_prop_tab(PropTab::Render);
    app.clear_test_rects();
    tick(engine, 2);
    // A search opens the matching sections, so their rows draw.
    if (const auto filter = app.test_rect("render_settings_filter")) {
        in.click(filter->center());
        for (char ch : std::string("density")) {
            engine.queue_input([ch](coopa::input::Input& i) { i.push_char(static_cast<uint32_t>(ch)); });
            tick(engine, 1);
        }
    }
    app.clear_test_rects();
    tick(engine, 2);
    expect(app.test_rect("locked:fog_density").has_value(), "the render rows the weather drives are locked");
    expect(!app.test_rect("locked:volumetrics_max_opacity").has_value() && !app.test_rect("locked:exposure").has_value(),
           "...and only those");
    dump(engine, "weather_locked_rows");

    // Effects wait for Play: rain in edit mode spawns nothing...
    w->set_condition("rain", 0.0f);
    tick(engine, 4);
    expect(engine.scene().find_object("weather_rain_drops") == nullptr, "edit mode: weather effects wait for Play");
    // ...until Preview > Show Effects, when they make runtime objects: listed, locked, in the Hierarchy.
    app.set_weather_preview_effects(true);
    app.clear_test_rects();
    tick(engine, 4);
    expect(engine.scene().find_object("Weather") != nullptr, "rain spawns the runtime Weather root");
    expect(app.test_rect("outliner_runtime").has_value(), "the Hierarchy lists runtime objects under a locked row");
    expect(engine.scene().find_object("weather_rain_drops") != nullptr, "...the rain among them");
    app.set_weather_preview_effects(false);
    tick(engine, 2);
    expect(engine.scene().find_object("weather_rain_drops") == nullptr, "Show Effects off: the effects go at once");
    app.set_weather_preview_effects(true);
    app.clear_test_rects();
    tick(engine, 4);
    {   // In edit mode (no physics running) the precipitation map comes from mesh bounds: the cube.
        const auto& probe = engine.weather()->ground_probe();
        const auto& field = probe.field();
        expect(probe.source() == toy::weather::GroundProbe::Source::Bounds && field, "edit mode: rain lands on mesh bounds");
        if (field) expect(std::abs(field->sample(0.0f, 0.0f) - 1.0f) < 0.05f,
                          "edit mode: rain stops on the starter cube (" + std::to_string(field->sample(0.0f, 0.0f)) + ")");
    }
    if (auto row = app.test_rect("runtime:Weather")) in.click({row->x + 60.0f, row->center().y});
    app.clear_test_rects();
    tick(engine, 2);
    expect(app.document().selection().empty() && app.test_rect("runtime_props").has_value(),
           "clicking a runtime row shows it read-only in Properties");
    dump(engine, "weather_runtime_outliner");
    app.set_prop_tab(PropTab::World);
    tick(engine, 2);
    dump(engine, "weather_world_tab_rain");

    // Live edits reach the running system without a rebuild.
    {
        Node v = make_float(7.0);
        app.set_scene_setting("weather", "time_of_day", &v);
        tick(engine, 2);
        expect(std::abs(engine.weather()->time_of_day() - 7.0f) < 0.01f, "editing the start time moves the live clock");
        expect(engine.scene().find_object("Weather") != nullptr, "...without rebuilding the scene (the rain is still there)");
    }

    app.play();
    tick(engine, 4);
    expect(app.playing() && engine.weather() && engine.weather()->state().enabled, "Play runs the scene's weather");
    app.stop();
    tick(engine, 3);

    const Node off(false);
    app.set_scene_setting("weather", "enabled", &off);
    tick(engine, 3);
    expect(engine.scene().find_object("Weather") == nullptr, "switched off, its runtime objects go");
    expect(std::abs(engine.render_config().fog_density - project_fog) < 1e-6f, "...and the render fog is the project's again");
    app.set_prop_tab(PropTab::Render);
    app.clear_test_rects();
    tick(engine, 2);
    expect(app.test_rect("setting_group:Fog").has_value() && !app.test_rect("locked:fog_density").has_value(), "...and its rows unlock");
    app.undo();
    tick(engine, 3);
    expect(engine.weather() && engine.weather()->enabled(), "undo switches it back on");

    expect(app.save_scene(), "the scene saves");
    const Node saved = coopa::yaml::load_document(project.assets() / "scenes" / "main" / "scene.yaml");
    const Node& sc = saved.at("scene");
    expect(sc.contains("settings") && sc.at("settings").contains("weather") &&
           sc.at("settings").at("weather").contains("conditions") &&
           sc.at("settings").at("weather").at("conditions").size() >= 6, "the weather is saved in the scene");
    bool runtime_saved = false;
    for (const auto& o : sc.at("root_objects").as_seq()) runtime_saved |= get_string(o, "name") == "Weather";
    expect(!runtime_saved, "runtime objects are never saved");
}

/**
 * @brief Editor themes and Edit > Preferences: a theme switch applies at once, keeps inherited
 *        roles and is remembered for the next session; the Preferences window's rows apply at
 *        once (theme tiles, the camera's orbit speed, the gizmo size slider) and land in
 *        ~/.toyengine_editor.yaml.
 */
COOPA_TEST(preferences_and_themes_apply_at_once_and_persist) {
    namespace imm = coopa::ui::imm;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    // Editor themes: Blender Dark by default, a switch applies at once and is remembered.
    expect(app.theme_id() == "blender_dark" && app.theme().name == "Blender Dark", "Blender Dark is the default theme");
    expect(app.ui().style.panel_bg == app.theme().style.panel_bg, "the theme drives the UI style");
    expect(app.set_theme("unity_dark"), "switch to Unity Dark");
    tick(engine, 3);
    dump(engine, "08_theme_unity_dark");
    expect(app.ui().style.selection == imm::load_theme(editor_themes_dir() / "unity_dark.yaml").style.selection,
           "the new theme applies immediately");
    expect(app.theme().style.axis_x == imm::load_theme(editor_themes_dir() / "blender_dark.yaml").style.axis_x,
           "`inherits:` keeps the base theme's roles");
    expect(get_string(Project::load_prefs(), "theme") == "unity_dark", "the choice is remembered");
    expect(!app.set_theme("no_such_theme") && app.theme_id() == "unity_dark", "a missing theme keeps the current one");
    app.set_theme("blender_dark");
    tick(engine, 2);

    app.open_preferences();
    app.clear_test_rects();
    tick(engine, 3);
    expect(app.ui().is_popup_open("Preferences"), "Edit > Preferences opens its window");
    expect(app.test_rect("prefs:Navigation").has_value() && app.test_rect("pref_theme:blender_light").has_value(),
           "...with categories and the theme tiles");
    dump(engine, "prefs_interface");
    if (auto tile = app.test_rect("pref_theme:blender_light")) in.click(tile->center());
    tick(engine, 2);
    expect(app.theme_id() == "blender_light", "clicking a theme tile switches the theme");
    expect(Project::load_prefs().contains("theme"), "...and remembers it");
    app.set_theme("blender_dark");

    if (auto nav = app.test_rect("prefs:Navigation")) in.click(nav->center());
    app.clear_test_rects();
    tick(engine, 2);
    expect(app.test_rect("pref:orbit_speed").has_value(), "Navigation lists the orbit speed");
    dump(engine, "prefs_navigation");

    auto& cam = app.camera();
    float yaw = cam.yaw_deg;
    cam.orbit({10.0f, 0.0f});
    const float base = yaw - cam.yaw_deg;
    expect(app.set_preference("orbit_speed", make_float(2.0)), "orbit_speed is a preference");
    yaw = cam.yaw_deg;
    cam.orbit({10.0f, 0.0f});
    expect(std::abs((yaw - cam.yaw_deg) - 2.0f * base) < 1e-3f, "Orbit Speed scales a drag");
    yaw = cam.yaw_deg;
    cam.turn(15.0f, 0.0f);
    expect(std::abs(cam.yaw_deg - yaw - 15.0f) < 1e-3f, "...but not the numpad's exact 15 deg steps");
    const Node prefs = Project::load_prefs();
    expect(prefs.contains("orbit_speed") && std::abs(get_float(prefs, "orbit_speed", 0.0f) - 2.0f) < 1e-4f, "it is saved");

    // A slider drag in the window: the gizmo size follows and is written once released.
    if (auto vp = app.test_rect("prefs:Viewport")) in.click(vp->center());
    app.clear_test_rects();
    tick(engine, 2);
    if (auto g = app.test_rect("pref:gizmo_size")) {
        in.move({g->x + 2.0f, g->center().y});
        in.drag({g->right() - 2.0f, g->center().y}, coopa::input::MouseButton::Left);
        tick(engine, 2);
        expect(app.gizmo().size_px > 190.0f, "dragging the Gizmo Size slider changes the gizmo (" + std::to_string(app.gizmo().size_px) + ")");
        expect(get_float(Project::load_prefs(), "gizmo_size", 0.0f) > 190.0f, "...and saves it on release");
    } else {
        expect(false, "Viewport lists the gizmo size");
    }
    if (auto km = app.test_rect("prefs:Keymap")) in.click(km->center());
    tick(engine, 3);
    dump(engine, "prefs_keymap");
    app.ui().close_all_popups();
    tick(engine, 2);

    expect(app.set_theme("blender_light"), "switch to Blender Light");
    tick(engine, 3);
    {
        EditorApp again(engine, session.project);   // a new session starts with the remembered theme
        expect(again.theme_id() == "blender_light", "the remembered theme loads at startup");
    }
}

} // namespace toy::editor::testing
