/**
 * @file test.cpp
 * @brief toyengine's test suite -- registered with ctest one GROUP at a time (see
 * CMakeLists.txt's add_test() calls), so `ctest -j` runs the expensive render groups as
 * parallel processes and a dev can run the Vulkan-free groups on every save.
 *
 * Run it directly for anything finer-grained:
 * @code
 * ./toyengine_tests                     # everything
 * ./toyengine_tests --list              # names and groups, run nothing
 * ./toyengine_tests --group math        # one group (math/config/scene are instant, no GPU)
 * ./toyengine_tests camera cloth        # every test whose name contains "camera" or "cloth"
 * ./toyengine_tests -v --group render_ui # print each assertion, not just the failures
 * @endcode
 *
 * ## Three conventions every render test here follows
 *
 * **1. Nothing visible, nothing grabbed.** `window.visible = false` (see make_test_config())
 * means the window is never mapped, so it cannot appear or take focus, and Engine stands its
 * cursor capture down -- a render test must not steal the pointer of whoever is using the
 * machine while it runs. NO_INPUT=1 completes it: the real keyboard and mouse are ignored, so
 * a frame is the same whatever the desktop is doing.
 *
 * **2. FIXED_DT=0 makes A/B comparisons exact.** With no time advancing and the temporal
 * SSAO/SSR resolves off, two captures of an unchanged config are byte-identical -- which
 * test_pixel_demo_toggles() asserts outright before trusting any of its diffs. That is what
 * lets this file compare frames with `== 0` and real area thresholds instead of the invented
 * noise tolerance a wall-clock-driven capture needs. Only the cloth test opts out: it needs
 * time to actually pass (FIXED_DT=1/60).
 *
 * **3. One Engine per scene, toggles flipped live.** An Engine is a window, a Vulkan device,
 * every pipeline in the frame graph and a fully loaded scene; constructing one to change one
 * bool is the most expensive way to do it. `outline_enabled`, `palette_enabled`,
 * `dither_enabled` and `sdf_enabled` are all re-read per frame, so Engine::render_config()
 * flips them on a live pipeline (see PixelRenderPipeline::render_config_mut()). The
 * STARTUP-FIXED toggles -- ssao/ssr/world_ui/screen_ui and friends -- genuinely cannot be, so
 * test_headless_render_with_all_toggles_off() keeps its own Engine to cover their "off"
 * construction branch.
 *
 * Captures never touch the disk: Engine::capture_image() hands back the pixels, and a PNG is
 * only written when an assertion FAILS, into a temp directory whose path is printed.
 */

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <gfxcoopa/util/image_readback.h>

#include <toyengine/core/engine.h>
#include <toyengine/render/pixel_math.h>
#include <toyengine/scene/cloth_renderer.h>
#include <toyengine/scene/health_driver.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/kinematic_controller.h>
#include <toyengine/scene/kinematic_mover.h>

#include <root_directory.h>

namespace {

// =====================================================================================
// Test runner
// =====================================================================================

int         g_total_failures = 0;  /**< Failures across the whole run; main()'s exit code. */
int         g_test_failures  = 0;  /**< Failures in the currently-running test. */
int         g_assertions     = 0;  /**< Assertions in the currently-running test. */
bool        g_verbose        = false;
const char* g_current_test   = "";

/**
 * @brief Records one assertion. Failures always print; passes only under -v.
 *
 * Quiet by default on purpose: the old suite printed an `[ OK ]` line per assertion, several
 * hundred of them, which buried the one line that mattered and hid which test was slow.
 */
void expect(bool condition, const std::string& what) {
    ++g_assertions;
    if (!condition) {
        std::cerr << "  [FAIL] " << g_current_test << ": " << what << "\n";
        ++g_test_failures;
        ++g_total_failures;
    } else if (g_verbose) {
        std::cout << "  [ OK ] " << what << "\n";
    }
}

/** @brief expect() for floats, printing both values on failure so a near-miss is diagnosable. */
void expect_near(float actual, float expected, float tolerance, const std::string& what) {
    const bool ok = std::fabs(actual - expected) <= tolerance;
    if (!ok) {
        std::cerr << "  [FAIL] " << g_current_test << ": " << what << " (got " << actual
                  << ", expected " << expected << " +-" << tolerance << ")\n";
        ++g_assertions;
        ++g_test_failures;
        ++g_total_failures;
        return;
    }
    expect(true, what);
}

/** @brief expect() for a count against a lower bound, printing the count on failure. */
void expect_at_least(long long actual, long long minimum, const std::string& what) {
    if (actual < minimum) {
        std::cerr << "  [FAIL] " << g_current_test << ": " << what << " (got " << actual
                  << ", needed >= " << minimum << ")\n";
        ++g_assertions;
        ++g_test_failures;
        ++g_total_failures;
        return;
    }
    expect(true, what);
    if (g_verbose) std::cout << "         (" << actual << ", needed >= " << minimum << ")\n";
}

// =====================================================================================
// Scratch files -- temp directory only, never the repo
// =====================================================================================

/**
 * @brief The one directory this suite is allowed to write to: `<tmp>/toyengine_tests`.
 *
 * Deliberately NOT the repo's output/ (where these tests used to drop PNGs and scratch YAML):
 * output/ is where a real run's captures land, and a test that litters it makes those harder
 * to tell apart -- and a test killed halfway through leaves the litter behind.
 */
const std::filesystem::path& tmp_dir() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path d = std::filesystem::temp_directory_path() / "toyengine_tests";
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

/** @brief A path under tmp_dir(), for a scratch config file or a dumped frame. */
std::string tmp_path(std::string_view name) {
    return (tmp_dir() / name).string();
}

using Frame = coopa::gfx::util::ImageData;

/**
 * @brief Writes a captured frame to tmp_dir() and prints where it went.
 *
 * Only ever called on the failure path: an image assertion that fails is nearly impossible to
 * diagnose from a pixel count alone, and equally not worth a file when it passes.
 */
void dump_frame(const Frame& frame, std::string_view name) {
    const std::string path = tmp_path(std::string(name) + ".png");
    coopa::gfx::util::save_image_png(frame, path);
    std::cerr << "         wrote " << path << "\n";
}

// =====================================================================================
// Environment overrides
// =====================================================================================

/**
 * @brief Sets an env var for a scope and restores it after -- the RAII form of Engine's
 * FIXED_DT / NO_INPUT / CURSOR_POS knobs.
 *
 * The hand-written setenv/unsetenv pairs this replaces leaked on every early return, so a
 * FIXED_DT set by one test could silently change the meaning of a later one.
 */
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            had_previous_ = true;
            previous_     = old;
        }
        setenv(name_, value.c_str(), /*overwrite=*/1);
    }

    ~ScopedEnv() {
        if (had_previous_) setenv(name_, previous_.c_str(), 1);
        else               unsetenv(name_);
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    bool        had_previous_ = false;
    std::string previous_;
};

// =====================================================================================
// Frame comparison helpers
// =====================================================================================

/** @brief True if two captures describe the same image geometry (so a per-pixel loop is valid). */
bool same_extent(const Frame& a, const Frame& b) {
    return a.width == b.width && a.height == b.height && a.channels == b.channels;
}

