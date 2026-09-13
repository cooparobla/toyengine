// toyengine test suite -- registered with ctest via CMakeLists.txt's
// enable_testing()/add_test().
//
// Declarations only for stb_image (used below to verify save_screenshot()'s
// output) -- the implementation is compiled once in gfxcoopa's src/gfx_impl.cpp.
#include <stb/stb_image.h>

#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

#include <toyengine/core/engine.h>
#include <toyengine/render/pixel_math.h>
#include <toyengine/scene/cloth_renderer.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/kinematic_controller.h>

#include <root_directory.h>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& what) {
    if (!condition) {
        std::cerr << "[FAIL] " << what << "\n";
        ++g_failures;
    } else {
        std::cout << "[ OK ] " << what << "\n";
    }
}

void test_letterbox_exact_fit() {
    // 1920x1080 window, 480x270 buffer -> scale 4, exact fit, no bars.
    auto rect = toy::render::compute_letterbox(1920, 1080, 480, 270);
    expect(rect.scale == 4.0f, "letterbox: 1920x1080 / 480x270 -> scale 4");
    expect(rect.w == 1920 && rect.h == 1080, "letterbox: 1920x1080 / 480x270 -> exact fit");
    expect(rect.x == 0 && rect.y == 0, "letterbox: 1920x1080 / 480x270 -> no offset");
}

void test_letterbox_with_bars() {
    // 1600x900 window, 480x270 buffer -> scale 3, 1440x810, centred with bars.
    auto rect = toy::render::compute_letterbox(1600, 900, 480, 270);
    expect(rect.scale == 3.0f, "letterbox: 1600x900 / 480x270 -> scale 3");
    expect(rect.w == 1440 && rect.h == 810, "letterbox: 1600x900 / 480x270 -> 1440x810");
    expect(rect.x == 80 && rect.y == 45, "letterbox: 1600x900 / 480x270 -> centred at (80,45)");
}

void test_letterbox_undersized_window_clamps_to_scale_1() {
    auto rect = toy::render::compute_letterbox(100, 100, 480, 270);
    expect(rect.scale == 1.0f, "letterbox: window smaller than buffer -> scale clamps to 1");
}

void test_fit_pillarbox_only() {
    // 1920x1080 window, 720x480 buffer (3:2 into 16:9) -> scale 2.25, 1620x1080,
    // pillarboxed left/right only -- the exact case a floored integer scale (2 ->
    // 1440x960) would letterbox on all four sides instead.
    auto rect = toy::render::compute_fit(1920, 1080, 720, 480);
    expect(std::fabs(rect.scale - 2.25f) < 1e-6f, "fit: 1920x1080 / 720x480 -> scale 2.25");
    expect(rect.w == 1620 && rect.h == 1080, "fit: 1920x1080 / 720x480 -> 1620x1080");
    expect(rect.x == 150 && rect.y == 0, "fit: 1920x1080 / 720x480 -> pillarboxed at (150,0), no top/bottom bars");
}

void test_fit_exact_match_fills_completely() {
    // Matching aspect (16:9 into 16:9) -> fills the window exactly, same as integer mode.
    auto rect = toy::render::compute_fit(1920, 1080, 480, 270);
    expect(rect.w == 1920 && rect.h == 1080 && rect.x == 0 && rect.y == 0,
          "fit: 1920x1080 / 480x270 -> exact fill, no bars");
}

void test_fit_beats_integer_bars() {
    // Same input compute_letterbox's test_letterbox_with_bars uses (scale 3 -> 1440x810,
    // 80/45px bars) -- fit uses the full fractional scale (3.333) and fills completely.
    auto rect = toy::render::compute_fit(1600, 900, 480, 270);
    expect(rect.w == 1600 && rect.h == 900 && rect.x == 0 && rect.y == 0,
          "fit: 1600x900 / 480x270 -> fills completely, unlike integer mode's 1440x810");
}

void test_fit_undersized_window() {
    auto rect = toy::render::compute_fit(100, 100, 480, 270);
    expect(rect.w == 100 && rect.h == 56, "fit: window smaller than buffer -> scales down, fills width");
    expect(rect.x == 0 && rect.y == 22, "fit: window smaller than buffer -> letterboxed top/bottom at (0,22)");
}

void test_display_rect_dispatches_on_upscale_mode() {
    toy::render::PixelRenderConfig integer_cfg;
    integer_cfg.upscale_mode = "integer";
    auto integer_rect = toy::render::compute_display_rect(integer_cfg, 1920, 1080, 720, 480);
    expect(integer_rect.w == 1440 && integer_rect.h == 960,
          "display_rect: upscale_mode=integer dispatches to compute_letterbox");

    toy::render::PixelRenderConfig fit_cfg;
    fit_cfg.upscale_mode = "fit";
    auto fit_rect = toy::render::compute_display_rect(fit_cfg, 1920, 1080, 720, 480);
    expect(fit_rect.w == 1620 && fit_rect.h == 1080,
          "display_rect: upscale_mode=fit dispatches to compute_fit");
}

void test_render_resolution_fixed_mode() {
    toy::render::PixelRenderConfig cfg;
    cfg.resolution_mode = "fixed";
    cfg.render_width = 480;
    cfg.render_height = 270;
    toy::render::RenderExtent e = toy::render::compute_render_extent(cfg, 1920, 1080);
    expect(e.width == 480 && e.height == 270, "render_resolution: fixed mode ignores swapchain size");
}

void test_render_resolution_divisor_mode() {
    toy::render::PixelRenderConfig cfg;
    cfg.resolution_mode = "divisor";
    cfg.scale_divisor = 4;
    toy::render::RenderExtent e = toy::render::compute_render_extent(cfg, 1920, 1080);
    expect(e.width == 480 && e.height == 270, "render_resolution: divisor mode divides swapchain size");
}

void test_pixel_render_config_aa_defaults() {
    // Defaults mirror blendy's PbrRenderPipeline field-for-field (see
    // PixelRenderConfig::aa_mode's own doc) except aa_mode itself, which defaults to "off"
    // here so a scene that never opts in renders exactly as it did before AA existed.
    toy::render::PixelRenderConfig cfg;
    expect(cfg.aa_mode == "off", "PixelRenderConfig: aa_mode defaults to off");
    expect(cfg.fxaa_subpixel == 0.75f, "PixelRenderConfig: fxaa_subpixel defaults to 0.75");
    expect(cfg.fxaa_edge_threshold == 0.166f, "PixelRenderConfig: fxaa_edge_threshold defaults to 0.166");
    expect(cfg.fxaa_edge_threshold_min == 0.0312f, "PixelRenderConfig: fxaa_edge_threshold_min defaults to 0.0312");
    expect(cfg.smaa_threshold == 0.1f, "PixelRenderConfig: smaa_threshold defaults to 0.1");
    expect(cfg.smaa_max_search_steps == 16, "PixelRenderConfig: smaa_max_search_steps defaults to 16");
    expect(cfg.taa_blending_weight == 0.9f, "PixelRenderConfig: taa_blending_weight defaults to 0.9");
    expect(cfg.taa_weight_scale == 30.0f, "PixelRenderConfig: taa_weight_scale defaults to 30.0");
}

void test_app_config_load_round_trips_aa_settings() {
    // Distinct, non-default values for every AA knob, so a parser bug that silently kept
    // the in-class default (e.g. a typo'd YAML key) wouldn't pass by coincidence.
    std::string path = std::string(ROOT_DIR) + "/output/test_aa_config.yaml";
    {
        std::ofstream out(path);
        out << "render:\n"
               "  aa_mode: taa\n"
               "  fxaa_subpixel: 0.5\n"
               "  fxaa_edge_threshold: 0.2\n"
               "  fxaa_edge_threshold_min: 0.01\n"
               "  smaa_threshold: 0.05\n"
               "  smaa_max_search_steps: 24\n"
               "  taa_blending_weight: 0.8\n"
               "  taa_weight_scale: 12.5\n";
    }

    toy::core::AppConfig config = toy::core::AppConfig::load(path);
    expect(config.render.aa_mode == "taa", "AppConfig::load: aa_mode round-trips");
    expect(config.render.fxaa_subpixel == 0.5f, "AppConfig::load: fxaa_subpixel round-trips");
    expect(config.render.fxaa_edge_threshold == 0.2f, "AppConfig::load: fxaa_edge_threshold round-trips");
    expect(config.render.fxaa_edge_threshold_min == 0.01f, "AppConfig::load: fxaa_edge_threshold_min round-trips");
    expect(config.render.smaa_threshold == 0.05f, "AppConfig::load: smaa_threshold round-trips");
    expect(config.render.smaa_max_search_steps == 24, "AppConfig::load: smaa_max_search_steps round-trips");
    expect(config.render.taa_blending_weight == 0.8f, "AppConfig::load: taa_blending_weight round-trips");
    expect(config.render.taa_weight_scale == 12.5f, "AppConfig::load: taa_weight_scale round-trips");

    std::filesystem::remove(path);
}

