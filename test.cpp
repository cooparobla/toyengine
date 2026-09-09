// toyengine test suite -- registered with ctest via CMakeLists.txt's
// enable_testing()/add_test().
//
// Declarations only for stb_image (used below to verify save_screenshot()'s
// output) -- the implementation is compiled once in gfxcoopa's src/gfx_impl.cpp.
#include <stb/stb_image.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

#include <toyengine/core/engine.h>
#include <toyengine/render/pixel_math.h>

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
    uint32_t w = 0, h = 0;
    toy::render::compute_render_resolution(cfg, 1920, 1080, w, h);
    expect(w == 480 && h == 270, "render_resolution: fixed mode ignores swapchain size");
}

void test_render_resolution_divisor_mode() {
    toy::render::PixelRenderConfig cfg;
    cfg.resolution_mode = "divisor";
    cfg.scale_divisor = 4;
    uint32_t w = 0, h = 0;
    toy::render::compute_render_resolution(cfg, 1920, 1080, w, h);
    expect(w == 480 && h == 270, "render_resolution: divisor mode divides swapchain size");
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

    if (g_failures > 0) {
        std::cerr << "\n" << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "\nAll tests passed.\n";
    return 0;
}
