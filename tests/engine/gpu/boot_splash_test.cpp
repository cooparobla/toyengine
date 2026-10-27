/**
 * @file boot_splash_test.cpp
 * @brief The boot splash as pixels: BootSplash builds its pipelines from uicoopa's shaders,
 *        rasterizes the logo, loads the wordmark font, and draws a readable startup screen --
 *        backdrop gradient, an upright logo, a bar that shows the progress it was given, and
 *        both lines of text.
 *
 * Rendered into an offscreen RGBA8 target rather than a swapchain: a swapchain image is created
 * COLOR_ATTACHMENT-only and cannot be read back at all, and the splash's real target is exactly
 * a colour attachment with the UI pass over it either way. The PNG is written to the test's
 * scratch directory on every run, because this is the one screen a normal session cannot be
 * paused on to look at.
 */

#include <uicoopa/ui_resources.h>
#include <coopa/testing/test.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/app/context.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/util/image_readback.h>

#include <toyengine/core/boot_splash.h>
#include <toyengine/core/engine.h>
#include <toyengine/core/runtime_paths.h>


#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

#include <root_directory.h>

COOPA_TEST_SUITE("boot_splash");

using namespace toy::test;

namespace {

constexpr uint32_t kW = 1280, kH = 720;

/** @brief An 8-bit RGB read of one pixel of an RGBA8 capture. */
glm::ivec3 pixel(const Frame& f, uint32_t x, uint32_t y) {
    const size_t i = (static_cast<size_t>(y) * f.width + x) * f.channels;
    return glm::ivec3(f.pixels[i], f.pixels[i + 1], f.pixels[i + 2]);
}

/** @brief Pixels in the box (fractions of the frame) within `tol` per channel of an RGB colour. */
long long count_color_in_box(const Frame& f, glm::ivec3 rgb, int tol,
                             float x0, float x1, float y0, float y1) {
    long long n = 0;
    const uint32_t xa = static_cast<uint32_t>(x0 * f.width), xb = static_cast<uint32_t>(x1 * f.width);
    const uint32_t ya = static_cast<uint32_t>(y0 * f.height), yb = static_cast<uint32_t>(y1 * f.height);
    for (uint32_t y = ya; y < yb && y < f.height; ++y) {
        for (uint32_t x = xa; x < xb && x < f.width; ++x) {
            const glm::ivec3 p = pixel(f, x, y);
            if (std::abs(p.r - rgb.r) <= tol && std::abs(p.g - rgb.g) <= tol && std::abs(p.b - rgb.b) <= tol) ++n;
        }
    }
    return n;
}

/** @brief Pixels in the box noticeably brighter than the backdrop there -- text, logo or bar. */
long long count_bright_in_box(const Frame& f, float x0, float x1, float y0, float y1, int over) {
    long long n = 0;
    const uint32_t xa = static_cast<uint32_t>(x0 * f.width), xb = static_cast<uint32_t>(x1 * f.width);
    const uint32_t ya = static_cast<uint32_t>(y0 * f.height), yb = static_cast<uint32_t>(y1 * f.height);
    for (uint32_t y = ya; y < yb && y < f.height; ++y) {
        // The backdrop is a vertical gradient, so "brighter than the backdrop" is measured
        // against this row's own left edge rather than one global colour.
        const glm::ivec3 bg = pixel(f, xa > 0 ? xa : 1, y);
        for (uint32_t x = xa; x < xb && x < f.width; ++x) {
            const glm::ivec3 p = pixel(f, x, y);
            if (p.r + p.g + p.b > bg.r + bg.g + bg.b + over) ++n;
        }
    }
    return n;
}

/**
 * @class SplashHarness
 * @brief One Context + offscreen target + BootSplash, rendering a frame per call.
 *
 * Shared across a test's captures rather than rebuilt per frame: a Context is a window, an
 * instance, a device and a swapchain, and re-presenting the same splash with new text is what
 * actually happens during a startup anyway.
 */
class SplashHarness {
public:
    explicit SplashHarness(toy::core::BootSplashStyle style = {})
        : ctx_(context_config_()),
          target_(ctx_.device(), ctx_.allocator(), kW, kH, coopa::gfx::Format::RGBA8_Unorm),
          splash_(ctx_.device(), ctx_.allocator(), ctx_.command_pool(), target_.render_pass_object(),
                  coopa::gfx::Format::RGBA8_Unorm,
                  coopa::gfx::pipeline::ShaderLibrary(
                      toy::core::RuntimeLayout::current().shader_roots(std::filesystem::path(ROOT_DIR))),
                  std::vector<std::string>{(std::filesystem::path(ROOT_DIR) / "assets").string()},
                  std::move(style)) {}