void test_pixel_render_config_dof_defaults() {
    // A scene/config that never opts in must render exactly as it did before DOF existed --
    // see PixelRenderConfig::dof_enabled's own doc.
    toy::render::PixelRenderConfig cfg;
    expect(cfg.dof_enabled == false, "PixelRenderConfig: dof_enabled defaults to false");
    expect(cfg.dof_focus_mode == "manual", "PixelRenderConfig: dof_focus_mode defaults to manual");
    expect(cfg.dof_focus_object == "", "PixelRenderConfig: dof_focus_object defaults to empty");
    expect(cfg.dof_focus_smoothing == 8.0f, "PixelRenderConfig: dof_focus_smoothing defaults to 8.0");
    expect(cfg.dof_focus_distance == 8.0f, "PixelRenderConfig: dof_focus_distance defaults to 8.0");
    expect(cfg.dof_aperture == 2.8f, "PixelRenderConfig: dof_aperture defaults to 2.8");
    expect(cfg.dof_focal_length == 0.0f, "PixelRenderConfig: dof_focal_length defaults to 0.0 (inherit camera lens)");
    expect(cfg.dof_sensor_width == 0.0f, "PixelRenderConfig: dof_sensor_width defaults to 0.0 (inherit camera sensor_width)");
    expect(cfg.dof_max_radius == 12.0f, "PixelRenderConfig: dof_max_radius defaults to 12.0");
    expect(cfg.dof_sample_count == 32, "PixelRenderConfig: dof_sample_count defaults to 32");
    expect(cfg.dof_blade_count == 0, "PixelRenderConfig: dof_blade_count defaults to 0 (perfect disc)");
    expect(cfg.dof_blade_rotation == 0.0f, "PixelRenderConfig: dof_blade_rotation defaults to 0.0");
    expect(cfg.dof_debug_view == false, "PixelRenderConfig: dof_debug_view defaults to false");
}

void test_app_config_load_round_trips_dof_settings() {
    // Distinct, non-default values for every DOF knob, so a parser bug that silently kept
    // the in-class default (e.g. a typo'd YAML key) wouldn't pass by coincidence.
    std::string path = std::string(ROOT_DIR) + "/output/test_dof_config.yaml";
    {
        std::ofstream out(path);
        out << "render:\n"
               "  dof_enabled: true\n"
               "  dof_debug_view: true\n"
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
               "  dof_blade_rotation: 30.0\n";
    }

    toy::core::AppConfig config = toy::core::AppConfig::load(path);
    expect(config.render.dof_enabled == true, "AppConfig::load: dof_enabled round-trips");
    expect(config.render.dof_debug_view == true, "AppConfig::load: dof_debug_view round-trips");
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

    std::filesystem::remove(path);
}

void test_dof_view_space_depth() {
    // Camera at (0,0,5) looking at the origin, standard RH lookAt -- glm's usual view-space
    // convention (camera looks down its own -Z) applies regardless of which world axis this
    // engine treats as "up" (CameraController's rig is Z-up; that only affects how a scene's
    // camera Transform is built, not this pure view-matrix math).
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    float depth_at_origin = toy::render::view_space_depth(view, glm::vec3(0.0f));
    expect(std::abs(depth_at_origin - 5.0f) < 1e-4f,
           "view_space_depth: point at the look-at target is 5m in front of a camera 5m away");

    float depth_closer = toy::render::view_space_depth(view, glm::vec3(0.0f, 0.0f, 2.0f));
    expect(std::abs(depth_closer - 3.0f) < 1e-4f,
           "view_space_depth: a point 3m closer to the camera reports 3m less depth");

    // A point behind the eye (camera at z=5 looking toward -z; z=8 is on the far side of the
    // camera from the look-at target) must come back negative -- the case
    // resolve_dof_focus_()'s `depth > 0.0f` guard exists to catch.
    float depth_behind = toy::render::view_space_depth(view, glm::vec3(0.0f, 0.0f, 8.0f));
    expect(depth_behind < 0.0f, "view_space_depth: a point behind the camera is negative");
}

void test_dof_focus_smoothing() {
    // rate <= 0 snaps straight to target, regardless of dt.
    expect(toy::render::exp_smooth_toward(0.0f, 10.0f, 0.0f, 0.5f) == 10.0f,
           "exp_smooth_toward: rate <= 0 snaps to target");
    expect(toy::render::exp_smooth_toward(0.0f, 10.0f, -1.0f, 0.5f) == 10.0f,
           "exp_smooth_toward: negative rate also snaps to target");

    // Monotone convergence toward the target, never overshooting it.
    float value = 0.0f;
    for (int i = 0; i < 60; ++i) {
        float next = toy::render::exp_smooth_toward(value, 10.0f, 8.0f, 1.0f / 60.0f);
        expect(next > value && next <= 10.0f, "exp_smooth_toward: monotone step toward target");
        value = next;
    }
    expect(value > 9.0f, "exp_smooth_toward: converges close to target after 1 second at rate 8");

    // Framerate independence: two half-steps at dt land at the same place as one step at 2*dt --
    // the property the 1 - exp(-rate*dt) form buys over a naive linear lerp.
    float two_steps = toy::render::exp_smooth_toward(
        toy::render::exp_smooth_toward(2.0f, 20.0f, 5.0f, 0.1f), 20.0f, 5.0f, 0.1f);
    float one_step = toy::render::exp_smooth_toward(2.0f, 20.0f, 5.0f, 0.2f);
    expect(std::abs(two_steps - one_step) < 1e-4f,
           "exp_smooth_toward: two dt steps match one 2*dt step (framerate-independent)");
}

void test_pixel_render_config_soft_shadow_defaults() {
    toy::render::PixelRenderConfig cfg;
    expect(cfg.soft_shadows == true, "PixelRenderConfig: soft_shadows defaults to true");
    expect(cfg.shadow_softness == 0.15f, "PixelRenderConfig: shadow_softness defaults to 0.15");
    expect(cfg.point_shadow_softness == 3.0f, "PixelRenderConfig: point_shadow_softness defaults to 3.0");
    expect(cfg.shadow_pcf_samples == 24u, "PixelRenderConfig: shadow_pcf_samples defaults to 24");
}

void test_app_config_load_round_trips_soft_shadow_settings() {
    // Distinct, non-default values for every soft-shadow knob, so a parser bug that silently
    // kept the in-class default (e.g. a typo'd YAML key) wouldn't pass by coincidence.
    std::string path = std::string(ROOT_DIR) + "/output/test_soft_shadow_config.yaml";
    {
        std::ofstream out(path);
        out << "render:\n"
               "  soft_shadows: false\n"
               "  shadow_softness: 0.42\n"
               "  point_shadow_softness: 0.07\n"
               "  shadow_pcf_samples: 8\n";
    }

    toy::core::AppConfig config = toy::core::AppConfig::load(path);
    expect(config.render.soft_shadows == false, "AppConfig::load: soft_shadows round-trips");
    expect(config.render.shadow_softness == 0.42f, "AppConfig::load: shadow_softness round-trips");
    expect(config.render.point_shadow_softness == 0.07f, "AppConfig::load: point_shadow_softness round-trips");
    expect(config.render.shadow_pcf_samples == 8u, "AppConfig::load: shadow_pcf_samples round-trips");

    std::filesystem::remove(path);
}

void test_directional_light_shadow_intensity_default() {
    coopa::gfx::engine::components::DirectionalLightComponent dl;
    expect(dl.shadow_intensity == 1.0f, "DirectionalLightComponent: shadow_intensity defaults to 1.0 (full occlusion)");
}

// --- Directional shadow frustum fit (pixel_math.h's compute_dir_shadow_fit) ---

toy::render::ShadowFitCamera make_shadow_fit_camera(const glm::vec3& eye, const glm::vec3& at) {
    toy::render::ShadowFitCamera cam;
    cam.view              = glm::lookAt(eye, at, glm::vec3(0.0f, 0.0f, 1.0f));
    cam.is_perspective    = true;
    cam.fov_degrees       = 45.0f;
    cam.near_clip         = 0.1f;
    cam.far_clip          = 1000.0f;
    cam.aspect            = 16.0f / 9.0f;
    return cam;
}

void test_dir_shadow_fit_no_camera_fallback() {
    // No camera: the fixed +-15 box, so texel size is 30 / resolution.
    auto fit = toy::render::compute_dir_shadow_fit(glm::vec3(0.3f, 0.4f, -1.0f), nullptr, 60.0f, 2048);
    expect(std::abs(fit.texel_world - 30.0f / 2048.0f) < 1e-6f,
           "dir_shadow_fit: no camera falls back to the fixed +-15 box");
    expect(fit.light_space_matrix != glm::mat4(1.0f),
           "dir_shadow_fit: no camera still produces a real light-space matrix");
}

