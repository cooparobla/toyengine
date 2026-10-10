/**
 * @file app_config_test.cpp
 * @brief AppConfig: config.yaml's blocks round-trip through AppConfig::load() (every knob set to
 *        a DISTINCT non-default value, so a parser that silently keeps a default cannot pass), quality
 *        tiers expand beneath explicit keys, scene `settings:` layer on top, and a bad key never
 *        swallows its neighbours. Also the project-module registry and project-root override.
 *
 * Deliberately not here: PixelRenderConfig's default values themselves -- they are tuning, and a
 * test that mirrors them only fails when someone retunes on purpose.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <fkYAML/node.hpp>
#include <toyengine/core/config.h>
#include <toyengine/core/engine.h>
#include <toyengine/core/module.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("app_config");

namespace {

/**
 * @brief Writes a scratch config file under coopa::test::scratch_dir() and loads it back.
 *
 * The round-trip tests all share this shape: every knob in a block set to a DISTINCT,
 * non-default value, so a parser bug that silently keeps the in-class default (a typo'd YAML
 * key being the usual one) cannot pass by coincidence.
 */
toy::core::AppConfig load_config_text(std::string_view filename, std::string_view yaml) {
    const std::string path = (coopa::test::scratch_dir() / filename).string();
    {
        std::ofstream out(path);
        out << yaml;
    }
    toy::core::AppConfig config = toy::core::AppConfig::load(path);
    std::filesystem::remove(path);
    return config;
}

// A project module, registered exactly the way a game project's src/ does it.
int g_test_module_inits = 0;

} // namespace

TOY_MODULE(test_project_module) { ++g_test_module_inits; }

COOPA_TEST(toy_module_registers_and_project_root_follows_the_env) {
    // Static registration ran before main(); Engine's constructor runs each body (exercised by
    // every render group's Engine, where this module just counts).
    const auto& mods = toy::core::modules();
    const auto it = std::find_if(mods.begin(), mods.end(), [](const toy::core::Module& m) { return m.name == "test_project_module"; });
    expect(it != mods.end() && it->on_engine_init, "TOY_MODULE registers a named module with an init body");

    // Project root: the compiled-in project (this checkout, for the engine's own build), unless
    // TOY_PROJECT_DIR points elsewhere.
    unsetenv("TOY_PROJECT_DIR");
    expect(toy::core::Engine::default_project_root() == std::filesystem::path(ROOT_DIR),
           "the engine's own build runs its own checkout as the project");
    setenv("TOY_PROJECT_DIR", "/tmp/some_project", 1);
    expect(toy::core::Engine::default_project_root() == std::filesystem::path("/tmp/some_project"),
           "TOY_PROJECT_DIR overrides the compiled-in project");
    unsetenv("TOY_PROJECT_DIR");
}

/**
 * @brief A scene's `settings:` layer onto config.yaml at the document level: overridden keys
 *        win, everything else is config.yaml's, and quality presets re-resolve BENEATH
 *        config.yaml's own explicit keys (a scene lowering shadow_quality still keeps the
 *        project's explicit shadow_pcf_samples). No overrides: the config comes back unchanged.
 */