    /**
     * @brief Releases the font the splash loaded, before this harness's allocator goes away.
     *
     * UIResourceCache is process-wide and holds the glyph atlas as a GPU image belonging to THIS
     * Context's allocator; left in the cache, it outlives the allocator and VMA aborts on a
     * block that still has live allocations. ~Engine does exactly this for the same reason.
     */
    ~SplashHarness() {
        ctx_.wait_idle();
        coopa::ui::UIResourceCache::instance().clear();
    }

    /** @brief Renders one splash frame into the target and reads it back. */
    Frame render(const std::string& status, float progress) {
        // prepare() before any recording, exactly as the contract demands: it writes descriptor
        // sets and may regrow the geometry buffers, neither of which is legal inside a pass.
        splash_.prepare(0, kW, kH, status, progress);
        ctx_.command_pool().submit_once([&](coopa::gfx::command::CommandBuffer& cmd) {
            target_.begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
            splash_.record(cmd, 0, kW, kH);
            target_.end(cmd);
        });
        ctx_.wait_idle();
        return coopa::gfx::util::read_image(ctx_.device(), ctx_.allocator(), ctx_.command_pool(),
                                            *target_.color_image_object());
    }

private:
    static coopa::gfx::app::ContextConfig context_config_() {
        coopa::gfx::app::ContextConfig cc;
        cc.title      = "toyengine_boot_splash_test";
        cc.width      = kW;
        cc.height     = kH;
        cc.visible    = false;   // never put a window on screen from a test
        cc.vsync      = false;
        cc.max_frames = 0;       // this suite never runs a frame loop; ignore MAX_FRAMES
        return cc;
    }

    coopa::gfx::app::Context ctx_;
    coopa::gfx::engine::targets::OffscreenTarget target_;
    toy::core::BootSplash splash_;
};

}  // namespace

COOPA_TEST(the_splash_draws_a_readable_startup_screen) {
    SplashHarness splash;
    const Frame f = splash.render("Loading scene...", 0.5f);
    dump_frame(f, "boot_splash");   // always: this screen is gone before a session can be paused
    expect(f.width == kW && f.height == kH, "splash: the frame came back at the target's size");

    // Backdrop: the authored colour at the top, darkened toward the bottom, everywhere -- no
    // unwritten pixels (the signature of a pipeline that never bound).
    const glm::ivec3 top = pixel(f, 4, 4);
    const glm::ivec3 bottom = pixel(f, 4, kH - 4);
    expect(top.b > top.r && top.b > top.g, "backdrop: indigo (blue is the strongest channel)");
    expect(top.r + top.g + top.b > bottom.r + bottom.g + bottom.b + 30,
           "backdrop: the gradient is brighter at the top");
    expect(bottom.r + bottom.g + bottom.b > 20, "backdrop: the bottom is the darkened colour, not black");
    expect(black_block_pixels(f) == 0, "splash: no unwritten blocks anywhere in the frame");

    // The logo: its yellow top face is in the UPPER half of the logo. A flipped V would put it
    // in the lower half, which is the one way to draw this quad that looks wrong but passes
    // every position check.
    // The logo is 152 px centred 70 px above the middle of a 720-row frame, so it spans rows
    // 0.30..0.51 of the image and its own midline is at 0.40.
    const long long yellow_above = count_color_in_box(f, glm::ivec3(255, 200, 69), 40,
                                                      0.42f, 0.58f, 0.30f, 0.40f);
    const long long yellow_below = count_color_in_box(f, glm::ivec3(255, 200, 69), 40,
                                                      0.42f, 0.58f, 0.42f, 0.52f);
    expect(yellow_above > 200, "logo: the yellow top face is drawn (" + std::to_string(yellow_above) + " px)");
    expect(yellow_above > yellow_below * 2,
           "logo: the top face is in the logo's upper half, so the image is upright (" +
               std::to_string(yellow_above) + " above vs " + std::to_string(yellow_below) + " below)");

    // The wordmark and the status line, from assets/fonts: glyphs are the only thing brighter
    // than the backdrop in those two bands.
    expect(count_bright_in_box(f, 0.3f, 0.7f, 0.53f, 0.60f, 60) > 300, "text: the wordmark is drawn");
    expect(count_bright_in_box(f, 0.3f, 0.7f, 0.66f, 0.70f, 60) > 150, "text: the status line is drawn");
}