void test_dir_shadow_fit_is_camera_only() {
    // Identical cameras must give an identical fit -- nothing else is an input, which is
    // what keeps a far-off or freefalling scene object from perturbing the shadow frustum.
    auto cam = make_shadow_fit_camera(glm::vec3(0.0f, -10.0f, 4.0f), glm::vec3(0.0f));
    glm::vec3 dir(0.3f, 0.4f, -1.0f);
    auto a = toy::render::compute_dir_shadow_fit(dir, &cam, 60.0f, 2048);
    auto b = toy::render::compute_dir_shadow_fit(dir, &cam, 60.0f, 2048);
    expect(a.light_space_matrix == b.light_space_matrix && a.texel_world == b.texel_world,
           "dir_shadow_fit: same camera -> identical fit");
}

void test_dir_shadow_fit_radius_stable_under_rotation() {
    // The bounding SPHERE of the frustum has a rotation-invariant radius, so orbiting the
    // camera about its own target must not resize the box (texel size tracks the extent).
    glm::vec3 dir(0.3f, 0.4f, -1.0f);
    auto cam_a = make_shadow_fit_camera(glm::vec3(0.0f, -10.0f, 4.0f), glm::vec3(0.0f));
    auto cam_b = make_shadow_fit_camera(glm::vec3(10.0f, 0.0f, 4.0f), glm::vec3(0.0f));
    auto a = toy::render::compute_dir_shadow_fit(dir, &cam_a, 60.0f, 2048);
    auto b = toy::render::compute_dir_shadow_fit(dir, &cam_b, 60.0f, 2048);
    expect(std::abs(a.texel_world - b.texel_world) < 1e-6f,
           "dir_shadow_fit: box size is invariant under camera rotation");
}

void test_dir_shadow_fit_center_snaps_to_texels() {
    // The box centre must land on a whole multiple of the texel size -- that snap is what
    // removes sub-texel shadow crawl as the camera translates.
    glm::vec3 dir(0.3f, 0.4f, -1.0f);
    auto cam = make_shadow_fit_camera(glm::vec3(1.234f, -9.117f, 4.0f), glm::vec3(0.5f, 0.25f, 0.0f));
    const uint32_t resolution = 2048;
    auto fit = toy::render::compute_dir_shadow_fit(dir, &cam, 60.0f, resolution);
    // The box half-extent follows from texel_world (= 2*extent / resolution). The matrix's
    // translation column survives the light rotation (which has none), so orthoRH_ZO's
    // [3][0] = -centre_x / extent recovers the light-space centre, which must be a whole
    // number of texels.
    const float extent   = fit.texel_world * static_cast<float>(resolution) * 0.5f;
    const float centre_x = -fit.light_space_matrix[3][0] * extent;
    const float ratio    = centre_x / fit.texel_world;
    expect(std::abs(ratio - std::round(ratio)) < 1e-2f,
           "dir_shadow_fit: box centre is snapped to a whole texel");
}

void test_dir_shadow_fit_degenerate_shadow_distance() {
    // A zero shadow distance collapses the frustum slice; the near/far separation floor
    // must still leave a usable (non-inverted) depth range.
    glm::vec3 dir(0.0f, 0.0f, -1.0f);
    auto cam = make_shadow_fit_camera(glm::vec3(0.0f, -8.0f, 3.0f), glm::vec3(0.0f));
    auto fit = toy::render::compute_dir_shadow_fit(dir, &cam, 0.0f, 1024);
    expect(fit.texel_world > 0.0f, "dir_shadow_fit: degenerate shadow_distance keeps a positive texel size");
    expect(std::isfinite(fit.light_space_matrix[2][2]) && fit.light_space_matrix[2][2] != 0.0f,
           "dir_shadow_fit: degenerate shadow_distance keeps a finite depth range");
}

void test_pixel_density_orthographic() {
    // ortho_size=5.4, render_height=270 -> 10.8/270 = 0.04 world units/px
    float density = toy::render::compute_pixel_density(true, 5.4f, 270);
    expect(std::fabs(density - 0.04f) < 1e-6f, "pixel_density: orthographic derives world units/px");
}

void test_pixel_density_perspective_disabled() {
    float density = toy::render::compute_pixel_density(false, 5.4f, 270);
    expect(density == 0.0f, "pixel_density: perspective camera disables snapping");
}

void test_sdf_clip_rect_on_screen() {
    // Camera at the origin looking down -Z (identity view); box 5 units in front,
    // well within a 45-degree-FOV frustum at that distance (tan(22.5deg)*5 ~= 2.07).
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    glm::mat4 view_proj = proj; // view = identity
    auto rect = toy::render::compute_sdf_clip_rect(view_proj, glm::vec3(-1, -1, -6), glm::vec3(1, 1, -4));
    expect(rect.visible, "sdf_clip_rect: an on-screen box is visible");
    expect(rect.ndc_min.x > -1.0f && rect.ndc_max.x < 1.0f &&
          rect.ndc_min.y > -1.0f && rect.ndc_max.y < 1.0f,
          "sdf_clip_rect: an on-screen box's rect stays strictly inside NDC bounds");
}

void test_sdf_clip_rect_off_screen() {
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    // Small box, far off to the side of a 5-unit-distant frustum slice -- outside the view cone.
    auto rect = toy::render::compute_sdf_clip_rect(proj, glm::vec3(49.9f, -0.1f, -5.1f), glm::vec3(50.1f, 0.1f, -4.9f));
    expect(!rect.visible, "sdf_clip_rect: a box entirely outside the frustum is culled");
}

void test_sdf_clip_rect_near_plane_straddle() {
    // Camera-space box straddling z=0 (some corners behind the camera, w <= 0) must fall back
    // to the full-screen rect rather than compute a partial (and potentially wrong) bound.
    glm::mat4 proj = glm::perspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    auto rect = toy::render::compute_sdf_clip_rect(proj, glm::vec3(-1, -1, -1), glm::vec3(1, 1, 1));
    expect(rect.visible, "sdf_clip_rect: a near-plane-straddling box stays visible (full-screen fallback)");
    expect(rect.ndc_min == glm::vec2(-1.0f) && rect.ndc_max == glm::vec2(1.0f),
          "sdf_clip_rect: a near-plane-straddling box falls back to the full [-1,1] rect");
}

void test_sdf_clip_rect_to_pixels_full_screen() {
    auto px = toy::render::sdf_clip_rect_to_pixels(glm::vec2(-1.0f), glm::vec2(1.0f), 160, 90);
    expect(px.x == 0 && px.y == 0 && px.w == 160 && px.h == 90,
          "sdf_clip_rect_to_pixels: full NDC rect covers the whole render target");
}

void test_sdf_clip_rect_to_pixels_flips_y() {
    // NDC y in [0, 1] is "up" (this engine's OpenGL-style convention, see
    // CameraComponent::get_projection_matrix()'s doc) -- that must map to the TOP half of the
    // framebuffer (Vulkan pixel space, y = 0 at the top), i.e. the same flip the negative-height
    // viewport applies to rasterized geometry (see sdf_clip_rect_to_pixels()'s own doc).
    auto px = toy::render::sdf_clip_rect_to_pixels(glm::vec2(-1.0f, 0.0f), glm::vec2(0.0f, 1.0f), 160, 90);
    expect(px.x == 0 && px.y == 0 && px.w == 80 && px.h == 45,
          "sdf_clip_rect_to_pixels: NDC top-left quadrant maps to the pixel top-left quadrant");
}

// --- toyengine/scene/camera_controller.h ---
// Pure Scene/SceneObject tests -- no Vulkan device needed, since orbit math
// only touches TransformComponent and (for the tracker case) Scene::find_object().

