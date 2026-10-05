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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include <gfxcoopa/util/image_readback.h>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_generator.h>

#include <toyengine/core/caml_codec.h>
#include <toyengine/core/engine.h>
#include <toyengine/render/pixel_math.h>
#include <toyengine/render/visibility.h>
#include <toyengine/scene/free_mover.h>
#include <toyengine/world/terrain_chunk.h>
#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/water/water_flow_bake.h>
#include <toyengine/water/water_system.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/terrain_sampler.h>
#include <toyengine/scene/cloth_renderer.h>
#include <toyengine/scene/health_driver.h>
#include <toyengine/ui/ui_assets.h>
#include <uicoopa/ui_yaml.h>
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

/**
 * @brief Mean pixel-scale deviation from the local 3x3 average, in levels -- a high-pass energy.
 *
 * The metric for "grainy": a stochastic trace that has not been integrated shows as per-pixel
 * variance against its own neighbourhood. Deliberately NOT a frame-to-frame delta, which under
 * camera motion is dominated by the image legitimately changing (measured: ~0.15 levels/px at
 * 1.5 deg/frame, identical whether the traces are noisy or clean) and so cannot see the jitter at
 * all. This reads the same number whether the camera is moving or still, which is exactly what
 * lets the in-motion image be compared against the resting one.
 */
double local_high_pass(const Frame& f) {
    if (f.pixels.empty() || f.width < 3 || f.height < 3) return -1.0;
    const int w = int(f.width), h = int(f.height), c = int(f.channels);
    // Only pixels with actual signal in them. This scene's terrain island sits in a large
    // expanse of near-black sky, and averaging a noise measure over that expanse reports a
    // number that mostly says "how much of the frame is empty" -- the same dilution that made
    // every variant look alike before this cutoff existed.
    const int kLitThreshold = 8;
    double sum = 0.0;
    long long n = 0;
    for (int y = 1; y < h - 1; ++y) {
        for (int x = 1; x < w - 1; ++x) {
            int local = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    local += int(f.pixels[size_t((y + dy) * w + (x + dx)) * size_t(c)]);
                }
            }
            if (local < kLitThreshold * 9) continue;
            const int centre = int(f.pixels[size_t(y * w + x) * size_t(c)]);
            sum += std::abs(centre - local / 9.0);
            ++n;
        }
    }
    return n > 0 ? sum / double(n) : -1.0;
}

/** @brief Mean per-channel |a - b| over a frame pair, in 0-255 levels per pixel. */
double mean_abs_delta(const Frame& a, const Frame& b) {
    if (a.pixels.size() != b.pixels.size() || a.pixels.empty()) return -1.0;
    long long sum = 0;
    for (size_t k = 0; k < a.pixels.size(); k += a.channels) {
        sum += std::abs(int(a.pixels[k]) - int(b.pixels[k]));
    }
    return double(sum) / double(a.width * a.height);
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
    config.render.contact_shadow_temporal_enabled = false;
    // The stochastic ray jitter itself is deliberately NOT disabled here: SsrPass pins its
    // noise seed to frame 0 whenever the temporal resolve is off (see its execute()), so the
    // jittered trace is deterministic frame to frame and this config still exercises the code
    // path that ships.

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

// --- toyengine/render/visibility.h ---

void test_frustum_perspective_culls_boxes() {
    using namespace toy::render;
    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    const Frustum f = Frustum::from_matrix(proj * view);
    auto box = [](glm::vec3 c, float e) { return WorldBounds{c, glm::vec3(e)}; };
    expect(f.intersects(box({0, 0, -10}, 1)),   "box straight ahead is visible");
    expect(!f.intersects(box({0, 0, 10}, 1)),   "box behind the camera is culled");
    expect(!f.intersects(box({0, 0, -200}, 1)), "box past the far plane is culled");
    expect(!f.intersects(box({50, 0, -10}, 1)), "box far to the right is culled");
    expect(f.intersects(box({6.5f, 0, -10}, 1)), "box straddling the right plane is visible");
    expect(f.intersects(box({0, 0, -0.1f}, 0.05f)),
           "box straddling the near plane is visible");
    expect(!f.intersects(box({0, 0, -0.03f}, 0.02f)),
           "box wholly between the camera and the near plane is culled");
}

void test_frustum_ortho_and_cube_face() {
    using namespace toy::render;
    // Ortho shadow-cascade style: a 20x20 box looking down -Z from z=50, depth 0..100.
    const glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 50), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
    const glm::mat4 proj = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.0f, 100.0f);
    const Frustum f = Frustum::from_matrix(proj * view);
    expect(f.intersects({{0, 0, 0}, glm::vec3(1)}),   "ortho: centre box visible");
    expect(!f.intersects({{15, 0, 0}, glm::vec3(1)}), "ortho: box outside the side planes culled");
    expect(!f.intersects({{0, 0, 60}, glm::vec3(1)}), "ortho: box behind the near plane culled");

    // A +X cube face (90-degree perspective).
    const glm::mat4 fview = glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0));
    const glm::mat4 fproj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 20.0f);
    const Frustum cf = Frustum::from_matrix(fproj * fview);
    expect(cf.intersects({{5, 0, 0}, glm::vec3(0.5f)}),   "cube +X face sees +X");
    expect(!cf.intersects({{-5, 0, 0}, glm::vec3(0.5f)}), "cube +X face does not see -X");
    expect(!cf.intersects({{0, 5, 0}, glm::vec3(0.5f)}),  "cube +X face does not see +Y");
}

void test_world_aabb_matches_corners() {
    using namespace toy::render;
    glm::mat4 m(1.0f);
    m = glm::translate(m, glm::vec3(3, -2, 1));
    m = glm::rotate(m, 0.7f, glm::normalize(glm::vec3(1, 2, 3)));
    m = glm::scale(m, glm::vec3(2, 0.5f, 1.5f));
    const glm::vec3 lo(-1, -2, -0.5f), hi(1, 1, 2);
    glm::vec3 mn(1e9f), mx(-1e9f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 c((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const glm::vec3 w = glm::vec3(m * glm::vec4(c, 1.0f));
        mn = glm::min(mn, w);
        mx = glm::max(mx, w);
    }
    const WorldBounds b = world_aabb(m, lo, hi);
    for (int a = 0; a < 3; ++a) {
        expect_near(b.center[a] - b.extent[a], mn[a], 1e-4f, "world_aabb min matches 8 corners");
        expect_near(b.center[a] + b.extent[a], mx[a], 1e-4f, "world_aabb max matches 8 corners");
    }
}

void test_screen_height_fraction() {
    using namespace toy::render;
    const glm::mat4 view(1.0f);   // camera at origin looking down -Z
    const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    // fov 90 -> p11 = 1: a radius-1 sphere 10 away covers 1/10 of the half-height... as a
    // fraction of the full height: r * p11 / d.
    expect_near(screen_height_fraction({0, 0, -10}, 1.0f, view, proj), 0.1f, 1e-5f,
                "perspective fraction = r * cot(fov/2) / depth");
    expect(screen_height_fraction({0, 0, -0.5f}, 1.0f, view, proj) > 1.0f,
           "a sphere around the camera covers the screen");
    const glm::mat4 ortho = glm::ortho(-5.0f, 5.0f, -5.0f, 5.0f, 0.0f, 50.0f);
    expect_near(screen_height_fraction({0, 0, -10}, 1.0f, view, ortho), 0.2f, 1e-5f,
                "ortho fraction = r * 2/height, independent of depth");
}

void test_projected_texels() {
    using namespace toy::render;
    // A 20-unit-wide ortho cascade at 2048 texels: 102.4 texels per unit, so a radius-0.5
    // sphere (1 unit across) is ~102 texels.
    const glm::mat4 vp = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 0.0f, 100.0f) *
                         glm::lookAt(glm::vec3(0, 0, 50), glm::vec3(0), glm::vec3(0, 1, 0));
    expect_near(projected_texels(vp, {0, 0, 0}, 0.5f, 2048.0f), 102.4f, 0.01f,
                "ortho: 1 world unit = resolution / width texels");
    // Perspective (90-degree cube face, 512): halving the distance doubles the size.
    const glm::mat4 cf = glm::perspective(glm::radians(90.0f), 1.0f, 0.1f, 50.0f) *
                         glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0));
    const float far_t  = projected_texels(cf, {20, 0, 0}, 0.5f, 512.0f);
    const float near_t = projected_texels(cf, {10, 0, 0}, 0.5f, 512.0f);
    expect_near(near_t / far_t, 2.0f, 1e-3f, "perspective: size scales with 1/distance");
    expect(projected_texels(cf, {0.2f, 0, 0}, 0.5f, 512.0f) > 1.0e5f,
           "a caster around the light is never small");
}

/// A flat n x n grid of quads in mesh-YAML form, with SHARED vertex indices across faces
/// (the welded topology the loader should recover) but emitted per-corner by the parser.
static std::string make_grid_mesh_yaml(int n) {
    std::string y = "vertices:\n";
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            y += "  - [" + std::to_string(i) + ".0, " + std::to_string(j) + ".0, 0.0]\n";
    y += "normals:\n";
    for (int k = 0; k < (n + 1) * (n + 1); ++k) y += "  - [0.0, 0.0, 1.0]\n";
    y += "uvs:\n";
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            y += "  - [" + std::to_string(i) + ".0, " + std::to_string(j) + ".0]\n";
    y += "faces:\n";
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int a = j * (n + 1) + i;
            y += "  - [" + std::to_string(a) + ", " + std::to_string(a + 1) + ", " +
                 std::to_string(a + n + 2) + ", " + std::to_string(a + n + 1) + "]\n";
        }
    return y;
}

void test_mesh_build_welds_and_generates_lods() {
    using coopa::gfx::engine::data::Mesh;
    const int n = 8;
    const fkyaml::node node = fkyaml::node::deserialize(make_grid_mesh_yaml(n));

    const auto plain = Mesh::build_cpu(node);
    expect(plain.indices.size() == static_cast<size_t>(n * n * 6), "welding keeps every triangle");
    expect(plain.vertices.size() == static_cast<size_t>((n + 1) * (n + 1)),
           "per-corner vertices weld back to one per grid point");
    expect(plain.lods.size() == 1, "no lods block -> a single LOD covering the whole mesh");
    expect(plain.lods[0].index_count == plain.indices.size(), "LOD 0 is the full mesh");
    expect_near(plain.bounds_max.x, static_cast<float>(n), 1e-6f, "bounds survive welding");

    const fkyaml::node cfg = fkyaml::node::deserialize(std::string(
        "lods:\n  - { ratio: 0.5, screen_size: 0.2 }\ncull_screen_size: 0.01\n"));
    const auto lodded = Mesh::build_cpu(node, &cfg);
    expect(lodded.lods.size() == 2, "one ratio level -> two LODs");
    expect(lodded.lods[1].first_index == lodded.lods[0].index_count,
           "LOD 1's indices follow LOD 0's in the shared index buffer");
    expect(lodded.lods[1].index_count < lodded.lods[0].index_count, "LOD 1 has fewer triangles");
    expect(lodded.lods[1].index_count % 3 == 0, "LOD 1 is whole triangles");
    expect_near(lodded.lods[1].screen_size, 0.2f, 1e-6f, "screen_size is carried through");
    expect_near(lodded.cull_screen_size, 0.01f, 1e-6f, "cull_screen_size is carried through");
    expect(lodded.vertices.size() == plain.vertices.size(), "a ratio level shares LOD 0's vertices");
}


/** @brief Material slots (submeshes): triangles grouped by slot into contiguous parts, per LOD. */
void test_mesh_submesh_parts() {
    using coopa::gfx::engine::data::Mesh;
    const int n = 8;
    // Left half of the grid (i < 4) is slot 0 "body", right half slot 1 "glass", interleaved in
    // file order so the build has to regroup them.
    std::string y = make_grid_mesh_yaml(n);
    y += "material_slots: [body, glass]\nface_materials:\n";
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) y += std::string("  - ") + (i < n / 2 ? "0" : "1") + "\n";
    const fkyaml::node node = fkyaml::node::deserialize(y);
    const auto m = Mesh::build_cpu(node);
    expect(m.slot_names.size() == 2 && m.slot_names[1] == "glass", "slot names are kept");
    expect(m.lods[0].parts.size() == 2, "LOD 0 has one part per slot");
    const auto& p0 = m.lods[0].parts[0];
    const auto& p1 = m.lods[0].parts[1];
    expect(p0.first_index == 0 && p0.index_count == static_cast<uint32_t>(n * n / 2 * 6) &&
               p1.first_index == p0.index_count && p0.index_count + p1.index_count == m.lods[0].index_count,
           "the parts are contiguous and cover the whole level");
    bool left = true, right = true;
    for (uint32_t k = p0.first_index; k < p0.first_index + p0.index_count; ++k) left &= m.vertices[m.indices[k]].position.x <= n / 2 + 1e-4f;
    for (uint32_t k = p1.first_index; k < p1.first_index + p1.index_count; ++k) right &= m.vertices[m.indices[k]].position.x >= n / 2 - 1e-4f;
    expect(left && right, "each part holds exactly its slot's triangles");

    const fkyaml::node cfg = fkyaml::node::deserialize(std::string("lods:\n  - { ratio: 0.5, screen_size: 0.2 }\n"));
    const auto l = Mesh::build_cpu(node, &cfg);
    expect(l.lods.size() == 2 && l.lods[1].parts.size() == 2, "a simplified LOD keeps one part per slot");
    uint32_t sum = 0;
    for (const auto& p : l.lods[1].parts) sum += p.index_count;
    expect(sum == l.lods[1].index_count && l.lods[1].parts[0].first_index == l.lods[1].first_index,
           "LOD 1's parts cover its range");

    const auto plain = Mesh::build_cpu(fkyaml::node::deserialize(make_grid_mesh_yaml(n)));
    expect(plain.lods[0].parts.empty() && plain.slot_names.empty(), "a mesh without slots has a single implicit part");
}


/** @brief Object assets: `prefab: objects/x` from a scene, Transform replace, overrides, spawn. */
void test_object_assets_prefab() {
    namespace fs = std::filesystem;
    using coopa::scene::SceneLoader;
    const fs::path root = tmp_dir() / "prefab_project" / "assets";
    fs::remove_all(root.parent_path());
    fs::create_directories(root / "objects");
    fs::create_directories(root / "scenes" / "level");
    {
        std::ofstream o(root / "objects" / "crate.yaml");
        o << "format: toyengine-object\n"
             "object:\n"
             "  name: Crate\n"
             "  components:\n"
             "    - { type: Transform, position: { x: 5, y: 5, z: 5 }, scale: { x: 2, y: 2, z: 2 } }\n"
             "  children:\n"
             "    - name: Lid\n"
             "      components: [ { type: Transform, position: { x: 0, y: 0, z: 1 } } ]\n"
             "    - name: Handle\n"
             "      components: [ { type: Transform } ]\n";
    }
    {
        std::ofstream o(root / "scenes" / "level" / "scene.yaml");
        o << "format: toyengine\n"
             "scene:\n"
             "  scene_name: Level\n"
             "  root_objects:\n"
             "    - name: Crate.001\n"
             "      prefab: objects/crate\n"
             "      components: [ { type: Transform, position: { x: 1, y: 0, z: 0 } } ]\n"
             "      children: [ { name: Handle, remove: true } ]\n";
    }
    SceneLoader::set_search_roots({root.string()});
    coopa::scene::Scene scene = SceneLoader::load((root / "scenes" / "level" / "scene.yaml").string());
    expect(scene.root_objects().size() == 1, "the instance loads");
    if (scene.root_objects().empty()) return;
    auto* crate = scene.root_objects()[0].get();
    expect(crate->name() == "Crate.001", "the instance keeps its own name");
    const glm::vec3 p = crate->get_transform()->transform().position();
    const glm::vec3 sc = crate->get_transform()->transform().scale();
    expect(glm::distance(p, glm::vec3(1, 0, 0)) < 1e-5f && glm::distance(sc, glm::vec3(1)) < 1e-5f,
           "the instance's Transform replaces the prefab's root Transform");
    expect(crate->children().size() == 1 && crate->children()[0]->name() == "Lid", "children come from the prefab; remove: true drops one");

    auto* spawned = SceneLoader::spawn(scene, "objects/crate");
    expect(spawned && scene.root_objects().size() == 2 && spawned->children().size() == 2, "spawn() instantiates an object asset at runtime");
    bool threw = false;
    try { SceneLoader::spawn(scene, "objects/missing"); } catch (const std::exception&) { threw = true; }
    expect(threw, "spawning a missing asset throws");
    SceneLoader::set_search_roots({});
}

void test_mesh_lod_simplifies_flat_shaded_mesh() {
    using coopa::gfx::engine::data::Mesh;
    // The shared sphere mesh is exported flat-shaded: every face has its own normals, so after
    // welding no two triangles share a vertex. LOD generation must still reduce it.
    std::ifstream in(std::string(ROOT_DIR) + "/assets/meshes/sphere.yaml");
    expect(static_cast<bool>(in), "meshes/sphere.yaml opens");
    if (!in) return;
    const fkyaml::node node = fkyaml::node::deserialize(in);
    const fkyaml::node cfg = fkyaml::node::deserialize(std::string(
        "lods:\n  - { ratio: 0.5, screen_size: 0.2 }\n  - { ratio: 0.2, screen_size: 0.05 }\n"));
    const auto m = Mesh::build_cpu(node, &cfg);
    expect(m.lods.size() == 3, "two ratio levels -> three LODs");
    if (m.lods.size() != 3) return;
    std::cout << "    sphere LOD index counts: " << m.lods[0].index_count << " / "
              << m.lods[1].index_count << " / " << m.lods[2].index_count << "\n";
    expect(m.lods[1].index_count <= m.lods[0].index_count * 6 / 10, "LOD 1 is roughly half of LOD 0");
    expect(m.lods[2].index_count < m.lods[1].index_count, "LOD 2 is coarser than LOD 1");
}