/**
 * @brief Pixels whose R, G or B differs by more than `tolerance`.
 *
 * Counts PIXELS, not bytes (the old byte counts made a 1-channel shift look like four), and
 * ignores alpha, which is a constant 255 in every target this compares.
 */
long long count_diff(const Frame& a, const Frame& b, int tolerance = 0) {
    if (!same_extent(a, b)) return -1;
    long long n = 0;
    const size_t pixels = static_cast<size_t>(a.width) * a.height;
    for (size_t i = 0; i < pixels; ++i) {
        for (int ch = 0; ch < 3; ++ch) {
            const int d = static_cast<int>(a.pixels[i * a.channels + ch]) -
                          static_cast<int>(b.pixels[i * b.channels + ch]);
            if (std::abs(d) > tolerance) { ++n; break; }
        }
    }
    return n;
}

/** @brief Pixels within `tolerance` of an 8-bit RGB colour, per channel. */
long long count_near_color(const Frame& f, int r, int g, int b, int tolerance) {
    long long n = 0;
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        const int pr = f.pixels[i * f.channels + 0];
        const int pg = f.pixels[i * f.channels + 1];
        const int pb = f.pixels[i * f.channels + 2];
        if (std::abs(pr - r) <= tolerance && std::abs(pg - g) <= tolerance &&
            std::abs(pb - b) <= tolerance) {
            ++n;
        }
    }
    return n;
}

/** @brief Pixels that are not exactly black. */
long long count_nonblack(const Frame& f) {
    long long n = 0;
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        if (f.pixels[i * f.channels + 0] || f.pixels[i * f.channels + 1] ||
            f.pixels[i * f.channels + 2]) {
            ++n;
        }
    }
    return n;
}

/** @brief The first pixel in `f` that matches no entry of `palette`, or -1 if all do. */
long long first_off_palette_pixel(const Frame& f, const uint8_t palette[][3], size_t entries) {
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t r = f.pixels[i * f.channels + 0];
        const uint8_t g = f.pixels[i * f.channels + 1];
        const uint8_t b = f.pixels[i * f.channels + 2];
        bool found = false;
        for (size_t e = 0; e < entries; ++e) {
            if (palette[e][0] == r && palette[e][1] == g && palette[e][2] == b) { found = true; break; }
        }
        if (!found) return static_cast<long long>(i);
    }
    return -1;
}

// =====================================================================================
// Render-test fixture
// =====================================================================================

/**
 * @brief An AppConfig shaped for a headless render test: invisible window, no vsync, no
 *        save-on-exit, and the temporal resolves off.
 *
 * `visible = false` is the one that matters to whoever is using the machine -- see this file's
 * doc. vsync off keeps a tick from sleeping to the monitor's refresh (a 60 Hz cap would make
 * the cloth test's 250-odd ticks take four seconds of pure waiting). The temporal SSAO/SSR
 * resolves are disabled because they accumulate across frames: with them on, two captures at
 * different frame indices differ even when nothing else changed, and every A/B assertion in
 * this file rests on them not doing that.
 *
 * @param scene Scene YAML path, relative to the repo root.
 * @param win_w Window width; also the display-resolution capture width.
 * @param win_h Window height.
 * @param rw    Internal render width (the low_res capture's width).
 * @param rh    Internal render height.
 */
toy::core::AppConfig make_test_config(const std::string& scene,
                                      uint32_t win_w, uint32_t win_h,
                                      uint32_t rw, uint32_t rh) {
    toy::core::AppConfig config;
    config.window.title   = "toyengine_tests";
    config.window.width   = win_w;
    config.window.height  = win_h;
    config.window.vsync   = false;
    config.window.visible = false;

    config.scene.default_scene = scene;

    config.render.render_width          = rw;
    config.render.render_height         = rh;
    config.render.ssao_temporal_enabled = false;
    config.render.ssr_temporal_enabled  = false;

    config.output.save_on_exit = false;
    return config;
}

/** @brief Advances an Engine by `frames` ticks. Named for what the call sites mean by it. */
void tick_frames(toy::core::Engine& engine, int frames) {
    for (int i = 0; i < frames; ++i) engine.tick();
}

// =====================================================================================
// Group "math" -- pure functions from toyengine/render/pixel_math.h. No GPU, no window.
// =====================================================================================

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
    expect_near(rect.scale, 2.25f, 1e-6f, "fit: 1920x1080 / 720x480 -> scale 2.25");
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

    // An indivisible swapchain size must still yield a usable (non-zero) extent -- every
    // target in the frame graph is allocated from it, and a zero dimension is not a valid
    // Vulkan image.
    toy::render::RenderExtent odd = toy::render::compute_render_extent(cfg, 1919, 1079);
    expect(odd.width > 0 && odd.height > 0,
          "render_resolution: divisor mode on an indivisible size still yields a non-zero extent");
}

void test_dof_view_space_depth() {
    // Camera at (0,0,5) looking at the origin, standard RH lookAt -- glm's usual view-space
    // convention (camera looks down its own -Z) applies regardless of which world axis this
    // engine treats as "up" (CameraController's rig is Z-up; that only affects how a scene's
    // camera Transform is built, not this pure view-matrix math).
    glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    expect_near(toy::render::view_space_depth(view, glm::vec3(0.0f)), 5.0f, 1e-4f,
                "view_space_depth: point at the look-at target is 5m in front of a camera 5m away");
    expect_near(toy::render::view_space_depth(view, glm::vec3(0.0f, 0.0f, 2.0f)), 3.0f, 1e-4f,
                "view_space_depth: a point 3m closer to the camera reports 3m less depth");

    // A point behind the eye (camera at z=5 looking toward -z; z=8 is on the far side of the
    // camera from the look-at target) must come back negative -- the case
    // resolve_dof_focus_()'s `depth > 0.0f` guard exists to catch.
    expect(toy::render::view_space_depth(view, glm::vec3(0.0f, 0.0f, 8.0f)) < 0.0f,
           "view_space_depth: a point behind the camera is negative");
}