namespace {

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::scene::CameraController;
using toy::scene::CameraControlMode;

/**
 * @brief Builds a one-object scene with a Transform + CameraController, NOT yet
 * started -- mirrors SceneLoader's real order (every component on an object is
 * configured from YAML before Scene::start() ever runs), so callers should set
 * any cc-> fields they care about (target, tracker, clamps, ...) before calling
 * scene->start() themselves. Getting this order right matters: start() seeds
 * distance/yaw/pitch and the initial smoothed_target_ from whatever `target`/
 * `tracker` already holds, so changing them afterwards would leave that seed
 * (and one frame of exponential lag before the real target catches up) stale.
 */
std::unique_ptr<Scene> make_orbit_scene(CameraController** out_cc, const glm::vec3& seed_pos) {
    auto scene = std::make_unique<Scene>("orbit_test");
    auto obj = std::make_unique<SceneObject>("camera");
    auto* tc = obj->add_component<TransformComponent>();
    tc->transform().set_position(seed_pos);
    *out_cc = obj->add_component<CameraController>();
    scene->add_root_object(std::move(obj));
    return scene;
}

/** @brief True if the camera's local -Z axis points at `target` within `epsilon_deg`. */
bool camera_aims_at(const TransformComponent& tc, const glm::vec3& target, float epsilon_deg = 0.5f) {
    glm::mat4 world = tc.get_world_matrix();
    glm::vec3 pos = glm::vec3(world[3]);
    glm::vec3 forward = -glm::normalize(glm::vec3(world[2]));
    glm::vec3 to_target = target - pos;
    if (glm::length(to_target) < 1e-5f) return true;
    to_target = glm::normalize(to_target);
    float cos_angle = glm::clamp(glm::dot(forward, to_target), -1.0f, 1.0f);
    return glm::degrees(std::acos(cos_angle)) <= epsilon_deg;
}

} // namespace

void test_camera_controller_orbit_aims_at_target() {
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -10.0f, 6.0f));
    cc->target = glm::vec3(0.0f, 0.0f, 0.5f);
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    // start() already seeded distance/yaw/pitch from the initial pose and aimed
    // at the target; update() with zero input should reproduce that exactly.
    scene->update(1.0f / 60.0f);
    expect(camera_aims_at(*tc, cc->target),
          "camera_controller: orbit with zero input still aims at the target");

    // Apply some mouse-driven yaw/pitch and confirm the aim survives it.
    cc->mouse_delta = glm::vec2(37.0f, -12.0f);
    scene->update(1.0f / 60.0f);
    expect(camera_aims_at(*tc, cc->target),
          "camera_controller: orbit after mouse yaw/pitch still aims at the target");
}

void test_camera_controller_pitch_clamp() {
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 2.0f));
    cc->min_pitch_deg = 0.0f;
    cc->max_pitch_deg = 85.0f;
    scene->start();

    // A huge upward mouse motion should saturate at max_pitch_deg, not overshoot or flip.
    cc->mouse_delta = glm::vec2(0.0f, 100000.0f);
    scene->update(1.0f / 60.0f);
    expect(cc->pitch_deg == cc->max_pitch_deg,
          "camera_controller: pitch saturates at max_pitch_deg instead of overshooting");

    cc->mouse_delta = glm::vec2(0.0f, -100000.0f);
    scene->update(1.0f / 60.0f);
    expect(cc->pitch_deg == cc->min_pitch_deg,
          "camera_controller: pitch saturates at min_pitch_deg instead of overshooting");
}

void test_camera_controller_zoom_clamp() {
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 0.0f));
    cc->min_distance = 1.0f;
    cc->max_distance = 10.0f;
    scene->start(); // seed distance ~= 5

    cc->scroll_input = 1000.0f; // zoom in hard
    scene->update(1.0f / 60.0f);
    expect(cc->distance == cc->min_distance,
          "camera_controller: zoom-in clamps at min_distance");

    cc->scroll_input = -1000.0f; // zoom out hard
    scene->update(1.0f / 60.0f);
    expect(cc->distance == cc->max_distance,
          "camera_controller: zoom-out clamps at max_distance");
}

void test_camera_controller_tracker_follow() {
    auto scene = std::make_unique<Scene>("tracker_test");

    auto target_obj = std::make_unique<SceneObject>("target");
    auto* target_tc = target_obj->add_component<TransformComponent>();
    target_tc->transform().set_position(glm::vec3(0.0f, 0.0f, 0.0f));
    scene->add_root_object(std::move(target_obj));

    auto cam_obj = std::make_unique<SceneObject>("camera");
    auto* cam_tc = cam_obj->add_component<TransformComponent>();
    cam_tc->transform().set_position(glm::vec3(0.0f, -5.0f, 2.0f));
    auto* cc = cam_obj->add_component<CameraController>();
    cc->tracker = "target";
    cc->follow_smoothing = 0.0f; // snap instantly
    scene->add_root_object(std::move(cam_obj));

    scene->start();

    // Move the tracked object and confirm the camera re-aims at its new position in one frame.
    scene->root_objects()[0]->get_transform()->transform().set_position(glm::vec3(3.0f, 1.0f, 0.5f));
    scene->update(1.0f / 60.0f);

    expect(camera_aims_at(*cam_tc, glm::vec3(3.0f, 1.0f, 0.5f)),
          "camera_controller: follow_smoothing=0 snaps onto the tracked object in one frame");
}

void test_camera_controller_unresolved_tracker_falls_back() {
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 2.0f));
    cc->tracker = "does_not_exist";
    cc->target = glm::vec3(1.0f, 2.0f, 0.0f);
    scene->start();

    // Should not crash, and should fall back to the static target.
    scene->update(1.0f / 60.0f);
    auto* tc = scene->root_objects()[0]->get_transform();
    expect(camera_aims_at(*tc, cc->target),
          "camera_controller: an unresolved tracker name falls back to the static target");
}

void test_camera_controller_seeds_without_teleport() {
    // A camera placed by hand in YAML (no explicit distance/yaw/pitch) should not
    // jump on frame one -- start() must derive those from the seed transform.
    glm::vec3 seed_pos(0.0f, -10.0f, 6.0f);
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, seed_pos);
    cc->target = glm::vec3(0.0f, 0.0f, 0.5f);
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    glm::vec3 pos_before = glm::vec3(tc->get_world_matrix()[3]);

    scene->update(0.0f); // zero dt, zero input -- must reproduce the seed pose exactly
    glm::vec3 pos_after = glm::vec3(tc->get_world_matrix()[3]);

    expect(glm::length(pos_after - pos_before) < 1e-3f,
          "camera_controller: seeding from the initial transform doesn't teleport on frame one");
}

void test_camera_controller_movement_smoothing_zero_is_instant() {
    // Default movement_smoothing (0) must reproduce the pre-drag behavior exactly:
    // a mouse input is fully applied to the actual camera pose the same frame.
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 0.0f)); // yaw=0, pitch=0, distance=5
    scene->start();
    auto* tc = scene->root_objects()[0]->get_transform();

    cc->mouse_delta = glm::vec2(600.0f, 0.0f); // 600px * 0.15 deg/px = 90 degrees of yaw
    scene->update(1.0f / 60.0f);

    glm::vec3 pos = glm::vec3(tc->get_world_matrix()[3]);
    expect(glm::length(pos - glm::vec3(5.0f, 0.0f, 0.0f)) < 0.01f,
          "camera_controller: movement_smoothing=0 applies mouse input to the pose in the same frame");
}

void test_camera_controller_movement_smoothing_drags_then_converges() {
    // At movement_smoothing=1 (max drag), the same single-frame yaw input should
    // barely move the camera at first, then converge close to the fully-applied
    // pose once given several seconds of simulated time with no further input.
    CameraController* cc = nullptr;
    auto scene = make_orbit_scene(&cc, glm::vec3(0.0f, -5.0f, 0.0f)); // yaw=0, pitch=0, distance=5
    cc->movement_smoothing = 1.0f;
    scene->start();
    auto* tc = scene->root_objects()[0]->get_transform();

    cc->mouse_delta = glm::vec2(600.0f, 0.0f); // -> raw yaw_deg jumps to 90 immediately
    scene->update(1.0f / 60.0f);
    expect(std::fabs(cc->yaw_deg - 90.0f) < 0.01f,
          "camera_controller: raw yaw_deg accumulates the full mouse input in one frame regardless of drag");

    glm::vec3 pos_one_frame = glm::vec3(tc->get_world_matrix()[3]);
    expect(glm::length(pos_one_frame - glm::vec3(0.0f, -5.0f, 0.0f)) < 0.2f,
          "camera_controller: movement_smoothing=1 keeps the camera pose nearly unchanged one frame after a large input");

    cc->mouse_delta = glm::vec2(0.0f); // mouse released; let the drag catch up
    for (int i = 0; i < 900; ++i) scene->update(1.0f / 60.0f); // ~15 simulated seconds
    glm::vec3 pos_converged = glm::vec3(tc->get_world_matrix()[3]);
    expect(glm::length(pos_converged - glm::vec3(5.0f, 0.0f, 0.0f)) < 0.3f,
          "camera_controller: movement_smoothing=1 still converges to the input-driven pose given enough time");
}

// --- KinematicController -------------------------------------------------------------------