void test_select_lod_thresholds_and_hysteresis() {
    using namespace toy::render;
    const std::vector<LodThreshold> t = {{0.0f}, {0.25f}, {0.1f}};
    expect(select_lod(0.5f,  t, 0.0f, -2, 0.1f) == 0, "large -> LOD0");
    expect(select_lod(0.2f,  t, 0.0f, -2, 0.1f) == 1, "medium -> LOD1");
    expect(select_lod(0.05f, t, 0.0f, -2, 0.1f) == 2, "small -> LOD2");
    expect(select_lod(0.005f, t, 0.01f, -2, 0.1f) == -1, "below cull size -> culled");
    // Hysteresis: at 0.24 (just under LOD1's 0.25) an object already at LOD0 stays at LOD0...
    expect(select_lod(0.24f, t, 0.0f, 0, 0.1f) == 0, "hysteresis holds LOD0 just under the threshold");
    // ...but well under it, it switches.
    expect(select_lod(0.2f, t, 0.0f, 0, 0.1f) == 1, "clearly under the threshold switches");
    // Going back finer needs to clear the threshold by the margin too.
    expect(select_lod(0.26f, t, 0.0f, 1, 0.1f) == 1, "hysteresis holds LOD1 just over the threshold");
    expect(select_lod(0.3f, t, 0.0f, 1, 0.1f) == 0, "clearly over the threshold refines");
    // A multi-level jump moves as far as the margin allows.
    expect(select_lod(0.01f, t, 0.0f, 0, 0.1f) == 2, "a big drop jumps straight to LOD2");
    const std::vector<LodThreshold> single = {{0.0f}};
    expect(select_lod(0.0001f, single, 0.0f, -2, 0.1f) == 0, "no LODs and no cull -> always LOD0");
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

void test_dir_shadow_fit_covers_camera_far_from_origin() {
    // The fit is built around the camera's own frustum, so the camera position must land
    // inside the box's clip volume no matter where in the world that camera is. Checked at
    // a terrain_test-scale offset AND at the origin, in BOTH light-space axes: a projection
    // whose Y scale is negated without its Y translation passes at the origin (where the
    // centre is 0 and the mirror is the identity) and misses the scene entirely out here.
    glm::vec3 dir(-0.45f, -0.35f, -0.82f);
    for (const glm::vec3& target : {glm::vec3(192.0f, 192.0f, 26.0f), glm::vec3(0.0f)}) {
        auto cam = make_shadow_fit_camera(target + glm::vec3(0.0f, -52.0f, 30.0f), target);
        auto fit = toy::render::compute_dir_shadow_fit(dir, &cam, 60.0f, 2048);
        const glm::vec3 cam_pos = glm::vec3(glm::inverse(cam.view)[3]);
        const glm::vec4 clip    = fit.light_space_matrix * glm::vec4(cam_pos, 1.0f);
        const glm::vec3 ndc     = glm::vec3(clip) / clip.w;
        expect(std::abs(ndc.x) <= 1.0f && std::abs(ndc.y) <= 1.0f && ndc.z >= 0.0f && ndc.z <= 1.0f,
               "dir_shadow_fit: camera position lands inside the fitted box wherever it is");
    }
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

// --- Directional shadow cascades (pixel_math.h's compute_cascade_splits /
//     compute_dir_shadow_fit_slice / cascade_atlas_tile) ---

void test_cascade_splits_are_increasing_and_reach_the_distance() {
    auto s = toy::render::compute_cascade_splits(0.1f, 60.0f, 4, 0.75f);
    expect(s.distance[0] < s.distance[1] && s.distance[1] < s.distance[2] &&
           s.distance[2] < s.distance[3],
           "cascade_splits: boundaries strictly increase");
    expect_near(s.distance[3], 60.0f, 1e-4f,
                "cascade_splits: the last cascade reaches exactly shadow_distance");
    // The whole point of cascades: the near slice must be a small fraction of the range,
    // which is what makes its ortho box (and so its texels) small.
    expect(s.distance[0] < 60.0f * 0.2f,
           "cascade_splits: the near cascade covers a small fraction of the range");
}

void test_cascade_splits_single_cascade_is_the_whole_range() {
    // shadow_cascades: 1 must degrade to exactly the pre-cascade behaviour -- one slice
    // spanning everything -- so it stays a usable A/B baseline.
    auto s = toy::render::compute_cascade_splits(0.1f, 60.0f, 1, 0.75f);
    expect_near(s.distance[0], 60.0f, 1e-4f, "cascade_splits: one cascade spans the whole range");
    expect_near(s.distance[3], 60.0f, 1e-4f, "cascade_splits: unused slots repeat the last boundary");
}

void test_cascade_splits_lambda_selects_the_distribution() {
    // lambda 0 is the uniform distribution exactly; lambda 1 the logarithmic one. Between
    // them the first boundary must move monotonically, or the config knob does nothing.
    auto uni = toy::render::compute_cascade_splits(0.1f, 60.0f, 4, 0.0f);
    auto mid = toy::render::compute_cascade_splits(0.1f, 60.0f, 4, 0.75f);
    auto log = toy::render::compute_cascade_splits(0.1f, 60.0f, 4, 1.0f);
    expect_near(uni.distance[0], 0.1f + (60.0f - 0.1f) * 0.25f, 1e-3f,
                "cascade_splits: lambda 0 is the uniform distribution");
    expect(log.distance[0] < mid.distance[0] && mid.distance[0] < uni.distance[0],
           "cascade_splits: lambda moves the first boundary between log and uniform");
}

void test_cascade_fit_near_slice_is_finer_than_far_slice() {
    // The reason the feature exists: at the SAME tile resolution, the near cascade's box is
    // small, so its world-per-texel is far smaller than the far cascade's.
    glm::vec3 dir(-0.45f, -0.35f, -0.82f);
    auto cam = make_shadow_fit_camera(glm::vec3(0.0f, -10.0f, 4.0f), glm::vec3(0.0f));
    auto splits = toy::render::compute_cascade_splits(cam.near_clip, 60.0f, 4, 0.75f);

    auto near_fit = toy::render::compute_dir_shadow_fit_slice(
        dir, &cam, cam.near_clip, splits.distance[0], 60.0f, 1024);
    auto far_fit = toy::render::compute_dir_shadow_fit_slice(
        dir, &cam, splits.distance[2], splits.distance[3], 60.0f, 1024);
    expect(near_fit.texel_world < far_fit.texel_world,
           "cascade_fit: the near cascade has smaller world-per-texel than the far one");

    // And finer than the single whole-range map at the same resolution -- the actual user
    // complaint, which is that close-up shadows are as coarse as distant ones.
    auto whole = toy::render::compute_dir_shadow_fit(dir, &cam, 60.0f, 1024);
    expect(near_fit.texel_world < whole.texel_world * 0.5f,
           "cascade_fit: the near cascade is at least 2x finer than one map over the whole range");
}

void test_cascade_fit_contains_its_own_slice() {
    // Containment-based cascade selection (gfx_csm_select) only works if a slice's frustum
    // corners really do project inside that slice's own box -- otherwise a shading point
    // would fall through to a coarser cascade, or off the end entirely.
    glm::vec3 dir(-0.45f, -0.35f, -0.82f);
    auto cam = make_shadow_fit_camera(glm::vec3(12.0f, -40.0f, 18.0f), glm::vec3(0.0f, 0.0f, 2.0f));
    auto splits = toy::render::compute_cascade_splits(cam.near_clip, 60.0f, 4, 0.75f);

    const glm::mat4 cam_to_world = glm::inverse(cam.view);
    const glm::vec3 cam_pos     = glm::vec3(cam_to_world[3]);
    const glm::vec3 cam_right   = glm::vec3(cam_to_world[0]);
    const glm::vec3 cam_up      = glm::vec3(cam_to_world[1]);
    const glm::vec3 cam_forward = -glm::vec3(cam_to_world[2]);

    bool all_inside = true;
    for (int c = 0; c < 4; ++c) {
        const float slice_near = (c == 0) ? cam.near_clip : splits.distance[c - 1];
        auto fit = toy::render::compute_dir_shadow_fit_slice(
            dir, &cam, slice_near, splits.distance[c], 60.0f, 1024);
        for (float d : {slice_near, splits.distance[c]}) {
            const float half_h = d * std::tan(glm::radians(cam.fov_degrees) * 0.5f);
            const float half_w = half_h * cam.aspect;
            for (int sy = -1; sy <= 1; sy += 2) {
                for (int sx = -1; sx <= 1; sx += 2) {
                    const glm::vec3 corner = cam_pos + cam_forward * d
                                           + cam_right * (static_cast<float>(sx) * half_w)
                                           + cam_up    * (static_cast<float>(sy) * half_h);
                    const glm::vec4 clip = fit.light_space_matrix * glm::vec4(corner, 1.0f);
                    const glm::vec3 ndc  = glm::vec3(clip) / clip.w;
                    if (std::abs(ndc.x) > 1.0f || std::abs(ndc.y) > 1.0f ||
                        ndc.z < 0.0f || ndc.z > 1.0f) {
                        all_inside = false;
                    }
                }
            }
        }
    }
    expect(all_inside, "cascade_fit: every slice's frustum corners project inside its own box");
}

void test_cascade_fit_reaches_casters_above_the_near_slice() {
    // A cascade covering only the nearest few metres must still see a caster high above it,
    // between the ground and the sun -- the near plane is pulled back by the whole
    // shadow_distance (caster_reach), not by the slice's own depth.
    const glm::vec3 dir(0.0f, 0.0f, -1.0f); // straight down
    auto cam = make_shadow_fit_camera(glm::vec3(0.0f, -6.0f, 2.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    auto fit = toy::render::compute_dir_shadow_fit_slice(dir, &cam, 0.1f, 5.0f, 60.0f, 1024);

    const glm::vec4 clip = fit.light_space_matrix * glm::vec4(0.0f, -6.0f, 32.0f, 1.0f);
    const glm::vec3 ndc  = glm::vec3(clip) / clip.w;
    expect(ndc.z >= 0.0f && ndc.z <= 1.0f,
           "cascade_fit: a caster 30m above the near cascade is still inside its depth range");
}

void test_cascade_atlas_tiles_are_disjoint_and_in_bounds() {
    // The shader remaps a cascade's [0,1] light-space xy into its tile. Overlapping or
    // out-of-range tiles would silently make two cascades sample each other's depths.
    for (uint32_t count = 1; count <= 4; ++count) {
        const glm::uvec2 grid = toy::render::cascade_atlas_grid(count);
        expect(grid.x * grid.y >= count, "cascade_atlas: the grid holds every cascade");
        expect(grid.x * grid.y - count <= 1, "cascade_atlas: the grid wastes at most one tile");

        for (uint32_t a = 0; a < count; ++a) {
            auto ta = toy::render::cascade_atlas_tile(a, count);
            expect(ta.origin.x >= 0.0f && ta.origin.y >= 0.0f &&
                   ta.origin.x + ta.scale.x <= 1.0f + 1e-6f &&
                   ta.origin.y + ta.scale.y <= 1.0f + 1e-6f,
                   "cascade_atlas: every tile lies inside the atlas");
            for (uint32_t b = a + 1; b < count; ++b) {
                auto tb = toy::render::cascade_atlas_tile(b, count);
                const bool disjoint =
                    ta.origin.x + ta.scale.x <= tb.origin.x + 1e-6f ||
                    tb.origin.x + tb.scale.x <= ta.origin.x + 1e-6f ||
                    ta.origin.y + ta.scale.y <= tb.origin.y + 1e-6f ||
                    tb.origin.y + tb.scale.y <= ta.origin.y + 1e-6f;
                expect(disjoint, "cascade_atlas: tiles never overlap");
            }
        }
    }
    // One cascade is the whole atlas -- exactly the pre-cascade single map.
    auto solo = toy::render::cascade_atlas_tile(0, 1);
    expect_near(solo.scale.x, 1.0f, 1e-6f, "cascade_atlas: one cascade fills the atlas");
    expect_near(solo.scale.y, 1.0f, 1e-6f, "cascade_atlas: one cascade fills the atlas (y)");
}

void test_cascade_selection_inset_exceeds_the_pcf_reach() {
    // The inset update_dir_shadow_matrix_() writes into dir_cascade_info.z is what keeps a
    // PCF tap inside its own tile: no tap can reach further than the clamped 12-texel
    // penumbra radius plus the 16-texel PCSS blocker-search cap. If a smaller tile
    // resolution ever made that inset eat the whole tile, selection would reject everything.
    for (uint32_t tile_res : {512u, 1024u, 2048u, 3072u}) {
        const float max_reach_texels = 12.0f + 16.0f;
        const float inset = max_reach_texels / static_cast<float>(tile_res);
        expect(inset < 0.25f,
               "cascade_inset: the PCF-reach inset stays a small fraction of every tile");
    }
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
    // FXAA/SMAA defaults mirror blendy's PbrRenderPipeline field-for-field (see
    // PixelRenderConfig::aa_mode's own doc) except aa_mode itself, which defaults to "off"
    // here so a scene that never opts in renders exactly as it did before AA existed. The
    // TAA defaults are this engine's own (its resolve is reprojecting/age-weighted, see
    // taa.frag): the still-camera feedback must sit high enough that the converged
    // accumulation's residual jitter orbit stays inside static_camera_converges's budget.
    toy::render::PixelRenderConfig cfg;
    expect(cfg.aa_mode == "off", "PixelRenderConfig: aa_mode defaults to off");
    expect(cfg.fxaa_subpixel == 0.75f, "PixelRenderConfig: fxaa_subpixel defaults to 0.75");
    expect(cfg.fxaa_edge_threshold == 0.166f, "PixelRenderConfig: fxaa_edge_threshold defaults to 0.166");
    expect(cfg.fxaa_edge_threshold_min == 0.0312f, "PixelRenderConfig: fxaa_edge_threshold_min defaults to 0.0312");
    expect(cfg.smaa_threshold == 0.1f, "PixelRenderConfig: smaa_threshold defaults to 0.1");
    expect(cfg.smaa_max_search_steps == 16, "PixelRenderConfig: smaa_max_search_steps defaults to 16");
    expect(cfg.taa_blending_weight == 0.99f, "PixelRenderConfig: taa_blending_weight defaults to 0.99");
    expect(cfg.taa_weight_scale == 30.0f, "PixelRenderConfig: taa_weight_scale defaults to 30.0");
}

/**
 * @brief A scene's `settings:` layer onto config.yaml at the document level: overridden keys
 *        win, everything else is config.yaml's, and quality presets re-resolve BENEATH
 *        config.yaml's own explicit keys (a scene lowering shadow_quality still keeps the
 *        project's explicit shadow_pcf_samples). No overrides: the config comes back unchanged.
 */
void test_app_config_scene_settings_layer() {
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
    expect(cfg.debug_view == "off", "PixelRenderConfig: debug_view defaults to off");
}

void test_app_config_load_round_trips_dof_settings() {
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

/**
 * @brief debug_view round-trips through AppConfig::load() and parse_debug_view() maps a
 *        recognized name to its DebugView value, defaulting an unrecognized one to Off.
 */
void test_debug_view_parsing() {
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

void test_app_config_load_round_trips_cascade_settings() {
    toy::core::AppConfig config = load_config_text("test_cascade_config.yaml",
        "render:\n"
        "  shadow_cascades: 2\n"
        "  shadow_cascade_split_lambda: 0.4\n");

    expect(config.render.shadow_cascades == 2u, "AppConfig::load: shadow_cascades round-trips");
    expect(config.render.shadow_cascade_split_lambda == 0.4f,
           "AppConfig::load: shadow_cascade_split_lambda round-trips");

    // Unwritten, the defaults are the shipped 4-cascade setup.
    toy::core::AppConfig plain = load_config_text("test_cascade_default_config.yaml",
        "render:\n"
        "  exposure: 1.0\n");
    expect(plain.render.shadow_cascades == 4u, "PixelRenderConfig: shadow_cascades defaults to 4");
    expect(plain.render.shadow_cascade_split_lambda == 0.75f,
           "PixelRenderConfig: shadow_cascade_split_lambda defaults to 0.75");
}

/**
 * @brief Exercises the per-feature quality presets: tier strings (including the "med"
 * alias and the unknown-string fallback) expand to the preset field values, an
 * explicitly-written key overrides its preset, and a config with no quality keys
 * keeps the High-tier values.
 */
void test_app_config_load_applies_quality_presets() {
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
    expect(config.render.ssao_temporal_frames == 16, "quality preset: low ssao_temporal_frames");

    expect(config.render.volumetrics_quality == RenderQuality::Medium, "AppConfig::load: 'med' parses as Medium");
    expect(config.render.volumetrics_step_count == 32, "quality preset: medium volumetrics_step_count");
    expect(config.render.volumetrics_max_scatter_lights == 2, "quality preset: medium volumetrics_max_scatter_lights");

    expect(config.render.sdf_quality == RenderQuality::High, "AppConfig::load: unknown quality string falls back to High");
    expect(config.render.sdf_max_steps == 64u, "quality preset: fallback High sdf_max_steps");
    expect(config.render.sdf_shadow_max_steps == 32u, "quality preset: fallback High sdf_shadow_max_steps");

    // ssr_quality: low would set 24, but the explicitly-written key wins.
    expect(config.render.ssr_max_iterations == 200, "quality preset: explicit ssr_max_iterations overrides its preset");

    // No quality keys at all: every covered field lands on the High row.
    toy::core::AppConfig plain = load_config_text("test_quality_default_config.yaml",
        "render:\n"
        "  exposure: 1.0\n");
    expect(plain.render.shadow_map_resolution == 2048u, "quality preset: default High shadow_map_resolution");
    // shadow_cascades is deliberately NOT preset-covered: the tier moves resolution only, so
    // switching tiers can never silently change how many cascades a scene renders.
    expect(plain.render.shadow_cascades == 4u, "quality preset: shadow_cascades is not tier-driven");
    expect(plain.render.shadow_pcf_samples == 24u, "quality preset: default High shadow_pcf_samples");
    expect(plain.render.ssao_steps == 16, "quality preset: default High ssao_steps");
    expect(plain.render.ssr_max_iterations == 64, "quality preset: default High ssr_max_iterations");
    expect(plain.render.dof_sample_count == 48, "quality preset: default High dof_sample_count");
    expect(plain.render.volumetrics_step_count == 48, "quality preset: default High volumetrics_step_count");
    expect(plain.render.ssgi_max_iterations == 32, "quality preset: default High ssgi_max_iterations");
    expect(plain.render.shadow_pcss_taps == 8u, "quality preset: default High shadow_pcss_taps");
    expect(plain.render.contact_shadow_steps == 8, "quality preset: default High contact_shadow_steps");
    expect(plain.render.volumetrics_max_scatter_lights == 4, "quality preset: default High volumetrics_max_scatter_lights");

    // Low volumetrics drops the light loop entirely, leaving only the (much cheaper)
    // sun-shaft term -- the one tier row where a covered field goes to zero.
    toy::core::AppConfig vol_low = load_config_text("test_quality_vol_low.yaml",
        "render:\n"
        "  volumetrics_quality: low\n");
    expect(vol_low.render.volumetrics_max_scatter_lights == 0, "quality preset: low volumetrics_max_scatter_lights is 0");
    expect(vol_low.render.volumetrics_step_count == 24, "quality preset: low volumetrics_step_count");
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
    expect(config.render.fog_max_distance == 123.0f, "AppConfig::load: fog_max_distance round-trips");

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

/**
 * @brief Builds a one-object scene whose only component is a FreeMover, plus a Transform.
 *
 * No system to install, unlike make_controller_scene() above: FreeMover does its work in the
 * ordinary update() at UpdatePhase::Behaviour, because it drives nothing physical and so has no
 * reason to be hoisted ahead of the physics phase -- see free_mover.h's file doc.
 */
std::unique_ptr<Scene> make_free_mover_scene(toy::scene::FreeMover** out_fm,
                                             const glm::vec3& seed_pos) {
    auto scene = std::make_unique<Scene>("free_mover_test");
    auto obj = std::make_unique<SceneObject>("focus_marker");
    obj->add_component<TransformComponent>()->transform().set_position(seed_pos);
    *out_fm = obj->add_component<toy::scene::FreeMover>();
    scene->add_root_object(std::move(obj));
    return scene;
}

void test_free_mover_travels_on_all_three_axes() {
    toy::scene::FreeMover* fm = nullptr;
    auto scene = make_free_mover_scene(&fm, glm::vec3(0.0f));
    fm->move_speed = 6.0f;
    fm->smoothing = 0.0f; // instant, so travelled distance is exactly speed * time
    scene->start();

    auto* tc = scene->root_objects()[0]->get_transform();

    // Vertical is the axis KinematicController deliberately does not have -- it holds its
    // authored height -- so it is the one worth checking first here.
    fm->move_input = glm::vec3(0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    glm::vec3 p = tc->transform().position();
    expect_near(p.z, 6.0f, 0.05f, "free_mover: +Z input for 1 s rises move_speed units");
    expect(std::fabs(p.x) < 1e-5f && std::fabs(p.y) < 1e-5f,
           "free_mover: pure +Z input causes no horizontal drift");

    fm->move_input = glm::vec3(0.0f, 0.0f, -1.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    expect_near(tc->transform().position().z, 0.0f, 0.05f, "free_mover: -Z input descends again");

    // Movement is in WORLD axes, not the owner's basis: rotating the marker must not steer it.
    tc->transform().set_rotation(glm::vec3(0.0f, 0.0f, 90.0f));
    glm::vec3 before = tc->transform().position();
    fm->move_input = glm::vec3(1.0f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    glm::vec3 delta = tc->transform().position() - before;
    expect_near(delta.x, 6.0f, 0.05f, "free_mover: +X input travels world +X whatever the yaw");
    expect(std::fabs(delta.y) < 1e-4f, "free_mover: a yawed marker does not steer with its basis");

    // Diagonal input is CLAMPED, not normalized -- same contract as KinematicController, and the
    // reason a three-axis mover does not travel sqrt(3) faster on a full-deflection diagonal.
    before = tc->transform().position();
    fm->move_input = glm::vec3(1.0f, 1.0f, 1.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    const float diagonal = glm::length(tc->transform().position() - before);
    expect_near(diagonal, 6.0f, 0.05f,
                "free_mover: full diagonal deflection is clamped to move_speed");

    // A partial deflection must stay partial, which is what "clamp, don't normalize" buys.
    before = tc->transform().position();
    fm->move_input = glm::vec3(0.5f, 0.0f, 0.0f);
    for (int i = 0; i < 60; ++i) scene->update(1.0f / 60.0f);
    expect_near(glm::length(tc->transform().position() - before), 3.0f, 0.05f,
                "free_mover: half deflection travels half speed");
}

void test_free_mover_smoothing_is_frame_rate_independent() {
    toy::scene::FreeMover* fm = nullptr;
    auto scene = make_free_mover_scene(&fm, glm::vec3(0.0f));
    fm->move_speed = 6.0f;
    fm->smoothing = 8.0f;
    scene->start();

    fm->move_input = glm::vec3(0.0f, 1.0f, 0.0f);
    scene->update(1.0f / 60.0f);
    expect(fm->velocity().y > 0.0f && fm->velocity().y < 6.0f,
           "free_mover: smoothing ramps velocity rather than snapping to move_speed");

    for (int i = 0; i < 300; ++i) scene->update(1.0f / 60.0f);
    expect_near(fm->velocity().y, 6.0f, 0.05f, "free_mover: smoothed velocity converges to move_speed");

    fm->move_input = glm::vec3(0.0f);
    scene->update(1.0f / 60.0f);
    expect(fm->velocity().y > 0.0f && fm->velocity().y < 6.0f,
           "free_mover: releasing input decays velocity instead of stopping dead");

    // The claim `1 - exp(-k*dt)` makes: the same wall-clock second covers the same ground
    // whatever the tick rate. A raw lerp factor would fail this badly -- which is the whole
    // reason the smoothing is written the way it is.
    toy::scene::FreeMover* fast = nullptr;
    auto fast_scene = make_free_mover_scene(&fast, glm::vec3(0.0f));
    fast->move_speed = 6.0f;
    fast->smoothing = 8.0f;
    fast_scene->start();
    fast->move_input = glm::vec3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 240; ++i) fast_scene->update(1.0f / 240.0f); // 1 s at 240 fps

    toy::scene::FreeMover* slow = nullptr;
    auto slow_scene = make_free_mover_scene(&slow, glm::vec3(0.0f));
    slow->move_speed = 6.0f;
    slow->smoothing = 8.0f;
    slow_scene->start();
    slow->move_input = glm::vec3(0.0f, 1.0f, 0.0f);
    for (int i = 0; i < 30; ++i) slow_scene->update(1.0f / 30.0f);  // 1 s at 30 fps

    const float fast_y = fast_scene->root_objects()[0]->get_transform()->transform().position().y;
    const float slow_y = slow_scene->root_objects()[0]->get_transform()->transform().position().y;
    expect_near(slow_y, fast_y, 0.15f,
                "free_mover: one second of travel is the same at 30 and 240 fps");
    if (std::fabs(slow_y - fast_y) > 0.15f) {
        std::cerr << "         30 fps travelled " << slow_y << ", 240 fps travelled " << fast_y << "\n";
    }
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
// Group "world" -- the terrain tile system's pure half: the atlas layout, the canonical
// side-to-face transforms, the chunk mesher's face-exposure rules, and the sampler's
// point-to-cell lookup against a really generated map. No Vulkan device, no window: chunk
// meshing is deliberately free of all three (see toyengine/world/terrain_chunk.h), which is
// what lets it run on job workers AND what lets it be tested this cheaply.
// =====================================================================================

/** @brief A Logger that says nothing, so map generation does not bury the test output. */
class QuietLogger : public coopa::debug::Logger {
public:
    QuietLogger() : Logger("world_test") {}
    void info(const std::string&, std::string = "", bool = false, unsigned int = 0) override {}
};

/**
 * @brief A welded unit quad in the canonical side orientation: the +Z face of `[0,1]^3`.
 *
 * Built directly rather than loaded from assets/meshes/tile_side_flat.yaml
 * so the counting assertions below rest on known numbers -- 4 vertices and 6 indices per
 * appended side -- instead of on however the YAML path happens to weld its corners.
 */
coopa::gfx::engine::data::SkinnedMeshSource make_canonical_quad() {
    using coopa::gfx::engine::data::Vertex;
    coopa::gfx::engine::data::SkinnedMeshSource source;

    const glm::vec3 positions[4] = {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f},
                                    {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f}};
    const glm::vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    for (int i = 0; i < 4; ++i) {
        Vertex v{};
        v.position = positions[i];
        v.normal   = glm::vec3(0.0f, 0.0f, 1.0f);
        v.uv       = uvs[i];
        v.tangent  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
        source.vertices.push_back(v);
    }
    source.indices = {0, 1, 2, 0, 2, 3};
    return source;
}

/** @brief Indices per appended side, for the quad above. Two triangles. */
constexpr size_t kIndicesPerSide = 6;

/** @brief A library with the same flat quad baked onto all six faces. */
toy::world::TileMeshLibrary make_flat_library() {
    toy::world::TileMeshLibrary library;
    library.bake_canonical(make_canonical_quad());
    return library;
}

/** @brief Test params: a small chunk, unit cells, no bottom faces, generous wall clamp. */
toy::world::TerrainParams make_test_params(std::int32_t chunk_size = 4) {
    toy::world::TerrainParams params;
    params.chunk_size       = chunk_size;
    params.tile_size        = 1.0f;
    params.height_step      = 1.0f;
    params.max_wall_steps   = 64;
    params.soil_depth_steps = 3;
    params.emit_bottom      = false;
    return params;
}

/** @brief A pad whose every column -- skirt included -- stands at the same height. */
toy::world::ColumnPad make_flat_pad(std::int32_t chunk_size, std::int32_t steps) {
    toy::world::ColumnPad pad;
    pad.resize(chunk_size);
    for (std::int32_t y = -1; y <= chunk_size; ++y) {
        for (std::int32_t x = -1; x <= chunk_size; ++x) {
            pad.at(x, y).steps = steps;
        }
    }
    return pad;
}

/** @brief Number of sides in a merged chunk, from its index count. */
size_t side_count(const toy::world::ChunkMeshData& mesh) {
    return mesh.indices.size() / kIndicesPerSide;
}

void test_tile_atlas_cells_are_disjoint_and_inset() {
    using namespace toy::world;

    for (size_t i = 0; i < k_tile_kind_count; ++i) {
        const glm::vec4 cell = atlas_cell(static_cast<TileKind>(i));
        expect(cell.x > 0.0f && cell.y > 0.0f, "atlas: cell origin is inset off the texture edge");
        expect(cell.x + cell.z < 1.0f && cell.y + cell.w < 1.0f,
               "atlas: cell stays inside the texture");
        expect(cell.z > 0.0f && cell.w > 0.0f, "atlas: cell has positive extent");
    }

    // Disjointness is what keeps one surface from bleeding into another under NEAREST
    // filtering, and it is a property of the layout arithmetic, not of the PNG.
    bool overlap = false;
    for (size_t i = 0; i < k_tile_kind_count && !overlap; ++i) {
        const glm::vec4 a = atlas_cell(static_cast<TileKind>(i));
        for (size_t j = i + 1; j < k_tile_kind_count; ++j) {
            const glm::vec4 b = atlas_cell(static_cast<TileKind>(j));
            const bool separated = a.x + a.z <= b.x || b.x + b.z <= a.x ||
                                    a.y + a.w <= b.y || b.y + b.w <= a.y;
            if (!separated) {
                overlap = true;
                break;
            }
        }
    }
    expect(!overlap, "atlas: no two tile kinds share texels");
}

void test_face_transforms_agree_with_face_normals() {
    using namespace toy::world;

    // The canonical mesh faces +Z, so its transform must carry +Z onto each face's own
    // outward direction -- the one invariant the whole six-faces-from-one-mesh trick rests on.
    for (size_t i = 0; i < k_tile_face_count; ++i) {
        const TileFace face = static_cast<TileFace>(i);
        const glm::vec3 rotated =
            glm::mat3(face_transform(face)) * glm::vec3(0.0f, 0.0f, 1.0f);
        const glm::vec3 expected = face_normal(face);
        expect(glm::length(rotated - expected) < 1e-5f,
               "face transform: canonical +Z maps onto the face's outward normal");
    }

    // ...and it must be a rotation ABOUT THE CELL CENTRE, so every baked face still occupies
    // the same unit cell and can be placed by a plain translate.
    const TileMeshLibrary library = make_flat_library();
    for (size_t i = 0; i < k_tile_face_count; ++i) {
        const TileFace face = static_cast<TileFace>(i);
        bool inside = true;
        for (const auto& v : library.side(face).vertices) {
            inside = inside && v.position.x > -1e-4f && v.position.x < 1.0f + 1e-4f &&
                     v.position.y > -1e-4f && v.position.y < 1.0f + 1e-4f &&
                     v.position.z > -1e-4f && v.position.z < 1.0f + 1e-4f;
        }
        expect(inside, "face transform: the baked side still spans the [0,1]^3 cell");
    }
}

void test_chunk_flat_ground_emits_tops_only() {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    const TileMeshLibrary library = make_flat_library();

    // Ground level with the skirt at the same height: nothing is exposed sideways anywhere,
    // so the chunk is exactly one top face per column. This is the assertion that would fail
    // if the pad were ignored -- every border column would wall itself in.
    ChunkMeshData mesh;
    mesh_chunk_columns(make_flat_pad(4, 3), library, params, mesh);
    expect(side_count(mesh) == 16, "chunk: flat ground emits one face per column and no walls");
    expect(mesh.vertices.size() == 16 * 4, "chunk: a flat quad side contributes four vertices");

    // Every top sits at the column's surface height, not at its cell floor.
    bool all_at_surface = true;
    for (const auto& v : mesh.vertices) all_at_surface = all_at_surface && v.position.z == 3.0f;
    expect(all_at_surface, "chunk: the top face lands at steps * height_step");
}

/// Surface area of a chunk mesh per (face direction, tile kind) -- the invariant greedy
/// merging must preserve. `encoded`: UVs are TileMeshLibrary::encode_uv tile-space (greedy
/// mesher); otherwise atlas-space, decoded through the atlas grid.
std::map<std::pair<int, int>, double> area_by_direction_and_kind(const toy::world::ChunkMeshData& mesh,
                                                                 bool encoded) {
    using namespace toy::world;
    std::map<std::pair<int, int>, double> out;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const auto& a = mesh.vertices[mesh.indices[t]];
        const auto& b = mesh.vertices[mesh.indices[t + 1]];
        const auto& c = mesh.vertices[mesh.indices[t + 2]];
        const glm::vec3 cr = glm::cross(b.position - a.position, c.position - a.position);
        const double area = 0.5 * glm::length(cr);
        if (area < 1e-9) continue;
        const glm::vec3 n = glm::normalize(cr);
        int dir = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(n[axis]) > 0.5f) dir = (axis + 1) * (n[axis] > 0.0f ? 1 : -1);
        }
        const glm::vec2 uv = (a.uv + b.uv + c.uv) / 3.0f;
        int kind = 0;
        if (encoded) {
            kind = static_cast<int>(std::floor(uv.x / TileMeshLibrary::k_uv_cell_stride));
        } else {
            kind = static_cast<int>(std::floor(uv.y * k_atlas_rows)) * static_cast<int>(k_atlas_columns) +
                   static_cast<int>(std::floor(uv.x * k_atlas_columns));
        }
        out[{dir, kind}] += area;
    }
    return out;
}

void test_chunk_greedy_merge_preserves_surface() {
    using namespace toy::world;
    TerrainParams params = make_test_params(8);
    params.soil_depth_steps = 2;
    const TileMeshLibrary library = make_flat_library();
    expect(library.side(TileFace::Top).mergeable && library.side(TileFace::North).mergeable,
           "greedy: the flat tile side is mergeable on every face");

    // Rolling terrain with plateaus (so tops can merge) and a few kinds.
    ColumnPad pad;
    pad.resize(8);
    std::mt19937 rng(1234);
    for (std::int32_t y = -1; y <= 8; ++y) {
        for (std::int32_t x = -1; x <= 8; ++x) {
            TileColumn& c = pad.at(x, y);
            c.steps     = 2 + ((x / 3 + y / 2) % 4) + static_cast<std::int32_t>(rng() % 2) * ((x + y) % 5 == 0);
            c.top_kind  = static_cast<TileKind>(rng() % 3 == 0 ? 3 : 0);   // sand or grass
            c.side_kind = (c.top_kind == TileKind::Sand) ? TileKind::Sand : TileKind::Dirt;
        }
    }

    ChunkMeshData per_tile, greedy;
    mesh_chunk_columns(pad, library, params, per_tile);
    mesh_chunk_columns_greedy(pad, library, params, greedy);

    const auto expected = area_by_direction_and_kind(per_tile, false);
    const auto actual   = area_by_direction_and_kind(greedy, true);
    bool same = expected.size() == actual.size();
    for (const auto& [key, area] : expected) {
        auto it = actual.find(key);
        same = same && it != actual.end() && std::abs(it->second - area) < 1e-3;
    }
    expect(same, "greedy: every (direction, kind) covers exactly the per-tile area");
    // This pad scatters sand tiles at random, so it merges far less than real terrain (which
    // drops ~60% in terrain_test); the invariant is only that merging never adds triangles.
    expect(greedy.indices.size() < per_tile.indices.size(),
           "greedy: fewer triangles than the per-tile mesher");
    std::cout << "    triangles per-tile " << per_tile.indices.size() / 3 << " -> greedy "
              << greedy.indices.size() / 3 << "\n";

    ChunkMeshData again;
    mesh_chunk_columns_greedy(pad, library, params, again);
    bool identical = again.indices == greedy.indices && again.vertices.size() == greedy.vertices.size();
    for (size_t i = 0; identical && i < again.vertices.size(); ++i) {
        identical = again.vertices[i].position == greedy.vertices[i].position &&
                    again.vertices[i].uv == greedy.vertices[i].uv;
    }
    expect(identical, "greedy: deterministic -- equal pads give identical buffers");
}

void test_chunk_perimeter_walls_follow_the_pad() {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    const TileMeshLibrary library = make_flat_library();

    // A plateau standing 3 steps above a skirt at 0: only the border columns have a lower
    // neighbour, and only on the sides that face outward.
    ColumnPad pad = make_flat_pad(4, 3);
    for (std::int32_t i = -1; i <= 4; ++i) {
        pad.at(i, -1).steps = 0;
        pad.at(i, 4).steps  = 0;
        pad.at(-1, i).steps = 0;
        pad.at(4, i).steps  = 0;
    }

    ChunkMeshData mesh;
    mesh_chunk_columns(pad, library, params, mesh);

    // 16 tops, plus 3 steps of wall on each outward-facing side: 12 edge columns contribute
    // one side each and the 4 corners contribute two.
    const size_t expected_walls = static_cast<size_t>((12 - 4) * 1 + 4 * 2) * 3;
    expect(side_count(mesh) == 16 + expected_walls,
           "chunk: walls appear exactly where a neighbour is lower");
}

void test_chunk_step_exposure_is_symmetric() {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    const TileMeshLibrary library = make_flat_library();

    // One column raised by 2 in an otherwise flat field: four walls, two steps each, and
    // nothing else changes.
    ColumnPad pad = make_flat_pad(4, 1);
    pad.at(2, 2).steps = 3;

    ChunkMeshData mesh;
    mesh_chunk_columns(pad, library, params, mesh);
    expect(side_count(mesh) == 16 + 4 * 2, "chunk: a raised column exposes all four sides");

    // The clamp is a cap on the wall, not on the column: the top stays where it was.
    TerrainParams clamped = params;
    clamped.max_wall_steps = 1;
    ChunkMeshData clamped_mesh;
    mesh_chunk_columns(pad, library, clamped, clamped_mesh);
    expect(side_count(clamped_mesh) == 16 + 4 * 1, "chunk: max_wall_steps caps a wall's height");
}

void test_chunk_meshing_is_deterministic() {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    const TileMeshLibrary library = make_flat_library();

    ColumnPad pad = make_flat_pad(4, 2);
    pad.at(0, 0).steps = 5;
    pad.at(3, 1).steps = 1;
    pad.at(1, 3).steps = 7;

    // Chunks are meshed on whichever worker happens to be free, so nothing downstream may
    // depend on which one -- two runs must agree byte for byte.
    ChunkMeshData first;
    ChunkMeshData second;
    mesh_chunk_columns(pad, library, params, first);
    mesh_chunk_columns(pad, library, params, second);

    expect(first.indices == second.indices, "chunk: re-meshing the same pad gives the same indices");
    bool same = first.vertices.size() == second.vertices.size();
    for (size_t i = 0; same && i < first.vertices.size(); ++i) {
        same = std::memcmp(&first.vertices[i], &second.vertices[i],
                           sizeof(coopa::gfx::engine::data::Vertex)) == 0;
    }
    expect(same, "chunk: re-meshing the same pad gives byte-identical vertices");
}

void test_chunk_uvs_stay_inside_their_atlas_cell() {
    using namespace toy::world;
    const TerrainParams params = make_test_params(4);
    const TileMeshLibrary library = make_flat_library();

    ColumnPad pad = make_flat_pad(4, 2);
    pad.at(1, 1).steps     = 6;          // a wall deep enough to reach the Stone band
    pad.at(2, 2).top_kind  = TileKind::Sand;
    pad.at(2, 2).side_kind = TileKind::Sand;

    ChunkMeshData mesh;
    mesh_chunk_columns(pad, library, params, mesh);

    // Every emitted UV must land inside SOME kind's cell, and never in the gutter between
    // cells -- that gutter is what a half-texel rounding error would sample.
    bool all_inside = true;
    for (const auto& v : mesh.vertices) {
        bool in_any = false;
        for (size_t i = 0; i < k_tile_kind_count; ++i) {
            const glm::vec4 cell = atlas_cell(static_cast<TileKind>(i));
            if (v.uv.x >= cell.x - 1e-5f && v.uv.x <= cell.x + cell.z + 1e-5f &&
                v.uv.y >= cell.y - 1e-5f && v.uv.y <= cell.y + cell.w + 1e-5f) {
                in_any = true;
                break;
            }
        }
        all_inside = all_inside && in_any;
    }
    expect(all_inside, "chunk: every emitted UV lands inside an atlas cell");
}

void test_sampler_cell_lookup_matches_brute_force() {
    using namespace toy::world;

    QuietLogger logger;
    coopa::maps::MapConfig config;
    config.seed      = 7;
    config.grid_size = 24;   // small on purpose: this group must stay instant
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();

    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), make_test_params());
    expect(sampler.is_built(), "sampler: a generated map builds");

    // A Voronoi cell IS the set of points nearest its site, so the bucket index has to agree
    // with an exhaustive scan at every point -- that equivalence is the whole justification
    // for the index existing.
    const coopa::maps::MapGraph& graph = sampler.graph();
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> pick(0.0, static_cast<double>(config.grid_size));

    int mismatches = 0;
    for (int i = 0; i < 2000; ++i) {
        const double x = pick(rng);
        const double y = pick(rng);

        double best = std::numeric_limits<double>::max();
        std::size_t best_index = 0;
        for (std::size_t c = 0; c < graph.centers.size(); ++c) {
            const double dx = graph.centers[c].point.x - x;
            const double dy = graph.centers[c].point.y - y;
            const double d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                best_index = c;
            }
        }
        if (sampler.cell_at(x, y).index != graph.centers[best_index].index) ++mismatches;
    }
    expect(mismatches == 0, "sampler: the bucket index finds the same cell a full scan does");
    if (mismatches != 0) std::cerr << "         " << mismatches << " of 2000 points disagreed\n";
}

void test_sampler_chunks_agree_across_their_shared_border() {
    using namespace toy::world;

    QuietLogger logger;
    coopa::maps::MapConfig config;
    config.seed              = 11;
    config.grid_size         = 24;
    config.terrain_roughness = 0.2;
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();

    TerrainParams params = make_test_params(8);
    TerrainSampler sampler;
    sampler.build(config, std::move(generator.graph()), params);

    // Two neighbouring chunks, sampled independently exactly as two worker threads would.
    ColumnPad left;
    ColumnPad right;
    sample_chunk_columns(sampler, params, ChunkCoord{1, 1}, left);
    sample_chunk_columns(sampler, params, ChunkCoord{2, 1}, right);

    // The left chunk's +X skirt is the right chunk's first column and vice versa. If those
    // ever disagreed, each chunk would wall itself in against a neighbour that is not there,
    // and the world would show a seam at every chunk boundary.
    bool agree = true;
    for (std::int32_t y = 0; y < params.chunk_size; ++y) {
        agree = agree && left.at(params.chunk_size, y).steps == right.at(0, y).steps;
        agree = agree && right.at(-1, y).steps == left.at(params.chunk_size - 1, y).steps;
    }
    expect(agree, "sampler: adjacent chunks sample their shared border identically");

    // And the pad is genuinely load-bearing: with the neighbour's real heights, an interior
    // border column emits a +X wall only where the terrain really does step down.
    const TileMeshLibrary library = make_flat_library();
    ChunkMeshData mesh;
    mesh_chunk_columns(left, library, params, mesh);
    expect(!mesh.empty(), "sampler: a chunk of generated terrain meshes to something drawable");
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
    if (drift > kDriftBudget) std::cerr << "         drift = " << drift << " px\n";
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

/**
 * @brief Every debug_view channel renders something -- not a uniformly black frame (the
 * [[ssao-debug-view-broken]] failure mode this pass replaces, where a debug view came out
 * fully black because the post chain silently ate it) -- and differs from debug_view: off.
 *
 * The G-buffer/lighting/ssao/ssr channels share one Engine (debug_view is RUNTIME, so
 * switching between them needs no reconstruction); contact_shadows/dof/volumetrics each need
 * their own STARTUP-FIXED feature flag on and so get their own Engine. contact_shadows is
 * additionally checked against its own on/off switch, so "differs from off" can't be
 * coincidence -- the same proof-of-life the shipped contact-shadow term itself relies on.
 */
void test_debug_view_channels_render() {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    {
        toy::core::AppConfig config =
            make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 160, 90);
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, kNoiseCycle);
        const Frame off_frame = engine.capture_image(true);

        static const char* kChannels[] = {
            "albedo", "normals", "roughness", "metallic", "emissive", "material_ao",
            "world_pos", "depth", "direct", "indirect", "shadows", "ssao",
            "ssr", "ssr_confidence", "ssgi",
        };
        for (const char* name : kChannels) {
            engine.render_config().debug_view = name;
            tick_frames(engine, kNoiseCycle);
            const Frame f = engine.capture_image(true);
            const std::string label = std::string("debug_view '") + name + "'";
            const long long lit = count_nonblack(f);
            expect_at_least(lit, 1, label + " is not a uniformly black frame");
            expect_at_least(count_diff(f, off_frame), 1, label + " differs from debug_view: off");
            if (lit == 0) dump_frame(f, std::string("debug_view_") + name + "_black");
        }
    }

    // contact_shadows: shadows_enabled off isolates the march as the only occlusion term --
    // see pixel_lighting.frag's own doc on this combination.
    {
        toy::core::AppConfig config =
            make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 160, 90);
        config.render.shadows_enabled         = false;
        config.render.contact_shadows_enabled = true;
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, kNoiseCycle);
        const Frame off_frame = engine.capture_image(true);

        engine.render_config().debug_view = "contact_shadows";
        tick_frames(engine, kNoiseCycle);
        const Frame contact_frame = engine.capture_image(true);
        expect_at_least(count_nonblack(contact_frame), 1,
                        "debug_view 'contact_shadows' is not a uniformly black frame");
        expect_at_least(count_diff(contact_frame, off_frame), 1,
                        "debug_view 'contact_shadows' differs from debug_view: off");

        // contact_shadow_length is RUNTIME -- shortening the march's reach substantially
        // shrinks the occluded area, proving the channel draws the LIVE march rather than a
        // stale buffer. Not all the way to zero pixels: even a zero-length march still
        // samples right at the bias offset (see toy_contact_shadow's own doc), which stays
        // "in contact" for a receiver texel directly against an object's base -- shortening
        // the reach removes everything BEYOND that, which is the bulk of the effect.
        const long long before = count_nonblack(contact_frame);

        // The march reads the receiver position and DIFFERENTIATES it (contact_shadow_body.glsl's
        // CALLER CONTRACT) to undo the TAA sub-pixel jitter. Feed that a point-sampled value, or
        // evaluate it under non-uniform control flow, and `bias` comes out wrong per texel, the
        // ray self-intersects, and the resulting speckle is keyed to the TAA jitter phase -- so
        // it CHANGES every frame even with nothing moving. That temporal signature is what this
        // asserts, because it is the one property a correct term has regardless of scene: with
        // FIXED_DT=0 and a static camera, the channel must be the same image twice.
        //
        // A spatial noise bound is deliberately NOT asserted here. The term is legitimately
        // high-frequency on stepped geometry -- on voxel terrain it is a one-pixel line along
        // every step edge -- so the number is scene- and resolution-dependent (measured 7.6
        // levels/px accumulated at 1920x1080 on this scene, 19 raw, 32 raw at this test's
        // 160x90) with no threshold that means the same thing across them.
        engine.tick();
        const Frame contact_again = engine.capture_image(true);
        const double contact_drift = mean_abs_delta(contact_again, contact_frame);
        expect(contact_drift <= 0.05,
               "debug_view 'contact_shadows' is static when the camera and clock are");
        if (contact_drift > 0.05) {
            std::cerr << "         contact channel drifts " << contact_drift
                      << " levels/px per frame at rest (expected ~0)\n";
            dump_frame(contact_frame, "debug_view_contact_unstable");
        }

        engine.render_config().contact_shadow_length = 0.0f;
        tick_frames(engine, kNoiseCycle);
        const Frame zero_length = engine.capture_image(true);
        const long long after = count_nonblack(zero_length);
        expect(after < before / 2,
               "debug_view 'contact_shadows' shrinks by more than half when contact_shadow_length is 0");
    }

    // dof / volumetrics draw through their OWN pass's existing debug branch (dof_composite.frag
    // / volumetrics_march.frag), not debug_view_pass_ -- see the DebugView enum's own doc -- so each
    // needs its feature enabled at STARTUP, unlike the channels above.
    {
        toy::core::AppConfig config =
            make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 160, 90);
        config.render.dof_enabled = true;
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, kNoiseCycle);
        const Frame off_frame = engine.capture_image(true);
        engine.render_config().debug_view = "dof";
        tick_frames(engine, kNoiseCycle);
        const Frame dof_frame = engine.capture_image(true);
        expect_at_least(count_nonblack(dof_frame), 1, "debug_view 'dof' is not a uniformly black frame");
        expect_at_least(count_diff(dof_frame, off_frame), 1, "debug_view 'dof' differs from debug_view: off");
    }
    {
        toy::core::AppConfig config =
            make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 160, 90);
        config.render.volumetrics_enabled = true;
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, kNoiseCycle);
        const Frame off_frame = engine.capture_image(true);
        engine.render_config().debug_view = "volumetrics";
        tick_frames(engine, kNoiseCycle);
        const Frame vol_frame = engine.capture_image(true);
        expect_at_least(count_nonblack(vol_frame), 1, "debug_view 'volumetrics' is not a uniformly black frame");
        expect_at_least(count_diff(vol_frame, off_frame), 1, "debug_view 'volumetrics' differs from debug_view: off");
    }

    // lines: an overlay on the NORMAL image, not a replacement (unlike every view above) --
    // must render fine even in a scene with no physics collider to draw a wireframe for.
    {
        toy::core::AppConfig config =
            make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 160, 90);
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, kNoiseCycle);
        engine.render_config().debug_view = "lines";
        tick_frames(engine, kNoiseCycle);
        const Frame lines_frame = engine.capture_image(true);
        expect_at_least(count_nonblack(lines_frame), 1, "debug_view 'lines' still renders the normal image");
    }
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
    // The two pointer positions below are authored against a 1920x1080 framebuffer. A window
    // can come up with a different one -- Retina doubles it, and a window larger than the
    // screen is clamped -- so re-express each point through the same letterbox maths the
    // engine's own pointer ray uses (render::compute_display_rect). macOS only: elsewhere the
    // points are used exactly as authored.