COOPA_TEST(scene_settings_layer_over_config_and_rerun_presets) {
    const fkyaml::node base_doc = fkyaml::node::deserialize(std::string(
        "window: { width: 800 }\n"
        "render: { shadow_quality: high, shadow_pcf_samples: 20, exposure: 1.5, fog_density: 0.02 }\n"
        "physics: { gravity: { x: 0.0, y: 0.0, z: -9.81 } }\n"));
    const toy::core::AppConfig base = toy::core::AppConfig::from_node(base_doc);
    expect(base.render.shadow_map_resolution == 2048 && base.render.shadow_pcf_samples == 20,
           "scene settings: the base config resolves its preset and explicit key");

    const fkyaml::node settings = fkyaml::node::deserialize(std::string(
        "render: { fog_density: 0.1, shadow_quality: low }\n"
        "physics: { gravity: { x: 0.0, y: 0.0, z: -3.0 } }\n"
        "window: { width: 1 }\n"));   // not an overridable section: ignored
    const toy::core::AppConfig eff = base.with_scene_settings(settings);
    expect(std::abs(eff.render.fog_density - 0.1f) < 1e-6f, "scene settings: an overridden key wins");
    expect(std::abs(eff.render.exposure - 1.5f) < 1e-6f, "scene settings: other keys stay config.yaml's");
    expect(eff.render.shadow_map_resolution == 512, "scene settings: an overridden quality tier re-runs its preset");
    expect(eff.render.shadow_pcf_samples == 20, "scene settings: ...beneath config.yaml's explicit keys");
    expect(std::abs(eff.physics.gravity.z + 3.0f) < 1e-6f, "scene settings: physics is overridable");
    expect(eff.window.width == 800, "scene settings: only render and physics can be overridden");

    const toy::core::AppConfig none = base.with_scene_settings(fkyaml::node());
    expect(!toy::core::AppConfig::has_scene_overrides(fkyaml::node()) &&
           std::abs(none.render.fog_density - 0.02f) < 1e-6f && none.render.shadow_map_resolution == 2048,
           "scene settings: no overrides leaves the config as it was");
}

/**
 * @brief Every AA, DOF, shadow (soft, cascade, focus fit), fog, volumetrics, bloom, tilt-shift,
 *        SSR, UI-toggle and window key round-trips through AppConfig::load(), one block at a time.
 *
 * window.visible is what the whole headless GPU tier depends on (see make_test_config()), so it
 * gets the same treatment as every other knob. The retired Exp2 fog mode loads as Exponential.
 */