/** @brief Builds a one-object scene with a KinematicController seeded at `seed_pos`. */
std::unique_ptr<Scene> make_controller_scene(toy::scene::KinematicController** out_kc,
                                             const glm::vec3& seed_pos) {
    auto scene = std::make_unique<Scene>("kinematic_controller_test");
    auto obj = std::make_unique<SceneObject>("ball");
    obj->add_component<TransformComponent>()->transform().set_position(seed_pos);
    *out_kc = obj->add_component<toy::scene::KinematicController>();
    scene->add_root_object(std::move(obj));
    // The controller moves nothing without this: its motion lives in advance(), driven here at
    // order 50 so it lands ahead of the physics phase. Installing it makes these tests exercise
    // the same path Engine uses, rather than a Behaviour-phase update() that no longer exists.
    toy::scene::install_kinematic_control_system(*scene);
    return scene;
}

void test_kinematic_controller_moves_on_input_and_holds_height() {
    toy::scene::KinematicController* kc = nullptr;
    auto scene = make_controller_scene(&kc, glm::vec3(0.0f, 0.0f, 2.6f));
    kc->move_speed = 4.0f;
    kc->smoothing = 0.0f; // instant, so the travelled distance is exactly speed * time
    scene->start();

    kc->move_input = glm::vec2(1.0f, 0.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);

    auto* tc = scene->root_objects()[0]->get_transform();
    const glm::vec3 p = tc->transform().position();
    expect(std::fabs(p.x - 4.0f) < 0.05f, "kinematic_controller: +X input for 1 s travels move_speed metres");
    expect(std::fabs(p.y) < 1e-5f, "kinematic_controller: no Y drift from pure +X input");
    expect(std::fabs(p.z - 2.6f) < 1e-5f, "kinematic_controller: holds the authored hover height");

    // Diagonal input is CLAMPED, not normalized: full deflection on both axes must not travel
    // faster than full deflection on one.
    kc->move_input = glm::vec2(1.0f, 1.0f);
    const glm::vec3 before = tc->transform().position();
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    const float diagonal = glm::length(tc->transform().position() - before);
    expect(std::fabs(diagonal - 4.0f) < 0.05f,
          "kinematic_controller: diagonal input is clamped to move_speed, not sqrt(2) faster");
}

void test_kinematic_controller_smoothing_ramps_then_converges() {
    toy::scene::KinematicController* kc = nullptr;
    auto scene = make_controller_scene(&kc, glm::vec3(0.0f));
    kc->move_speed = 4.0f;
    kc->smoothing = 8.0f;
    scene->start();

    kc->move_input = glm::vec2(1.0f, 0.0f);
    scene->update(1.0f / 60.0f);
    expect(kc->velocity().x > 0.0f && kc->velocity().x < 4.0f,
          "kinematic_controller: smoothing ramps velocity rather than snapping to move_speed");

    for (int i = 0; i < 300; ++i) scene->update(1.0f / 60.0f);
    expect(std::fabs(kc->velocity().x - 4.0f) < 0.05f,
          "kinematic_controller: smoothed velocity converges to move_speed");

    // Releasing the key must decay back to rest, not stop dead -- PhysicsSystem derives the
    // kinematic body's velocity from this motion, so a discontinuity would jolt anything attached.
    kc->move_input = glm::vec2(0.0f);
    scene->update(1.0f / 60.0f);
    expect(kc->velocity().x > 0.0f && kc->velocity().x < 4.0f,
          "kinematic_controller: releasing input decays velocity instead of stopping instantly");
}

/**
 * @brief The regression test for the clipping bug: physics must see the pose written THIS frame.
 *
 * KinematicControlSystem runs at order 50 and PhysicsSystem at 100, so by the time Scene::update()
 * returns, the body's position must equal the Transform the controller just wrote. If the
 * controller ever drifts back to the Behaviour phase (200), physics spends each frame solving
 * against the PREVIOUS pose while the renderer draws the new one -- invisible for rigid contacts,
 * but it is exactly what made the ball clip through the cloth in cloth_test.
 */
void test_kinematic_control_runs_before_physics() {
    using coopa::physx::components::SphereCollider;
    using coopa::physx::components::RigidbodyComponent;

    Scene scene("kinematic_order_test");
    auto obj = std::make_unique<SceneObject>("ball");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 0.0f, 2.0f));
    obj->add_component<SphereCollider>()->set_radius(1.0f);
    auto* rb = obj->add_component<RigidbodyComponent>();
    rb->is_kinematic = true;
    rb->use_gravity = false;
    auto* kc = obj->add_component<toy::scene::KinematicController>();
    kc->move_speed = 4.0f;
    kc->smoothing = 0.0f;
    SceneObject* ball = obj.get();
    scene.add_root_object(std::move(obj));

    scene.start();
    toy::scene::install_kinematic_control_system(scene);
    auto* phys = coopa::physx::system::install_physics_system(scene);

    const float dt = 1.0f / 60.0f;
    kc->move_input = glm::vec2(1.0f, 0.0f);
    for (int i = 0; i < 30; ++i) {
        kc->move_input = glm::vec2(1.0f, 0.0f); // re-assert; Engine would push this every frame
        scene.update(dt);
        scene.late_update(dt);
    }

    const float transform_x = ball->get_transform()->transform().position().x;
    const coopa::physx::dynamics::Body* body = phys->world().get_body(rb->body_id());
    expect(body != nullptr, "kinematic order: the ball bound to a physics body");
    expect(transform_x > 1.0f, "kinematic order: the controller actually moved the ball");
    if (body) {
        // One frame of lag would be move_speed * dt = 6.7 cm; require far tighter than that.
        expect(std::fabs(body->position.x - transform_x) < 1e-4f,
              "kinematic order: physics saw the pose written this frame, not the previous one");
    }
}

/**
 * @brief Full headless render of the cloth scene: the sheet must actually simulate (its particles
 * move and end up outside the ball) AND that simulation must reach the screen (two frames far
 * apart differ).
 *
 * The rendered-difference half is the part that matters most: everything else about cloth is
 * covered by physxcoopa's own headless suite, but nothing there can catch a broken dynamic vertex
 * buffer -- a mesh uploaded once and never again would still pass every physics assertion while
 * drawing a frozen flat sheet.
 */
