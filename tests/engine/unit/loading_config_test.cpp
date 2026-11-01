/**
 * @file loading_config_test.cpp
 * @brief scene_load_options() / apply_scene_loading_block() (toyengine/core/scene_loading.h):
 *        the three-way layering between LoadingConfig, the scene's own `settings.loading:`
 *        block, and the opt-out shape a disabled project falls back to.
 */

#include <coopa/testing/test.h>

#include <fkYAML/node.hpp>
#include <toyengine/core/loading_config.h>
#include <toyengine/core/scene_loading.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("loading_config");

using toy::core::LoadingConfig;
using toy::core::SceneLoadOptions;
using toy::core::SceneTransition;
using toy::core::apply_scene_loading_block;
using toy::core::scene_load_options;

COOPA_TEST(project_defaults_cover_with_a_warming_loading_screen) {
    LoadingConfig config;
    config.background = glm::vec3(0.1f, 0.2f, 0.3f);
    config.fade_in = 0.4f;
    config.fade_out = 0.6f;
    config.warm_up_timeout = 12.0f;
    config.warm_up_budget_ms = 5.0f;
    config.min_display_time = 0.25f;
    config.overlay_ui = "ui/my_overlay";

    const SceneLoadOptions opts = scene_load_options(config);
    expect(opts.transition.kind == SceneTransition::Kind::LoadingScreen,
           "scene_load_options: the project default covers with a loading screen");
    expect(opts.warm_up, "scene_load_options: the project default waits for warm-up");
    expect(opts.transition.color == config.background, "scene_load_options: the fade colour is LoadingConfig::background");
    expect(opts.transition.fade_in == 0.4f && opts.transition.fade_out == 0.6f,
           "scene_load_options: fade_in/fade_out carry over");
    expect(opts.warm_up_timeout == 12.0f && opts.warm_up_budget_ms == 5.0f,
           "scene_load_options: warm-up timeout/budget carry over");
    expect(opts.min_display_time == 0.25f, "scene_load_options: min_display_time carries over");
    expect(opts.transition.loading_screen == "ui/my_overlay", "scene_load_options: overlay_ui becomes the transition's overlay ref");
}

COOPA_TEST(disabled_project_falls_back_to_no_cover) {
    LoadingConfig config;
    config.enabled = false;
    const SceneLoadOptions opts = scene_load_options(config);
    expect(opts.transition.kind == SceneTransition::Kind::None, "scene_load_options: disabled -> no transition");
    expect(!opts.warm_up, "scene_load_options: disabled -> no warm-up");
}

COOPA_TEST(scene_settings_override_individual_keys_over_the_project_default) {
    SceneLoadOptions opts = scene_load_options(LoadingConfig{});
    const fkyaml::node settings = fkyaml::node::deserialize(std::string(
        "loading: { fade_in: 0.1, warm_up_timeout: 3.0 }\n"));
    apply_scene_loading_block(settings, opts);
    expect(opts.transition.kind == SceneTransition::Kind::LoadingScreen,
           "apply_scene_loading_block: an unrelated key leaves the project's cover on");
    expect(opts.transition.fade_in == 0.1f, "apply_scene_loading_block: fade_in overrides the project default");
    expect(opts.warm_up_timeout == 3.0f, "apply_scene_loading_block: warm_up_timeout overrides the project default");
}

COOPA_TEST(scene_settings_ui_turns_on_a_cover_even_when_the_project_disabled_it) {
    SceneLoadOptions opts = scene_load_options(LoadingConfig{/* enabled = */ false});
    const fkyaml::node settings = fkyaml::node::deserialize(std::string(
        "loading: { ui: ui/explicit_screen }\n"));
    apply_scene_loading_block(settings, opts);
    expect(opts.transition.kind == SceneTransition::Kind::LoadingScreen,
           "apply_scene_loading_block: an explicit ui: asks for a cover even with the project off");
    expect(opts.warm_up, "apply_scene_loading_block: ...and implies warm_up, since warm_up wasn't itself set");
    expect(opts.transition.loading_screen == "ui/explicit_screen",
           "apply_scene_loading_block: ui: sets the overlay ref");
}

COOPA_TEST(scene_settings_can_explicitly_turn_warm_up_off_without_changing_the_kind) {
    SceneLoadOptions opts = scene_load_options(LoadingConfig{});
    const fkyaml::node settings = fkyaml::node::deserialize(std::string(
        "loading: { warm_up: false }\n"));
    apply_scene_loading_block(settings, opts);
    expect(opts.transition.kind == SceneTransition::Kind::LoadingScreen,
           "apply_scene_loading_block: warm_up: false alone does not change the transition kind");
    expect(!opts.warm_up, "apply_scene_loading_block: warm_up: false wins over the project default");
}

COOPA_TEST(no_loading_block_leaves_opts_entirely_alone) {
    SceneLoadOptions opts = scene_load_options(LoadingConfig{});
    const SceneLoadOptions before = opts;
    apply_scene_loading_block(fkyaml::node(), opts);
    expect(opts.transition.kind == before.transition.kind && opts.warm_up == before.warm_up,
           "apply_scene_loading_block: a null settings node changes nothing");

    const fkyaml::node no_loading_key = fkyaml::node::deserialize(std::string("render: { exposure: 1.0 }\n"));
    apply_scene_loading_block(no_loading_key, opts);
    expect(opts.transition.kind == before.transition.kind && opts.warm_up == before.warm_up,
           "apply_scene_loading_block: settings with no loading: key changes nothing");
}