COOPA_TEST(every_render_and_window_key_round_trips) {
    {
        toy::core::AppConfig config = load_config_text("test_aa_config.yaml",
            "render:\n"
            "  aa_mode: taa\n"
            "  fxaa_subpixel: 0.5\n"
            "  fxaa_edge_threshold: 0.2\n"
            "  fxaa_edge_threshold_min: 0.01\n"
            "  smaa_threshold: 0.05\n"
            "  smaa_max_search_steps: 24\n"
            "  taa_blending_weight: 0.8\n"
            "  taa_weight_scale: 12.5\n");

        expect(config.render.aa_mode == "taa", "AppConfig::load: aa_mode round-trips");
        expect(config.render.fxaa_subpixel == 0.5f, "AppConfig::load: fxaa_subpixel round-trips");
        expect(config.render.fxaa_edge_threshold == 0.2f, "AppConfig::load: fxaa_edge_threshold round-trips");
        expect(config.render.fxaa_edge_threshold_min == 0.01f, "AppConfig::load: fxaa_edge_threshold_min round-trips");
        expect(config.render.smaa_threshold == 0.05f, "AppConfig::load: smaa_threshold round-trips");
        expect(config.render.smaa_max_search_steps == 24, "AppConfig::load: smaa_max_search_steps round-trips");
        expect(config.render.taa_blending_weight == 0.8f, "AppConfig::load: taa_blending_weight round-trips");
        expect(config.render.taa_weight_scale == 12.5f, "AppConfig::load: taa_weight_scale round-trips");
    }
    {
        toy::core::AppConfig config = load_config_text("test_dof_config.yaml",
            "render:\n"
            "  dof_enabled: true\n"
            "  debug_view: dof\n"
            "  dof_focus_mode: object\n"
            "  dof_focus_object: sdf_blob:sdf_blob_sphere\n"
            "  dof_focus_smoothing: 3.5\n"
            "  dof_focus_distance: 5.5\n"
            "  dof_aperture: 1.4\n"
            "  dof_focal_length: 85.0\n"
            "  dof_sensor_width: 24.0\n"
            "  dof_max_radius: 20.0\n"
            "  dof_sample_count: 16\n"
            "  dof_blade_count: 6\n"
            "  dof_blade_rotation: 30.0\n");

        expect(config.render.dof_enabled == true, "AppConfig::load: dof_enabled round-trips");
        expect(config.render.debug_view == "dof", "AppConfig::load: debug_view round-trips");
        expect(config.render.dof_focus_mode == "object", "AppConfig::load: dof_focus_mode round-trips");
        expect(config.render.dof_focus_object == "sdf_blob:sdf_blob_sphere", "AppConfig::load: dof_focus_object round-trips");
        expect(config.render.dof_focus_smoothing == 3.5f, "AppConfig::load: dof_focus_smoothing round-trips");
        expect(config.render.dof_focus_distance == 5.5f, "AppConfig::load: dof_focus_distance round-trips");
        expect(config.render.dof_aperture == 1.4f, "AppConfig::load: dof_aperture round-trips");
        expect(config.render.dof_focal_length == 85.0f, "AppConfig::load: dof_focal_length round-trips");
        expect(config.render.dof_sensor_width == 24.0f, "AppConfig::load: dof_sensor_width round-trips");
        expect(config.render.dof_max_radius == 20.0f, "AppConfig::load: dof_max_radius round-trips");
        expect(config.render.dof_sample_count == 16, "AppConfig::load: dof_sample_count round-trips");
        expect(config.render.dof_blade_count == 6, "AppConfig::load: dof_blade_count round-trips");
        expect(config.render.dof_blade_rotation == 30.0f, "AppConfig::load: dof_blade_rotation round-trips");
    }
    {
        toy::core::AppConfig config = load_config_text("test_soft_shadow_config.yaml",
            "render:\n"
            "  soft_shadows: false\n"
            "  shadow_softness: 0.42\n"
            "  point_shadow_softness: 0.07\n"
            "  shadow_pcf_samples: 8\n");

        expect(config.render.soft_shadows == false, "AppConfig::load: soft_shadows round-trips");
        expect(config.render.shadow_softness == 0.42f, "AppConfig::load: shadow_softness round-trips");
        expect(config.render.point_shadow_softness == 0.07f, "AppConfig::load: point_shadow_softness round-trips");
        expect(config.render.shadow_pcf_samples == 8u, "AppConfig::load: shadow_pcf_samples round-trips");
    }
    {
        toy::core::AppConfig config = load_config_text("test_cascade_config.yaml",
            "render:\n"
            "  shadow_cascades: 2\n"
            "  shadow_cascade_split_lambda: 0.4\n");

        expect(config.render.shadow_cascades == 2u, "AppConfig::load: shadow_cascades round-trips");
        expect(config.render.shadow_cascade_split_lambda == 0.4f,
               "AppConfig::load: shadow_cascade_split_lambda round-trips");

        toy::core::AppConfig focus = load_config_text("test_focus_shadow_config.yaml",
            "render:\n"
            "  shadow_fit: focus\n"
            "  shadow_focus_radius: 9.5\n"
            "  shadow_focus_distance: 30.0\n"
            "  shadow_pcf_max_texels: 20.0\n"
            "  shadow_receiver_plane_bias: true\n"
            "  shadow_receiver_max_slope: 3.0\n");
        expect(focus.render.shadow_fit == "focus", "AppConfig::load: shadow_fit round-trips");
        expect(focus.render.shadow_focus_radius == 9.5f && focus.render.shadow_focus_distance == 30.0f,
               "AppConfig::load: shadow_focus_radius/distance round-trip");
        expect(focus.render.shadow_pcf_max_texels == 20.0f, "AppConfig::load: shadow_pcf_max_texels round-trips");
        expect(focus.render.shadow_receiver_plane_bias && focus.render.shadow_receiver_max_slope == 3.0f,
               "AppConfig::load: the receiver-plane bias keys round-trip");
    }
    {
        toy::core::AppConfig config = load_config_text("test_atmosphere_config.yaml",
            "render:\n"
            "  fog_enabled: true\n"
            "  fog_mode: 1\n"
            "  fog_density: 0.07\n"
            "  fog_linear_start: 3.5\n"
            "  fog_linear_end: 44.0\n"
            "  fog_height_base: 1.5\n"
            "  fog_height_falloff: 0.25\n"
            "  fog_sky_blend: 0.6\n"
            "  fog_sun_amount: 0.4\n"
            "  fog_sun_anisotropy: 0.55\n"
            "  fog_max_opacity: 0.9\n"
            "  fog_sun_start_distance: 7.0\n"
            "  fog_start_distance: 2.5\n"
            "  fog_cutoff_distance: 300.0\n"
            "  fog_sky_distance: 800.0\n"
            "  volumetrics_enabled: true\n"
            "  volumetrics_step_count: 12\n"
            "  volumetrics_max_distance: 22.0\n"
            "  volumetrics_max_opacity: 0.45\n"
            "  volumetrics_sun_anisotropy: 0.35\n"
            "  debug_view: volumetrics\n"
            "  bloom_enabled: true\n"
            "  bloom_threshold: 0.8\n"
            "  bloom_soft_knee: 0.3\n"
            "  bloom_intensity: 1.7\n"
            "  bloom_scatter: 0.55\n"
            "  bloom_radius: 1.3\n"
            "  bloom_clamp: 9.0\n"
            "  tilt_shift_enabled: true\n"
            "  tilt_shift_focus_center: 0.4\n"
            "  tilt_shift_focus_width: 0.12\n"
            "  tilt_shift_ramp_width: 0.3\n"
            "  tilt_shift_blur_top: 0.8\n"
            "  tilt_shift_blur_bottom: 0.45\n"
            "  tilt_shift_max_radius: 9.0\n"
            "  tilt_shift_angle: 12.0\n");

        expect(config.render.fog_enabled == true, "AppConfig::load: fog_enabled round-trips");
        expect(config.render.fog_mode == 1, "AppConfig::load: fog_mode round-trips");
        expect(config.render.fog_density == 0.07f, "AppConfig::load: fog_density round-trips");
        expect(config.render.fog_linear_start == 3.5f, "AppConfig::load: fog_linear_start round-trips");
        expect(config.render.fog_linear_end == 44.0f, "AppConfig::load: fog_linear_end round-trips");
        expect(config.render.fog_height_base == 1.5f, "AppConfig::load: fog_height_base round-trips");
        expect(config.render.fog_height_falloff == 0.25f, "AppConfig::load: fog_height_falloff round-trips");
        expect(config.render.fog_sky_blend == 0.6f, "AppConfig::load: fog_sky_blend round-trips");
        expect(config.render.fog_sun_amount == 0.4f, "AppConfig::load: fog_sun_amount round-trips");
        expect(config.render.fog_sun_anisotropy == 0.55f, "AppConfig::load: fog_sun_anisotropy round-trips");
        expect(config.render.fog_max_opacity == 0.9f, "AppConfig::load: fog_max_opacity round-trips");
        expect(config.render.fog_sun_start_distance == 7.0f, "AppConfig::load: fog_sun_start_distance round-trips");
        expect(config.render.fog_start_distance == 2.5f, "AppConfig::load: fog_start_distance round-trips");
        expect(config.render.fog_cutoff_distance == 300.0f, "AppConfig::load: fog_cutoff_distance round-trips");
        expect(config.render.fog_sky_distance == 800.0f, "AppConfig::load: fog_sky_distance round-trips");

        // The retired Exp2 mode loads as Exponential (squaring a height-integrated depth is unphysical).
        toy::core::AppConfig legacy = load_config_text("test_fog_legacy_config.yaml",
            "render:\n  fog_mode: 2\n  fog_max_distance: 60.0\n");
        expect(legacy.render.fog_mode == 1, "AppConfig::load: legacy fog_mode 2 (Exp2) loads as 1");

        expect(config.render.volumetrics_enabled == true, "AppConfig::load: volumetrics_enabled round-trips");
        expect(config.render.volumetrics_step_count == 12, "AppConfig::load: volumetrics_step_count round-trips");
        expect(config.render.volumetrics_max_distance == 22.0f, "AppConfig::load: volumetrics_max_distance round-trips");
        expect(config.render.volumetrics_max_opacity == 0.45f, "AppConfig::load: volumetrics_max_opacity round-trips");
        expect(config.render.volumetrics_sun_anisotropy == 0.35f, "AppConfig::load: volumetrics_sun_anisotropy round-trips");
        expect(config.render.debug_view == "volumetrics", "AppConfig::load: debug_view round-trips");

        expect(config.render.bloom_enabled == true, "AppConfig::load: bloom_enabled round-trips");
        expect(config.render.bloom_threshold == 0.8f, "AppConfig::load: bloom_threshold round-trips");
        expect(config.render.bloom_soft_knee == 0.3f, "AppConfig::load: bloom_soft_knee round-trips");
        expect(config.render.bloom_intensity == 1.7f, "AppConfig::load: bloom_intensity round-trips");
        expect(config.render.bloom_scatter == 0.55f, "AppConfig::load: bloom_scatter round-trips");
        expect(config.render.bloom_radius == 1.3f, "AppConfig::load: bloom_radius round-trips");
        expect(config.render.bloom_clamp == 9.0f, "AppConfig::load: bloom_clamp round-trips");

        expect(config.render.tilt_shift_enabled == true, "AppConfig::load: tilt_shift_enabled round-trips");
        expect(config.render.tilt_shift_focus_center == 0.4f, "AppConfig::load: tilt_shift_focus_center round-trips");
        expect(config.render.tilt_shift_focus_width == 0.12f, "AppConfig::load: tilt_shift_focus_width round-trips");
        expect(config.render.tilt_shift_ramp_width == 0.3f, "AppConfig::load: tilt_shift_ramp_width round-trips");
        expect(config.render.tilt_shift_blur_top == 0.8f, "AppConfig::load: tilt_shift_blur_top round-trips");
        expect(config.render.tilt_shift_blur_bottom == 0.45f, "AppConfig::load: tilt_shift_blur_bottom round-trips");
        expect(config.render.tilt_shift_max_radius == 9.0f, "AppConfig::load: tilt_shift_max_radius round-trips");
        expect(config.render.tilt_shift_angle == 12.0f, "AppConfig::load: tilt_shift_angle round-trips");
    }
    {
        toy::core::AppConfig config = load_config_text("test_ssr_window_config.yaml",
            "window:\n"
            "  title: headless\n"
            "  width: 800\n"
            "  height: 600\n"
            "  vsync: false\n"
            "  visible: false\n"
            "render:\n"
            "  ssr_enabled: false\n"
            "  ssr_max_distance: 7.5\n"
            "  ssr_max_iterations: 32\n"
            "  ssr_thickness: 0.11\n"
            "  ssr_thickness_scale: 0.02\n"
            "  ssr_bias_texels: 2.5\n"
            "  ssr_roughness_cutoff: 0.6\n"
            "  ssr_start_mip: 2\n"
            "  ssr_min_mip0_steps: 3\n"
            "  ssr_temporal_enabled: false\n"
            "  ssr_temporal_blend: 0.5\n"
            "  ssr_blur_radius: 0.25\n"
            "  ssr_jitter: 0.4\n"
            "  ssr_cone_prefilter: 0.75\n"
            "  ssr_skip_behind: true\n"
            "  ssr_temporal_gamma: 1.5\n"
            "  world_ui_enabled: false\n"
            "  screen_ui_enabled: false\n");

        expect(config.window.title == "headless", "AppConfig::load: window.title round-trips");
        expect(config.window.width == 800u && config.window.height == 600u,
               "AppConfig::load: window size round-trips");
        expect(config.window.vsync == false, "AppConfig::load: window.vsync round-trips");
        expect(config.window.visible == false, "AppConfig::load: window.visible round-trips");

        expect(config.render.ssr_enabled == false, "AppConfig::load: ssr_enabled round-trips");
        expect(config.render.ssr_max_distance == 7.5f, "AppConfig::load: ssr_max_distance round-trips");
        expect(config.render.ssr_max_iterations == 32, "AppConfig::load: ssr_max_iterations round-trips");
        expect(config.render.ssr_thickness == 0.11f, "AppConfig::load: ssr_thickness round-trips");
        expect(config.render.ssr_thickness_scale == 0.02f, "AppConfig::load: ssr_thickness_scale round-trips");
        expect(config.render.ssr_bias_texels == 2.5f, "AppConfig::load: ssr_bias_texels round-trips");
        expect(config.render.ssr_roughness_cutoff == 0.6f, "AppConfig::load: ssr_roughness_cutoff round-trips");
        expect(config.render.ssr_start_mip == 2, "AppConfig::load: ssr_start_mip round-trips");
        expect(config.render.ssr_min_mip0_steps == 3, "AppConfig::load: ssr_min_mip0_steps round-trips");
        expect(config.render.ssr_temporal_enabled == false, "AppConfig::load: ssr_temporal_enabled round-trips");
        expect(config.render.ssr_temporal_blend == 0.5f, "AppConfig::load: ssr_temporal_blend round-trips");
        expect(config.render.ssr_blur_radius == 0.25f, "AppConfig::load: ssr_blur_radius round-trips");
        expect(config.render.ssr_jitter == 0.4f, "AppConfig::load: ssr_jitter round-trips");
        expect(config.render.ssr_cone_prefilter == 0.75f, "AppConfig::load: ssr_cone_prefilter round-trips");
        expect(config.render.ssr_skip_behind == true, "AppConfig::load: ssr_skip_behind round-trips");
        expect(config.render.ssr_temporal_gamma == 1.5f, "AppConfig::load: ssr_temporal_gamma round-trips");
        expect(config.render.world_ui_enabled == false, "AppConfig::load: world_ui_enabled round-trips");
        expect(config.render.screen_ui_enabled == false, "AppConfig::load: screen_ui_enabled round-trips");
    }
}