void test_headless_render_cloth_scene_simulates_and_animates() {
    toy::core::AppConfig config;
    config.window.title  = "toyengine_tests";
    config.window.width  = 640;
    config.window.height = 360;
    config.window.vsync  = false;
    config.scene.default_scene = "assets/scenes/cloth_test/scene.yaml";
    config.render.render_width  = 320;
    config.render.render_height = 180;
    config.output.save_on_exit = false;

    const std::string early_path = std::string(ROOT_DIR) + "/output/test_cloth_early.png";
    const std::string late_path  = std::string(ROOT_DIR) + "/output/test_cloth_late.png";

    // Pin the tick delta. Without this the Engine runs on wall-clock dt, which headless is a few
    // milliseconds -- so the fixed per-frame displacement below would describe a ~22 m/s ball
    // rather than the 4 m/s the scene is authored for, and most frames would run no physics substep
    // at all. Both make the drape unreproducible. Same knob the world-canvas hover test uses.
    setenv("FIXED_DT", "0.016666667", /*overwrite=*/1);

    {
        toy::core::Engine engine(std::move(config));

        // A fixed dt, so the drape is reproducible rather than wall-clock dependent.
        const float dt = 1.0f / 60.0f;
        for (int i = 0; i < 4; ++i) engine.tick();
        engine.save_screenshot(early_path, /*low_res=*/true);

        auto* cc = engine.scene().find_first_component<coopa::physx::components::ClothComponent>();
        expect(cc != nullptr, "cloth scene: the Cloth component parsed from YAML");
        const coopa::physx::cloth::Cloth* sim = cc ? cc->cloth() : nullptr;
        expect(sim != nullptr, "cloth scene: PhysicsSystem bound a simulated cloth to it");
        expect(sim && sim->particles.size() == 25u * 25u, "cloth scene: resolution 25x25 round-trips from YAML");
        expect(sim && !sim->anchors.empty(), "cloth scene: the ball anchor resolved by name");

        auto* cr = engine.scene().find_first_component<toy::scene::ClothRenderer>();
        expect(cr != nullptr && cr->is_ready(),
              "cloth scene: ClothRenderer built and published its dynamic GPU mesh");

        const float start_min_z = sim ? sim->bounds.min.z : 0.0f;
        for (int i = 0; i < 150; ++i) engine.tick();
        engine.save_screenshot(late_path, /*low_res=*/true);

        sim = cc ? cc->cloth() : nullptr;
        if (sim) {
            expect(sim->bounds.min.z < start_min_z - 0.5f,
                  "cloth scene: the sheet drapes downward over the ball instead of staying flat");
            // The ball is a unit sphere at z = 2.6; no particle may be inside it.
            bool outside = true;
            for (const auto& p : sim->particles) {
                if (glm::length(p.position - glm::vec3(0.0f, 0.0f, 2.6f)) < 1.0f) { outside = false; break; }
            }
            expect(outside, "cloth scene: no particle ends up inside the ball's collider");
        }

        // Now the moving-ball half. The ball's Transform is written directly rather than through
        // its KinematicController: Engine::drive_kinematic_controllers_() re-reads the keyboard and
        // overwrites move_input at the top of every tick(), so a value poked in from outside can
        // never survive to Scene::update() -- and a headless test has no keyboard to press. The
        // input -> move_input -> Transform half is covered by the two KinematicController tests
        // above; what only this test can cover is everything BELOW the Transform write, which is
        // exactly what is exercised here: Transform -> PhysicsSystem's derived kinematic velocity
        // -> cloth anchors -> particles -> the uploaded vertex buffer.
        expect(engine.scene().find_first_component<toy::scene::KinematicController>() != nullptr,
              "cloth scene: the ball has a KinematicController");
        auto* ball = engine.scene().find_object("ball");
        expect(ball != nullptr, "cloth scene: the ball object resolves by name");
        const glm::vec3 ball_before = ball ? ball->get_transform()->transform().position() : glm::vec3(0.0f);
        float cloth_x_before = 0.0f;
        if (sim) {
            for (const auto& p : sim->particles) cloth_x_before += p.position.x;
            cloth_x_before /= static_cast<float>(sim->particles.size());
        }
        // Worst penetration over EVERY moving frame, not just the final one: the clipping this
        // guards against is transient by nature (it appears while the ball travels and vanishes the
        // moment it stops), so sampling only the end state would miss it entirely.
        float worst_clearance = 1e9f;
        for (int i = 0; i < 120; ++i) {
            if (ball) {
                coopa::util::Transform& t = ball->get_transform()->transform();
                t.set_position(t.position() + glm::vec3(4.0f / 60.0f, 0.0f, 0.0f));
            }
            engine.tick();
            // Measured against the pose the ball was DRAWN at this frame -- the whole bug was that
            // this differs from the pose the cloth was solved against.
            const glm::vec3 drawn = ball ? ball->get_transform()->transform().position() : glm::vec3(0.0f);
            if (const auto* live = cc ? cc->cloth() : nullptr) {
                for (const auto& p : live->particles) {
                    worst_clearance = std::min(worst_clearance, glm::length(p.position - drawn) - 1.0f);
                }
            }
        }
        expect(worst_clearance > 0.0f,
              "cloth scene: no particle ever enters the ball at the pose it is rendered at");
        if (worst_clearance <= 0.0f) {
            std::cerr << "       worst clearance was " << worst_clearance << " m\n";
        }

        const glm::vec3 ball_after = ball ? ball->get_transform()->transform().position() : glm::vec3(0.0f);
        expect(ball_after.x > ball_before.x + 0.5f, "cloth scene: the ball travels along +X");
        expect(std::fabs(ball_after.z - ball_before.z) < 1e-4f,
              "cloth scene: the ball holds its hover height while moving");

        sim = cc ? cc->cloth() : nullptr;
        if (sim) {
            float cloth_x_after = 0.0f;
            for (const auto& p : sim->particles) cloth_x_after += p.position.x;
            cloth_x_after /= static_cast<float>(sim->particles.size());
            expect(cloth_x_after > cloth_x_before + 0.4f, "cloth scene: the sheet travels with the ball");
            expect(std::fabs(cloth_x_after - ball_after.x) < 1.0f,
                  "cloth scene: the sheet trails the ball rather than being left behind");
            bool outside = true;
            for (const auto& p : sim->particles) {
                if (glm::length(p.position - ball_after) < 1.0f) { outside = false; break; }
            }
            expect(outside, "cloth scene: no particle penetrates the ball while it is moving");
        }
        (void)dt;
    }

    unsetenv("FIXED_DT");

    int w1 = 0, h1 = 0, c1 = 0, w2 = 0, h2 = 0, c2 = 0;
    uint8_t* early = stbi_load(early_path.c_str(), &w1, &h1, &c1, 4);
    uint8_t* late  = stbi_load(late_path.c_str(),  &w2, &h2, &c2, 4);
    expect(early != nullptr && late != nullptr, "cloth scene: both screenshots round-trip through stb_image");

    if (early && late && w1 == w2 && h1 == h2) {
        int diff = 0;
        for (int i = 0; i < w1 * h1 * 4; ++i) {
            if (early[i] != late[i]) ++diff;
        }
        expect(diff > w1 * h1 / 20,
              "cloth scene: the dynamic vertex buffer reaches the screen (frames 4 and 154 differ substantially)");
    }

    if (early) stbi_image_free(early);
    if (late)  stbi_image_free(late);
    std::filesystem::remove(early_path);
    std::filesystem::remove(late_path);
}

/**
 * @brief Full headless render through a real Vulkan device: constructs an
 * Engine against the demo scene with a palette configured, ticks it a few
 * frames, and asserts the saved low-res buffer is exactly the configured
 * dimensions and every pixel is a member of the palette. This one assertion
 * validates the render path, the outline/dither/quantize post pass, and the
 * UNORM/gamma choice all at once -- see gfxcoopa's pixel_stylize_pass.h file doc.
 */
void test_headless_render_matches_palette() {
    toy::core::AppConfig config;
    config.window.title  = "toyengine_tests";
    config.window.width  = 640;
    config.window.height = 360;
    config.window.vsync  = false;
    config.scene.default_scene = "assets/scenes/pixel_demo/scene.yaml";
    config.render.render_width  = 160;
    config.render.render_height = 90;
    config.render.palette_path    = "assets/palettes/pico8.png";
    config.render.dither_strength = 0.08f;
    config.output.save_on_exit = false;

    static const uint8_t kPalette[8][3] = {
        {0, 0, 0}, {29, 43, 83}, {126, 37, 83}, {0, 135, 81},
        {171, 82, 54}, {95, 87, 79}, {194, 195, 199}, {255, 241, 232},
    };

    std::string out_path = std::string(ROOT_DIR) + "/output/test_frame.png";

    {
        toy::core::Engine engine(std::move(config));
        for (int i = 0; i < 3; ++i) engine.tick();
        engine.save_screenshot(out_path, /*low_res=*/true);
    }

    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = stbi_load(out_path.c_str(), &w, &h, &channels, 4);
    expect(pixels != nullptr, "headless render: screenshot round-trips through stb_image");
    if (!pixels) return;

    expect(w == 160 && h == 90, "headless render: low-res buffer has the configured dimensions");

    bool all_in_palette = true;
    for (int i = 0; i < w * h; ++i) {
        uint8_t r = pixels[i * 4 + 0], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2];
        bool found = false;
        for (const auto& entry : kPalette) {
            if (entry[0] == r && entry[1] == g && entry[2] == b) { found = true; break; }
        }
        if (!found) { all_in_palette = false; break; }
    }
    expect(all_in_palette, "headless render: every output pixel matches a palette entry");

    stbi_image_free(pixels);
    std::filesystem::remove(out_path);
}

/**
 * @brief Headless render with every independently-toggleable feature turned off
 * (outline, palette, dither, ssao, ssr), mirroring
 * test_headless_render_matches_palette() but exercising the opposite branch of
 * every toggle at once: SsaoPass::invalidate_history() instead of execute(),
 * no HiZPass/SceneColorMipPass/SsrPass construction at all, the manual
 * gbuffer-depth transition instead of HiZPass's, and pixel_stylize_pass_ reading
 * offscreen_target_ directly instead of ssr_pass_'s composite output. Ticks a
 * few frames and asserts the saved buffer has the configured dimensions and
 * contains at least one non-black pixel -- catching the kind of bug an
 * all-zero G-buffer read, a missing descriptor bind, or a null pass pointer
 * dereference in one of those "off" branches would produce.
 */
void test_headless_render_with_all_toggles_off() {
    toy::core::AppConfig config;
    config.window.title  = "toyengine_tests";
    config.window.width  = 640;
    config.window.height = 360;
    config.window.vsync  = false;
    config.scene.default_scene = "assets/scenes/pixel_demo/scene.yaml";
    config.render.render_width  = 160;
    config.render.render_height = 90;
    config.render.outline_enabled = false;
    config.render.palette_enabled = false;
    config.render.dither_enabled  = false;
    config.render.ssao_enabled    = false;
    config.render.ssr_enabled     = false;
    config.output.save_on_exit = false;

    std::string out_path = std::string(ROOT_DIR) + "/output/test_frame_toggles_off.png";

    {
        toy::core::Engine engine(std::move(config));
        for (int i = 0; i < 5; ++i) engine.tick();
        engine.save_screenshot(out_path, /*low_res=*/true);
    }

    int w = 0, h = 0, channels = 0;
    uint8_t* pixels = stbi_load(out_path.c_str(), &w, &h, &channels, 4);
    expect(pixels != nullptr, "all-toggles-off headless render: screenshot round-trips through stb_image");
    if (!pixels) return;

    expect(w == 160 && h == 90, "all-toggles-off headless render: low-res buffer has the configured dimensions");

    bool any_nonblack = false;
    for (int i = 0; i < w * h; ++i) {
        if (pixels[i * 4 + 0] != 0 || pixels[i * 4 + 1] != 0 || pixels[i * 4 + 2] != 0) {
            any_nonblack = true;
            break;
        }
    }
    expect(any_nonblack, "all-toggles-off headless render: output is not entirely black");

    stbi_image_free(pixels);
    std::filesystem::remove(out_path);
}