void test_dof_focus_smoothing() {
    // rate <= 0 snaps straight to target, regardless of dt.
    expect(toy::render::exp_smooth_toward(0.0f, 10.0f, 0.0f, 0.5f) == 10.0f,
           "exp_smooth_toward: rate <= 0 snaps to target");
    expect(toy::render::exp_smooth_toward(0.0f, 10.0f, -1.0f, 0.5f) == 10.0f,
           "exp_smooth_toward: negative rate also snaps to target");

    // Monotone convergence toward the target, never overshooting it. Asserted ONCE over the
    // whole second rather than per step: 60 identical assertions say nothing 1 cannot.
    float value = 0.0f;
    bool  monotone = true;
    for (int i = 0; i < 60; ++i) {
        const float next = toy::render::exp_smooth_toward(value, 10.0f, 8.0f, 1.0f / 60.0f);
        if (!(next > value && next <= 10.0f)) monotone = false;
        value = next;
    }
    expect(monotone, "exp_smooth_toward: every step of a second moves toward the target without overshooting");
    expect(value > 9.0f, "exp_smooth_toward: converges close to target after 1 second at rate 8");

    // Framerate independence: two half-steps at dt land at the same place as one step at 2*dt --
    // the property the 1 - exp(-rate*dt) form buys over a naive linear lerp.
    const float two_steps = toy::render::exp_smooth_toward(
        toy::render::exp_smooth_toward(2.0f, 20.0f, 5.0f, 0.1f), 20.0f, 5.0f, 0.1f);
    const float one_step = toy::render::exp_smooth_toward(2.0f, 20.0f, 5.0f, 0.2f);
    expect_near(two_steps, one_step, 1e-4f,
                "exp_smooth_toward: two dt steps match one 2*dt step (framerate-independent)");
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
    expect_near(fit.texel_world, 30.0f / 2048.0f, 1e-6f,
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
    expect_near(a.texel_world, b.texel_world, 1e-6f,
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
    expect_near(ratio, std::round(ratio), 1e-2f, "dir_shadow_fit: box centre is snapped to a whole texel");
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
    expect_near(toy::render::compute_pixel_density(true, 5.4f, 270), 0.04f, 1e-6f,
                "pixel_density: orthographic derives world units/px");
}

void test_pixel_density_perspective_disabled() {
    expect(toy::render::compute_pixel_density(false, 5.4f, 270) == 0.0f,
           "pixel_density: perspective camera disables snapping");
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

// =====================================================================================
// Group "config" -- PixelRenderConfig defaults and AppConfig::load()'s YAML round-trips.
// =====================================================================================

/**
 * @brief Writes a scratch config file under tmp_dir() and loads it back.
 *
 * The round-trip tests all share this shape: every knob in a block set to a DISTINCT,
 * non-default value, so a parser bug that silently keeps the in-class default (a typo'd YAML
 * key being the usual one) cannot pass by coincidence.
 */
toy::core::AppConfig load_config_text(std::string_view filename, std::string_view yaml) {
    const std::string path = tmp_path(filename);
    {
        std::ofstream out(path);
        out << yaml;
    }
    toy::core::AppConfig config = toy::core::AppConfig::load(path);
    std::filesystem::remove(path);
    return config;
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
    toy::core::AppConfig config = load_config_text("test_dof_config.yaml",
        "render:\n"
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
        "  dof_blade_rotation: 30.0\n");

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
}

void test_pixel_render_config_soft_shadow_defaults() {
    toy::render::PixelRenderConfig cfg;
    expect(cfg.soft_shadows == true, "PixelRenderConfig: soft_shadows defaults to true");
    expect(cfg.shadow_softness == 0.15f, "PixelRenderConfig: shadow_softness defaults to 0.15");
    expect(cfg.point_shadow_softness == 3.0f, "PixelRenderConfig: point_shadow_softness defaults to 3.0");
    expect(cfg.shadow_pcf_samples == 24u, "PixelRenderConfig: shadow_pcf_samples defaults to 24");
}

void test_app_config_load_round_trips_soft_shadow_settings() {
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

/**
 * @brief Round-trips the fog, volumetrics, bloom and tilt-shift blocks -- every one of them
 * parsed by AppConfig::load() and, until now, by nothing that checks.
 */
void test_app_config_load_round_trips_atmosphere_settings() {
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
        "  fog_max_distance: 123.0\n"
        "  volumetrics_enabled: true\n"
        "  volumetrics_step_count: 12\n"
        "  volumetrics_max_distance: 22.0\n"
        "  volumetrics_max_opacity: 0.45\n"
        "  volumetrics_sun_anisotropy: 0.35\n"
        "  volumetrics_debug_view: true\n"
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
    expect(config.render.fog_max_distance == 123.0f, "AppConfig::load: fog_max_distance round-trips");

    expect(config.render.volumetrics_enabled == true, "AppConfig::load: volumetrics_enabled round-trips");
    expect(config.render.volumetrics_step_count == 12, "AppConfig::load: volumetrics_step_count round-trips");
    expect(config.render.volumetrics_max_distance == 22.0f, "AppConfig::load: volumetrics_max_distance round-trips");
    expect(config.render.volumetrics_max_opacity == 0.45f, "AppConfig::load: volumetrics_max_opacity round-trips");
    expect(config.render.volumetrics_sun_anisotropy == 0.35f, "AppConfig::load: volumetrics_sun_anisotropy round-trips");
    expect(config.render.volumetrics_debug_view == true, "AppConfig::load: volumetrics_debug_view round-trips");

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

/**
 * @brief Round-trips the SSR/SSGI block plus the two UI toggles and window.visible.
 *
 * window.visible is what the whole headless suite now depends on (see make_test_config()), so
 * it gets the same treatment as every other knob: set it to the non-default value and check.
 */
void test_app_config_load_round_trips_ssr_and_window_settings() {
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
        "  ssr_temporal_gamma: 1.5\n"
        "  ssr_reflect_transparent: true\n"
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
    expect(config.render.ssr_temporal_gamma == 1.5f, "AppConfig::load: ssr_temporal_gamma round-trips");
    expect(config.render.ssr_reflect_transparent == true, "AppConfig::load: ssr_reflect_transparent round-trips");
    expect(config.render.world_ui_enabled == false, "AppConfig::load: world_ui_enabled round-trips");
    expect(config.render.screen_ui_enabled == false, "AppConfig::load: screen_ui_enabled round-trips");
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
void test_app_config_load_tolerates_unknown_and_commented_keys() {
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
    toy::core::AppConfig missing = toy::core::AppConfig::load(tmp_path("does_not_exist.yaml"));
    expect(missing.render.exposure == toy::render::PixelRenderConfig{}.exposure,
           "AppConfig::load: a missing file falls back to defaults");
}

void test_directional_light_shadow_intensity_default() {
    coopa::gfx::engine::components::DirectionalLightComponent dl;
    expect(dl.shadow_intensity == 1.0f, "DirectionalLightComponent: shadow_intensity defaults to 1.0 (full occlusion)");
}

// =====================================================================================
// Group "scene" -- toyengine's scene components, driven through a real Scene but with no
// Vulkan device: orbit math, kinematic motion and the health cycle only ever touch
// TransformComponent, Scene::find_object() and their own state.
// =====================================================================================

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::scene::CameraController;

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
    expect_near(cc->yaw_deg, 90.0f, 0.01f,
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
    expect_near(p.x, 4.0f, 0.05f, "kinematic_controller: +X input for 1 s travels move_speed metres");
    expect(std::fabs(p.y) < 1e-5f, "kinematic_controller: no Y drift from pure +X input");
    expect_near(p.z, 2.6f, 1e-5f, "kinematic_controller: holds the authored hover height");

    // Diagonal input is CLAMPED, not normalized: full deflection on both axes must not travel
    // faster than full deflection on one.
    kc->move_input = glm::vec2(1.0f, 1.0f);
    const glm::vec3 before = tc->transform().position();
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    const float diagonal = glm::length(tc->transform().position() - before);
    expect_near(diagonal, 4.0f, 0.05f,
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
    expect_near(kc->velocity().x, 4.0f, 0.05f,
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
        expect_near(body->position.x, transform_x, 1e-4f,
                    "kinematic order: physics saw the pose written this frame, not the previous one");
    }
}

// --- KinematicMover -----------------------------------------------------------------------

/** @brief Builds a one-object scene with a KinematicMover seeded at `seed_pos`, un-started. */
std::unique_ptr<Scene> make_mover_scene(toy::scene::KinematicMover** out_km,
                                        const glm::vec3& seed_pos) {
    auto scene = std::make_unique<Scene>("kinematic_mover_test");
    auto obj = std::make_unique<SceneObject>("platform");
    obj->add_component<TransformComponent>()->transform().set_position(seed_pos);
    *out_km = obj->add_component<toy::scene::KinematicMover>();
    scene->add_root_object(std::move(obj));
    // Same reason as make_controller_scene(): the motion is in advance(), which only the
    // order-50 system calls. Without this the object never moves and every assertion below
    // would pass or fail for the wrong reason.
    toy::scene::install_kinematic_control_system(*scene);
    return scene;
}

/**
 * @brief PingPong oscillates about the seed position and comes back to it.
 *
 * The scripted counterpart to KinematicController, driven by the same order-50 system and used
 * by assets/scenes/physics_test/scene.yaml's moving platforms. The sinusoid is the point: a
 * linear back-and-forth would reverse instantaneously, and PhysicsSystem derives the body's
 * velocity from this Transform delta, so the discontinuity would kick anything standing on it.
 */
void test_kinematic_mover_pingpong_oscillates_about_origin() {
    toy::scene::KinematicMover* km = nullptr;
    const glm::vec3 seed(1.0f, 2.0f, 3.0f);
    auto scene = make_mover_scene(&km, seed);
    km->mode     = toy::scene::KinematicMoverMode::PingPong;
    km->axis     = glm::vec3(1.0f, 0.0f, 0.0f);
    km->distance = 4.0f;   // peak-to-peak, so +-2 about the seed
    km->speed    = 1.0f;   // 1 Hz: peak at t = 0.25 s, back to the seed at t = 0.5 s
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    const float dt = 1.0f / 240.0f;

    // Quarter period -> one full peak in +axis.
    for (int i = 0; i < 60; ++i) scene->update(dt);
    glm::vec3 peak = tc->transform().position();
    expect_near(peak.x, seed.x + 2.0f, 0.05f, "kinematic_mover: pingpong reaches +distance/2 at the quarter period");
    expect(std::fabs(peak.y - seed.y) < 1e-5f && std::fabs(peak.z - seed.z) < 1e-5f,
           "kinematic_mover: pingpong only moves along `axis`");

    // Half period -> back through the seed position.
    for (int i = 0; i < 60; ++i) scene->update(dt);
    expect_near(tc->transform().position().x, seed.x, 0.05f,
                "kinematic_mover: pingpong returns to the seed position at the half period");

    // Three quarters -> the opposite peak. Motion is centred on the seed, not offset from it.
    for (int i = 0; i < 60; ++i) scene->update(dt);
    expect_near(tc->transform().position().x, seed.x - 2.0f, 0.05f,
                "kinematic_mover: pingpong swings symmetrically to -distance/2");
}

/** @brief Orbit circles orbit_center at orbit_radius in XY, holding the seed's Z height. */
void test_kinematic_mover_orbit_holds_radius_and_height() {
    toy::scene::KinematicMover* km = nullptr;
    auto scene = make_mover_scene(&km, glm::vec3(0.0f, 0.0f, 1.75f));
    km->mode         = toy::scene::KinematicMoverMode::Orbit;
    km->orbit_center = glm::vec3(2.0f, -1.0f, 0.0f);
    km->orbit_radius = 3.0f;
    km->speed        = 90.0f; // degrees/sec
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    const float dt = 1.0f / 120.0f;

    float worst_radius_error = 0.0f;
    float worst_height_error = 0.0f;
    for (int i = 0; i < 480; ++i) { // four seconds = one full revolution at 90 deg/s
        scene->update(dt);
        const glm::vec3 p = tc->transform().position();
        const float radius = glm::length(glm::vec2(p.x, p.y) - glm::vec2(km->orbit_center));
        worst_radius_error = std::max(worst_radius_error, std::fabs(radius - km->orbit_radius));
        worst_height_error = std::max(worst_height_error, std::fabs(p.z - 1.75f));
    }
    expect(worst_radius_error < 1e-3f,
           "kinematic_mover: orbit stays on orbit_radius for a whole revolution");
    expect(worst_height_error < 1e-5f,
           "kinematic_mover: orbit holds the seed transform's Z height (the engine is Z-up)");

    // A full revolution lands back where it started, rather than accumulating drift.
    const glm::vec3 after_one_revolution = tc->transform().position();
    for (int i = 0; i < 480; ++i) scene->update(dt);
    expect(glm::length(tc->transform().position() - after_one_revolution) < 1e-3f,
           "kinematic_mover: orbit is periodic, with no per-frame drift");
}

/** @brief Spin rotates in place: the rotation changes, the position does not. */
void test_kinematic_mover_spin_rotates_in_place() {
    toy::scene::KinematicMover* km = nullptr;
    const glm::vec3 seed(0.5f, -0.5f, 2.0f);
    auto scene = make_mover_scene(&km, seed);
    km->mode       = toy::scene::KinematicMoverMode::Spin;
    km->spin_axis  = glm::vec3(0.0f, 0.0f, 1.0f);
    km->spin_speed = 90.0f; // degrees/sec
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();
    const glm::quat seed_rotation = tc->transform().rotation_quat();

    for (int i = 0; i < 120; ++i) scene->update(1.0f / 120.0f); // one second = 90 degrees

    expect(glm::length(tc->transform().position() - seed) < 1e-6f,
           "kinematic_mover: spin never moves the object");

    // A 90-degree Z rotation takes local +X onto +Y.
    const glm::vec3 local_x = tc->transform().rotation_quat() * glm::vec3(1.0f, 0.0f, 0.0f);
    expect(glm::length(local_x - glm::vec3(0.0f, 1.0f, 0.0f)) < 0.02f,
           "kinematic_mover: spin_speed=90 turns the object a quarter turn in one second");
    expect(glm::length(tc->transform().rotation_quat() - seed_rotation) > 1e-3f,
           "kinematic_mover: spin actually changes the rotation");
}

// --- HealthDriver -------------------------------------------------------------------------

/**
 * @brief The demo health cycle drains, turns around at turnaround_fraction, and refills.
 *
 * Runs with no ProgressBar in the scene at all, which is the interesting half of the contract:
 * find_bar_() returning nullptr must leave the Resource cycling normally (world_canvas_test's
 * 1080p render is the only thing that exercises the bound-bar path, and it can't tell you WHY
 * a bar stopped moving). Also guards the two bounds the cycle must never cross -- a Resource
 * that bottoms out at 0 reads as broken, and one that overshoots max never turns around.
 */
void test_health_driver_cycles_between_turnaround_and_full() {
    Scene scene("health_driver_test");
    auto obj = std::make_unique<SceneObject>("player");
    auto* hd = obj->add_component<toy::scene::HealthDriver>();
    hd->max_health          = 100.0f;
    hd->start_health        = 100.0f;
    hd->damage_per_second   = 50.0f;
    hd->regen_per_second    = 50.0f;
    hd->turnaround_fraction = 0.25f;
    scene.add_root_object(std::move(obj));
    scene.start();

    expect(hd->health().current == 100.0f, "health_driver: start() seeds the Resource from start_health");
    expect(hd->health().max == 100.0f, "health_driver: start() seeds the Resource's max");

    const float dt = 1.0f / 60.0f;

    // Draining: one second at 50/s from full lands at 50, i.e. past nothing and still falling.
    for (int i = 0; i < 60; ++i) scene.update(dt);
    expect(hd->health().current < 100.0f, "health_driver: the cycle starts by draining");

    // Run several full cycles, tracking the extremes. 0.25 * 100 = 25 is the turnaround; a
    // single frame of overshoot at 50/s is 0.83, so 1.0 of slack is a frame, not a bug.
    float lowest  = hd->health().current;
    float highest = hd->health().current;
    bool  refilled_after_turnaround = false;
    for (int i = 0; i < 600; ++i) { // ten seconds, several drain/refill cycles at these rates
        const float before = hd->health().current;
        scene.update(dt);
        const float after = hd->health().current;
        lowest  = std::min(lowest, after);
        highest = std::max(highest, after);
        if (before <= 26.0f && after > before) refilled_after_turnaround = true;
    }

    expect(refilled_after_turnaround,
           "health_driver: the cycle turns around and refills once past turnaround_fraction");
    expect(lowest >= 24.0f,
           "health_driver: health never falls meaningfully below turnaround_fraction * max");
    expect(highest <= 100.0f,
           "health_driver: regen never pushes health past max");
    expect(highest > 99.0f,
           "health_driver: the refill half of the cycle reaches full health");

    // dt <= 0 is a no-op, not a NaN: Engine ticks with FIXED_DT=0 in several tests here.
    const float before_zero_dt = hd->health().current;
    scene.update(0.0f);
    expect(hd->health().current == before_zero_dt, "health_driver: a zero-dt frame changes nothing");
}

// =====================================================================================
// Group "render_pixel" -- full headless renders of assets/scenes/pixel_demo through a real
// Vulkan device.
// =====================================================================================

/// The eight pico-8 entries assets/scenes/pixel_demo actually resolves to.
const uint8_t kPico8Subset[8][3] = {
    {0, 0, 0}, {29, 43, 83}, {126, 37, 83}, {0, 135, 81},
    {171, 82, 54}, {95, 87, 79}, {194, 195, 199}, {255, 241, 232},
};

/**
 * @brief How many frames apart comparable captures are taken, and how much they may still
 *        differ when nothing changed.
 *
 * Two frame-index-driven noise sources survive FIXED_DT=0, both deliberately (they
 * decorrelate per-pixel noise frame to frame, and neither cares whether time passed):
 * SSAO rotates its noise tile by `frame_index_ & 0x7`, and SSR's interleaved-gradient dither
 * runs on `frame_index_ & 0xFF` -- see ssao_params.noise_rotation and ssr_params.frame_index
 * in PixelRenderPipeline::render(). So "identical" has a period of 256 frames, not 1.
 *
 * Measured on this scene at 160x90: captures 5 frames apart differ by 2 pixels, 8 apart
 * (SSAO's period, which kills the larger source) by 1, and 256 apart by 0 -- but 256 ticks per
 * comparison costs ~20x the whole rest of this group, for one pixel. So captures are spaced by
 * SSAO's cycle and allowed kDriftBudget pixels of SSR dither, a floor 30x below the smallest
 * real signal any assertion below looks for (545 pixels, for the SDF toggle).
 */
constexpr int kNoiseCycle  = 8;
constexpr int kDriftBudget = 16; // 0.1% of a 160x90 frame

/**
 * @brief One Engine, four claims: the render path produces exactly the configured
 * buffer, every pixel of it is a palette entry, the frames are reproducible, and each live
 * toggle measurably changes the image.
 *
 * The palette assertion alone validates the render path, the outline/dither/quantize post pass
 * and the UNORM/gamma choice together -- see gfxcoopa's pixel_stylize_pass.h file doc. What is
 * new here is the SECOND assertion: two captures a noise cycle apart, with nothing
 * changed, must be byte-identical. That is not a property of the renderer being tested for its
 * own sake -- it is what earns the right to compare the toggle frames below with exact counts
 * instead of a guessed tolerance. If it ever fails, every diff threshold in this group is
 * measuring noise and should be disbelieved before the toggles are.
 *
 * The three toggles are then flipped on the LIVE pipeline (palette, then SDF, then outline),
 * which is the whole reason this is one test and not four Engines: each is re-read from the
 * config every frame (see PixelRenderPipeline::render_config_mut()), so a flip plus a
 * noise cycle of ticks costs milliseconds against the ~second a fresh device, pipeline set and scene load costs.
 * Thresholds are deliberately loose -- the claim is "this toggle reaches the screen", and the
 * actual counts print on failure so a real change in coverage is easy to re-baseline.
 */
void test_pixel_demo_render_and_live_toggles() {
    ScopedEnv fixed_dt("FIXED_DT", "0");   // freeze time: see this file's doc
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 160, 90);
    config.render.palette_path    = "assets/palettes/pico8.png";
    config.render.dither_strength = 0.08f;

    toy::core::Engine engine(std::move(config));
    tick_frames(engine, kNoiseCycle); // frame 0 has no temporal history of any kind

    const Frame palette_frame = engine.capture_image(/*low_res=*/true);
    expect(palette_frame.width == 160 && palette_frame.height == 90,
           "headless render: low-res buffer has the configured dimensions");
    expect(palette_frame.channels == 4, "headless render: capture_image returns 4 bytes/texel");

    const long long off_palette =
        first_off_palette_pixel(palette_frame, kPico8Subset, std::size(kPico8Subset));
    expect(off_palette < 0, "headless render: every output pixel matches a palette entry");
    if (off_palette >= 0) {
        const size_t i = static_cast<size_t>(off_palette) * palette_frame.channels;
        std::cerr << "         first off-palette pixel " << off_palette << " = ("
                  << int(palette_frame.pixels[i]) << ", " << int(palette_frame.pixels[i + 1])
                  << ", " << int(palette_frame.pixels[i + 2]) << ")\n";
        dump_frame(palette_frame, "pixel_demo_off_palette");
    }

    // The load-bearing assertion for every diff below it.
    tick_frames(engine, kNoiseCycle);
    const Frame repeat = engine.capture_image(true);
    const long long drift = count_diff(repeat, palette_frame);
    expect(drift <= kDriftBudget,
           "headless render: with FIXED_DT=0, two captures a noise cycle apart are the same frame");
    if (drift > kDriftBudget) {
        std::cerr << "         " << drift << " pixels drifted with nothing changed -- every"
                     " threshold below is unreliable until this passes\n";
        dump_frame(palette_frame, "pixel_demo_frame_a");
        dump_frame(repeat, "pixel_demo_frame_b");
    }

    // --- palette_enabled / dither_enabled ---
    engine.render_config().palette_enabled = false;
    engine.render_config().dither_enabled  = false;
    tick_frames(engine, kNoiseCycle);
    const Frame unquantized = engine.capture_image(true);
    expect_at_least(count_diff(unquantized, palette_frame), 1000,
                    "palette_enabled + dither_enabled toggle measurably changes the rendered frame");
    expect(first_off_palette_pixel(unquantized, kPico8Subset, std::size(kPico8Subset)) >= 0,
           "palette_enabled=false lets the output leave the palette (so the check above means something)");

    // --- sdf_enabled: the demo has an opaque SdfRenderer blob and a BLEND sphere ---
    engine.render_config().sdf_enabled = false;
    tick_frames(engine, kNoiseCycle);
    const Frame no_sdf = engine.capture_image(true);
    const long long sdf_diff = count_diff(no_sdf, unquantized);
    expect_at_least(sdf_diff, 200, "sdf_enabled toggle measurably changes the rendered frame");
    if (sdf_diff < 200) {
        dump_frame(unquantized, "pixel_demo_sdf_on");
        dump_frame(no_sdf, "pixel_demo_sdf_off");
    }

    // --- outline_enabled: edge detect over the G-buffer, so it changes silhouettes only ---
    engine.render_config().outline_enabled = false;
    tick_frames(engine, kNoiseCycle);
    const Frame no_outline = engine.capture_image(true);
    const long long outline_diff = count_diff(no_outline, no_sdf);
    expect_at_least(outline_diff, 100, "outline_enabled toggle measurably changes the rendered frame");
    if (outline_diff < 100) {
        dump_frame(no_sdf, "pixel_demo_outline_on");
        dump_frame(no_outline, "pixel_demo_outline_off");
    }
}

/**
 * @brief Headless render with every STARTUP-FIXED toggle off, which is the only way to cover
 * their "off" construction branch.
 *
 * This one keeps an Engine of its own on purpose. Unlike the live toggles above, these
 * decisions are baked into descriptors when the pipeline is built (see
 * pixel_render_pipeline.h's rule 1), so the code below only ever runs in a pipeline
 * constructed this way: SsaoPass::invalidate_history() instead of execute(), no
 * HiZPass/SceneColorMipPass/SsrPass construction at all, the manual gbuffer-depth transition
 * instead of HiZPass's, pixel_stylize_pass_ reading offscreen_target_ directly instead of
 * ssr_pass_'s composite output, and neither UI pipeline nor the scene-depth descriptor built.
 *
 * A non-black frame of the right size is a low bar and deliberately so -- it is exactly what
 * an all-zero G-buffer read, a missing descriptor bind or a null pass pointer in one of those
 * branches would fail.
 */
void test_headless_render_with_all_toggles_off() {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 160, 90);
    config.render.outline_enabled   = false;
    config.render.palette_enabled   = false;
    config.render.dither_enabled    = false;
    config.render.ssao_enabled      = false;
    config.render.ssr_enabled       = false;
    config.render.world_ui_enabled  = false;
    config.render.screen_ui_enabled = false;

    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 3);
    const Frame frame = engine.capture_image(true);

    expect(frame.width == 160 && frame.height == 90,
           "all-toggles-off headless render: low-res buffer has the configured dimensions");

    const long long lit = count_nonblack(frame);
    expect_at_least(lit, 1, "all-toggles-off headless render: output is not entirely black");
    if (lit == 0) dump_frame(frame, "all_toggles_off_black");
}

// =====================================================================================
// Group "render_ui" -- world-space UI, at display resolution.
// =====================================================================================

/**
 * @brief Parks the pointer on the world-space "Heal" Button in world_canvas_test, then moves it
 * off, and checks the button lights up and goes out -- the end-to-end proof that world-space UI
 * is both DRAWN and INTERACTIVE.
 *
 * Everything between a window pixel and a tinted button is exercised here and nowhere else:
 * window pixel -> letterbox rect -> internal render extent -> NDC (through the negative-height
 * viewport convention) -> world ray -> ray/plane intersection against the canvas -> canvas
 * pixels -> Raycaster -> EventSystem -> Button::on_pointer_enter -> ColorTransition. uicoopa's
 * own headless tests cover the ray/plane maths exactly; only a real render can cover the rest
 * of that chain, because the letterbox and viewport conventions live in this repo.
 *
 * It also subsumes the "does world UI reach the image at all" question that used to need two
 * more Engines with world_ui_enabled on and off: a few hundred pixels of the button's
 * HIGHLIGHT colour can only be there if the canvas laid out, emitted, drew as 3D geometry and
 * composited over the frame. The A/B is the same pointer logic either way, differing only in
 * WHERE it points, so a failure means the mapping is wrong rather than that the UI is missing.
 *
 * Both frames come from ONE Engine via Engine::set_cursor_override(): world_ui_enabled is
 * startup-fixed, but the pointer is not, and two 1920x1080 Engines to move the mouse 250 pixels
 * was the single most expensive thing this suite did.
 *
 * Captured at DISPLAY resolution (low_res = false). The world UI is not part of the low-res
 * image: it renders into its own layer and composites after AA and tilt shift, so
 * low_res_color_image() is scene-only by construction (see
 * PixelRenderPipeline::overlay_target_) and a low-res capture would compare two frames that
 * genuinely are identical.
 *
 * FIXED_DT is a tiny 0.5 ms rather than 0: the scene's camera auto-orbits (so this keeps the
 * button essentially still across the ticks) but Button's colour chase and the EventSystem
 * still need the frame to advance at all. aa_mode stays at its "off" default deliberately --
 * TAA's non-reprojecting history clamp would ghost a canvas moving under an orbiting camera
 * and make the comparison depend on frame count.
 */
void test_world_canvas_button_hover() {
    ScopedEnv fixed_dt("FIXED_DT", "0.0005");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/world_canvas_test/scene.yaml", 1920, 1080, 1440, 960);

    toy::core::Engine engine(std::move(config));

    // Scene colours: normal (0.20, 0.42, 0.30), highlighted (0.45, 0.92, 0.60). World UI is
    // composited AFTER tonemapping, so these reach the framebuffer very nearly 1:1. Counting
    // near-matches beats sampling one hardcoded coordinate: the saved image's size depends on
    // config, and the camera orbits, so a fixed pixel index is exactly the assertion that rots.
    auto count_highlight = [](const Frame& f) { return count_near_color(f, 115, 235, 153, 26); };

    // Three ticks after each pointer move: Button's colour chase runs in update(), a phase
    // EARLIER than the late_update() that detects the hover, so the tint can only land from the
    // second frame onward however short fade_duration is.
    engine.set_cursor_override(glm::vec2(1097.0f, 407.0f)); // the Heal button, in window pixels
    tick_frames(engine, 3);
    const Frame hovered = engine.capture_image(/*low_res=*/false);

    engine.set_cursor_override(glm::vec2(850.0f, 760.0f));  // empty floor below it
    tick_frames(engine, 3);
    const Frame idle = engine.capture_image(false);

    expect(same_extent(hovered, idle), "world canvas hover: both captures share one extent");
    if (!same_extent(hovered, idle)) return;

    const long long on_count  = count_highlight(hovered);
    const long long off_count = count_highlight(idle);

    // The button is 34x13 canvas pixels on a 150x38 canvas; at this render extent that is a few
    // hundred framebuffer pixels, comfortably clear of any stray match in the scene.
    expect_at_least(on_count, 200,
                    "hovered world-space button lights up (canvas -> ray -> EventSystem works)");
    expect(off_count < 50, "un-hovered world-space button stays its normal colour");
    expect(on_count > off_count * 4 + 100, "hover is a large, unambiguous change, not noise");

    // The change must also be LOCAL. Nothing but the pointer differs between these two frames,
    // so a diff spanning the whole image would mean the camera (or time) moved instead -- which
    // would make the colour counts above coincidental rather than causal.
    const long long changed = count_diff(hovered, idle, /*tolerance=*/8);
    const long long pixels  = static_cast<long long>(hovered.width) * hovered.height;
    expect_at_least(changed, 200, "the hover changes a button-sized region of the frame");
    expect(changed < pixels / 20,
           "the hover changes only a small part of the frame (the camera and clock held still)");

    if (g_test_failures > 0) {
        dump_frame(hovered, "world_canvas_hovered");
        dump_frame(idle, "world_canvas_idle");
    }
}

// =====================================================================================
// Group "render_material" -- the material texture path.
// =====================================================================================

/**
 * @brief Renders assets/scenes/material_maps_test's two scenes -- the SAME lit cube,
 * byte-identical YAML but for scene_mapped's three texture_albedo/texture_normal/
 * texture_metallic_roughness keys (see those files' own comments) -- and asserts the frames
 * differ.
 *
 * Exercises the whole path end to end: TextureLoader's sRGB/linear colour-space split,
 * MaterialTextureCache's 4-binding material set, and gbuffer_fs.glsl's albedo/normal/MR
 * sampling. Two Engines here are unavoidable -- the difference lives in two different scene
 * files, and a scene is loaded once at construction -- but FIXED_DT=0 means the two frames are
 * each reproducible, so the comparison can demand a real area of change rather than the single
 * differing byte the old "> 0" threshold accepted.
 */
void test_material_maps_change_output() {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    auto render_scene = [](const char* scene_path) {
        toy::core::AppConfig config = make_test_config(scene_path, 640, 360, 160, 90);
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, 3);
        return engine.capture_image(/*low_res=*/true);
    };

    const Frame flat   = render_scene("assets/scenes/material_maps_test/scene_flat.yaml");
    const Frame mapped = render_scene("assets/scenes/material_maps_test/scene_mapped.yaml");

    expect(same_extent(flat, mapped), "material maps: both captures share one extent");
    if (!same_extent(flat, mapped)) return;

    const long long diff = count_diff(flat, mapped);
    expect_at_least(diff, 50,
                    "albedo/normal/metallic_roughness maps measurably change the rendered frame");
    if (diff < 50) {
        dump_frame(flat, "material_maps_flat");
        dump_frame(mapped, "material_maps_mapped");
    }
}

// =====================================================================================
// Group "render_cloth" -- the only test here that needs time to actually pass.
// =====================================================================================

/**
 * @brief Full headless render of the cloth scene: the sheet must actually simulate (its
 * particles move and end up outside the ball) AND that simulation must reach the screen (two
 * frames far apart differ).
 *
 * The rendered-difference half is the part that matters most: everything else about cloth is
 * covered by physxcoopa's own headless suite, but nothing there can catch a broken dynamic
 * vertex buffer -- a mesh uploaded once and never again would still pass every physics
 * assertion while drawing a frozen flat sheet.
 *
 * Alone among the render tests, this one pins dt to a real 1/60 rather than 0: a drape is a
 * function of elapsed simulated time. Without the pin the Engine would run on wall-clock dt,
 * which headless is a few milliseconds -- so the fixed per-frame displacement below would
 * describe a ~22 m/s ball rather than the 4 m/s the scene is authored for, and most frames
 * would run no physics substep at all. Both make the drape unreproducible.
 */
void test_cloth_scene_simulates_and_animates() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/cloth_test/scene.yaml", 640, 360, 320, 180);

    toy::core::Engine engine(std::move(config));

    tick_frames(engine, 4);
    const Frame early = engine.capture_image(/*low_res=*/true);

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
    tick_frames(engine, 150);
    const Frame late = engine.capture_image(true);

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

    // Now the moving-ball half. The ball's Transform is written directly rather than through its
    // KinematicController: Engine::drive_kinematic_controllers_() re-reads the keyboard and
    // overwrites move_input at the top of every tick(), so a value poked in from outside can
    // never survive to Scene::update() -- and a headless test has no keyboard to press. The
    // input -> move_input -> Transform half is covered by the KinematicController tests in the
    // "scene" group; what only this test can cover is everything BELOW the Transform write:
    // Transform -> PhysicsSystem's derived kinematic velocity -> cloth anchors -> particles ->
    // the uploaded vertex buffer.
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
    for (int i = 0; i < 60; ++i) {
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
        std::cerr << "         worst clearance was " << worst_clearance << " m\n";
    }

    const glm::vec3 ball_after = ball ? ball->get_transform()->transform().position() : glm::vec3(0.0f);
    expect(ball_after.x > ball_before.x + 0.5f, "cloth scene: the ball travels along +X");
    expect_near(ball_after.z, ball_before.z, 1e-4f,
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

    // The rendered half: a dynamic vertex buffer that stopped being re-uploaded would draw the
    // same flat sheet in both captures and pass every assertion above.
    expect(same_extent(early, late), "cloth scene: both captures share one extent");
    if (same_extent(early, late)) {
        const long long pixels  = static_cast<long long>(early.width) * early.height;
        const long long changed = count_diff(early, late);
        expect_at_least(changed, pixels / 20,
                        "cloth scene: the dynamic vertex buffer reaches the screen (frames 4 and 154 differ substantially)");
        if (changed < pixels / 20) {
            dump_frame(early, "cloth_early");
            dump_frame(late, "cloth_late");
        }
    }
}

// =====================================================================================
// Registry
// =====================================================================================

/** @brief One registered test: its name (also its filter key), its group, and its body. */
struct TestCase {
    const char* name;
    const char* group;
    void (*fn)();
};

/**
 * @brief Every test in the suite.
 *
 * A table rather than a list of calls in main(): the groups are what CMakeLists.txt registers
 * with ctest and what --group selects, and a test added to the file but not to a call list is
 * a test that silently never runs.
 *
 * Group costs: math/config/scene need no Vulkan device and finish in milliseconds;
 * render_* each construct at least one Engine (window, device, every pipeline, a loaded scene)
 * and are registered separately so `ctest -j` overlaps them.
 */
const TestCase kTests[] = {
    // --- math: pure functions, no GPU ---
    {"letterbox_exact_fit",                        "math", test_letterbox_exact_fit},
    {"letterbox_with_bars",                        "math", test_letterbox_with_bars},
    {"letterbox_undersized_window",                "math", test_letterbox_undersized_window_clamps_to_scale_1},
    {"fit_pillarbox_only",                         "math", test_fit_pillarbox_only},
    {"fit_exact_match_fills_completely",           "math", test_fit_exact_match_fills_completely},
    {"fit_beats_integer_bars",                     "math", test_fit_beats_integer_bars},
    {"fit_undersized_window",                      "math", test_fit_undersized_window},
    {"display_rect_dispatches_on_upscale_mode",    "math", test_display_rect_dispatches_on_upscale_mode},
    {"render_resolution_fixed_mode",               "math", test_render_resolution_fixed_mode},
    {"render_resolution_divisor_mode",             "math", test_render_resolution_divisor_mode},
    {"dof_view_space_depth",                       "math", test_dof_view_space_depth},
    {"dof_focus_smoothing",                        "math", test_dof_focus_smoothing},
    {"dir_shadow_fit_no_camera_fallback",          "math", test_dir_shadow_fit_no_camera_fallback},
    {"dir_shadow_fit_is_camera_only",              "math", test_dir_shadow_fit_is_camera_only},
    {"dir_shadow_fit_radius_stable_under_rotation","math", test_dir_shadow_fit_radius_stable_under_rotation},
    {"dir_shadow_fit_center_snaps_to_texels",      "math", test_dir_shadow_fit_center_snaps_to_texels},
    {"dir_shadow_fit_degenerate_shadow_distance",  "math", test_dir_shadow_fit_degenerate_shadow_distance},
    {"pixel_density_orthographic",                 "math", test_pixel_density_orthographic},
    {"pixel_density_perspective_disabled",         "math", test_pixel_density_perspective_disabled},
    {"sdf_clip_rect_on_screen",                    "math", test_sdf_clip_rect_on_screen},
    {"sdf_clip_rect_off_screen",                   "math", test_sdf_clip_rect_off_screen},
    {"sdf_clip_rect_near_plane_straddle",          "math", test_sdf_clip_rect_near_plane_straddle},
    {"sdf_clip_rect_to_pixels_full_screen",        "math", test_sdf_clip_rect_to_pixels_full_screen},
    {"sdf_clip_rect_to_pixels_flips_y",            "math", test_sdf_clip_rect_to_pixels_flips_y},

    // --- config: defaults and YAML round-trips ---
    {"config_aa_defaults",                         "config", test_pixel_render_config_aa_defaults},
    {"config_aa_round_trip",                       "config", test_app_config_load_round_trips_aa_settings},
    {"config_dof_defaults",                        "config", test_pixel_render_config_dof_defaults},
    {"config_dof_round_trip",                      "config", test_app_config_load_round_trips_dof_settings},
    {"config_soft_shadow_defaults",                "config", test_pixel_render_config_soft_shadow_defaults},
    {"config_soft_shadow_round_trip",              "config", test_app_config_load_round_trips_soft_shadow_settings},
    {"config_atmosphere_round_trip",               "config", test_app_config_load_round_trips_atmosphere_settings},
    {"config_ssr_and_window_round_trip",           "config", test_app_config_load_round_trips_ssr_and_window_settings},
    {"config_tolerates_unknown_keys",              "config", test_app_config_load_tolerates_unknown_and_commented_keys},
    {"directional_light_shadow_intensity_default", "config", test_directional_light_shadow_intensity_default},

    // --- scene: components through a real Scene, no GPU ---
    {"camera_controller_orbit_aims_at_target",     "scene", test_camera_controller_orbit_aims_at_target},
    {"camera_controller_pitch_clamp",              "scene", test_camera_controller_pitch_clamp},
    {"camera_controller_zoom_clamp",               "scene", test_camera_controller_zoom_clamp},
    {"camera_controller_tracker_follow",           "scene", test_camera_controller_tracker_follow},
    {"camera_controller_unresolved_tracker",       "scene", test_camera_controller_unresolved_tracker_falls_back},
    {"camera_controller_seeds_without_teleport",   "scene", test_camera_controller_seeds_without_teleport},
    {"camera_controller_smoothing_zero_instant",   "scene", test_camera_controller_movement_smoothing_zero_is_instant},
    {"camera_controller_smoothing_converges",      "scene", test_camera_controller_movement_smoothing_drags_then_converges},
    {"kinematic_controller_moves_on_input",        "scene", test_kinematic_controller_moves_on_input_and_holds_height},
    {"kinematic_controller_smoothing",             "scene", test_kinematic_controller_smoothing_ramps_then_converges},
    {"kinematic_control_runs_before_physics",      "scene", test_kinematic_control_runs_before_physics},
    {"kinematic_mover_pingpong",                   "scene", test_kinematic_mover_pingpong_oscillates_about_origin},
    {"kinematic_mover_orbit",                      "scene", test_kinematic_mover_orbit_holds_radius_and_height},
    {"kinematic_mover_spin",                       "scene", test_kinematic_mover_spin_rotates_in_place},
    {"health_driver_cycle",                        "scene", test_health_driver_cycles_between_turnaround_and_full},

    // --- render_*: one Vulkan device each ---
    {"pixel_demo_render_and_live_toggles",         "render_pixel",    test_pixel_demo_render_and_live_toggles},
    {"headless_render_with_all_toggles_off",       "render_pixel",    test_headless_render_with_all_toggles_off},
    {"world_canvas_button_hover",                  "render_ui",       test_world_canvas_button_hover},
    {"material_maps_change_output",                "render_material", test_material_maps_change_output},
    {"cloth_scene_simulates_and_animates",         "render_cloth",    test_cloth_scene_simulates_and_animates},
};

/** @brief True if `name` contains any of `filters` (or there are none, i.e. run everything). */
bool matches_filters(const char* name, const std::vector<std::string>& filters) {
    if (filters.empty()) return true;
    const std::string_view haystack(name);
    for (const std::string& f : filters) {
        if (haystack.find(f) != std::string_view::npos) return true;
    }
    return false;
}

void print_usage() {
    std::cout << "usage: toyengine_tests [-v] [--list] [--group <name>] [name-substring ...]\n"
                 "  --list           print every test and its group, run nothing\n"
                 "  --group <name>   run one group: math, config, scene, render_pixel,\n"
                 "                   render_ui, render_material, render_cloth\n"
                 "  -v, --verbose    print every assertion, not just failures\n"
                 "  <substring>      run the tests whose name contains it\n";
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> filters;
    std::string              group;
    bool                     list_only = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            g_verbose = true;
        } else if (arg == "--list") {
            list_only = true;
        } else if (arg == "--group") {
            if (i + 1 >= argc) {
                std::cerr << "--group needs a name\n";
                return 2;
            }
            group = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "unknown option '" << arg << "'\n";
            print_usage();
            return 2;
        } else {
            filters.push_back(arg);
        }
    }

    if (list_only) {
        for (const TestCase& t : kTests) std::cout << t.group << "\t" << t.name << "\n";
        return 0;
    }

    int selected = 0;
    int failed_tests = 0;
    const auto suite_start = std::chrono::steady_clock::now();

    for (const TestCase& test : kTests) {
        if (!group.empty() && group != test.group) continue;
        if (!matches_filters(test.name, filters)) continue;

        ++selected;
        g_current_test  = test.name;
        g_test_failures = 0;
        g_assertions    = 0;

        const auto start = std::chrono::steady_clock::now();
        std::cout << "[ RUN  ] " << test.group << "/" << test.name << std::endl;
        test.fn();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (g_test_failures > 0) {
            ++failed_tests;
            std::cout << "[ FAIL ] " << test.name << " -- " << g_test_failures << " of "
                      << g_assertions << " assertions failed (" << ms << " ms)\n";
        } else {
            std::cout << "[  OK  ] " << test.name << " -- " << g_assertions
                      << " assertions (" << ms << " ms)\n";
        }
    }

    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - suite_start).count();

    if (selected == 0) {
        std::cerr << "\nNo test matched";
        if (!group.empty()) std::cerr << " group '" << group << "'";
        std::cerr << " -- nothing ran, which is a failure, not a pass.\n";
        return 2;
    }

    if (g_total_failures > 0) {
        std::cerr << "\n" << g_total_failures << " assertion(s) failed across " << failed_tests
                  << " of " << selected << " test(s), in " << total_ms << " ms.\n"
                  << "Any frames dumped for inspection are under " << tmp_dir().string() << "\n";
        return 1;
    }

    std::cout << "\nAll " << selected << " test(s) passed in " << total_ms << " ms.\n";
    return 0;
}