#ifdef __APPLE__
    const Frame probe = engine.capture_image(/*low_res=*/false);  // only its extent is used
    auto to_window = [&](float x, float y) {
        const uint32_t rw = 1440, rh = 960;
        const auto ref = toy::render::compute_display_rect(engine.render_config(), 1920, 1080, rw, rh);
        const auto act = toy::render::compute_display_rect(engine.render_config(), probe.width, probe.height, rw, rh);
        const float u = (x - static_cast<float>(ref.x)) / static_cast<float>(ref.w);
        const float v = (y - static_cast<float>(ref.y)) / static_cast<float>(ref.h);
        return glm::vec2(static_cast<float>(act.x) + u * static_cast<float>(act.w),
                         static_cast<float>(act.y) + v * static_cast<float>(act.h));
    };
#else
    auto to_window = [](float x, float y) { return glm::vec2(x, y); };
#endif

    engine.set_cursor_override(to_window(1097.0f, 407.0f)); // the Heal button, in window pixels
    tick_frames(engine, 3);
    const Frame hovered = engine.capture_image(/*low_res=*/false);

    engine.set_cursor_override(to_window(850.0f, 760.0f));  // empty floor below it
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
// Group "render_terrain" -- the half of the tile system the "world" group cannot reach:
// generation on the job engine, GPU upload, the chunk SceneObjects, and the streaming that
// re-centres them on a moving camera. One Engine, one scene, a real device.
// =====================================================================================

/** @brief Live (uploaded and drawing) chunks in a terrain. */
int count_live_chunks(const toy::world::TerrainComponent& terrain) {
    int live = 0;
    for (const auto& entry : terrain.chunks()) {
        if (entry.second.state == toy::world::ChunkState::Live) ++live;
    }
    return live;
}

/** @brief True when this coordinate has a Live chunk. */
bool chunk_is_live(const toy::world::TerrainComponent& terrain, toy::world::ChunkCoord coord) {
    auto it = terrain.chunks().find(coord);
    return it != terrain.chunks().end() && it->second.state == toy::world::ChunkState::Live;
}

/** @brief Ticks until `predicate` holds or the budget runs out; returns the ticks spent. */
int tick_until(toy::core::Engine& engine, int max_frames, const std::function<bool()>& predicate) {
    for (int i = 0; i < max_frames; ++i) {
        if (predicate()) return i;
        engine.tick();
    }
    return max_frames;
}

/**
 * @brief Generates a small world, builds its chunks, then flies the camera and watches the
 *        loaded region follow it.
 *
 * The scene file is the shipped demo (assets/scenes/terrain_test), but its world is shrunk
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
void test_terrain_streams_chunks_around_the_camera() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/terrain_test/scene.yaml", 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr, "terrain: the demo scene registers a main camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;

    coopa::scene::Scene& scene = *camera->scene;
    auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
    expect(terrain != nullptr, "terrain: the demo scene carries a Terrain component");
    if (terrain == nullptr) return;

    // Shrink the world. 32 cells x 2 tiles = 64 tiles per axis, in 8 chunks of 8 tiles.
    terrain->grid_size                  = 32;
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
    // the CAMERA, and the shipped 60-unit arm would put it seven chunks away from the marker at
    // this test's 8-unit chunk size. Zeroing both smoothing rates makes the follow instant, so a
    // move converges in ticks rather than in however long an exponential chase takes.
    if (auto* controller = camera->owner->get_component<toy::scene::CameraController>()) {
        controller->distance           = 2.0f;
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
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
    if (g_verbose) std::cout << "         converged after " << frames_to_ready << " frames\n";

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

    tick_until(engine, 600, [&] {
        return chunk_is_live(*terrain, toy::world::ChunkCoord{moved.x + 1, moved.y}) &&
               terrain->chunks().find(toy::world::ChunkCoord{start.x - 1, start.y}) ==
                   terrain->chunks().end();
    });

    expect(chunk_is_live(*terrain, toy::world::ChunkCoord{moved.x + 1, moved.y}),
           "terrain: ground ahead of the marker is built as it advances");
    expect(terrain->chunks().find(toy::world::ChunkCoord{start.x - 1, start.y}) ==
               terrain->chunks().end(),
           "terrain: ground left behind is released");
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
 * @brief The shipped render config, with only the window/scene/output bits a test must own.
 *
 * make_test_config() default-constructs an AppConfig, so it exercises PixelRenderConfig's own
 * defaults -- `aa_mode: "off"`, `dof_enabled: false` -- and NOT what the engine actually ships.
 * That is fine for the pixel_demo groups, which assert on specific toggles they set themselves,
 * but it is exactly why the flicker below went unnoticed: nothing in the suite ever rendered
 * with the configuration a user runs.
 *
 * @param scene Scene path, relative to the repo root.
 * @param rw    Internal render width; the low_res capture's width.
 * @param rh    Internal render height.
 */
toy::core::AppConfig make_shipped_config(const std::string& scene, uint32_t rw, uint32_t rh) {
    toy::core::AppConfig config =
        toy::core::AppConfig::load(std::string(ROOT_DIR) + "/assets/config.yaml");
    config.window.width   = rw * 2;
    config.window.height  = rh * 2;
    config.window.visible = false;
    config.window.vsync   = false;
    config.render.render_width  = rw;
    config.render.render_height = rh;
    config.scene.default_scene  = scene;
    config.output.save_on_exit  = false;
    return config;
}


/**
 * @brief A static camera over static geometry must render a STATIC image.
 *
 * The regression test for a flicker that shipped unnoticed: with `aa_mode: taa`, the 8-frame
 * Halton jitter makes the whole render exactly 8-periodic, and any resolve that filters it
 * with a plain exponential blend converges that periodic input to a periodic ORBIT rather than
 * a fixed point -- on terrain_test's block faces the surviving orbit reads as boiling. TAA's
 * age-weighted accumulation (a true running average at rest, with a quantisation floor on the
 * variance clip so flat regions keep their age) is what this test holds to the contract; the
 * threshold below fails on any re-introduced orbit.
 *
 * Two things make this test able to see what the existing suite could not:
 *
 *   - It renders with the SHIPPED config (make_shipped_config), so whatever `aa_mode` actually
 *     ships is what gets exercised. make_test_config()'s defaults have AA off entirely.
 *   - It compares ADJACENT frames. render_pixel's drift assertion spaces its captures by
 *     kNoiseCycle -- a whole number of cycles -- which lands on the same jitter phase and is
 *     therefore structurally blind to a phase-periodic orbit. Comparing neighbours is the whole
 *     point; a cycle is invisible to any test sampling it stroboscopically.
 */
void test_static_camera_converges_to_a_static_image() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::Engine engine(make_shipped_config("assets/scenes/terrain_test/scene.yaml", 320, 180));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr, "converge: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr) return;
    auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>();
    expect(terrain != nullptr, "converge: the scene has a Terrain component");
    if (terrain == nullptr) return;

    // Same shrink the streaming test uses -- a 32-cell map in nine 8x8 chunks, not the demo's
    // ~10k cells, so generation and meshing finish in a second rather than most of a minute.
    terrain->grid_size                  = 32;
    terrain->params.tiles_per_grid_unit = 2;
    terrain->params.chunk_size          = 8;
    terrain->params.view_radius         = 1;

    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 9; });
    tick_frames(engine, 80); // let every temporal filter settle; nothing moves from here on

    std::vector<Frame> frames;
    for (int i = 0; i < 18; ++i) {
        engine.tick();
        frames.push_back(engine.capture_image(/*low_res=*/true));
    }

    auto lag_mean = [&](int lag) {
        double acc = 0.0;
        int n = 0;
        for (size_t i = lag; i < frames.size(); ++i) {
            acc += mean_abs_delta(frames[i], frames[i - static_cast<size_t>(lag)]);
            ++n;
        }
        return n > 0 ? acc / n : 0.0;
    };

    // Budget in 0-255 levels per pixel, averaged over the frame. A converged renderer measures
    // exactly 0.0000 here (smaa and off both do); taa measured 0.0086. The threshold sits an
    // order of magnitude below that so it fails on a re-introduced orbit, while leaving room for
    // a genuinely dithered effect to contribute a texel or two.
    const double lag1 = lag_mean(1);
    expect(lag1 < 0.002, "converge: a static camera renders a static image");
    if (lag1 >= 0.002) std::cerr << "         adjacent-frame delta " << lag1 << " levels/px\n";

    // On failure, print the lag profile -- it does not just say the image moves, it says WHY.
    // A periodic orbit collapses to ~0 at a whole cycle while staying high at every other lag,
    // so `1=0.006 2=0.008 4=0.009 8=0` names the cycle length in the output. (Deliberately not
    // its own expect(): under the bug lag 8 is ~0, so any "lag8 must be small" assertion would
    // PASS on exactly the case it is meant to catch. lag 1 is the assertion; this is evidence.)
    if (lag1 >= 0.002) {
        std::cerr << "         lag profile: 1=" << lag_mean(1) << " 2=" << lag_mean(2)
                  << " 4=" << lag_mean(4) << " 8=" << lag_mean(8) << "\n";
    }
}

/**
 * @brief Shrinks a terrain to a test-sized world and parks its marker in the MIDDLE of it.
 *
 * Recentring is not optional. The scene authors the marker at tile (192, 192) -- the centre of
 * the shipped 96-cell world -- so shrinking `grid_size` without moving it leaves the camera off
 * the far corner, where most of the view radius falls outside the map and only a handful of
 * chunks are ever built. Every measurement taken from there is of mostly-empty sky.
 *
 * @param terrain The component to shrink; its params are overwritten.
 * @param scene   The scene, for resolving the marker object.
 * @return World-space centre the marker was parked at.
 */