COOPA_TEST(the_bar_fills_from_the_left_by_the_progress_it_is_given) {
    const glm::ivec3 fill(255, 200, 69);   // toy yellow, the bar's fill colour
    // The bar band only: a narrow strip around the middle of the screen, left half vs right half.
    // The bar is 5 px tall, 96 px below the middle of a 720-row frame: rows 0.629..0.638.
    auto fill_pixels = [&](const Frame& f, float x0, float x1) {
        return count_color_in_box(f, fill, 40, x0, x1, 0.625f, 0.645f);
    };

    SplashHarness splash;
    const Frame half = splash.render("Loading assets...", 0.5f);
    const long long left = fill_pixels(half, 0.25f, 0.5f);
    const long long right = fill_pixels(half, 0.5f, 0.75f);
    expect(left > 100, "bar: half progress fills the left half (" + std::to_string(left) + " px)");
    // Not exactly zero: the fill's right cap is anti-aliased across the half-way column itself.
    expect(right < 20, "bar: half progress leaves the right half empty (" + std::to_string(right) + " px)");

    const Frame done = splash.render("Loading assets...", 1.0f);
    const long long done_right = fill_pixels(done, 0.5f, 0.75f);
    expect(done_right > 100, "bar: full progress reaches the right half (" + std::to_string(done_right) + " px)");
    if (done_right <= 100) dump_frame(done, "boot_splash_full");

    const Frame none = splash.render("Starting renderer...", 0.0f);
    expect(fill_pixels(none, 0.25f, 0.75f) == 0, "bar: zero progress draws no fill at all");
    // ...and the track is still there, a dim line the fill sits in.
    expect(count_bright_in_box(none, 0.3f, 0.7f, 0.625f, 0.645f, 10) > 200, "bar: the empty track is drawn");
}

COOPA_TEST(a_project_can_replace_the_wordmark_and_the_backdrop) {
    toy::core::BootSplashStyle style;
    style.backdrop = glm::vec3(0.6f, 0.1f, 0.1f);   // a project's own (red) branding
    style.title    = "A Game";
    const Frame f = SplashHarness(style).render("Loading scene...", 0.5f);

    const glm::ivec3 top = pixel(f, 4, 4);
    expect(top.r > top.b && top.r > top.g, "style: the backdrop follows splash_color");
    expect(count_bright_in_box(f, 0.3f, 0.7f, 0.53f, 0.60f, 60) > 150,
           "style: the replacement wordmark is drawn");

    // An empty title draws no wordmark, and nothing else moves.
    style.title.clear();
    const Frame bare = SplashHarness(style).render("Loading scene...", 0.5f);
    expect(count_bright_in_box(bare, 0.3f, 0.7f, 0.53f, 0.60f, 60) == 0,
           "style: an empty splash_title draws no wordmark");
    expect(count_color_in_box(bare, glm::ivec3(255, 200, 69), 40, 0.42f, 0.58f, 0.30f, 0.40f) > 200,
           "style: the logo is unaffected by the missing wordmark");
}

COOPA_TEST(an_engine_shows_the_splash_through_startup_and_drops_it_on_the_first_frame) {
    // The window a test owns is never mapped, so TOY_SPLASH=1 is what lets this drive the real
    // path: BootSplash built from Engine's own context, presented into the actual swapchain at
    // each startup milestone, then released. Everything here is a construction-order question
    // the offscreen tests above cannot ask.
    ::setenv("TOY_SPLASH", "1", 1);
    {
        toy::core::Engine engine(
            make_test_config("tests/fixtures/scenes/kitchen_sink/scene.yaml", 320, 180, 160, 90));
        expect(engine.booting(), "engine: the splash is up once the constructor returns");
        // Pumping it from outside is what the editor and the hub do in their own constructors.
        engine.boot_progress("Starting the editor...", 0.9f);
        expect(engine.booting(), "engine: a pump keeps the splash up");

        expect(engine.tick(), "engine: the first frame renders");
        expect(!engine.booting(), "engine: the first rendered frame drops the splash");
        expect(engine.tick(), "engine: the frame after it still renders");
        engine.boot_progress("ignored", 0.5f);   // a no-op once it is gone, not a crash
    }
    ::unsetenv("TOY_SPLASH");
}