/**
 * @brief debug_view round-trips through AppConfig::load() and parse_debug_view() maps a
 *        recognized name to its DebugView value, defaulting an unrecognized one to Off.
 */
COOPA_TEST(debug_view_names_parse_with_an_off_fallback) {
    toy::core::AppConfig config = load_config_text("test_debug_view_config.yaml",
        "render:\n"
        "  debug_view: contact_shadows\n");
    expect(config.render.debug_view == "contact_shadows", "AppConfig::load: debug_view round-trips");
    expect(toy::render::parse_debug_view(config.render.debug_view) == toy::render::DebugView::ContactShadows,
           "parse_debug_view: 'contact_shadows' maps to DebugView::ContactShadows");

    expect(toy::render::parse_debug_view("off") == toy::render::DebugView::Off,
           "parse_debug_view: 'off' maps to DebugView::Off");
    expect(toy::render::parse_debug_view("lines") == toy::render::DebugView::Lines,
           "parse_debug_view: 'lines' maps to DebugView::Lines");
    expect(toy::render::parse_debug_view("nonsense") == toy::render::DebugView::Off,
           "parse_debug_view: an unrecognized name defaults to DebugView::Off");
}

/**
 * @brief Exercises the per-feature quality presets: tier strings (including the "med"
 * alias and the unknown-string fallback) expand to the preset field values, an
 * explicitly-written key overrides its preset, and a config with no quality keys
 * keeps the High-tier values.
 */