/**
 * @brief Renders the demo scene (which includes an opaque SdfRenderer blob and a BLEND
 * SdfRenderer sphere -- see assets/scenes/pixel_demo/scene.yaml's sdf_blob/sdf_glass) twice,
 * identically except for sdf_enabled, and asserts the two frames differ. Doesn't depend on
 * knowing exactly which pixels the SDF objects land on (the orbiting CameraController makes
 * that fragile to hardcode) -- only that turning the whole system off measurably changes the
 * rendered output, which would fail if the SDF gather, any of the four SDF passes, or the
 * scissor/AABB culling silently no-op'd.
 */
void test_headless_render_sdf_toggle_changes_output() {
    auto render_with_sdf = [](bool sdf_enabled) {
        toy::core::AppConfig config;
        config.window.title  = "toyengine_tests";
        config.window.width  = 640;
        config.window.height = 360;
        config.window.vsync  = false;
        config.scene.default_scene = "assets/scenes/pixel_demo/scene.yaml";
        config.render.render_width  = 160;
        config.render.render_height = 90;
        config.render.sdf_enabled = sdf_enabled;
        config.output.save_on_exit = false;

        std::string out_path = std::string(ROOT_DIR) + "/output/test_frame_sdf_" +
            std::string(sdf_enabled ? "on" : "off") + ".png";
        {
            toy::core::Engine engine(std::move(config));
            for (int i = 0; i < 3; ++i) engine.tick();
            engine.save_screenshot(out_path, /*low_res=*/true);
        }
        return out_path;
    };

    std::string path_on  = render_with_sdf(true);
    std::string path_off = render_with_sdf(false);

    int w1 = 0, h1 = 0, c1 = 0, w2 = 0, h2 = 0, c2 = 0;
    uint8_t* px_on  = stbi_load(path_on.c_str(), &w1, &h1, &c1, 4);
    uint8_t* px_off = stbi_load(path_off.c_str(), &w2, &h2, &c2, 4);
    expect(px_on != nullptr && px_off != nullptr, "sdf toggle: both screenshots round-trip through stb_image");

    if (px_on && px_off && w1 == w2 && h1 == h2) {
        int diff_count = 0;
        for (int i = 0; i < w1 * h1 * 4; ++i) {
            if (px_on[i] != px_off[i]) ++diff_count;
        }
        expect(diff_count > 0, "sdf_enabled toggle measurably changes the rendered frame");
    }

    if (px_on)  stbi_image_free(px_on);
    if (px_off) stbi_image_free(px_off);
    std::filesystem::remove(path_on);
    std::filesystem::remove(path_off);
}


/// Renders assets/scenes/world_canvas_test/scene.yaml with world_ui_enabled on and off. The
/// scene's two WorldSpace canvases (a camera-facing health bar over the cube, and a
/// Transform-mode plate) are the only intended difference between the two frames, so a
/// substantial region of changed pixels proves the world-space UI reached the captured image
/// -- i.e. that CanvasComponent laid out, emitted, and UiWorldPass actually drew it as 3D
/// geometry, and that the composite stage put it back on top of the frame.
///
/// Captures at DISPLAY resolution (low_res = false). The world UI is no longer part of the
/// low-resolution image: it renders into its own layer and is composited over the frame after
/// AA and tilt shift, so low_res_color_image() is scene-only by construction now (see
/// PixelRenderPipeline::overlay_target_). A low-res capture here would compare two frames that
/// genuinely are identical.
///
/// The comparison counts only pixels that differ by more than kNoiseTolerance in some channel.
/// Two Engines tick on real wall-clock dt, which reaches the shaders as gfx_time, and that
/// alone moves roughly a thousand pixels of sky by +-1 between any two runs -- a raw
/// byte-inequality count sits above that floor whether or not the UI drew at all, which is
/// exactly the kind of pass this test must not hand out.
///
/// Same A/B shape as test_headless_render_sdf_toggle_changes_output() above, and for the same
/// reason it uses two separate Engines: world_ui_enabled is startup-fixed (it decides whether
/// the UI pipelines and the scene-depth descriptor get built at all), so it cannot be flipped
/// on a live pipeline.
///
/// aa_mode is left at its "off" default deliberately -- TAA's non-reprojecting history clamp
/// would ghost a canvas moving under the scene's auto-orbiting camera and make the comparison
/// depend on frame count.
void test_headless_render_world_canvas_changes_output() {
    auto render_with_world_ui = [](bool world_ui_enabled) {
        toy::core::AppConfig config;
        config.window.title  = "toyengine_tests";
        config.window.width  = 640;
        config.window.height = 360;
        config.window.vsync  = false;
        config.scene.default_scene = "assets/scenes/world_canvas_test/scene.yaml";
        config.render.render_width  = 160;
        config.render.render_height = 90;
        config.render.world_ui_enabled = world_ui_enabled;
        config.output.save_on_exit = false;

        std::string out_path = std::string(ROOT_DIR) + "/output/test_frame_world_ui_" +
            std::string(world_ui_enabled ? "on" : "off") + ".png";
        {
            toy::core::Engine engine(std::move(config));
            // Three ticks, matching the SDF test: frame 0 has no temporal history, and the
            // HealthDriver needs a tick or two for the bar to leave its full-health state.
            for (int i = 0; i < 3; ++i) engine.tick();
            engine.save_screenshot(out_path, /*low_res=*/false);
        }
        return out_path;
    };

    std::string path_on  = render_with_world_ui(true);
    std::string path_off = render_with_world_ui(false);

    int w1 = 0, h1 = 0, c1 = 0, w2 = 0, h2 = 0, c2 = 0;
    uint8_t* px_on  = stbi_load(path_on.c_str(), &w1, &h1, &c1, 4);
    uint8_t* px_off = stbi_load(path_off.c_str(), &w2, &h2, &c2, 4);
    expect(px_on != nullptr && px_off != nullptr,
           "world canvas: both screenshots round-trip through stb_image");

    if (px_on && px_off && w1 == w2 && h1 == h2) {
        // Above the +-1 gfx_time noise floor described above, below any real UI coverage.
        constexpr int kNoiseTolerance = 8;
        int diff_count = 0;
        for (int i = 0; i < w1 * h1; ++i) {
            for (int ch = 0; ch < 3; ++ch) { // RGB; alpha is a constant 255 in both
                if (std::abs(static_cast<int>(px_on[i * 4 + ch]) -
                             static_cast<int>(px_off[i * 4 + ch])) > kNoiseTolerance) {
                    ++diff_count;
                    break;
                }
            }
        }
        expect(diff_count > 0, "world_ui_enabled toggle measurably changes the rendered frame");
        // A bar-plus-label canvas covers a real chunk of a 640x360 frame. Requiring a
        // meaningful area (rather than diff_count > 0 alone) is what keeps this test honest
        // if the canvas ever regresses to a stray pixel or two of edge difference.
        expect(diff_count > 2000, "world canvas covers a substantial area of the frame");
    }

    if (px_on)  stbi_image_free(px_on);
    if (px_off) stbi_image_free(px_off);
    std::filesystem::remove(path_on);
    std::filesystem::remove(path_off);
}