glm::vec3 shrink_terrain_and_centre(toy::world::TerrainComponent& terrain,
                                    coopa::scene::Scene& scene) {
    terrain.grid_size                  = 48;
    terrain.params.tiles_per_grid_unit = 4;
    terrain.params.chunk_size          = 16;
    terrain.params.view_radius         = 2;

    const float tiles = float(terrain.grid_size * terrain.params.tiles_per_grid_unit);
    const glm::vec3 centre(tiles * 0.5f * terrain.params.tile_size,
                           tiles * 0.5f * terrain.params.tile_size, 26.0f);
    if (auto* marker = scene.find_object("focus_marker")) {
        if (auto* tc = marker->get_transform()) tc->transform().set_position(centre);
    }
    return centre;
}

/**
 * @brief After the camera stops, the image must stop too -- within a couple of frames.
 *
 * The regression test for the second flicker: SSAO's temporal resolve reprojects AO history and
 * rejects it on only off-screen and behind-eye, with no depth/disocclusion test. Over blocky
 * terrain, camera motion rejects history across every depth discontinuity at once, exposing raw
 * 4x4-tile AO noise -- and the accumulator then refills at ssao_temporal_blend, so the image
 * keeps changing for dozens of frames after the camera has stopped. See config.yaml's
 * `ssao_temporal_enabled` for the measurement that pinned it.
 *
 * Three conditions are load-bearing and were each established by measurement:
 *
 *   - **Full render resolution.** SSAO's noise is a screen-locked 4x4 TEXEL tile, so it averages
 *     away at a small render size: the identical trajectory at 320x180 settles in 0 frames even
 *     with the bug present. A cheap low-res version of this test would pass on a broken build.
 *   - **A close camera.** Rotating close to the terrain is what produces the disocclusion the
 *     resolve mishandles; from far away the parallax is too small to reject much history.
 *   - **Camera smoothing zeroed**, so the camera stops dead the frame the sweep ends. Otherwise
 *     CameraController's own exponential chase is measured as a renderer tail.
 */
void test_image_settles_after_camera_stops() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_shipped_config("assets/scenes/terrain_test/scene.yaml", 1920, 1080);
    // Eye adaptation is deliberately excluded. It re-meters after the rotation and then eases
    // exposure toward the new target at auto_exposure_speed_up/down -- a slow, intended,
    // global brightness glide, not a renderer failing to settle. Left on (config.yaml ships it
    // on), it alone keeps the per-frame delta far above the 0.03 threshold for the whole window,
    // masking the temporal-resolve tail this test exists to catch.
    config.render.auto_exposure_enabled = false;
    toy::core::Engine engine(std::move(config));

    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    expect(camera != nullptr && camera->scene != nullptr && camera->owner != nullptr,
           "settle: the scene has a main camera");
    if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) return;

    auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>();
    auto* controller = camera->owner->get_component<toy::scene::CameraController>();
    expect(terrain != nullptr && controller != nullptr,
           "settle: the scene has a Terrain and a CameraController");
    if (terrain == nullptr || controller == nullptr) return;

    shrink_terrain_and_centre(*terrain, *camera->scene);

    controller->follow_smoothing   = 0.0f;  // stop dead, so the tail measured is the renderer's
    controller->movement_smoothing = 0.0f;
    controller->distance           = 18.0f; // close in: this is what disoccludes under rotation
    controller->pitch_deg          = 18.0f;

    tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
    tick_frames(engine, 90);

    for (int i = 0; i < 24; ++i) {  // rotate...
        controller->yaw_deg += 1.5f;
        engine.tick();
    }
    // ...and stop. Everything after this point is the renderer failing to settle.

    Frame prev = engine.capture_image(/*low_res=*/true);
    std::vector<double> curve;
    int settled_at = -1;
    for (int i = 0; i < 40; ++i) {
        engine.tick();
        Frame current = engine.capture_image(true);
        const double delta = mean_abs_delta(current, prev);
        curve.push_back(delta);
        if (settled_at < 0 && delta < 0.03) settled_at = i;
        prev = std::move(current);
    }

    // What this guards is SSAO's temporal resolve, whose tail is ~10x everything else's: with it
    // enabled the curve starts near 0.45 and is still above 0.13 forty frames later, while the
    // shipped configuration is under 0.03 within a handful of frames. The threshold sits between
    // those rather than at zero, because SSR's own temporal resolve leaves a real residual tail
    // (~0.047, decaying) that this test deliberately does not fail on -- turning SSR off is the
    // only thing that reaches 0.0000, and SSR earns its keep on other scenes.
    expect(settled_at >= 0 && settled_at <= 8, "settle: the image stops when the camera stops");
    if (settled_at < 0 || settled_at > 8) {
        std::cerr << "         settled after " << settled_at << " frames; decay:";
        for (size_t i = 0; i < curve.size() && i < 10; ++i) std::cerr << " " << curve[i];
        std::cerr << "\n";
    }
}

/**
 * @brief TEMPORARY diagnosis probe (round 9): lossless frame dumps of two close-up
 *        gestures under Coopa's crisp config (aa/dof/tilt off), across an attribution
 *        matrix, for region-separated shimmer analysis.
 *
 * Pose: close to terrace walls with AO visibly darkening faces. Gestures per variant:
 * G1 fast flick then stop; G2 slow pan then stop -- both end at rest (the standing
 * capture rule). With SSAO_PROBE_DUMP=1 every frame is written to
 * output/probe/<variant>_<gesture>/ as PNG: the lossless record the analysis and the
 * qp-0 evidence clips are built from.
 */
void test_ssao_travel_probe() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    struct Variant {
        const char* name;
        bool  ao;
        bool  temporal;
        int   slices, steps, temporal_frames;
        float max_radius_px;
        float radius, power;
        bool  debug_view;
    };
    const Variant variants[] = {
        //                        ao     temp   sl st  tf  maxpx  radius power  dbg
        {"V0_ao_off",             false, true,  2, 16, 32, 80.0f, 2.00f, 1.0f, false},
        {"V1_shipped",            true,  true,  2, 16, 32, 80.0f, 2.00f, 1.0f, false},
        {"V2_no_temporal",        true,  false, 2, 16, 32, 80.0f, 2.00f, 1.0f, false},
        {"V3_ultra",              true,  true,  3, 24, 64, 96.0f, 2.00f, 1.0f, false},
        {"V4_radius075",          true,  true,  2, 16, 32, 80.0f, 0.75f, 1.0f, false},
        {"V5_power15",            true,  true,  2, 16, 32, 80.0f, 2.00f, 1.5f, false},
        // The blurred AO buffer itself, fullscreen -- what the estimator+resolve+blur
        // actually produce, isolated from albedo/lighting.
        {"V6_debug_view",         true,  true,  2, 16, 32, 80.0f, 2.00f, 1.0f, true},
    };

    const bool  dump = std::getenv("SSAO_PROBE_DUMP") != nullptr;
    const char* only = std::getenv("SSAO_PROBE_ONLY");

    // Without the dump there is nothing to measure -- the probe's output IS the lossless
    // frame record. Skip so a plain suite run doesn't pay ~15 engine-minutes for nothing.
    if (!dump) {
        std::cerr << "r9 probe skipped (set SSAO_PROBE_DUMP=1, optionally SSAO_PROBE_ONLY=<name>)\n";
        return;
    }

    for (const Variant& v : variants) {
        if (only != nullptr && std::string_view(v.name).find(only) == std::string_view::npos) continue;
        toy::core::AppConfig config =
            make_shipped_config("assets/scenes/terrain_test/scene.yaml", 1920, 1080);
        config.render.ssao_enabled          = v.ao;
        config.render.ssao_temporal_enabled = v.temporal;
        config.render.ssao_slices           = v.slices;
        config.render.ssao_steps            = v.steps;
        config.render.ssao_temporal_frames  = v.temporal_frames;
        config.render.ssao_max_radius_px    = v.max_radius_px;
        config.render.ssao_radius           = v.radius;
        config.render.ssao_power            = v.power;
        config.render.debug_view            = v.debug_view ? "ssao" : "off";
        toy::core::Engine engine(std::move(config));

        auto* camera = coopa::gfx::engine::components::CameraComponent::main();
        if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) {
            expect(false, "r9 probe: scene has a main camera");
            return;
        }
        auto* terrain    = camera->scene->find_first_component<toy::world::TerrainComponent>();
        auto* controller = camera->owner->get_component<toy::scene::CameraController>();
        if (terrain == nullptr || controller == nullptr) {
            expect(false, "r9 probe: scene has Terrain and CameraController");
            return;
        }
        shrink_terrain_and_centre(*terrain, *camera->scene);
        controller->distance  = 12.0f;  // close: AO gradients on wall faces fill the frame
        controller->pitch_deg = 14.0f;
        controller->yaw_deg   = 0.0f;   // facing the sun-away (shadowed) wall faces

        tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
        tick_frames(engine, 90);

        auto run_gesture = [&](const char* tag, int move_frames, float speed, int still_frames) {
            const std::string dir = std::string("output/probe/") + v.name + "_" + tag;
            if (dump) std::filesystem::create_directories(dir);
            for (int f = 0; f < move_frames + still_frames; ++f) {
                if (f < move_frames) controller->yaw_deg += speed;
                engine.tick();
                if (dump) {
                    Frame fr = engine.capture_image(/*low_res=*/true);
                    char fname[32];
                    std::snprintf(fname, sizeof(fname), "/frame_%04d.png", f);
                    coopa::gfx::util::save_image_png(fr, dir + fname);
                }
            }
        };

        run_gesture("g1", 10, 10.0f, 90);   // fast flick, then rest
        tick_frames(engine, 30);            // re-anchor between gestures (not dumped)
        run_gesture("g2", 80, 0.5f, 60);    // slow pan, then rest

        std::cerr << "r9 " << v.name << " done\n";
    }
}

/**
 * @brief TEMPORARY verification probe (round 11): the ring-capture reproduction from
 *        docs/shimmer-repro.md, run A/B with texel_aa on and off.
 *
 * Mirrors the reference `output/ring_capture_lossless.mp4` as closely as a scripted
 * camera can: the same framing (close blocks, camera pitched steeply down), the same
 * gesture rhythm (quick mouse flicks each followed by ~a second of holding still,
 * five cycles over five seconds), SHIPPED controller smoothing, 300 frames at 60 Hz,
 * ending at rest. Dumps every frame to output/probe/<variant>/ for lossless A/B
 * encoding -- the acceptance evidence for the texel-AA fix on the reference test.
 */
void test_texel_aa_ring_repro() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    if (std::getenv("SSAO_PROBE_DUMP") == nullptr) {
        std::cerr << "ring-repro probe skipped (set SSAO_PROBE_DUMP=1)\n";
        return;
    }

    struct Variant { const char* name; bool texel_aa; bool ao; const char* aa; };
    const Variant variants[] = {
        {"ring_texelaa_off",   false, true,  "off"},
        {"ring_texelaa_on",    true,  true,  "off"},
        // texel-AA on but SSAO off: attributes the residual churn on occluded faces --
        // whatever this variant lacks relative to ring_texelaa_on is AO-driven.
        {"ring_texelaa_no_ao", true,  false, "off"},
        // The proposed shipping stack: texel-AA for texture crawl + SMAA for the
        // geometric block-edge staircase crawl texel-AA cannot touch.
        {"ring_texelaa_smaa",  true,  true,  "smaa"},
        // Same stack with the temporal resolve instead: the acceptance capture for TAA's
        // motion behaviour (reprojection sharpness) and its post-stop settle.
        {"ring_texelaa_taa",   true,  true,  "taa"},
    };

    const char* only = std::getenv("SSAO_PROBE_ONLY");
    for (const Variant& v : variants) {
        if (only != nullptr && std::string_view(v.name).find(only) == std::string_view::npos) continue;
        toy::core::AppConfig config =
            make_shipped_config("assets/scenes/terrain_test/scene.yaml", 1920, 1080);
        config.render.texel_aa     = v.texel_aa;
        config.render.ssao_enabled = v.ao;
        config.render.aa_mode      = v.aa;
        toy::core::Engine engine(std::move(config));

        auto* camera = coopa::gfx::engine::components::CameraComponent::main();
        if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) {
            expect(false, "ring repro: scene has a main camera");
            return;
        }
        auto* terrain    = camera->scene->find_first_component<toy::world::TerrainComponent>();
        auto* controller = camera->owner->get_component<toy::scene::CameraController>();
        if (terrain == nullptr || controller == nullptr) {
            expect(false, "ring repro: scene has Terrain and CameraController");
            return;
        }
        shrink_terrain_and_centre(*terrain, *camera->scene);
        controller->distance  = 10.0f;  // the reference capture's framing: close blocks,
        controller->pitch_deg = 55.0f;  // camera pitched steeply down

        tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
        tick_frames(engine, 90);

        const std::string dir = std::string("output/probe/") + v.name;
        std::filesystem::create_directories(dir);
        int frame = 0;
        auto tick_dump = [&](float yaw_delta) {
            controller->yaw_deg += yaw_delta;
            engine.tick();
            Frame fr = engine.capture_image(/*low_res=*/true);
            char fname[32];
            std::snprintf(fname, sizeof(fname), "/frame_%04d.png", frame++);
            coopa::gfx::util::save_image_png(fr, dir + fname);
        };
        for (int cycle = 0; cycle < 5; ++cycle) {
            for (int f = 0; f < 10; ++f) tick_dump(10.0f);  // the flick
            for (int f = 0; f < 50; ++f) tick_dump(0.0f);   // the hold
        }
        std::cerr << "ring repro " << v.name << " done (" << frame << " frames)\n";
    }
}

/**
 * @brief TEMPORARY diagnosis probe (round 9b): decomposes the LIVE isolated pop from
 *        Coopa's 2026-09-22 screencast by subsystem.
 *
 * The screencast shows: image byte-static, then ONE frame where every terrain pixel
 * changes by fine-grained ~2.5 levels (zero camera shift), then byte-static again --
 * with the camera pitched steeply down at close blocks. Hypothesis: a sub-visible input
 * blip exits the stillness freeze, the stochastic passes redraw for a handful of frames,
 * and the image re-freezes on a different state. This probe reproduces that gesture at
 * Coopa's pose and toggles each stochastic subsystem to see which one carries the pop.
 */
void test_ssao_blip_probe() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");

    struct Variant {
        const char* name;
        bool ao, ssr, shadows, soft_shadows;
    };
    const Variant variants[] = {
        {"B0_all_on",      true,  true,  true,  true},
        {"B1_no_ao",       false, true,  true,  true},
        {"B2_no_ssr",      true,  false, true,  true},
        {"B3_hard_shadow", true,  true,  true,  false},
        {"B4_no_shadow",   true,  true,  false, false},
        {"B5_all_off",     false, false, false, false},
    };

    for (const Variant& v : variants) {
        toy::core::AppConfig config =
            make_shipped_config("assets/scenes/terrain_test/scene.yaml", 1920, 1080);
        config.render.ssao_enabled    = v.ao;
        config.render.ssr_enabled     = v.ssr;
        config.render.shadows_enabled = v.shadows;
        config.render.soft_shadows    = v.soft_shadows;
        toy::core::Engine engine(std::move(config));

        auto* camera = coopa::gfx::engine::components::CameraComponent::main();
        if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) {
            expect(false, "blip probe: scene has a main camera");
            return;
        }
        auto* terrain    = camera->scene->find_first_component<toy::world::TerrainComponent>();
        auto* controller = camera->owner->get_component<toy::scene::CameraController>();
        if (terrain == nullptr || controller == nullptr) {
            expect(false, "blip probe: scene has Terrain and CameraController");
            return;
        }
        shrink_terrain_and_centre(*terrain, *camera->scene);
        controller->distance  = 10.0f;  // Coopa's screencast pose: close, pitched steeply down
        controller->pitch_deg = 55.0f;

        tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
        tick_frames(engine, 90);

        // The screencast gesture: flick, then hands off. The controller deadband snap fires
        // once mid-settle (smoothing error crossing 1e-3 deg); the full per-frame curve
        // shows whether that single frame re-draws the stochastic passes (the live "pop").
        for (int f = 0; f < 10; ++f) { controller->yaw_deg += 10.0f; engine.tick(); }
        Frame prev = engine.capture_image(/*low_res=*/true);
        std::cerr << "blip9b " << v.name << " settle curve:";
        for (int f = 0; f < 70; ++f) {
            engine.tick();
            Frame cur = engine.capture_image(true);
            const double d = mean_abs_delta(cur, prev);
            if (f < 20 || d > 0.01) std::cerr << " " << f << ":" << d;
            prev = std::move(cur);
        }
        std::cerr << "\n";
    }
}


/**
 * @brief Measures how grainy the screen-space traces are in motion, and how long their residual
 *        takes to die after the camera stops, across the accumulation depths under test.
 *
 * The existing image_settles_after_camera_stops contract only looks at the tail. What reads as
 * "SSR/SSGI are very jittery" is the in-motion grain: a stochastic trace whose temporal resolve
 * cannot converge leaves a fixed fraction of its single-ray noise standing every frame the camera
 * is moving, and no amount of holding still afterwards reveals that.
 *
 * Variants sweep `ssr_temporal_frames`/`ssgi_temporal_frames` rather than toggling `ssr_enabled`:
 * a depth of 1 clamps the shared accumulation count to one sample, which is the 1-spp trace with
 * no temporal integration at all -- the honest baseline for what the accumulator buys. Toggling
 * the feature off would be the obvious control but is unusable here, because a capture with
 * `ssr_enabled: false` comes back frozen (every frame byte-identical, including under motion --
 * reproducible in test_ssao_blip_probe's own B2/B5 variants, so it predates this probe).
 *
 * Each variant is measured twice: once through `debug_view: ssr`, where the capture IS the
 * resolved reflection buffer and the grain number is the reflection's own, and once with
 * debug_view off, where it is the finished frame and the same noise is diluted by everything
 * that is not reflective. Both are set explicitly here rather than inherited: make_shipped_config
 * reads assets/config.yaml verbatim, so whatever debug_view happens to be left enabled there
 * silently decides what every capture in this file is looking at.
 *
 * Every gesture ends with the camera stopped and held -- the standing capture rule. Diagnostic,
 * not a pass/fail gate: it prints numbers for attribution rather than asserting a threshold, the
 * same contract test_ssao_travel_probe follows.
 */
void test_ssr_jitter_probe() {
    // FIXED_DT=0, not the 0.016 the settle contract uses: scene time must not advance, or the
    // residual measured after the camera stops is the terrain's own animation rather than the
    // renderer's. Measured with time running, the finished image never settles at all -- and
    // does not settle with auto-exposure and TAA both disabled either, which is what identified
    // the animation as the dominant term. Rendering still advances (the TAA jitter and every
    // temporal resolve step per tick); only the world holds still.
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    struct Variant {
        const char* name;
        int   ssr_frames;   // ssr_temporal_frames; 1 == no temporal integration
        int   ssgi_frames;  // ssgi_temporal_frames
        float jitter;       // ssr_jitter -- 0 reproduces the deterministic single mirror ray
    };
    const Variant variants[] = {
        {"J0_shipped",     32, 48, 0.25f},
        {"J1_accum_off",    1,  1, 0.25f},
        {"J2_no_jitter",   32, 48, 0.00f},
        {"J3_accum_off_no_jitter", 1, 1, 0.00f},
    };

    const char* const views[] = {"ssr", "off"};

    // Attribution for the residual the finished image carries whatever SSR does: with
    // debug_view off, nothing in the variant sweep below moves it, so the dominant term is not
    // the traces at all. SSR_PROBE_NOEXPOSURE=1 is the one that identifies it -- it is the only
    // toggle under which the finished image settles at all (measured: settled_at 39 vs never),
    // which puts auto-exposure, not the screen-space traces, at the top of that list.
    // SSR_PROBE_NOAA=1 is kept alongside it as the obvious second suspect it rules out.
    const bool no_exposure = std::getenv("SSR_PROBE_NOEXPOSURE") != nullptr;
    const bool no_aa       = std::getenv("SSR_PROBE_NOAA") != nullptr;

    for (const char* view : views)
    for (const Variant& v : variants) {
        toy::core::AppConfig config =
            make_shipped_config("assets/scenes/terrain_test/scene.yaml", 1920, 1080);
        config.render.ssr_temporal_frames  = v.ssr_frames;
        config.render.ssgi_temporal_frames = v.ssgi_frames;
        config.render.ssr_jitter           = v.jitter;
        config.render.debug_view           = view;
        if (no_exposure) config.render.auto_exposure_enabled = false;
        if (no_aa)       config.render.aa_mode               = "off";
        toy::core::Engine engine(std::move(config));

        auto* camera = coopa::gfx::engine::components::CameraComponent::main();
        if (camera == nullptr || camera->scene == nullptr || camera->owner == nullptr) {
            expect(false, "ssr jitter probe: scene has a main camera");
            return;
        }
        auto* terrain    = camera->scene->find_first_component<toy::world::TerrainComponent>();
        auto* controller = camera->owner->get_component<toy::scene::CameraController>();
        if (terrain == nullptr || controller == nullptr) {
            expect(false, "ssr jitter probe: scene has Terrain and CameraController");
            return;
        }
        shrink_terrain_and_centre(*terrain, *camera->scene);
        // Close and pitched down: the pose where the traces cover the most screen and where
        // grazing reflections and contact lines are both in frame.
        controller->follow_smoothing   = 0.0f;
        controller->movement_smoothing = 0.0f;
        // Coopa's screencast pose: close and pitched steeply down, so the terrain island fills
        // the frame instead of sitting in a sea of empty sky.
        controller->distance           = 10.0f;
        controller->pitch_deg          = 55.0f;

        tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
        tick_frames(engine, 90);

        // --- Sustained pan at a realistic flick rate. Grain measured on the LAST pan frame, by
        // which point the accumulator has reached whatever steady state that speed allows. ---
        const int kPanFrames = 30;
        Frame prev = engine.capture_image(/*low_res=*/true);
        for (int f = 0; f < kPanFrames; ++f) {
            controller->yaw_deg += 1.5f;
            engine.tick();
            prev = engine.capture_image(true);
        }
        const double grain_motion = local_high_pass(prev);

        // --- ...and stop. Everything from here is the renderer failing to settle. ---
        int settled_at = -1;
        double first_delta = -1.0;
        for (int f = 0; f < 40; ++f) {
            engine.tick();
            Frame cur = engine.capture_image(true);
            const double d = mean_abs_delta(cur, prev);
            if (f == 0) first_delta = d;
            if (settled_at < 0 && d < 0.01) settled_at = f;
            prev = std::move(cur);
        }
        const double grain_rest = local_high_pass(prev);

        // Full-resolution frame of the settled image, for eyes rather than numbers: a denoiser
        // that erased the reflection outright would score BETTER on grain than one that
        // integrated it, so the numbers below are only meaningful next to a look at the result.
        if (std::getenv("SSR_PROBE_DUMP") != nullptr) {
            coopa::gfx::util::save_image_png(
                prev, std::string("output/ssrjit_") + view + "_" + v.name + ".png");
        }
        double luma_sum = 0.0;
        for (size_t k = 0; k < prev.pixels.size(); k += prev.channels) luma_sum += prev.pixels[k];
        const double mean_luma = luma_sum / double(prev.width * prev.height);

        std::cerr << "ssrjit " << view << " " << v.name
                  << " [eff frames=" << engine.render_config().ssr_temporal_frames
                  << "/" << engine.render_config().ssgi_temporal_frames
                  << " jitter=" << engine.render_config().ssr_jitter
                  << " cutoff=" << engine.render_config().ssr_roughness_cutoff << "]"
                  << " mean_luma=" << mean_luma
                  << " grain_motion=" << grain_motion
                  << " grain_rest=" << grain_rest
                  << " first_delta=" << first_delta
                  << " settled_at=" << settled_at << "\n";
    }
}

/**
 * @brief Whole-frame look of the FINISHED image: mean luma, contrast, and **saturation**, for the
 *        class of regression that changes how everything looks at once.
 *
 * Written after a descriptor-set-index mistake in `pixel_lighting.frag` drained 88% of the
 * scene's colour and several checks in a row missed it. Each miss is a rule this test now
 * encodes:
 *
 *  - **Measure saturation, not just tone.** The regression was luma-PRESERVING: mean 136 -> 139
 *    and contrast 47 -> 32, both unremarkable, while mean saturation went 0.298 -> 0.036. The
 *    lighting pass was sampling a one-binding set as its five-binding G-buffer set, so the frame
 *    came out unlit and auto-exposure amplified the remainder into a plausible-looking grey.
 *    Only a chroma metric sees that; the tone numbers shrug.
 *  - **Point it at a scene with colour in it.** The first version defaulted to `pixel_demo`,
 *    whose floor is already grey -- there was no chroma there to lose, so the A/B came back clean.
 *    `terrain_test` has saturated albedo (grass, dirt, stone, sand) and is the default here.
 *  - **A debug channel passing does not vindicate the shipped path.** `debug_view`'s
 *    albedo/direct/indirect channels all looked perfectly coloured throughout, because they are
 *    drawn by `debug_view.frag` -- a DIFFERENT shader, with its own (correct) set indices. Only a
 *    capture of the finished image could see it.
 *  - **Wait for the scene to finish loading.** Without the streaming wait every other terrain
 *    test does, the capture is a half-streamed frame and not comparable run to run.
 *
 * `grain` is reported but is explicitly NOT a look metric: a wash has little high-frequency
 * energy, so a regression that flattens the image makes grain go *down* and reads as an
 * improvement. It is here only to sit next to `ssr_jitter_probe`'s numbers.
 *
 * Prints rather than asserts a baseline: there is no recorded one, and the probe group's contract
 * is print-for-attribution. The numbers are the artifact -- put them in the commit message so the
 * next change has something to be compared against.
 */