COOPA_TEST(quality_tiers_expand_beneath_explicit_keys) {
    using toy::render::RenderQuality;

    // Tier expansion, "med" alias, and the explicit-key override in one config.
    toy::core::AppConfig config = load_config_text("test_quality_config.yaml",
        "render:\n"
        "  shadow_quality: ultra\n"
        "  ssao_quality: low\n"
        "  ssr_quality: low\n"
        "  volumetrics_quality: med\n"
        "  ssgi_quality: low\n"
        "  sdf_quality: nonsense\n"
        "  ssr_max_iterations: 200\n");

    expect(config.render.shadow_quality == RenderQuality::Ultra, "AppConfig::load: shadow_quality parses ultra");
    // Per CASCADE, not the whole directional image: at the default 4 cascades the atlas is
    // twice this on each axis, so ultra allocates 6144^2 (see PixelRenderConfig's doc).
    expect(config.render.shadow_map_resolution == 3072u, "quality preset: ultra shadow_map_resolution");
    expect(config.render.cube_shadow_resolution == 1024u, "quality preset: ultra cube_shadow_resolution");
    expect(config.render.spot_shadow_resolution == 2048u, "quality preset: ultra spot_shadow_resolution");
    expect(config.render.shadow_pcf_samples == 32u, "quality preset: ultra shadow_pcf_samples");
    // The two PCSS/contact-shadow cost dials ride the same tier.
    expect(config.render.shadow_pcss_taps == 16u, "quality preset: ultra shadow_pcss_taps");
    expect(config.render.contact_shadow_steps == 16, "quality preset: ultra contact_shadow_steps");

    expect(config.render.ssgi_quality == RenderQuality::Low, "AppConfig::load: ssgi_quality parses low");
    expect(config.render.ssgi_max_iterations == 12, "quality preset: low ssgi_max_iterations");

    expect(config.render.ssao_slices == 1, "quality preset: low ssao_slices");
    expect(config.render.ssao_steps == 6, "quality preset: low ssao_steps");
    expect(config.render.ssao_max_radius_px == 32.0f, "quality preset: low ssao_max_radius_px");
    expect(config.render.ssao_temporal_frames == 4, "quality preset: low ssao_temporal_frames");

    expect(config.render.volumetrics_quality == RenderQuality::Medium, "AppConfig::load: 'med' parses as Medium");
    expect(config.render.volumetrics_step_count == 32, "quality preset: medium volumetrics_step_count");
    expect(config.render.volumetrics_max_scatter_lights == 2, "quality preset: medium volumetrics_max_scatter_lights");

    expect(config.render.sdf_quality == RenderQuality::High, "AppConfig::load: unknown quality string falls back to High");
    expect(config.render.sdf_max_steps == 64u, "quality preset: fallback High sdf_max_steps");
    expect(config.render.sdf_shadow_max_steps == 32u, "quality preset: fallback High sdf_shadow_max_steps");

    // ssr_quality: low would set 24, but the explicitly-written key wins.
    expect(config.render.ssr_max_iterations == 200, "quality preset: explicit ssr_max_iterations overrides its preset");

    // No quality keys at all: the High row (one representative field -- the rest are tuning).
    toy::core::AppConfig plain = load_config_text("test_quality_default_config.yaml",
        "render:\n"
        "  exposure: 1.0\n");
    expect(plain.render.shadow_map_resolution == 2048u, "quality preset: default High shadow_map_resolution");
    // shadow_cascades is deliberately NOT preset-covered: the tier moves resolution only, so
    // switching tiers can never silently change how many cascades a scene renders.
    expect(plain.render.shadow_cascades == 4u, "quality preset: shadow_cascades is not tier-driven");

    // Low volumetrics drops the light loop entirely, leaving only the (much cheaper)
    // sun-shaft term -- the one tier row where a covered field goes to zero.
    toy::core::AppConfig vol_low = load_config_text("test_quality_vol_low.yaml",
        "render:\n"
        "  volumetrics_quality: low\n");
    expect(vol_low.render.volumetrics_max_scatter_lights == 0, "quality preset: low volumetrics_max_scatter_lights is 0");
    expect(vol_low.render.volumetrics_step_count == 24, "quality preset: low volumetrics_step_count");
}