/// Parks the pointer on the world-space "Heal" Button in world_canvas_test and checks it
/// actually lights up -- the end-to-end proof that world-space UI is INTERACTIVE, not just
/// drawn.
///
/// Everything between a window pixel and a tinted button is exercised here and nowhere else:
/// window pixel -> letterbox rect -> internal render extent -> NDC (through the
/// negative-height viewport convention) -> world ray -> ray/plane intersection against the
/// canvas -> canvas pixels -> Raycaster -> EventSystem -> Button::on_pointer_enter ->
/// ColorTransition. uicoopa's own headless tests cover the ray/plane maths exactly; only a
/// real render can cover the rest of that chain, because the letterbox and viewport
/// conventions live in this repo.
///
/// The A/B is the same pointer position logic either way, differing only in WHERE it points,
/// so a failure means the mapping is wrong rather than that the UI is missing entirely (which
/// is what test_headless_render_world_canvas_changes_output() above covers).
void test_headless_render_world_canvas_button_hover() {
    // Window pixels. The scene's camera auto-orbits, so a tiny FIXED_DT keeps the button
    // essentially still across the three ticks while still letting the frame advance.
    auto render_with_cursor = [](const char* cursor_pos, const char* tag) {
        setenv("CURSOR_POS", cursor_pos, /*overwrite=*/1);
        setenv("FIXED_DT", "0.0005", 1);
        setenv("NO_INPUT", "1", 1);

        toy::core::AppConfig config;
        config.window.title  = "toyengine_tests";
        config.window.width  = 1920;
        config.window.height = 1080;
        config.window.vsync  = false;
        config.scene.default_scene = "assets/scenes/world_canvas_test/scene.yaml";
        config.render.render_width  = 1440;
        config.render.render_height = 960;
        config.output.save_on_exit = false;

        std::string out_path = std::string(ROOT_DIR) + "/output/test_frame_hover_" + tag + ".png";
        {
            toy::core::Engine engine(std::move(config));
            // Three ticks: Button's colour chase runs in update(), a phase EARLIER than the
            // late_update() that detects the hover, so the tint can only land from the second
            // frame onward however short fade_duration is.
            for (int i = 0; i < 3; ++i) engine.tick();
            engine.save_screenshot(out_path, /*low_res=*/false);
        }
        unsetenv("CURSOR_POS");
        unsetenv("FIXED_DT");
        unsetenv("NO_INPUT");
        return out_path;
    };

    // (1097, 407) in window pixels is the Heal button; (850, 760) is empty floor below it.
    std::string path_on  = render_with_cursor("1097,407", "on");
    std::string path_off = render_with_cursor("850,760",  "off");

    int w1 = 0, h1 = 0, c1 = 0, w2 = 0, h2 = 0, c2 = 0;
    uint8_t* px_on  = stbi_load(path_on.c_str(),  &w1, &h1, &c1, 4);
    uint8_t* px_off = stbi_load(path_off.c_str(), &w2, &h2, &c2, 4);
    expect(px_on != nullptr && px_off != nullptr,
           "world canvas hover: both screenshots round-trip through stb_image");

    if (px_on && px_off && w1 == w2 && h1 == h2) {
        // Count pixels close to the button's HIGHLIGHTED colour rather than sampling one
        // hardcoded coordinate. The saved image's size depends on config (it is the display
        // rect only when tilt-shift is on, the render extent otherwise) and the scene's camera
        // auto-orbits, so a fixed pixel index is exactly the kind of assertion that rots.
        //
        // Scene colours: normal (0.20, 0.42, 0.30), highlighted (0.45, 0.92, 0.60). World UI
        // is composited AFTER tonemapping, so these reach the framebuffer very nearly 1:1.
        auto count_highlight = [](const uint8_t* px, int w, int h) {
            int n = 0;
            for (int i = 0; i < w * h; ++i) {
                int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
                if (std::abs(r - 115) < 26 && std::abs(g - 235) < 26 && std::abs(b - 153) < 26) ++n;
            }
            return n;
        };
        int on_count  = count_highlight(px_on,  w1, h1);
        int off_count = count_highlight(px_off, w2, h2);

        // The button is 34x13 canvas pixels on a 150x38 canvas; at this render extent that is
        // a few hundred framebuffer pixels, comfortably clear of any stray match in the scene.
        expect(off_count < 50,
               "un-hovered world-space button stays its normal colour");
        expect(on_count > 200,
               "hovered world-space button lights up (ray -> canvas -> EventSystem works)");
        expect(on_count > off_count * 4 + 100,
               "hover is a large, unambiguous change, not noise");
    }

    if (px_on)  stbi_image_free(px_on);
    if (px_off) stbi_image_free(px_off);
    std::filesystem::remove(path_on);
    std::filesystem::remove(path_off);
}

/// Renders assets/scenes/material_maps_test/scene_flat.yaml and scene_mapped.yaml -- the SAME
/// lit cube, byte-identical scenes but for scene_mapped's three texture_albedo/texture_normal/
/// texture_metallic_roughness keys (see those files' own comments) -- and asserts the two
/// frames differ. Exercises the whole path end to end: TextureLoader's sRGB/linear color-space
/// split, MaterialTextureCache's 4-binding material set, and gbuffer_fs.glsl's albedo/normal/MR
/// sampling. Same A/B shape as test_headless_render_sdf_toggle_changes_output() above.
void test_headless_render_material_maps_change_output() {
    auto render_scene = [](const char* scene_path, const char* tag) {
        toy::core::AppConfig config;
        config.window.title  = "toyengine_tests";
        config.window.width  = 640;
        config.window.height = 360;
        config.window.vsync  = false;
        config.scene.default_scene = scene_path;
        config.render.render_width  = 160;
        config.render.render_height = 90;
        config.output.save_on_exit = false;

        std::string out_path = std::string(ROOT_DIR) + "/output/test_frame_material_maps_" + tag + ".png";
        {
            toy::core::Engine engine(std::move(config));
            for (int i = 0; i < 3; ++i) engine.tick();
            engine.save_screenshot(out_path, /*low_res=*/true);
        }
        return out_path;
    };

    std::string path_flat   = render_scene("assets/scenes/material_maps_test/scene_flat.yaml",   "flat");
    std::string path_mapped = render_scene("assets/scenes/material_maps_test/scene_mapped.yaml", "mapped");

    int w1 = 0, h1 = 0, c1 = 0, w2 = 0, h2 = 0, c2 = 0;
    uint8_t* px_flat   = stbi_load(path_flat.c_str(),   &w1, &h1, &c1, 4);
    uint8_t* px_mapped = stbi_load(path_mapped.c_str(), &w2, &h2, &c2, 4);
    expect(px_flat != nullptr && px_mapped != nullptr,
          "material maps: both screenshots round-trip through stb_image");

    if (px_flat && px_mapped && w1 == w2 && h1 == h2) {
        int diff_count = 0;
        for (int i = 0; i < w1 * h1 * 4; ++i) {
            if (px_flat[i] != px_mapped[i]) ++diff_count;
        }
        expect(diff_count > 0, "albedo/normal/metallic_roughness maps measurably change the rendered frame");
    }

    if (px_flat)   stbi_image_free(px_flat);
    if (px_mapped) stbi_image_free(px_mapped);
    std::filesystem::remove(path_flat);
    std::filesystem::remove(path_mapped);
}

} // namespace

int main() {
    test_letterbox_exact_fit();
    test_letterbox_with_bars();
    test_letterbox_undersized_window_clamps_to_scale_1();
    test_fit_pillarbox_only();
    test_fit_exact_match_fills_completely();
    test_fit_beats_integer_bars();
    test_fit_undersized_window();
    test_display_rect_dispatches_on_upscale_mode();
    test_render_resolution_fixed_mode();
    test_render_resolution_divisor_mode();
    test_pixel_render_config_aa_defaults();
    test_app_config_load_round_trips_aa_settings();
    test_pixel_render_config_dof_defaults();
    test_app_config_load_round_trips_dof_settings();
    test_dof_view_space_depth();
    test_dof_focus_smoothing();
    test_pixel_render_config_soft_shadow_defaults();
    test_app_config_load_round_trips_soft_shadow_settings();
    test_directional_light_shadow_intensity_default();
    test_dir_shadow_fit_no_camera_fallback();
    test_dir_shadow_fit_is_camera_only();
    test_dir_shadow_fit_radius_stable_under_rotation();
    test_dir_shadow_fit_center_snaps_to_texels();
    test_dir_shadow_fit_degenerate_shadow_distance();
    test_pixel_density_orthographic();
    test_pixel_density_perspective_disabled();
    test_sdf_clip_rect_on_screen();
    test_sdf_clip_rect_off_screen();
    test_sdf_clip_rect_near_plane_straddle();
    test_sdf_clip_rect_to_pixels_full_screen();
    test_sdf_clip_rect_to_pixels_flips_y();
    test_camera_controller_orbit_aims_at_target();
    test_camera_controller_pitch_clamp();
    test_camera_controller_zoom_clamp();
    test_camera_controller_tracker_follow();
    test_camera_controller_unresolved_tracker_falls_back();
    test_camera_controller_seeds_without_teleport();
    test_camera_controller_movement_smoothing_zero_is_instant();
    test_camera_controller_movement_smoothing_drags_then_converges();
    test_headless_render_matches_palette();
    test_headless_render_with_all_toggles_off();
    test_headless_render_sdf_toggle_changes_output();
    test_headless_render_world_canvas_changes_output();
    test_headless_render_world_canvas_button_hover();
    test_headless_render_material_maps_change_output();
    test_kinematic_controller_moves_on_input_and_holds_height();
    test_kinematic_controller_smoothing_ramps_then_converges();
    test_kinematic_control_runs_before_physics();
    test_headless_render_cloth_scene_simulates_and_animates();

    if (g_failures > 0) {
        std::cerr << "\n" << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "\nAll tests passed.\n";
    return 0;
}