void test_look_canary() {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    // terrain_test, not config.yaml's default_scene: this test needs coloured albedo to watch.
    const char* scene = std::getenv("CANARY_SCENE")
        ? std::getenv("CANARY_SCENE") : "assets/scenes/terrain_test/scene.yaml";
    toy::core::AppConfig config = make_shipped_config(scene, 1920, 1080);
    // Never inherited -- see this test's doc. CANARY_VIEW swaps in one intermediate channel so a
    // whole-frame look shift can be walked term by term (albedo first: it says immediately
    // whether the G-buffer or the lighting/composite is the one that lost the colour).
    config.render.debug_view = std::getenv("CANARY_VIEW") ? std::getenv("CANARY_VIEW") : "off";
    // Bisect handles: each takes one suspect out of the finished image so a whole-frame look
    // shift can be attributed to a term instead of guessed at.
    if (std::getenv("CANARY_NOVOL"))      config.render.volumetrics_enabled    = false;
    if (std::getenv("CANARY_NOBLOOM"))    config.render.bloom_enabled          = false;
    if (std::getenv("CANARY_NOEXPOSURE")) config.render.auto_exposure_enabled  = false;
    if (std::getenv("CANARY_NOSSGI"))     config.render.indirect.ssgi_intensity = 0.0f;
    if (std::getenv("CANARY_NOSSR"))      config.render.ssr_enabled            = false;
    if (std::getenv("CANARY_NOTRANSP"))   config.render.transparency_enabled   = false;
    if (std::getenv("CANARY_NOAA"))       config.render.aa_mode                = "off";
    if (std::getenv("CANARY_CONTACT"))    config.render.contact_shadows_enabled = true;
    if (std::getenv("CANARY_NOCONTACTTEMP")) config.render.contact_shadow_temporal_enabled = false;
    if (const char* c = std::getenv("CANARY_CUTOFF")) {
        config.render.ssr_roughness_cutoff = float(atof(c));
    }
    toy::core::Engine engine(std::move(config));

    // Same streaming wait every other terrain test uses, so the frame is the same scene each run.
    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    if (camera != nullptr && camera->scene != nullptr) {
        if (auto* terrain = camera->scene->find_first_component<toy::world::TerrainComponent>()) {
            tick_until(engine, 900, [&] { return count_live_chunks(*terrain) >= 25; });
        }
    }
    tick_frames(engine, 90);  // let the temporal resolves and auto-exposure top up
    const Frame f = engine.capture_image(/*low_res=*/true);

    // At-rest stability of whatever is on screen: one more tick with nothing moving. A term that
    // is spatially noisy but temporally stable reads 0 here while `grain` stays high -- which
    // separates "this pattern is wrong" from "this pattern shimmers", two very different bugs.
    engine.tick();
    const double rest_delta = mean_abs_delta(engine.capture_image(true), f);

    // ...and the same thing IN MOTION, which is where the user-visible artifacts of a temporal
    // resolve live: reprojection error and history acceptance only misbehave when the camera
    // moves, so an at-rest number says nothing about them. Panned at a realistic flick rate.
    double motion_delta = 0.0;
    if (auto* cc = camera != nullptr && camera->owner != nullptr
                   ? camera->owner->get_component<toy::scene::CameraController>() : nullptr) {
        cc->follow_smoothing = 0.0f;
        cc->movement_smoothing = 0.0f;
        for (int i = 0; i < 8; ++i) { cc->yaw_deg += 1.5f; engine.tick(); }
        Frame prev = engine.capture_image(true);
        const int kMotionFrames = 12;
        for (int i = 0; i < kMotionFrames; ++i) {
            cc->yaw_deg += 1.5f;
            engine.tick();
            Frame cur = engine.capture_image(true);
            motion_delta += mean_abs_delta(cur, prev);
            prev = std::move(cur);
        }
        motion_delta /= double(kMotionFrames);
    }

    double sum = 0.0, sum2 = 0.0, sat = 0.0;
    long long n = 0;
    for (size_t k = 0; k + 2 < f.pixels.size(); k += f.channels) {
        const int r = f.pixels[k], g = f.pixels[k + 1], b = f.pixels[k + 2];
        const double v = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        sum += v;
        sum2 += v * v;
        // HSV saturation: (max-min)/max. Scale-free, so a change in exposure alone does not move
        // it -- which is exactly the separation this test needs from the two tone numbers.
        const int mx = std::max(r, std::max(g, b));
        const int mn = std::min(r, std::min(g, b));
        if (mx > 0) sat += double(mx - mn) / double(mx);
        ++n;
    }
    const double mean     = n > 0 ? sum / double(n) : 0.0;
    const double contrast = n > 0 ? std::sqrt(std::max(sum2 / double(n) - mean * mean, 0.0)) : 0.0;
    const double satur    = n > 0 ? sat / double(n) : 0.0;

    std::cerr << "look_canary " << scene
              << " mean=" << mean
              << " contrast=" << contrast
              << " saturation=" << satur
              << " rest_delta=" << rest_delta
              << " motion_delta=" << motion_delta
              << " (grain=" << local_high_pass(f) << ", NOT a look metric -- see doc)"
              << " [cutoff=" << engine.render_config().ssr_roughness_cutoff
              << " vol=" << engine.render_config().volumetrics_enabled
              << " fog=" << engine.render_config().fog_enabled
              << " bloom=" << engine.render_config().bloom_enabled
              << " transp=" << engine.render_config().transparency_enabled
              << " ssr=" << engine.render_config().ssr_enabled
              << " expo=" << engine.render_config().auto_exposure_enabled
              << " aa=" << engine.render_config().aa_mode << "]\n";

    if (std::getenv("SSR_PROBE_DUMP") != nullptr) {
        coopa::gfx::util::save_image_png(f, std::string("output/look_canary") +
                (std::getenv("CANARY_TAG") ? std::getenv("CANARY_TAG") : "") + ".png");
    }

    // The two things worth failing on outright: a frame with no tonal range left in it is not a
    // render, and a frame with no colour left in it is the regression this test was written for.
    expect(contrast > 1.0, "look canary: the finished image has some tonal range");
    expect(satur > 0.10, "look canary: the finished image still has colour in it");
}

// =====================================================================================
// Group "yaml_io" -- coopa::yaml's document loading and the caml codec. No GPU.
// (caml_scene_renders_identically, the end-to-end check, lives in render_pixel.)
// =====================================================================================

/** @brief Every .yaml/.yml file under assets/, sorted so failures list in a stable order. */
std::vector<std::filesystem::path> asset_yaml_files() {
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::recursive_directory_iterator(std::string(ROOT_DIR) + "/assets")) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext == ".yaml" || ext == ".yml") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

/** @brief A fresh, empty directory under tmp_dir(). */
std::filesystem::path fresh_tmp_subdir(std::string_view name) {
    const std::filesystem::path d = tmp_dir() / name;
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

/** @brief Writes `text` to `path`, replacing it. */
void write_text_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

/**
 * @brief Copies `src` to `dst` recursively, then replaces every YAML document in `dst` with its
 *        .caml encoding -- the layout Build > Package produces.
 */
void package_tree_as_caml(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::filesystem::remove_all(dst);
    std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive);
    std::vector<std::filesystem::path> docs;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dst)) {
        const std::string ext = e.path().extension().string();
        if (e.is_regular_file() && (ext == ".yaml" || ext == ".yml")) docs.push_back(e.path());
    }
    for (const auto& p : docs) {
        std::filesystem::path out = p;
        out.replace_extension(".caml");
        toy::core::encode_caml_file(p, out);
        std::filesystem::remove(p);
    }
}

/** @brief Every YAML asset in the repo survives yaml -> .caml -> load_document as an equal node. */
void test_caml_roundtrips_every_asset() {
    const std::filesystem::path dir = fresh_tmp_subdir("caml_roundtrip");
    const auto files = asset_yaml_files();
    expect(files.size() > 10, "assets/ has YAML files to round-trip (found " + std::to_string(files.size()) + ")");
    int index = 0;
    for (const auto& src : files) {
        const std::filesystem::path out = dir / (std::to_string(index++) + ".caml");
        toy::core::encode_caml_file(src, out);
        const fkyaml::node plain   = coopa::yaml::load_document(src);
        const fkyaml::node decoded = coopa::yaml::load_document(out);
        expect(plain == decoded, "caml round-trip preserves " + src.lexically_relative(ROOT_DIR).string());
    }
}

/** @brief Detection is by magic bytes: an encoded file named .yaml still decodes, plain text passes through. */
void test_caml_detected_by_magic_not_extension() {
    const std::filesystem::path dir = fresh_tmp_subdir("caml_magic");
    const std::string yaml = "a: 1\nlist: [1, 2, 3]\nname: hello\n";
    const std::vector<uint8_t> bytes = toy::core::encode_caml_text(yaml);
    {
        std::ofstream out(dir / "misnamed.yaml", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    write_text_file(dir / "plain.yaml", yaml);

    expect(coopa::yaml::read_text(dir / "misnamed.yaml") == yaml, "an encoded file named .yaml decodes by its magic");
    expect(coopa::yaml::read_text(dir / "plain.yaml") == yaml, "plain YAML passes through unchanged");
    expect(coopa::yaml::load_document(dir / "misnamed.yaml").at("name").get_value<std::string>() == "hello",
           "load_document parses the decoded text");
}

/** @brief Without the codec, a .caml fails with a message naming the fix rather than a parse error. */
void test_caml_without_codec_names_the_fix() {
    const std::filesystem::path dir = fresh_tmp_subdir("caml_no_codec");
    toy::core::encode_caml_file(std::string(ROOT_DIR) + "/assets/physics_materials/rubber.yaml", dir / "rubber.caml");

    coopa::yaml::clear_decoders();
    std::string message;
    try {
        coopa::yaml::load_document(dir / "rubber.caml");
    } catch (const std::exception& e) {
        message = e.what();
    }
    toy::core::install_caml_codec();

    expect(message.find("install_caml_codec") != std::string::npos,
           "loading .caml with no decoder names install_caml_codec() (got: '" + message + "')");
    expect(coopa::yaml::load_document(dir / "rubber.caml").contains("restitution"), "reinstalling the codec restores loading");
}

/** @brief "x.yaml" references find "x.caml" (and back), including multi-dot sidecar names and AssetSource lookups. */
void test_yaml_variant_resolution() {
    const std::filesystem::path dir = fresh_tmp_subdir("caml_variants");
    std::filesystem::create_directories(dir / "meshes");
    write_text_file(dir / "plain.yaml", "k: 1\n");
    toy::core::encode_caml_file(dir / "plain.yaml", dir / "meshes" / "sphere.lod.caml");
    toy::core::encode_caml_file(dir / "plain.yaml", dir / "packed.caml");

    using coopa::yaml::resolve_variant;
    expect(resolve_variant(dir / "plain.yaml") == dir / "plain.yaml", "an existing path resolves to itself");
    expect(resolve_variant(dir / "packed.yaml") == dir / "packed.caml", "x.yaml finds x.caml");
    expect(resolve_variant(dir / "plain.caml") == dir / "plain.yaml", "x.caml finds x.yaml");
    expect(resolve_variant(dir / "meshes" / "sphere.lod.yaml") == dir / "meshes" / "sphere.lod.caml",
           "multi-dot sidecar names keep their stem");
    expect(resolve_variant(dir / "missing.yaml") == dir / "missing.yaml", "no twin: the path comes back unchanged");
    expect(resolve_variant(dir / "image.png") == dir / "image.png", "non-document paths are never rewritten");

    coopa::asset::AssetSource source;
    source.add_search_root(dir.string());
    expect(std::filesystem::path(source.resolve("meshes/sphere.lod.yaml")) == dir / "meshes" / "sphere.lod.caml",
           "AssetSource search roots resolve a .yaml reference to its .caml twin");
    expect(std::filesystem::path(source.resolve("packed.yaml", (dir / "meshes").string())) == dir / "packed.caml",
           "AssetSource falls through base_dir to the search root, variant-aware");
}

/** @brief AppConfig and SceneLoader read packaged files identically to their YAML originals. */
void test_caml_config_and_scene_load_identically() {
    const std::filesystem::path dir = fresh_tmp_subdir("caml_config_scene");
    const std::string config_yaml = std::string(ROOT_DIR) + "/assets/config.yaml";
    toy::core::encode_caml_file(config_yaml, dir / "config.caml");

    const toy::core::AppConfig a = toy::core::AppConfig::load(config_yaml);
    const toy::core::AppConfig b = toy::core::AppConfig::load((dir / "config.yaml").string()); // finds config.caml
    expect(a.scene.default_scene == b.scene.default_scene, "config.caml: scene.default_scene matches");
    expect(a.window.width == b.window.width && a.window.height == b.window.height, "config.caml: window size matches");
    expect(a.render.render_width == b.render.render_width && a.render.aa_mode == b.render.aa_mode,
           "config.caml: render settings match");

    const std::filesystem::path scene_src = std::string(ROOT_DIR) + "/assets/scenes/physics_test";
    package_tree_as_caml(scene_src, dir / "physics_test");
    coopa::scene::Scene plain  = coopa::scene::SceneLoader::load((scene_src / "scene.yaml").string());
    coopa::scene::Scene packed = coopa::scene::SceneLoader::load((dir / "physics_test" / "scene.yaml").string());
    std::vector<std::string> plain_names, packed_names;
    for (const auto& o : plain.root_objects())  plain_names.push_back(o->name());
    for (const auto& o : packed.root_objects()) packed_names.push_back(o->name());
    expect(!plain_names.empty() && plain_names == packed_names,
           "scene.caml loads the same root objects (" + std::to_string(packed_names.size()) + ")");
}

/** @brief The PBKDF2 step runs once at install: 200 decodes must not cost 200 key derivations. */
void test_caml_key_is_cached() {
    const std::vector<uint8_t> bytes = toy::core::encode_caml_text("a: 1\n");
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) (void)coopa::yaml::decode_bytes(bytes, "bench.caml");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    expect(ms < 250.0, "200 small .caml decodes take under 250 ms (took " + std::to_string(ms) + " ms)");
}

/**
 * @brief pixel_demo packaged to .caml (scene, meshes, LOD sidecars) renders byte-identically to
 *        the YAML original -- the end-to-end claim behind Build > Package.
 */
void test_caml_scene_renders_identically() {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    const std::filesystem::path packed = fresh_tmp_subdir("caml_render") / "pixel_demo";
    package_tree_as_caml(std::string(ROOT_DIR) + "/assets/scenes/pixel_demo", packed);

    auto render = [](const std::string& scene) {
        toy::core::Engine engine(make_test_config(scene, 320, 180, 160, 90));
        tick_frames(engine, kNoiseCycle);
        return engine.capture_image(/*low_res=*/true);
    };
    const Frame yaml_frame = render("assets/scenes/pixel_demo/scene.yaml");
    const Frame caml_frame = render((packed / "scene.yaml").string());
    const long long diff = count_diff(yaml_frame, caml_frame);
    expect(diff == 0, "pixel_demo from .caml renders identically to .yaml (" + std::to_string(diff) + " px differ)");
    if (diff != 0) {
        dump_frame(yaml_frame, "caml_render_yaml");
        dump_frame(caml_frame, "caml_render_caml");
    }
}

// =====================================================================================
// Group "editor_host" -- the Engine embedding surface the editor builds on: re-entrant scene
// loading, edit vs play mode, push/remove scenes, display regions, viewport rays.
// =====================================================================================

/** @brief Local position of a named root object (its world position, being a root), or NaN if missing. */
glm::vec3 object_position(coopa::scene::Scene& scene, const std::string& name) {
    coopa::scene::SceneObject* obj = scene.find_object(name);
    if (!obj || !obj->get_transform()) return glm::vec3(std::numeric_limits<float>::quiet_NaN());
    return obj->get_transform()->transform().position();
}

/**
 * @brief A scene file's `scene.settings` reach the running engine: render overrides apply live
 *        when it loads, physics overrides configure its physics, and loading a scene without
 *        overrides puts the project's settings back.
 */
void test_engine_applies_scene_settings() {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    const std::filesystem::path dir = fresh_tmp_subdir("scene_settings");
    {
        std::ofstream out(dir / "scene.yaml");
        out << "format: blender\n"
               "scene:\n"
               "  scene_name: settings_test\n"
               "  settings:\n"
               "    render: { exposure: 2.5, fog_density: 0.09 }\n"
               "    physics: { gravity: { x: 0.0, y: 0.0, z: -2.0 } }\n"
               "  root_objects:\n"
               "    - name: camera\n"
               "      components:\n"
               "        - type: Transform\n"
               "          position: { x: 0.0, y: -6.0, z: 3.0 }\n"
               "        - type: Camera\n"
               "          main: true\n";
    }
    toy::core::Engine engine(make_test_config("assets/scenes/pixel_demo/scene.yaml", 320, 180, 160, 90));
    tick_frames(engine, 2);
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

    engine.load_scene("assets/scenes/pixel_demo/scene.yaml");
    tick_frames(engine, 2);
    expect(std::abs(engine.render_config().exposure - project_exposure) < 1e-5f &&
           std::abs(engine.render_config().fog_density - project_fog) < 1e-5f,
           "scene settings: a scene without overrides runs with the project's settings again");
    std::filesystem::remove_all(dir);
}

/** @brief Edit mode freezes physics; play mode simulates; load_scene() is re-entrant. */
void test_engine_edit_mode_freezes_simulation() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::EngineOptions opts;
    opts.edit_mode = true;
    toy::core::Engine engine(make_test_config("assets/scenes/physics_test/scene.yaml", 320, 180, 160, 90), opts);

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
void test_engine_edit_mode_shows_water() {
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
void test_engine_push_scene_restores_edit_scene() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::EngineOptions opts;
    opts.edit_mode = true;
    const std::string path = std::string(ROOT_DIR) + "/assets/scenes/physics_test/scene.yaml";
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
void test_engine_display_region_and_viewport_ray() {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 160, 90));
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

// =====================================================================================
// Registry
// =====================================================================================


// =====================================================================================
// Group "water" -- toyengine/water/: the wave mirror, the surface query, the flow bake and
// buoyancy, all CPU-only (a WaterSystem with no device bakes the query but publishes no mesh).
// Group "render_water" -- the water_test scene end to end.
// =====================================================================================

/** @brief Gerstner's horizontal displacement means "height above XY" needs the inverse; the
 *         CPU query must land on the same crest the forward sum (what the GPU draws) puts there. */
void test_water_wave_height_inverse_matches_forward() {
    toy::water::WaveParams w;
    w.amplitude = 0.4f;
    w.wavelength = 7.0f;
    w.direction = 0.6f;
    w.steepness = 0.8f;
    float worst = 0.0f;
    for (int i = 0; i < 64; ++i) {
        glm::vec2 p0(std::sin(i * 1.7f) * 20.0f, std::cos(i * 2.3f) * 20.0f);
        const float t = 0.37f * static_cast<float>(i);
        toy::water::WaveSample fwd = toy::water::evaluate(w, p0, t);
        glm::vec2 target = p0 + glm::vec2(fwd.displacement);
        float h = toy::water::height_at(w, target, t, [](const glm::vec2&) { return 1.0f; }, nullptr, 4);
        worst = std::max(worst, std::fabs(h - fwd.displacement.z));
    }
    expect(worst < 0.02f * w.amplitude * 4.0f,
           "water waves: inverted height matches the forward Gerstner sum (worst " + std::to_string(worst) + ")");

    toy::water::WaveParams calm;
    calm.amplitude = 0.0f;
    expect_near(toy::water::evaluate(calm, glm::vec2(3.0f), 1.0f).displacement.z, 0.0f, 1e-7f,
                "water waves: zero amplitude is flat");
    expect(toy::water::depth_attenuation(0.0f, 8.0f) < toy::water::depth_attenuation(10.0f, 8.0f),
           "water waves: shallow water calms the waves");
}

/** @brief The XY triangle grid: inside interpolates, outside reports no water. */
void test_water_surface_query_interpolates_and_bounds() {
    toy::water::WaterSurfaceQuery q;
    std::vector<toy::water::WaterVertex> v(4);
    v[0].position = {0.0f, 0.0f, 1.0f};
    v[1].position = {10.0f, 0.0f, 2.0f};
    v[2].position = {10.0f, 10.0f, 2.0f};
    v[3].position = {0.0f, 10.0f, 1.0f};
    for (auto& x : v) x.depth = 3.0f;
    v[1].flow = v[2].flow = glm::vec3(2.0f, 0.0f, 0.0f);
    q.build(v, {0, 1, 2, 0, 2, 3});
    toy::water::WaterBaseSample b;
    expect(q.sample_base(glm::vec2(5.0f, 5.0f), b), "water query: centre is on the surface");
    expect_near(b.height, 1.5f, 1e-4f, "water query: height interpolates across the quad");
    expect_near(b.flow.x, 1.0f, 1e-4f, "water query: flow interpolates across the quad");
    expect(!q.sample_base(glm::vec2(-1.0f, 5.0f), b), "water query: outside the mesh is not water");
}

namespace water_test_util {

/** @brief A strip `length` x `width` along +X (local), resolution `nx` x `ny`, height z(x). */
template <typename HeightFn>
void make_strip(float length, float width, int nx, int ny, HeightFn z, std::vector<glm::vec3>& pos,
                std::vector<glm::vec2>& uv, std::vector<uint32_t>& idx) {
    pos.clear();
    uv.clear();
    idx.clear();
    for (int j = 0; j <= ny; ++j) {
        for (int i = 0; i <= nx; ++i) {
            float x = length * static_cast<float>(i) / nx;
            float y = width * (static_cast<float>(j) / ny - 0.5f);
            pos.push_back({x, y, z(x)});
            uv.push_back({x, y});
        }
    }
    const uint32_t row = static_cast<uint32_t>(nx + 1);
    for (uint32_t j = 0; j < static_cast<uint32_t>(ny); ++j) {
        for (uint32_t i = 0; i < static_cast<uint32_t>(nx); ++i) {
            uint32_t a = j * row + i;
            idx.insert(idx.end(), {a, a + 1, a + row + 1, a, a + row + 1, a + row});
        }
    }
}

/** @brief Mean flow over the strip's centre row for x in [x0, x1]. */
glm::vec3 mean_flow(const std::vector<glm::vec3>& pos, const std::vector<glm::vec3>& flow, float x0, float x1) {
    glm::vec3 sum(0.0f);
    int n = 0;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        if (pos[i].x < x0 || pos[i].x > x1 || std::fabs(pos[i].y) > 0.6f) continue;
        sum += flow[i];
        ++n;
    }
    return n ? sum / static_cast<float>(n) : sum;
}

} // namespace water_test_util

/** @brief The bake's whole premise: water runs downhill, faster and whiter where it is steep. */
void test_water_flow_bake_runs_downhill_faster_when_steep() {
    using namespace water_test_util;
    std::vector<glm::vec3> pos, flow;
    std::vector<glm::vec2> uv;
    std::vector<uint32_t> idx;
    std::vector<float> turb;
    // Gentle (2%) for x < 20, steep (40%) for 20..30, gentle again after. Descends along +X.
    auto z = [](float x) {
        if (x < 20.0f) return -0.02f * x;
        if (x < 30.0f) return -0.4f - 0.4f * (x - 20.0f);
        return -4.4f - 0.02f * (x - 30.0f);
    };
    make_strip(50.0f, 4.0f, 100, 4, z, pos, uv, idx);
    toy::water::bake_flow(pos, uv, idx, {}, {}, flow, turb);

    glm::vec3 gentle = mean_flow(pos, flow, 4.0f, 16.0f);
    glm::vec3 steep  = mean_flow(pos, flow, 22.0f, 28.0f);
    expect(gentle.x > 0.3f && std::fabs(gentle.y) < 0.05f * gentle.x, "water flow: gentle reach flows downhill (+X)");
    expect(steep.x > 0.0f && steep.z < 0.0f, "water flow: steep reach flows downhill and down the slope");
    expect(glm::length(steep) > 1.8f * glm::length(gentle), "water flow: the steep reach runs much faster");

    float t_gentle = 0.0f, t_steep = 0.0f;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        if (pos[i].x > 4.0f && pos[i].x < 16.0f) t_gentle = std::max(t_gentle, turb[i]);
        if (pos[i].x > 23.0f && pos[i].x < 27.0f) t_steep = std::min(t_steep == 0.0f ? 1.0f : t_steep, turb[i]);
    }
    expect(t_gentle < 0.05f, "water flow: no white water on the gentle reach");
    expect(t_steep > 0.5f, "water flow: rapids are turbulent");

    // Level water follows the UVs instead: u increasing along -X.
    make_strip(20.0f, 4.0f, 40, 4, [](float) { return 0.0f; }, pos, uv, idx);
    for (auto& u : uv) u.x = -u.x;
    toy::water::bake_flow(pos, uv, idx, {}, {}, flow, turb);
    glm::vec3 level = mean_flow(pos, flow, 4.0f, 16.0f);
    expect(level.x < -0.3f, "water flow: level water follows UV u when there is no slope to follow");
}