/**
 * @brief A key the parser doesn't know, and a commented-out one, must both leave every OTHER
 * field alone.
 *
 * This is the failure mode worth a test of its own: a config where one line is wrong (or one
 * comment wraps onto the next line and swallows the keys below it) fails SILENTLY -- the
 * affected fields simply keep their in-class defaults, the render looks subtly wrong, and
 * nothing in the log says why. Here the neighbouring keys prove the parser kept reading.
 */
COOPA_TEST(unknown_and_commented_keys_leave_other_fields_alone) {
    toy::core::AppConfig config = load_config_text("test_tolerant_config.yaml",
        "render:\n"
        "  exposure: 1.5\n"
        "  not_a_real_key: 42\n"
        "  # dither_strength: 0.5   <- commented out, must stay at its default\n"
        "  light_bands: 6.0\n");

    expect(config.render.exposure == 1.5f,
           "AppConfig::load: a key before an unknown one is still applied");
    expect(config.render.light_bands == 6.0f,
           "AppConfig::load: an unknown key does not stop the keys after it being read");
    expect(config.render.dither_strength == toy::render::PixelRenderConfig{}.dither_strength,
           "AppConfig::load: a commented-out key keeps its in-class default");

    // A missing file is a fall-back-to-defaults, not a startup failure (see AppConfig::load()).
    toy::core::AppConfig missing = toy::core::AppConfig::load((coopa::test::scratch_dir() / "does_not_exist.yaml").string());
    expect(missing.render.exposure == toy::render::PixelRenderConfig{}.exposure,
           "AppConfig::load: a missing file falls back to defaults");
}