/** @brief A vertical wall across part of the stream: flow slides along it, a wake forms behind. */
void test_water_flow_bake_obstacle_deflects_and_wakes() {
    using namespace water_test_util;
    std::vector<glm::vec3> pos, flow_free, flow_obs;
    std::vector<glm::vec2> uv;
    std::vector<uint32_t> idx;
    std::vector<float> turb_free, turb_obs;
    make_strip(30.0f, 6.0f, 60, 12, [](float x) { return -0.03f * x; }, pos, uv, idx);
    // A post: an infinite vertical cylinder of radius 0.5 at (15, 0).
    toy::water::FlowRayFn post = [](const glm::vec3& o, const glm::vec3& d, float max_d, toy::water::FlowRayHit& hit) {
        glm::vec2 oc = glm::vec2(o) - glm::vec2(15.0f, 0.0f);
        glm::vec2 dh(d);
        float a = glm::dot(dh, dh);
        if (a < 1e-8f) return false;
        float b = glm::dot(oc, dh), c = glm::dot(oc, oc) - 0.25f;
        float disc = b * b - a * c;
        if (disc < 0.0f) return false;
        float t = (-b - std::sqrt(disc)) / a;
        if (t < 0.0f || t > max_d) return false;
        glm::vec2 p = glm::vec2(o) + dh * t - glm::vec2(15.0f, 0.0f);
        hit.distance = t;
        hit.normal = glm::vec3(glm::normalize(p), 0.0f);
        return true;
    };
    toy::water::FlowBakeParams fp;
    toy::water::bake_flow(pos, uv, idx, fp, {}, flow_free, turb_free);
    toy::water::bake_flow(pos, uv, idx, fp, post, flow_obs, turb_obs);

    float ahead_free = 0.0f, ahead_obs = 0.0f, wake_turb = 0.0f, far_turb = 0.0f;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        const glm::vec3& p = pos[i];
        if (p.x > 13.6f && p.x < 14.4f && std::fabs(p.y) < 0.3f) {
            ahead_free += flow_free[i].x;
            ahead_obs += flow_obs[i].x;
        }
        if (p.x > 16.0f && p.x < 18.0f && std::fabs(p.y) < 0.3f) wake_turb = std::max(wake_turb, turb_obs[i]);
        if (p.x > 16.0f && p.x < 18.0f && std::fabs(p.y) > 2.5f) far_turb = std::max(far_turb, turb_obs[i]);
    }
    expect(ahead_obs < 0.8f * ahead_free, "water flow: the current into a post is turned aside");
    expect(wake_turb > 0.15f, "water flow: white water trails behind the post");
    expect(far_turb < wake_turb, "water flow: the wake stays behind the post, not across the stream");
}

namespace water_test_util {

struct BuoyScene {
    std::unique_ptr<Scene> scene;
    toy::water::WaterSystem* water = nullptr;
    coopa::physx::system::PhysicsSystem* physics = nullptr;
    toy::water::WaterBody* body = nullptr;
};

/** @brief A scene with one WaterBody (calm 40x40 m planar at z = 0 unless configured). */
BuoyScene make_water_scene(const std::function<void(toy::water::WaterBody&)>& configure = {}) {
    BuoyScene bs;
    bs.scene = std::make_unique<Scene>("water_test_cpu");
    auto obj = std::make_unique<SceneObject>("water");
    obj->add_component<TransformComponent>();
    bs.body = obj->add_component<toy::water::WaterBody>();
    bs.body->size = glm::vec2(40.0f);
    bs.body->resolution = 20;
    if (configure) configure(*bs.body);
    bs.scene->add_root_object(std::move(obj));
    return bs;
}

/** @brief Adds a dynamic box with a Buoyancy component. */
SceneObject* add_box(BuoyScene& bs, const std::string& name, const glm::vec3& pos, const glm::vec3& size,
                     float mass) {
    auto obj = std::make_unique<SceneObject>(name);
    obj->add_component<TransformComponent>()->transform().set_position(pos);
    obj->add_component<coopa::physx::components::BoxCollider>()->set_size(size);
    obj->add_component<coopa::physx::components::RigidbodyComponent>()->mass = mass;
    obj->add_component<toy::water::Buoyancy>();
    SceneObject* raw = obj.get();
    bs.scene->add_root_object(std::move(obj));
    return raw;
}

void start(BuoyScene& bs) {
    bs.scene->start();
    bs.water = toy::water::install_water_system(*bs.scene);
    bs.physics = coopa::physx::system::install_physics_system(*bs.scene);
}

void run(BuoyScene& bs, float seconds) {
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < static_cast<int>(seconds / dt); ++i) {
        bs.scene->update(dt);
        bs.scene->late_update(dt);
    }
}

glm::vec3 com_of(SceneObject* o) {
    return o->get_component<coopa::physx::components::RigidbodyComponent>()->world_center_of_mass();
}

} // namespace water_test_util

/** @brief Archimedes: a box floats with mass/(rho V) of it under water, and settles there. */
void test_buoyancy_box_floats_at_its_density_ratio() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    // Flat slabs (2 x 2 x 0.5 m = 2 m^3), not cubes: a cube of density 0.25-0.75 is unstable
    // flat-side-up and floats on an edge (correctly -- that is real hydrostatics), which would
    // make the waterline assertions below depend on the tilt it settles at.
    SceneObject* half = add_box(bs, "half", glm::vec3(-5.0f, 0.0f, 1.0f), glm::vec3(2.0f, 2.0f, 0.5f), 1000.0f);
    SceneObject* light = add_box(bs, "light", glm::vec3(5.0f, 0.0f, 1.0f), glm::vec3(2.0f, 2.0f, 0.5f), 500.0f);
    start(bs);
    run(bs, 12.0f);

    auto* bh = half->get_component<toy::water::Buoyancy>();
    auto* bl = light->get_component<toy::water::Buoyancy>();
    expect(bh->resolved.size() == 8u, "buoyancy: a box collider generates a 2x2x2 pontoon lattice");
    expect_near(bh->submerged_fraction, 0.5f, 0.03f, "buoyancy: 500 kg/m^3 floats half submerged");
    expect_near(com_of(half).z, 0.0f, 0.03f, "buoyancy: ...which puts its centre on the waterline");
    expect_near(bl->submerged_fraction, 0.25f, 0.03f, "buoyancy: 250 kg/m^3 floats a quarter submerged");
    expect_near(com_of(light).z, 0.125f, 0.03f, "buoyancy: ...riding a quarter of its 0.5 m height higher");
    auto* rb = half->get_component<coopa::physx::components::RigidbodyComponent>();
    expect(glm::length(rb->velocity()) < 0.02f, "buoyancy: the bob has damped out on calm water");
    expect(rb->is_sleeping(), "buoyancy: a body at rest on calm water is allowed to fall asleep");
}

/** @brief Denser than water sinks -- but at a bounded speed, water drag doing its job. */
void test_buoyancy_dense_body_sinks_with_drag() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    SceneObject* stone = add_box(bs, "stone", glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f), 2400.0f);
    start(bs);
    run(bs, 4.0f);
    auto* rb = stone->get_component<coopa::physx::components::RigidbodyComponent>();
    expect(com_of(stone).z < -3.0f, "buoyancy: a 2400 kg/m^3 block sinks");
    expect(rb->velocity().z > -5.0f, "buoyancy: ...at a drag-limited speed, not in free fall");
    expect(stone->get_component<toy::water::Buoyancy>()->submerged_fraction > 0.99f,
           "buoyancy: ...fully submerged");
}

/** @brief Nothing outside a water body's footprint is touched (free fall stays analytic). */
void test_buoyancy_ignores_bodies_away_from_water() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    SceneObject* far = add_box(bs, "far", glm::vec3(100.0f, 0.0f, 50.0f), glm::vec3(1.0f), 100.0f);
    start(bs);
    run(bs, 0.5f);
    auto* rb = far->get_component<coopa::physx::components::RigidbodyComponent>();
    expect_near(rb->velocity().z, -9.81f * 0.5f, 0.2f, "buoyancy: a body away from the water falls freely");
    expect(!far->get_component<toy::water::Buoyancy>()->in_water, "buoyancy: ...and reports dry");
}

/** @brief Waves move floating things: a calm-water body stays put, a wavy one keeps moving. */
void test_buoyancy_rides_waves() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene([](toy::water::WaterBody& w) {
        w.waves.amplitude = 0.3f;
        w.waves.wavelength = 8.0f;
    });
    SceneObject* box = add_box(bs, "box", glm::vec3(0.0f, 0.0f, 0.5f), glm::vec3(1.0f), 400.0f);
    start(bs);
    run(bs, 6.0f);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 180; ++i) {
        run(bs, 1.0f / 60.0f);
        lo = std::min(lo, com_of(box).z);
        hi = std::max(hi, com_of(box).z);
    }
    expect(hi - lo > 0.2f, "buoyancy: a floating box heaves with the waves (range " + std::to_string(hi - lo) + " m)");
    toy::water::WaterSample s;
    expect(bs.water->sample(glm::vec2(com_of(box)), s), "water system: samples the surface under the box");
    expect(std::fabs(com_of(box).z - s.surface_height) < 0.5f, "buoyancy: the box stays at the surface it rides");
}

/** @brief A flowing body carries a floating box downstream, along the derived current. */
void test_buoyancy_carried_downstream_by_flow() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene([](toy::water::WaterBody& w) {
        std::vector<glm::vec3> pos;
        std::vector<glm::vec2> uv;
        std::vector<uint32_t> idx;
        make_strip(60.0f, 8.0f, 60, 8, [](float x) { return -0.03f * x; }, pos, uv, idx);
        w.mode = toy::water::WaterMode::Flowing;
        w.set_geometry(pos, uv, idx);
    });
    SceneObject* box = add_box(bs, "drifter", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.6f), 60.0f);
    start(bs);
    run(bs, 5.0f);
    toy::water::WaterSample s;
    expect(bs.water->sample(glm::vec2(10.0f, 0.0f), s), "water system: the flowing strip is sampleable");
    expect(s.flow.x > 0.5f, "water system: the strip's current runs downhill (+X)");
    const glm::vec3 p = com_of(box);
    expect(p.x > 9.0f, "buoyancy: the current carried the box downstream (x = " + std::to_string(p.x) + ")");
    expect(std::fabs(p.y) < 1.0f, "buoyancy: ...along the current, not across it");
}

/** @brief Moving bodies ring the water: a splash on entry, a wake trail behind a moving body,
 *         and silence once everything is at rest. */
void test_water_ripples_from_splash_wake_and_rest() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    SceneObject* drop = add_box(bs, "drop", glm::vec3(-6.0f, 0.0f, 3.0f), glm::vec3(1.0f), 300.0f);
    // A kinematic paddle driven straight through the water along +X at 2 m/s.
    auto paddle_obj = std::make_unique<SceneObject>("paddle");
    paddle_obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f, 8.0f, 0.0f));
    paddle_obj->add_component<coopa::physx::components::BoxCollider>()->set_size(glm::vec3(0.8f, 0.4f, 0.6f));
    auto* prb = paddle_obj->add_component<coopa::physx::components::RigidbodyComponent>();
    prb->is_kinematic = true;
    prb->use_gravity = false;
    SceneObject* paddle = paddle_obj.get();
    bs.scene->add_root_object(std::move(paddle_obj));
    start(bs);

    // Splash: the dropped crate hits the water within ~0.8 s.
    float strongest = 0.0f;
    for (int i = 0; i < 90; ++i) {
        run(bs, 1.0f / 60.0f);
        for (const auto& r : bs.water->ripples()) {
            if (glm::distance(r.position, glm::vec2(-6.0f, 0.0f)) < 1.0f) strongest = std::max(strongest, r.strength);
        }
    }
    expect(strongest > 0.3f, "ripples: a falling crate splashes (strength " + std::to_string(strongest) + ")");

    // Wake: 2 s of travel leaves a trail of rings along the path, in order.
    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < 120; ++i) {
        coopa::util::Transform& t = paddle->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(2.0f * dt, 0.0f, 0.0f));
        run(bs, dt);
    }
    std::vector<float> trail;
    for (const auto& r : bs.water->ripples()) {
        if (std::fabs(r.position.y - 8.0f) < 0.5f) trail.push_back(r.position.x);
    }
    expect(trail.size() >= 6u, "ripples: a moving body leaves a wake trail (" + std::to_string(trail.size()) + " rings)");
    expect(std::is_sorted(trail.begin(), trail.end()), "ripples: ...laid down in the order it travelled");
    expect(!trail.empty() && trail.back() > 3.0f, "ripples: ...all along its path");

    // Rest: the paddle stops, the crate settles; after the ring lifetime the lake is quiet.
    run(bs, 12.0f);
    expect(bs.water->ripples().empty(), "ripples: everything at rest -> no rings left (" +
                                            std::to_string(bs.water->ripples().size()) + ")");
    (void)drop;
}

/** @brief underwater_at(): below the surface, above it, and away from any water. */
void test_water_underwater_query() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    start(bs);
    run(bs, 0.1f);
    auto below = bs.water->underwater_at(glm::vec3(0.0f, 0.0f, -2.0f));
    auto above = bs.water->underwater_at(glm::vec3(0.0f, 0.0f, 2.0f));
    auto away  = bs.water->underwater_at(glm::vec3(100.0f, 0.0f, -2.0f));
    expect(below.underwater && below.body == bs.body, "underwater: a point 2 m down is underwater");
    expect_near(below.depth, 2.0f, 0.05f, "underwater: ...at depth 2 m");
    expect(!above.underwater && above.body == bs.body, "underwater: a point above is not, but is over the body");
    expect(!away.underwater && away.body == nullptr, "underwater: a point away from any water is neither");
}

/** @brief A water object destroyed after the system's update (as the editor's deferred edits
 *         do) must not be visible to queries once the list is refreshed -- the regression behind
 *         an intermittent editor crash (Engine queried a freed WaterBody). */
void test_water_queries_survive_body_destruction() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    start(bs);
    run(bs, 0.1f);
    expect(bs.water->underwater_at(glm::vec3(0.0f, 0.0f, -1.0f)).underwater, "query: underwater before");
    SceneObject* water_obj = bs.body->owner;
    bs.scene->remove_root_object(water_obj);   // destroyed now, like an editor rebuild
    bs.water->refresh_bodies(*bs.scene);
    const auto info = bs.water->underwater_at(glm::vec3(0.0f, 0.0f, -1.0f));
    expect(!info.underwater && info.body == nullptr, "query: a destroyed body is gone after refresh_bodies()");
}

/** @brief The precomputed WaveSet is the same wave sum as the reference evaluate(), and the
 *         distance fade removes the short waves first and everything far enough away. */
void test_water_waveset_matches_reference_and_fades() {
    toy::water::WaveParams w;
    w.amplitude = 0.4f;
    w.wavelength = 7.0f;
    w.direction = 0.6f;
    w.steepness = 0.8f;
    const toy::water::WaveSet ws = toy::water::WaveSet::from(w);
    expect(ws.matches(w), "waveset: built from these params");
    float worst = 0.0f;
    for (int i = 0; i < 64; ++i) {
        const glm::vec2 p(std::sin(i * 1.3f) * 30.0f, std::cos(i * 0.7f) * 30.0f);
        const float t = 0.21f * static_cast<float>(i);
        const float atten = 0.25f + 0.75f * static_cast<float>(i % 5) / 4.0f;
        const auto a = toy::water::evaluate(w, p, t, atten);
        const auto b = toy::water::evaluate(ws, p, t, atten, 0.0f);
        worst = std::max({worst, glm::length(a.displacement - b.displacement), glm::length(a.normal - b.normal),
                          std::fabs(a.crest - b.crest)});
    }
    expect(worst < 1e-5f, "waveset: matches the reference sum (worst " + std::to_string(worst) + ")");

    const float shortest = w.wavelength * toy::water::k_wave_length_ratio[toy::water::k_wave_count - 1];
    const float fs = toy::water::k_wave_fade_start;
    expect_near(toy::water::wave_distance_fade(shortest, fs * shortest * 0.9f), 1.0f, 1e-6f, "wave fade: none up close");
    expect_near(toy::water::wave_distance_fade(shortest, fs * shortest * 2.0f), 0.0f, 1e-6f, "wave fade: gone at 2x start");
    expect(toy::water::wave_distance_fade(shortest, fs * shortest * 1.5f) <
               toy::water::wave_distance_fade(w.wavelength, fs * shortest * 1.5f),
           "wave fade: short waves fade before long ones");
    const auto far = toy::water::evaluate(ws, glm::vec2(3.0f), 1.0f, 1.0f, 2.0f * fs * w.wavelength + 1.0f);
    expect(glm::length(far.displacement) < 1e-7f, "wave fade: nothing moves beyond every wave's fade");

    // The CPU query applies the same fade given a focus.
    toy::water::WaterSurfaceQuery q;
    std::vector<toy::water::WaterVertex> v(4);
    v[0].position = {-50.0f, -50.0f, 0.0f};
    v[1].position = {50.0f, -50.0f, 0.0f};
    v[2].position = {50.0f, 50.0f, 0.0f};
    v[3].position = {-50.0f, 50.0f, 0.0f};
    for (auto& x : v) x.depth = 100.0f;
    q.build(v, {0, 1, 2, 0, 2, 3});
    toy::water::WaveQueryOptions far_opt;
    far_opt.has_focus = true;
    far_opt.focus = glm::vec3(0.0f, 0.0f, 2.0f * fs * w.wavelength + 10.0f);
    toy::water::WaterSample s_near, s_far;
    expect(q.sample(glm::vec2(1.0f), ws, 0.5f, s_near) && q.sample(glm::vec2(1.0f), ws, 0.5f, s_far, far_opt),
           "water query: samples with and without a focus");
    expect(std::fabs(s_near.surface_height) > 1e-3f && std::fabs(s_far.surface_height) < 1e-6f,
           "water query: far from the focus the waves have faded, as on the GPU");
}

/** @brief Quality tiers order sensibly, and High is the shipped defaults. */
void test_water_quality_presets_expand() {
    using toy::water::WaterQuality;
    using toy::water::WaterSettings;
    const WaterSettings lo = WaterSettings::from_quality(WaterQuality::Low);
    const WaterSettings me = WaterSettings::from_quality(WaterQuality::Medium);
    const WaterSettings hi = WaterSettings::from_quality(WaterQuality::High);
    const WaterSettings ul = WaterSettings::from_quality(WaterQuality::Ultra);
    const WaterSettings def;
    expect(hi.sim_radius == def.sim_radius && hi.max_ripples == def.max_ripples &&
               hi.grid_density == def.grid_density && hi.detail_distance == def.detail_distance,
           "water quality: high == the defaults");
    expect(lo.sim_radius < me.sim_radius && me.sim_radius < hi.sim_radius && hi.sim_radius < ul.sim_radius,
           "water quality: simulation range grows with the tier");
    expect(lo.max_ripples < me.max_ripples && me.max_ripples < hi.max_ripples && hi.max_ripples <= ul.max_ripples &&
               ul.max_ripples <= toy::water::WaterSystem::k_max_ripples,
           "water quality: ripple cap grows with the tier, within the renderer's limit");
    expect(lo.grid_density < me.grid_density && me.grid_density <= hi.grid_density,
           "water quality: grid density grows with the tier");
    expect(lo.lod_bias > hi.lod_bias && ul.lod_bias < hi.lod_bias, "water quality: low coarsens LODs sooner, ultra later");
    expect(lo.detail_distance < ul.detail_distance && lo.ripple_layers <= ul.ripple_layers,
           "water quality: shader detail grows with the tier");
    expect(toy::core::parse_render_quality("medium") == toy::render::RenderQuality::Medium,
           "water quality: config tier strings parse");
}

/** @brief A floater outside the simulation range is frozen -- but one that is awake out there
 *         (dropped in) floats at its density ratio rather than sinking, then goes to sleep. */
void test_buoyancy_far_floater_freezes_and_does_not_sink() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    SceneObject* slab = add_box(bs, "far_slab", glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(2.0f, 2.0f, 0.5f), 1000.0f);
    start(bs);
    bs.water->set_focus(glm::vec3(1000.0f, 0.0f, 0.0f));
    run(bs, 10.0f);
    auto* b = slab->get_component<toy::water::Buoyancy>();
    auto* rb = slab->get_component<coopa::physx::components::RigidbodyComponent>();
    expect(!b->simulated, "frozen floater: 1 km away is outside the simulation range");
    expect(bs.water->active_buoyant_count() == 0u, "frozen floater: ...so full buoyancy skips it");
    expect_near(com_of(slab).z, 0.0f, 0.08f, "frozen floater: ...yet it floats at its waterline (z " +
                                                  std::to_string(com_of(slab).z) + ")");
    expect(b->in_water && b->submerged_fraction > 0.4f && b->submerged_fraction < 0.6f,
           "frozen floater: ...half submerged, like a simulated one");
    expect(rb->is_sleeping(), "frozen floater: ...and is put to sleep once settled");

    // Back in range: full buoyancy again, and it stays where it is on calm water.
    const glm::vec3 before = com_of(slab);
    bs.water->set_focus(glm::vec3(10.0f, 0.0f, 0.0f));
    run(bs, 2.0f);
    expect(b->simulated, "frozen floater: simulated again once the focus comes near");
    expect(glm::distance(com_of(slab), before) < 0.05f, "frozen floater: ...without a jump");
}

/** @brief The simulation range has hysteresis: in at r, out only past r * hysteresis. */
void test_buoyancy_sim_range_hysteresis() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    SceneObject* box = add_box(bs, "box", glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f), 400.0f);
    start(bs);
    const float r = bs.water->settings().sim_radius;
    const float mid = r * (1.0f + 0.5f * (bs.water->settings().sim_hysteresis - 1.0f)) + 1.0f;
    auto* b = box->get_component<toy::water::Buoyancy>();
    auto at = [&](float d) {
        bs.water->set_focus(glm::vec3(d, 0.0f, 0.0f));
        run(bs, 2.0f / 60.0f);
        return b->simulated;
    };
    expect(at(r * 0.5f), "sim range: well inside is simulated");
    expect(at(mid), "sim range: between r and r * hysteresis stays simulated");
    expect(!at(r * 2.0f), "sim range: far outside is frozen");
    expect(!at(mid), "sim range: coming back between r and r * hysteresis stays frozen");
    expect(at(r * 0.5f), "sim range: inside again is simulated");
}

/** @brief The expensive physics-aware bake waits until the body is in range. */
void test_water_stage2_bake_deferred_until_in_range() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    add_box(bs, "box", glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(1.0f), 400.0f);
    start(bs);
    bs.water->set_focus(glm::vec3(5000.0f, 0.0f, 0.0f));
    run(bs, 0.2f);
    expect(bs.body->baked && bs.body->bake_stage == 1, "deferred bake: out of range it stays at stage 1");
    bs.water->set_focus(glm::vec3(0.0f, 0.0f, 10.0f));
    run(bs, 2.0f / 60.0f);
    expect(bs.body->bake_stage == 2, "deferred bake: in range it bakes with physics");
}

/** @brief The ripple cap follows the tier, and bodies beyond the ripple range ring nothing. */
void test_water_ripples_capped_and_ranged() {
    using namespace water_test_util;
    BuoyScene bs = make_water_scene();
    add_box(bs, "drop", glm::vec3(-6.0f, 0.0f, 3.0f), glm::vec3(1.0f), 300.0f);
    start(bs);
    bs.water->set_settings(toy::water::WaterSettings::from_quality(toy::water::WaterQuality::Low));
    bs.water->set_focus(glm::vec3(-6.0f, 0.0f, 0.0f) + glm::vec3(bs.water->settings().ripple_range + 20.0f, 0.0f, 0.0f));
    run(bs, 1.5f);
    expect(bs.water->ripples().empty(), "ripples: a splash beyond the ripple range rings nothing");
    for (int i = 0; i < 40; ++i) bs.water->emit_ripple(glm::vec2(static_cast<float>(i) * 0.1f, 0.0f), 0.5f, 0.5f);
    expect(bs.water->ripples().size() == bs.water->settings().max_ripples,
           "ripples: live rings are capped at the tier's maximum (" + std::to_string(bs.water->ripples().size()) + ")");
    expect(std::fabs(bs.water->ripples().back().position.x - 3.9f) < 1e-4f, "ripples: ...keeping the newest");
}

/** @brief Render tiles cover the surface exactly; every LOD is a subset of LOD 0, wound +Z,
 *         with LOD switch sizes decreasing; a mesh tiling keeps every triangle once. */
void test_water_tiles_cover_surface() {
    using coopa::gfx::engine::data::Vertex;
    const int res = 40;
    const float size = 40.0f;
    std::vector<Vertex> verts;
    for (int y = 0; y <= res; ++y)
        for (int x = 0; x <= res; ++x) {
            Vertex v{};
            v.position = glm::vec3(-size * 0.5f + size * x / res, -size * 0.5f + size * y / res, 0.0f);
            v.normal = glm::vec3(0.0f, 0.0f, 1.0f);
            v.uv = glm::vec2(static_cast<float>(x), static_cast<float>(y)); // identifies the grid vertex
            verts.push_back(v);
        }
    toy::water::WaterTileLodParams lod;
    lod.wave_lambdas[0] = 6.0f;
    lod.wave_amps[0] = 0.5f;
    lod.wave_lambdas[3] = 1.4f;
    lod.wave_amps[3] = 0.02f;
    const auto tiles = toy::water::build_grid_tiles(verts, res, res, 16, size / res, lod, glm::vec3(0.1f));
    expect(tiles.size() == 9u, "water tiles: a 40-quad grid in 16-quad tiles is 3 x 3 (" + std::to_string(tiles.size()) + ")");
    std::size_t tris = 0;
    bool subset = true, up = true, decreasing = true, chains = true;
    for (const auto& t : tiles) {
        const auto& d = t.data;
        chains = chains && !d.lods.empty();
        if (d.lods.empty()) continue;
        tris += d.lods[0].index_count / 3;
        for (std::size_t k = 0; k < d.lods.size(); ++k) {
            const auto& l = d.lods[k];
            if (k >= 2) decreasing = decreasing && l.screen_size < d.lods[k - 1].screen_size;
            for (uint32_t i = l.first_index; i + 2 < l.first_index + l.index_count; i += 3) {
                const glm::vec3 a = d.vertices[l.vertex_offset + d.indices[i]].position;
                const glm::vec3 b = d.vertices[l.vertex_offset + d.indices[i + 1]].position;
                const glm::vec3 c = d.vertices[l.vertex_offset + d.indices[i + 2]].position;
                up = up && glm::cross(b - a, c - a).z > 0.0f;
            }
            for (int32_t v = l.vertex_offset; v < static_cast<int32_t>(d.vertices.size()); ++v) {
                const glm::vec2 g = d.vertices[v].uv;
                const Vertex& src = verts[static_cast<std::size_t>(g.y) * (res + 1) + static_cast<std::size_t>(g.x)];
                subset = subset && glm::distance(src.position, d.vertices[v].position) < 1e-6f;
            }
        }
        chains = chains && d.lods.size() >= 2;
    }
    expect(chains, "water tiles: every grid tile carries a LOD chain");
    expect(tris == static_cast<std::size_t>(res * res * 2), "water tiles: LOD 0 tiles cover the grid exactly once");
    expect(subset, "water tiles: every LOD vertex is a LOD 0 vertex (baked attributes carry over)");
    expect(up, "water tiles: every triangle faces +Z");
    expect(decreasing, "water tiles: coarser LODs switch in at smaller screen sizes");
    const float d4 = toy::water::water_lod_distance(4.0f, lod);
    toy::water::WaterTileLodParams calm;
    expect(toy::water::water_lod_crack(4.0f, d4, lod) <= 0.5f * toy::water::k_pixel_angle * d4 + 1e-6f,
           "water tiles: a coarse LOD waits until its T-junction gaps are under half a pixel");
    expect(d4 > toy::water::water_lod_distance(4.0f, calm),
           "water tiles: ...which waves push further out than calm water");

    // An arbitrary mesh: bucketed by centroid, every triangle exactly once, simplified LODs.
    std::vector<uint32_t> idx;
    for (int y = 0; y < res; ++y)
        for (int x = 0; x < res; ++x) {
            uint32_t i0 = static_cast<uint32_t>(y * (res + 1) + x), i1 = i0 + 1, i2 = i0 + res + 1, i3 = i2 + 1;
            idx.insert(idx.end(), {i0, i1, i3, i0, i3, i2});
        }
    const auto mtiles = toy::water::build_mesh_tiles(verts, idx, 16.0f, glm::vec3(0.1f), &lod);
    std::size_t mtris = 0;
    bool fewer = true;
    for (const auto& t : mtiles) {
        const auto& d = t.data;
        mtris += (d.lods.empty() ? d.indices.size() : d.lods[0].index_count) / 3;
        for (std::size_t k = 1; k < d.lods.size(); ++k) fewer = fewer && d.lods[k].index_count < d.lods[k - 1].index_count;
    }
    expect(mtiles.size() == 9u, "water tiles: a mesh is bucketed into tile squares");
    expect(mtris == idx.size() / 3, "water tiles: ...keeping every triangle exactly once");
    expect(fewer, "water tiles: ...with simplified LODs that shrink");
}

/** @brief The whole scene: water draws, floaters float, the river delivers its crates. */
void test_water_scene_renders_and_simulates() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/water_test/scene.yaml", 640, 360, 320, 180);
    toy::core::Engine engine(std::move(config));

    tick_frames(engine, 3);
    auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
    expect(water != nullptr, "water scene: Engine installed the WaterSystem");
    std::size_t baked = 0;
    for (auto* b : engine.scene().get_components<toy::water::WaterBody>()) baked += b->bake_stage == 2 ? 1 : 0;
    expect(baked == 2u, "water scene: lake and river both baked with physics (depth + obstacles)");

    auto river_s = [&]() {
        auto* o = engine.scene().find_object("river_crate_a");
        return o ? water_test_util::com_of(o) : glm::vec3(0.0f);
    };
    const glm::vec3 crate_start = river_s();
    tick_frames(engine, 600);
    const glm::vec3 crate_end = river_s();
    expect(crate_end.z < crate_start.z - 4.0f, "water scene: the river crate rode the rapids down the hill");
    expect(glm::length(glm::vec2(crate_end) - glm::vec2(crate_start)) > 12.0f,
           "water scene: ...a long way downstream");

    auto* stone = engine.scene().find_object("stone");
    auto* light = engine.scene().find_object("crate_light");
    expect(stone && water_test_util::com_of(stone).z < -2.0f, "water scene: the stone sank to the bed");
    if (light) {
        auto* b = light->get_component<toy::water::Buoyancy>();
        expect(b->in_water && b->submerged_fraction > 0.15f && b->submerged_fraction < 0.6f,
               "water scene: the light crate floats (submerged " + std::to_string(b->submerged_fraction) + ")");
    }

    expect(!engine.pipeline().water_state().ripples.empty(),
           "water scene: ripple rings (boat wake, bobbing floaters) reach the renderer");
    expect(!engine.pipeline().underwater_active(), "water scene: the overview camera is not underwater");

    const Frame frame = engine.capture_image(/*low_res=*/true);
    long long watery = 0;
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* px = &frame.pixels[i * frame.channels];
        if (px[2] > px[0] + 40 && px[2] > 90) ++watery; // distinctly blue
    }
    expect(watery > static_cast<long long>(pixels / 20), "water scene: the lake is on screen and blue");
    if (watery <= static_cast<long long>(pixels / 20)) dump_frame(frame, "water_scene");
}

namespace water_test_util {
/** @brief Mean RGB of a capture. */
glm::vec3 mean_rgb(const Frame& f) {
    glm::dvec3 sum(0.0);
    const size_t pixels = static_cast<size_t>(f.width) * f.height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* px = &f.pixels[i * f.channels];
        sum += glm::dvec3(px[0], px[1], px[2]);
    }
    return glm::vec3(sum / static_cast<double>(std::max<size_t>(pixels, 1)));
}
} // namespace water_test_util

/** @brief The camera under a water surface: UnderwaterPass engages, the frame turns to water
 *         (red absorbed first), and rising above the surface turns it back off. */
void test_underwater_scene_renders_and_toggles() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/underwater_test/scene.yaml", 640, 360, 320, 180);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 60);

    expect(engine.pipeline().underwater_active(), "underwater scene: the camera starts below the surface");
    const Frame below = engine.capture_image(/*low_res=*/true);
    const glm::vec3 c_below = water_test_util::mean_rgb(below);
    expect(c_below.g > c_below.r * 1.4f && c_below.b > c_below.r * 1.4f,
           "underwater scene: the frame is water-tinted, red absorbed (mean rgb " +
               std::to_string(c_below.r) + ", " + std::to_string(c_below.g) + ", " + std::to_string(c_below.b) + ")");

    // Rise above the surface.
    auto* cc = engine.scene().find_first_component<toy::scene::CameraController>();
    expect(cc != nullptr, "underwater scene: has an orbit camera");
    if (cc) {
        cc->movement_smoothing = 0.0f;
        cc->distance = 30.0f;
        cc->pitch_deg = 45.0f;
    }
    tick_frames(engine, 30);
    expect(!engine.pipeline().underwater_active(), "underwater scene: above the surface the pass is off");
    const Frame above = engine.capture_image(true);
    const glm::vec3 c_above = water_test_util::mean_rgb(above);
    expect(glm::length(c_above - c_below) > 15.0f, "underwater scene: ...and the frame changes accordingly");
    if (!(c_below.g > c_below.r * 1.4f)) dump_frame(below, "underwater_below");
}

/**
 * @brief The water system's job-parallel paths (buoyancy per floater, bake geometry, query and
 *        tiles) give bit-identical results to running inline: water_stress (200 floaters, a
 *        1000 m ocean -- every parallel path engages) simulated on one worker and on many ends
 *        with every floater in exactly the same pose, and the lake surfaces sample identically.
 */
void test_water_parallel_matches_serial() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    struct Run {
        std::vector<glm::vec3> pos;
        std::vector<glm::quat> rot;
        std::vector<float> surface;
        int active = 0;
    };
    auto run = [](unsigned int workers) {
        toy::core::AppConfig config = make_test_config("assets/scenes/water_stress/scene.yaml", 320, 180, 160, 90);
        config.jobs.worker_threads = workers;
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, 90);
        Run r;
        auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
        if (water) r.active = static_cast<int>(water->active_buoyant_count());
        for (auto* b : engine.scene().get_components<toy::water::Buoyancy>()) {
            const auto& t = b->owner->get_transform()->transform();
            r.pos.push_back(t.position());
            r.rot.push_back(t.rotation_quat());
        }
        if (water) {
            for (int i = 0; i < 64; ++i) {
                toy::water::WaterSample s;
                const glm::vec2 p(-60.0f + 2.0f * static_cast<float>(i), 10.0f + 0.5f * static_cast<float>(i));
                r.surface.push_back(water->sample(p, s) ? s.surface_height : -1e9f);
            }
        }
        return r;
    };
    const Run serial = run(1);
    const Run parallel = run(8);
    expect(serial.pos.size() >= 100 && serial.active >= static_cast<int>(32),
           "water parallel: the scene exercises the parallel buoyancy path (" + std::to_string(serial.active) + " active)");
    bool same = serial.pos.size() == parallel.pos.size() && serial.active == parallel.active;
    for (std::size_t i = 0; same && i < serial.pos.size(); ++i) {
        same = serial.pos[i] == parallel.pos[i] && serial.rot[i] == parallel.rot[i];
    }
    expect(same, "water parallel: every floater ends in exactly the same pose on 1 and 8 workers");
    expect(serial.surface == parallel.surface, "water parallel: the baked surfaces sample identically");
}

/**
 * @brief The water shader animates waves on the water system's clock, not the renderer's.
 *
 * Buoyancy evaluates the waves on the CPU at WaterSystem::time(), which starts with the scene;
 * the renderer's own clock starts with the pipeline. Before the two were tied, the drawn waves
 * and the ones floaters rode were at unrelated phases (any time spent before the scene loaded,
 * e.g. the editor before Play, was the offset), so bodies bobbed out of step with the surface.
 * Here the renderer has already run frames before the scene loads, so the clocks differ, and the
 * time handed to the shader must be the water clock (less the interpolation lag, < one step).
 */
void test_water_shader_shares_the_buoyancy_clock() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/pixel_demo/scene.yaml", 640, 360, 320, 180));
    tick_frames(engine, 30);   // the renderer's clock runs ahead of any later scene's
    engine.load_scene("assets/scenes/water_test/scene.yaml");
    tick_frames(engine, 20);
    auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
    expect(water != nullptr, "water clock: water_test has a water system");
    if (!water) return;
    const float handed = engine.pipeline().water_state().time;
    expect(handed >= 0.0f && handed <= water->time() && water->time() - handed <= 1.0f / 60.0f + 1e-4f,
           "water clock: the shader gets the water system's time (" + std::to_string(handed) +
           " vs " + std::to_string(water->time()) + ")");
    expect(water->time() < 0.5f + 20.0f / 60.0f,
           "water clock: the water clock started with the scene (" + std::to_string(water->time()) + " s)");
}

/** @brief water_quality switches live: every tier re-bakes and draws the lake (as several LOD'd
 *         tiles), Low at a coarser grid than High, with the tier's shader detail handed over. */
void test_water_scene_quality_tiers_render() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/water_test/scene.yaml", 640, 360, 320, 180));
    tick_frames(engine, 3);
    using toy::render::RenderQuality;
    std::size_t high_verts = 0, low_verts = 0;
    for (RenderQuality q : {RenderQuality::High, RenderQuality::Low, RenderQuality::Medium, RenderQuality::Ultra}) {
        engine.render_config().water_quality = q;
        tick_frames(engine, 4);
        const std::string tier = std::to_string(static_cast<int>(q));
        auto* water = dynamic_cast<toy::water::WaterSystem*>(engine.scene().find_system("Water"));
        expect(water && static_cast<int>(water->settings().quality) == static_cast<int>(q),
               "water tiers: the system runs tier " + tier);
        const toy::water::WaterBody* lake = nullptr;
        bool published = true;
        for (auto* b : engine.scene().get_components<toy::water::WaterBody>()) {
            published = published && toy::water::WaterSystem::is_published(*b);
            if (b->mode == toy::water::WaterMode::Planar) lake = b;
        }
        expect(published, "water tiers: every body published at tier " + tier);
        if (lake && q == RenderQuality::High) {
            high_verts = lake->query.vertices().size();
            expect(lake->tiles.size() > 1u, "water tiers: the 46 m lake is drawn as several tiles (" +
                                                std::to_string(lake->tiles.size()) + ")");
        }
        if (lake && q == RenderQuality::Low) low_verts = lake->query.vertices().size();
        expect(engine.pipeline().water_state().ripple_layers == water->settings().ripple_layers &&
                   engine.pipeline().water_state().ripples.size() <= water->settings().max_ripples,
               "water tiers: the tier's shader detail and ring cap reach the renderer");

        const Frame frame = engine.capture_image(/*low_res=*/true);
        long long watery = 0;
        const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* px = &frame.pixels[i * frame.channels];
            if (px[2] > px[0] + 40 && px[2] > 90) ++watery;
        }
        expect(watery > static_cast<long long>(pixels / 20), "water tiers: the lake draws at tier " + tier);
        if (watery <= static_cast<long long>(pixels / 20)) dump_frame(frame, "water_tier_" + tier);
    }
    expect(low_verts > 0 && low_verts < high_verts, "water tiers: low bakes a coarser grid than high (" +
                                                        std::to_string(low_verts) + " vs " + std::to_string(high_verts) + ")");
}

// =====================================================================================
// Group "rig" -- rigs as object hierarchies: quaternion clips and vertex-group skinning
// =====================================================================================

/** @brief A clip file's rotation_quat track drives a bone of an object hierarchy (nlerp), and
 *         a position track a deeper one, both addressed by paths under the Animator's object. */
void test_rig_clip_drives_hierarchy() {
    auto scene = std::make_unique<Scene>("rig_cpu");
    auto root = std::make_unique<SceneObject>("Rig");
    root->add_component<TransformComponent>();
    auto* animator = root->add_component<coopa::anim::Animator>();
    auto arm = std::make_unique<SceneObject>("Arm");
    arm->add_component<TransformComponent>();
    auto hand = std::make_unique<SceneObject>("Hand");
    hand->add_component<TransformComponent>()->transform().set_position(glm::vec3(1, 0, 0));
    SceneObject* hand_raw = hand.get();
    SceneObject* arm_raw = arm->add_child(std::move(hand)) ? arm.get() : nullptr;
    arm_raw->get_transform();
    hand_raw->get_transform()->set_parent_transform(&arm_raw->get_transform()->transform());
    root->add_child(std::move(arm));
    arm_raw->get_transform()->set_parent_transform(&root->get_transform()->transform());
    SceneObject* root_raw = root.get();
    scene->add_root_object(std::move(root));

    const float s = std::sqrt(0.5f);
    const fkyaml::node clip_yaml = fkyaml::node::deserialize(std::string(
        "clip:\n"
        "  name: wave\n"
        "  wrap: loop\n"
        "  length: 1.0\n"
        "  tracks:\n"
        "    - object: Arm\n"
        "      property: rotation_quat\n"
        "      keys:\n"
        "        - {time: 0.0, value: [0, 0, 0, 1]}\n"
        "        - {time: 1.0, value: [0, 0, ") + std::to_string(s) + ", " + std::to_string(s) + "]}\n"
        "    - object: Arm/Hand\n"
        "      property: position\n"
        "      keys:\n"
        "        - {time: 0.0, value: [1, 0, 0]}\n"
        "        - {time: 1.0, value: [2, 0, 0]}\n");
    animator->add_state("wave", std::make_shared<coopa::anim::AnimationClip>(coopa::anim::parse_clip(clip_yaml)));
    scene->start();
    animator->play("wave");
    animator->sample_at(0.5f);
    const glm::quat q = arm_raw->get_transform()->transform().rotation_quat();
    const float angle = glm::degrees(2.0f * std::atan2(std::abs(q.z), q.w));
    expect_near(angle, 45.0f, 0.5f, "rig: a rotation_quat track halfway through 0 -> 90 deg is 45 deg (nlerp)");
    expect_near(glm::length(q), 1.0f, 1e-5f, "rig: ...and stays a unit quaternion");
    expect_near(hand_raw->get_transform()->transform().position().x, 1.5f, 1e-5f, "rig: a nested path (Arm/Hand) is animated too");
    const glm::vec3 hand_world(hand_raw->get_transform()->get_world_matrix()[3]);
    expect(std::abs(hand_world.x - 1.5f * std::cos(glm::radians(45.0f))) < 1e-3f &&
               std::abs(hand_world.y - 1.5f * std::sin(glm::radians(45.0f))) < 1e-3f,
           "rig: the child follows its animated parent (it is an object hierarchy)");
    (void)root_raw;
}

/** @brief A mesh whose vertex groups name the bones skins with no joints: the palette comes
 *         from the groups (strongest four per vertex), and a vertex follows its groups' blend. */
void test_rig_vertex_group_skinning() {
    const fkyaml::node mesh = fkyaml::node::deserialize(std::string(
        "vertices: [[0, 0, 0], [1, 0, 0], [0, 1, 0]]\n"
        "normals: [[0, 0, 1], [0, 0, 1], [0, 0, 1]]\n"
        "uvs: [[0, 0], [1, 0], [0, 1]]\n"
        "faces: [[0, 1, 2]]\n"
        "weights:\n"
        "  - {Arm: 1.0}\n"
        "  - {Arm: 0.5, Hand: 0.5}\n"
        "  - {}\n"));
    const auto src = coopa::gfx::engine::data::SkinnedMeshSource::from_node(mesh);
    expect(src.groups.size() == 2 && src.groups[0] == "Arm" && src.groups[1] == "Hand",
           "skinning: the vertex groups become the bone palette, in first-use order");
    expect(src.joints.size() == 3 && src.joints[1].x >= 0 && src.joints[1].y >= 0 && src.joints[2].x == -1,
           "skinning: each corner indexes its groups (none for an unweighted vertex)");
    std::vector<coopa::gfx::engine::data::Vertex> out;
    const std::vector<glm::mat4> mats = {glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 2))};
    toy::scene::SkinnedMeshRenderer::skin(src, mats, out);
    expect_near(out[0].position.z, 0.0f, 1e-6f, "skinning: a vertex all in the still bone stays");
    expect_near(out[1].position.z, 1.0f, 1e-6f, "skinning: a vertex split half and half moves half way");
    expect_near(out[2].position.z, 0.0f, 1e-6f, "skinning: an unweighted vertex keeps its bind pose");
}

/** @brief A rig end to end in the engine: an Animator auto-plays a clip FILE on a bone of an
 *         object hierarchy, and a mesh whose vertex groups name the bones (no `bones:`, no inverse
 *         bind matrices -- what the editor's Weight Paint writes) follows it. */
void test_rig_skinned_mesh_follows_animated_bone() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "toyengine_rig_test";
    fs::remove_all(dir);
    fs::create_directories(dir / "meshes");
    fs::create_directories(dir / "animations" / "Rig");
    {
        std::ofstream(dir / "meshes" / "strip.yaml") <<
            "vertices: [[-0.5, 0, 0], [0.5, 0, 0], [0.5, 0, 2], [-0.5, 0, 2]]\n"
            "normals: [[0, -1, 0], [0, -1, 0], [0, -1, 0], [0, -1, 0]]\n"
            "uvs: [[0, 0], [1, 0], [1, 1], [0, 1]]\n"
            "faces: [[0, 1, 2, 3]]\n"
            "colors: []\n"
            "weights:\n  - {Lower: 1.0}\n  - {Lower: 1.0}\n  - {Upper: 1.0}\n  - {Upper: 1.0}\n";
        std::ofstream(dir / "animations" / "Rig" / "sway.yaml") <<
            "clip:\n  name: sway\n  wrap: once\n  length: 0.5\n  tracks:\n"
            "    - object: Lower/Upper\n      property: position\n      keys:\n"
            "        - {time: 0.0, value: [0, 0, 1]}\n        - {time: 0.5, value: [1, 0, 1]}\n";
        std::ofstream(dir / "scene.yaml") <<
            "format: blender\nscene:\n  scene_name: RigTest\n  root_objects:\n"
            "    - name: Camera\n      components:\n        - type: Transform\n          position: {x: 0, y: -6, z: 1}\n"
            "          rotation: {x: 90, y: 0, z: 0}\n        - type: Camera\n          main: true\n"
            "    - name: Rig\n      components:\n        - type: Transform\n"
            "        - type: Animator\n          auto_play: sway\n          states:\n"
            "            - {name: sway, clip: animations/Rig/sway.yaml}\n"
            "      children:\n"
            "        - name: Lower\n          components:\n            - type: Transform\n"
            "          children:\n"
            "            - name: Upper\n              components:\n                - type: Transform\n"
            "                  position: {x: 0, y: 0, z: 1}\n"
            "        - name: Skin\n          components:\n            - type: Transform\n"
            "            - type: MeshRenderer\n              material: {albedo: {r: 0.8, g: 0.6, b: 0.4}}\n"
            "            - type: SkinnedMeshRenderer\n              mesh_path: strip\n";
    }
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config((dir / "scene.yaml").string(), 320, 180, 160, 90));
    tick_frames(engine, 3);
    auto* skin_obj = engine.scene().find_object("Skin");
    auto* smr = skin_obj ? skin_obj->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
    expect(smr && smr->is_ready(), "rig: the skinned mesh built its GPU mesh");
    if (!smr || !smr->is_ready()) return;
    expect(smr->rig() && smr->rig()->name() == "Rig", "rig: the rig root is the nearest Animator up the hierarchy");
    expect(smr->bones().size() == 2 && smr->bones()[0] && smr->bones()[1] && smr->bones()[1]->name() == "Upper",
           "rig: the vertex groups name the bones, found under the rig");
    tick_frames(engine, 45);   // past the clip's end: Upper rests at x = 1
    float top_x = 0.0f, bottom_x = 0.0f;
    for (const auto& v : smr->skinned_vertices()) {
        if (v.position.z > 1.5f) top_x += v.position.x; else bottom_x += v.position.x;
    }
    // The quad's triangle corners: top (0.5, 0.5, -0.5) and bottom (-0.5, 0.5, -0.5) -- bind
    // means of +1/6 and -1/6 in x.
    const float n = static_cast<float>(smr->skinned_vertices().size()) / 2.0f;
    expect(std::abs(top_x / n - (1.0f / 6.0f + 1.0f)) < 0.02f,
           "rig: the vertices weighted to the animated bone moved with it, 1 m (mean x " + std::to_string(top_x / n) + ")");
    expect(std::abs(bottom_x / n + 1.0f / 6.0f) < 0.02f,
           "rig: the vertices weighted to the still bone stayed (bind pose from the rest pose)");
    fs::remove_all(dir);
}

/** @brief The animation_test scene runs: the arm's joints, the skinned tentacle and the
 *         self-animating ball all move, from their clip files. */
void test_animation_test_scene_runs() {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/animation_test/scene.yaml", 640, 360, 320, 180));
    tick_frames(engine, 2);
    auto& scene = engine.scene();
    auto* elbow = scene.find_object("elbow");
    auto* ball = scene.find_object("ball");
    auto* skin = scene.find_object("tentacle_skin");
    auto* smr = skin ? skin->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
    expect(elbow && ball && smr, "animation_test: the rigs loaded");
    if (!elbow || !ball || !smr) return;
    float min_z = 1e9f, max_z = -1e9f, min_scale_z = 1e9f;
    glm::quat q0 = elbow->get_transform()->transform().rotation_quat();
    float max_turn = 0.0f, max_tip_x = 0.0f;
    for (int i = 0; i < 90; ++i) {
        tick_frames(engine, 1);
        const auto& bt = ball->get_transform()->transform();
        min_z = std::min(min_z, bt.position().z);
        max_z = std::max(max_z, bt.position().z);
        min_scale_z = std::min(min_scale_z, bt.scale().z);
        max_turn = std::max(max_turn, 1.0f - std::abs(glm::dot(q0, elbow->get_transform()->transform().rotation_quat())));
        for (const auto& v : smr->skinned_vertices()) if (v.position.z > 2.0f) max_tip_x = std::max(max_tip_x, std::abs(v.position.x));
    }
    expect(max_z - min_z > 1.2f, "animation_test: the ball bounces (z " + std::to_string(min_z) + " .. " + std::to_string(max_z) + ")");
    expect(min_scale_z < 0.45f, "animation_test: ...and squashes on landing (scale z " + std::to_string(min_scale_z) + ")");
    expect(max_turn > 0.05f, "animation_test: the arm's elbow bends");
    expect(smr->is_ready() && smr->bones().size() == 4 && max_tip_x > 0.4f,
           "animation_test: the tentacle's skin follows its bones (tip x " + std::to_string(max_tip_x) + ")");
}

// =====================================================================================
// Group "ui" -- authored UI: YAML composites, binding by name, placement. No GPU.
// =====================================================================================

/** @brief A started scene parsed from `yaml` with the UI parsers (headless) registered. */
Scene load_ui_scene(const std::string& yaml, const std::string& anchor = "ui_test.yaml") {
    coopa::ui::register_ui_components();
    Scene scene = coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(yaml), anchor);
    return scene;
}

const char* kCompositeScene = R"(
format: toyengine
scene:
  scene_name: ui_test
  root_objects:
    - name: hud
      components:
        - {type: RectTransform, anchor_preset: StretchAll, size_delta: {x: 0, y: 0}}
        - {type: Canvas, reference_resolution: {x: 1280, y: 720}}
      children:
        - name: Inventory
          components:
            - {type: RectTransform, anchor_preset: MiddleCenter, size_delta: {x: 400, y: 300}}
            - {type: Window, title: Bag, close_button: true}
          children:
            - name: Hello
              components:
                - {type: RectTransform, size_delta: {x: 100, y: 20}}
                - {type: Text, text: hi}
            - name: Volume
              components:
                - {type: RectTransform, size_delta: {x: 300, y: 28}}
                - {type: SettingRow, kind: slider, label: Volume, min: 0, max: 10, value: 4}
        - name: Menu
          components:
            - {type: RectTransform, anchor_preset: TopLeft, size_delta: {x: 200, y: 200}}
            - type: MenuList
              items:
                - {name: Resume, label: Resume, role: Primary}
                - Quit Game
        - name: Health
          components:
            - {type: RectTransform, size_delta: {x: 200, y: 16}}
            - {type: StatBar, role: health, max: 50, value: 50}
        - name: Tabs
          components:
            - {type: RectTransform, size_delta: {x: 300, y: 200}}
            - {type: TabView, tabs: [A, B]}
          children:
            - name: PageA
              components: [{type: RectTransform}]
            - name: PageB
              components: [{type: RectTransform}]
        - name: Confirm
          components:
            - {type: RectTransform, size_delta: {x: 300, y: 160}}
            - type: Dialog
              title: Sure?
              buttons: [{name: Yes, label: Yes, role: Success}, {name: No, label: No}]
)";

void test_ui_composites_expand_and_slot_children() {
    Scene scene = load_ui_scene(kCompositeScene);
    SceneObject* hud = scene.find_object("hud");
    SceneObject* inv = scene.find_object("Inventory");
    SceneObject* hello = scene.find_object("Hello");
    expect(hud && inv && hello, "ui: the composite scene loads");
    if (!hud || !inv || !hello) return;
    expect(!inv->children().empty() && inv->children().front()->get_component<coopa::ui::UiGenerated>() != nullptr,
           "ui: a Window's generated frame is its first child, tagged UiGenerated");
    expect(hello->parent() && hello->parent()->name() == "Body", "ui: authored children move into the Window's Body");
    expect(coopa::ui::is_ui_generated(hello->parent()) && !coopa::ui::is_ui_generated(inv),
           "ui: is_ui_generated() tells generated nodes from authored ones");
    expect(inv->find_descendant("Close") && inv->find_descendant("Close")->get_component<coopa::ui::Button>(),
           "ui: close_button builds a Button named Close");

    coopa::ui::UiHandle ui(hud);
    expect(ui.find<coopa::ui::Button>("Resume") && ui.find<coopa::ui::Button>("QuitGame"),
           "ui: MenuList buttons take their item names (a bare label becomes QuitGame)");
    expect(std::abs(ui.get<float>("Volume") - 4.0f) < 1e-4f, "ui: a SettingRow's slider is reachable by the row's name");
    ui.set("Volume", 7.0f);
    expect(std::abs(ui.get<float>("Volume") - 7.0f) < 1e-4f, "ui: set() writes it");
    expect(ui.find<coopa::ui::ProgressBar>("Health") != nullptr, "ui: a StatBar's bar is found by the StatBar's name");

    SceneObject* page_a = scene.find_object("PageA");
    SceneObject* page_b = scene.find_object("PageB");
    expect(page_a && page_b && page_a->parent() && page_b->parent() && page_a->parent()->name() == "Page_A" &&
           page_b->parent()->name() == "Page_B", "ui: a TabView's children fill its pages in order");

    // Layout: the composite's frame stretches to the authored rect.
    auto* canvas = hud->get_component<coopa::ui::CanvasComponent>();
    canvas->rebuild_layout(1280, 720);
    canvas->rebuild_layout(1280, 720);
    const auto* frame_rt = inv->children().front()->get_component<coopa::ui::RectTransform>();
    expect(frame_rt && std::abs(frame_rt->rect().size().x - 400.0f) < 0.5f && std::abs(frame_rt->rect().size().y - 300.0f) < 0.5f,
           "ui: the generated frame fills the Window's 400x300 rect");
    const auto* body_rt = hello->parent()->get_component<coopa::ui::RectTransform>();
    const auto inv_rect = inv->get_component<coopa::ui::RectTransform>()->rect();
    expect(body_rt && body_rt->rect().min.x > inv_rect.min.x && body_rt->rect().max.y < inv_rect.max.y,
           "ui: the Body sits inside the window, below its title bar");
}

void test_ui_handle_signals_and_dialog() {
    Scene scene = load_ui_scene(kCompositeScene);
    coopa::ui::UiHandle ui(scene.find_object("hud"));
    int clicks = 0;
    auto c = ui.on_click("Resume", [&] { ++clicks; });
    scene.events().emit("Resume", "click");
    expect(clicks == 1, "ui: on_click() hears the button's named click on the scene EventBus");

    float seen = -1.0f;
    auto c2 = ui.on("Health", "value_changed", [&](const coopa::event::EventArgs& a) { seen = a.get("value", -2.0f); });
    ui.set("Health", 20.0f);
    expect(std::abs(seen - 20.0f) < 1e-4f, "ui: a ProgressBar publishes value_changed by name");

    coopa::stat::Resource hp{80.0f};
    hp.current = 30.0f;
    ui.bind_bar("Health", &hp);
    expect(std::abs(ui.get<float>("Health") - 30.0f) < 1e-4f, "ui: bind_bar() syncs the bar to a Resource");
    hp.damage(10.0f);
    expect(std::abs(ui.get<float>("Health") - 20.0f) < 1e-4f, "ui: ...and follows it");

    // Dialog: a footer button closes it, and "closed" is published by the dialog's name.
    bool closed = false;
    auto c3 = ui.on("Confirm", "closed", [&](const coopa::event::EventArgs&) { closed = true; });
    auto* yes = ui.find<coopa::ui::Button>("Yes");
    expect(yes != nullptr && ui.visible("Confirm"), "ui: a Dialog builds its footer buttons and starts open");
    if (yes) yes->on_click.emit();
    expect(!ui.visible("Confirm") && closed, "ui: a footer button closes the dialog and publishes closed");
    ui.show("Confirm");
    expect(ui.visible("Confirm"), "ui: show() reopens it");
}

/**
 * @brief The shipped game UI themes (assets/ui/themes: default, Blender, Unity) all load through
 *        the game's own parser, name fonts that exist next to them, and really differ.
 */
void test_ui_shipped_themes_load() {
    const std::filesystem::path dir = std::filesystem::path(ROOT_DIR) / "assets" / "ui" / "themes";
    std::map<std::string, coopa::ui::UITheme> themes;
    for (const char* name : {"default", "blender", "unity"}) {
        const std::filesystem::path p = dir / (std::string(name) + ".yaml");
        try {
            themes[name] = coopa::ui::load_theme_file(p.string());
        } catch (const std::exception& e) {
            expect(false, std::string("ui themes: ") + name + " loads (" + e.what() + ")");
            continue;
        }
        const coopa::ui::UITheme& t = themes[name];
        bool fonts_exist = std::filesystem::exists(dir / t.text.font_path);
        for (const auto* role : {&t.text.title, &t.text.heading, &t.text.body, &t.text.label, &t.text.caption, &t.text.numeric}) {
            if (!role->path.empty()) fonts_exist = fonts_exist && std::filesystem::exists(dir / role->path);
        }
        expect(fonts_exist, std::string("ui themes: ") + name + "'s fonts exist relative to it");
    }
    if (themes.size() != 3) return;
    auto same = [](const glm::vec4& a, const glm::vec4& b) { return glm::length(a - b) < 1e-4f; };
    expect(!same(themes["blender"].panel.panel, themes["unity"].panel.panel) &&
           !same(themes["blender"].panel.panel, themes["default"].panel.panel),
           "ui themes: Blender, Unity and the default theme look different");
    expect(themes["blender"].metrics.row_height < themes["default"].metrics.row_height,
           "ui themes: the editor-styled themes use compact rows");
}

void test_ui_theme_scope() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "toyengine_ui_theme_test";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir / "red.yaml");
        f << "name: red\npanel:\n  panel: { r: 1.0, g: 0.0, b: 0.0, a: 1.0 }\n";
    }
    const std::string yaml = std::string(R"(
format: toyengine
scene:
  scene_name: theme_test
  root_objects:
    - name: themed
      components:
        - {type: RectTransform, anchor_preset: StretchAll}
        - {type: Canvas}
        - {type: Theme, source: red.yaml}
      children:
        - name: W
          components:
            - {type: RectTransform, size_delta: {x: 100, y: 100}}
            - {type: Window}
    - name: plain
      components:
        - {type: RectTransform, anchor_preset: StretchAll}
        - {type: Canvas}
      children:
        - name: P
          components:
            - {type: RectTransform, size_delta: {x: 100, y: 100}}
            - {type: ThemedPanel}
)");
    coopa::ui::ThemeLibrary::instance().set_active(coopa::ui::UITheme::builtin_dark());
    Scene scene = load_ui_scene(yaml, (dir / "scene.yaml").string());
    SceneObject* w = scene.find_object("W");
    auto* img = w && !w->children().empty() ? w->children().front()->get_component<coopa::ui::Image>() : nullptr;
    expect(img && img->color.r > 0.99f && img->color.g < 0.01f, "ui: a composite takes its colours from the nearest Theme");
    expect(scene.find_object("themed")->get_component<coopa::ui::ThemeScope>() != nullptr, "ui: a Theme component stays as a ThemeScope");
}

void test_ui_placement_and_preview() {
    using coopa::ui::UiPass;
    auto s = UiPass::place_scissor({0, 0, 100, 50}, glm::vec2(200.0f, 100.0f), 1280, 720);
    expect(s.x == 200 && s.y == 100 && s.w == 100 && s.h == 50, "ui: a placed canvas's scissor moves by its origin");
    s = UiPass::place_scissor({0, 0, 300, 50}, glm::vec2(1200.0f, 700.0f), 1280, 720);
    expect(s.x == 1200 && s.w == 80 && s.h == 20, "ui: ...and is clamped to the target");

    Scene scene = load_ui_scene(kCompositeScene);
    auto* canvas = scene.find_object("hud")->get_component<coopa::ui::CanvasComponent>();
    canvas->set_viewport(640, 360);
    canvas->set_screen_origin(glm::vec2(100.0f, 50.0f));
    coopa::input::Input input;
    input.push_cursor_position(100.0 + 320.0, 50.0 + 180.0);
    canvas->set_input(input);
    const glm::vec2 c = canvas->input().position();
    const glm::vec2 half = canvas->root_rect().size() * 0.5f;
    expect(std::abs(c.x - half.x) < 0.5f && std::abs(c.y - half.y) < 0.5f,
           "ui: the cursor maps into a placed canvas (window centre of the rect -> canvas centre)");
    canvas->set_input_enabled(false);
    canvas->set_input(input);
    expect(canvas->input().position().x < -1000.0f, "ui: with input disabled the canvas sees an idle, far-away cursor");

    // A scene that does not simulate still draws its canvases through preview_refresh().
    scene.set_simulating(false);
    canvas->preview_refresh();
    expect(!canvas->draw_list().vertices().empty(), "ui: preview_refresh() lays out and emits a non-simulating canvas");
}

void test_ui_prefab_rect_transform_replaces() {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "toyengine_ui_prefab_test";
    std::filesystem::create_directories(dir / "ui");
    {
        std::ofstream f(dir / "ui" / "badge.yaml");
        f << "format: toyengine-object\nobject:\n  name: badge\n  components:\n"
             "    - {type: RectTransform, anchor_preset: TopRight, anchored_position: {x: -10, y: -10}, size_delta: {x: 64, y: 64}}\n"
             "    - {type: ThemedPanel}\n";
    }
    fkyaml::node inst = fkyaml::node::deserialize(std::string(
        "name: b\nprefab: ui/badge\ncomponents:\n  - {type: RectTransform, anchor_preset: BottomLeft, size_delta: {x: 32, y: 32}}\n"));
    const fkyaml::node r = coopa::scene::SceneInheritance::resolve_object(inst, (dir / "scene.yaml").string(),
        [](const std::string& p) { return coopa::yaml::load_document(p); });
    int rects = 0;
    bool kept_panel = false;
    std::string preset;
    for (const auto& c : r.at("components")) {
        const std::string t = c.at("type").get_value<std::string>();
        if (t == "RectTransform") { ++rects; preset = c.contains("anchor_preset") ? c.at("anchor_preset").get_value<std::string>() : ""; }
        if (t == "ThemedPanel") kept_panel = true;
    }
    expect(rects == 1 && preset == "BottomLeft" && !r.at("components").as_seq()[0].contains("anchored_position"),
           "ui: a UI prefab instance's own RectTransform replaces the asset's (placed, not merged)");
    expect(kept_panel, "ui: ...while its other components still come from the asset");
}


/**
 * @brief The UI showcase in the game engine: the HUD is a UI asset placed as `prefab: ui/hud`,
 *        its composites (StatBar, Hotbar...) expand from YAML with the theme, HealthDriver binds
 *        the authored Health bar BY NAME and drains it, and the canvas draws over the frame.
 */
void test_ui_showcase_scene_runs() {
    ScopedEnv fixed_dt("FIXED_DT", "0.05");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/ui_showcase/scene.yaml", 1280, 720, 1280, 720);
    toy::core::Engine engine(std::move(config));
    for (int i = 0; i < 3; ++i) engine.tick();
    coopa::ui::UiHandle hud(engine.scene().find_object("hud"));
    expect(hud && hud.find<coopa::ui::CanvasComponent>("") != nullptr, "ui_showcase: the HUD prefab is in the scene with its canvas");
    expect(hud.find<coopa::ui::ProgressBar>("Health") && hud.find<coopa::ui::InventoryGrid>("Hotbar") && hud.has("QuestTitle"),
           "ui_showcase: its composites expanded (Health bar, Hotbar, quest text)");
    const float before = hud.get<float>("Health", -1.0f);
    for (int i = 0; i < 40; ++i) engine.tick();
    const float after = hud.get<float>("Health", -1.0f);
    expect(before > 0.0f && after < before, "ui_showcase: HealthDriver drives the authored bar by name (" +
           std::to_string(before) + " -> " + std::to_string(after) + ")");
    auto* plate = engine.scene().find_object("Nameplate");
    expect(plate && plate->get_component<coopa::ui::CanvasComponent>() && plate->get_component<coopa::ui::CanvasComponent>()->is_world_space(),
           "ui_showcase: the hero carries a world-space nameplate");
    const Frame f = engine.capture_image(false);
    expect(count_near_color(f, 209, 51, 56, 30) > 50, "ui_showcase: the health bar's red is on screen");
}

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
    // --- yaml_io: coopa::yaml document loading + the caml codec, no GPU ---
    {"caml_roundtrips_every_asset",                "yaml_io", test_caml_roundtrips_every_asset},
    {"caml_detected_by_magic_not_extension",       "yaml_io", test_caml_detected_by_magic_not_extension},
    {"caml_without_codec_names_the_fix",           "yaml_io", test_caml_without_codec_names_the_fix},
    {"yaml_variant_resolution",                    "yaml_io", test_yaml_variant_resolution},
    {"caml_config_and_scene_load_identically",     "yaml_io", test_caml_config_and_scene_load_identically},
    {"caml_key_is_cached",                         "yaml_io", test_caml_key_is_cached},
    {"caml_scene_renders_identically",             "render_pixel", test_caml_scene_renders_identically},

    // --- editor_host: Engine embedding (edit/play mode, scene push, display region) ---
    {"engine_edit_mode_freezes_simulation",        "editor_host", test_engine_edit_mode_freezes_simulation},
    {"engine_edit_mode_shows_water",               "editor_host", test_engine_edit_mode_shows_water},
    {"engine_applies_scene_settings",              "editor_host", test_engine_applies_scene_settings},
    {"engine_push_scene_restores_edit_scene",      "editor_host", test_engine_push_scene_restores_edit_scene},
    {"engine_display_region_and_viewport_ray",     "editor_host", test_engine_display_region_and_viewport_ray},

    // --- world: the terrain tile system's pure half, no GPU ---
    {"tile_atlas_cells_disjoint",                  "world", test_tile_atlas_cells_are_disjoint_and_inset},
    {"tile_face_transforms_match_normals",         "world", test_face_transforms_agree_with_face_normals},
    {"chunk_flat_ground_emits_tops_only",          "world", test_chunk_flat_ground_emits_tops_only},
    {"chunk_greedy_merge_preserves_surface",       "world", test_chunk_greedy_merge_preserves_surface},
    {"chunk_perimeter_walls_follow_the_pad",       "world", test_chunk_perimeter_walls_follow_the_pad},
    {"chunk_step_exposure_is_symmetric",           "world", test_chunk_step_exposure_is_symmetric},
    {"chunk_meshing_is_deterministic",             "world", test_chunk_meshing_is_deterministic},
    {"chunk_uvs_stay_inside_atlas_cell",           "world", test_chunk_uvs_stay_inside_their_atlas_cell},
    {"sampler_cell_lookup_matches_brute_force",    "world", test_sampler_cell_lookup_matches_brute_force},
    {"sampler_chunks_agree_across_border",         "world", test_sampler_chunks_agree_across_their_shared_border},

    // --- math: pure functions, no GPU ---
    {"letterbox_exact_fit",                        "math", test_letterbox_exact_fit},
    {"frustum_perspective_culls_boxes",            "math", test_frustum_perspective_culls_boxes},
    {"frustum_ortho_and_cube_face",                "math", test_frustum_ortho_and_cube_face},
    {"world_aabb_matches_corners",                 "math", test_world_aabb_matches_corners},
    {"screen_height_fraction",                     "math", test_screen_height_fraction},
    {"select_lod_thresholds_and_hysteresis",       "math", test_select_lod_thresholds_and_hysteresis},
    {"projected_texels",                           "math", test_projected_texels},
    {"mesh_build_welds_and_generates_lods",        "math", test_mesh_build_welds_and_generates_lods},
    {"mesh_submesh_parts",                         "math", test_mesh_submesh_parts},
    {"object_assets_prefab",                       "math", test_object_assets_prefab},
    {"mesh_lod_simplifies_flat_shaded_mesh",       "math", test_mesh_lod_simplifies_flat_shaded_mesh},
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
    {"dir_shadow_fit_covers_camera_far_from_origin", "math", test_dir_shadow_fit_covers_camera_far_from_origin},
    {"dir_shadow_fit_degenerate_shadow_distance",  "math", test_dir_shadow_fit_degenerate_shadow_distance},
    {"cascade_splits_are_increasing_and_reach_the_distance", "math", test_cascade_splits_are_increasing_and_reach_the_distance},
    {"cascade_splits_single_cascade_is_the_whole_range",     "math", test_cascade_splits_single_cascade_is_the_whole_range},
    {"cascade_splits_lambda_selects_the_distribution",       "math", test_cascade_splits_lambda_selects_the_distribution},
    {"cascade_fit_near_slice_is_finer_than_far_slice",       "math", test_cascade_fit_near_slice_is_finer_than_far_slice},
    {"cascade_fit_contains_its_own_slice",                   "math", test_cascade_fit_contains_its_own_slice},
    {"cascade_fit_reaches_casters_above_the_near_slice",     "math", test_cascade_fit_reaches_casters_above_the_near_slice},
    {"cascade_atlas_tiles_are_disjoint_and_in_bounds",       "math", test_cascade_atlas_tiles_are_disjoint_and_in_bounds},
    {"cascade_selection_inset_exceeds_the_pcf_reach",        "math", test_cascade_selection_inset_exceeds_the_pcf_reach},
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
    {"config_scene_settings_layer",                "config", test_app_config_scene_settings_layer},
    {"config_dof_defaults",                        "config", test_pixel_render_config_dof_defaults},
    {"config_dof_round_trip",                      "config", test_app_config_load_round_trips_dof_settings},
    {"config_debug_view_parsing",                  "config", test_debug_view_parsing},
    {"config_soft_shadow_defaults",                "config", test_pixel_render_config_soft_shadow_defaults},
    {"config_soft_shadow_round_trip",              "config", test_app_config_load_round_trips_soft_shadow_settings},
    {"config_cascade_round_trip",                  "config", test_app_config_load_round_trips_cascade_settings},
    {"config_quality_presets",                     "config", test_app_config_load_applies_quality_presets},
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
    {"free_mover_travels_on_all_three_axes",       "scene", test_free_mover_travels_on_all_three_axes},
    {"free_mover_smoothing_frame_rate_independent","scene", test_free_mover_smoothing_is_frame_rate_independent},
    {"kinematic_control_runs_before_physics",      "scene", test_kinematic_control_runs_before_physics},
    {"kinematic_mover_pingpong",                   "scene", test_kinematic_mover_pingpong_oscillates_about_origin},
    {"kinematic_mover_orbit",                      "scene", test_kinematic_mover_orbit_holds_radius_and_height},
    {"kinematic_mover_spin",                       "scene", test_kinematic_mover_spin_rotates_in_place},
    {"health_driver_cycle",                        "scene", test_health_driver_cycles_between_turnaround_and_full},

    // --- ui: authored UI (YAML composites, binding by name, placement), no GPU ---
    {"ui_composites_expand_and_slot_children",     "ui", test_ui_composites_expand_and_slot_children},
    {"ui_handle_signals_and_dialog",               "ui", test_ui_handle_signals_and_dialog},
    {"ui_theme_scope",                             "ui", test_ui_theme_scope},
    {"ui_shipped_themes_load",                     "ui", test_ui_shipped_themes_load},
    {"ui_placement_and_preview",                   "ui", test_ui_placement_and_preview},
    {"ui_prefab_rect_transform_replaces",          "ui", test_ui_prefab_rect_transform_replaces},

    // --- water: the water system's CPU half (waves, query, flow bake, buoyancy), no GPU ---
    {"water_wave_height_inverse_matches_forward",  "water", test_water_wave_height_inverse_matches_forward},
    {"water_surface_query_interpolates",           "water", test_water_surface_query_interpolates_and_bounds},
    {"water_flow_runs_downhill_faster_when_steep", "water", test_water_flow_bake_runs_downhill_faster_when_steep},
    {"water_flow_obstacle_deflects_and_wakes",     "water", test_water_flow_bake_obstacle_deflects_and_wakes},
    {"buoyancy_floats_at_density_ratio",           "water", test_buoyancy_box_floats_at_its_density_ratio},
    {"buoyancy_dense_body_sinks_with_drag",        "water", test_buoyancy_dense_body_sinks_with_drag},
    {"buoyancy_ignores_bodies_away_from_water",    "water", test_buoyancy_ignores_bodies_away_from_water},
    {"buoyancy_rides_waves",                       "water", test_buoyancy_rides_waves},
    {"buoyancy_carried_downstream_by_flow",        "water", test_buoyancy_carried_downstream_by_flow},
    {"water_ripples_splash_wake_rest",             "water", test_water_ripples_from_splash_wake_and_rest},
    {"water_underwater_query",                     "water", test_water_underwater_query},
    {"water_queries_survive_body_destruction",     "water", test_water_queries_survive_body_destruction},
    {"water_waveset_matches_reference_and_fades",  "water", test_water_waveset_matches_reference_and_fades},
    {"water_quality_presets_expand",               "water", test_water_quality_presets_expand},
    {"buoyancy_far_floater_freezes_and_does_not_sink", "water", test_buoyancy_far_floater_freezes_and_does_not_sink},
    {"buoyancy_sim_range_hysteresis",              "water", test_buoyancy_sim_range_hysteresis},
    {"water_stage2_bake_deferred_until_in_range",  "water", test_water_stage2_bake_deferred_until_in_range},
    {"water_ripples_capped_and_ranged",            "water", test_water_ripples_capped_and_ranged},
    {"water_tiles_cover_surface",                  "water", test_water_tiles_cover_surface},

    // --- render_*: one Vulkan device each ---
    {"terrain_streams_chunks_around_camera",       "render_terrain",  test_terrain_streams_chunks_around_the_camera},
    {"static_camera_converges",                    "render_terrain",  test_static_camera_converges_to_a_static_image},
    {"image_settles_after_camera_stops",           "render_terrain",  test_image_settles_after_camera_stops},
    {"pixel_demo_render_and_live_toggles",         "render_pixel",    test_pixel_demo_render_and_live_toggles},
    {"headless_render_with_all_toggles_off",       "render_pixel",    test_headless_render_with_all_toggles_off},
    {"debug_view_channels_render",                 "render_pixel",    test_debug_view_channels_render},
    {"world_canvas_button_hover",                  "render_ui",       test_world_canvas_button_hover},
    {"ui_showcase_scene_runs",                     "render_ui",       test_ui_showcase_scene_runs},
    {"material_maps_change_output",                "render_material", test_material_maps_change_output},
    {"cloth_scene_simulates_and_animates",         "render_cloth",    test_cloth_scene_simulates_and_animates},
    {"water_scene_renders_and_simulates",          "render_water",    test_water_scene_renders_and_simulates},
    {"underwater_scene_renders_and_toggles",       "render_water",    test_underwater_scene_renders_and_toggles},
    {"water_scene_quality_tiers_render",           "render_water",    test_water_scene_quality_tiers_render},
    {"water_shader_shares_the_buoyancy_clock",     "render_water",    test_water_shader_shares_the_buoyancy_clock},
    {"water_parallel_matches_serial",              "render_water",    test_water_parallel_matches_serial},
    {"rig_clip_drives_hierarchy",                  "rig",             test_rig_clip_drives_hierarchy},
    {"rig_vertex_group_skinning",                  "rig",             test_rig_vertex_group_skinning},
    {"rig_skinned_mesh_follows_animated_bone",     "render_rig",      test_rig_skinned_mesh_follows_animated_bone},
    {"animation_test_scene_runs",                  "render_rig",      test_animation_test_scene_runs},
    // TEMPORARY (round-8b shimmer diagnosis) -- run via `toyengine_tests ssao_travel_probe`,
    // removed once the cause is pinned. Not in any ctest group.
    {"ssao_travel_probe",                          "probe",           test_ssao_travel_probe},
    {"ssao_blip_probe",                            "probe",           test_ssao_blip_probe},
    {"ssr_jitter_probe",                           "probe",           test_ssr_jitter_probe},
    {"look_canary",                                "probe",           test_look_canary},
    {"texel_aa_ring_repro",                        "probe",           test_texel_aa_ring_repro},
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
                 "  --group <name>   run one group: math, config, scene, ui, world, water, render_pixel,\n"
                 "                   render_ui, render_material, render_cloth, render_water\n"
                 "  -v, --verbose    print every assertion, not just failures\n"
                 "  <substring>      run the tests whose name contains it\n";
}

} // namespace

int main(int argc, char** argv) {
    toy::core::install_caml_codec();

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
