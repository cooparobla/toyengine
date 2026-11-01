/**
 * @file boot_splash_test.cpp
 * @brief The boot splash as pixels: BootSplash builds its pipeline from uicoopa's shaders and
 *        draws the same background-fill-and-cycling-mark screen a scene load's own cover does
 *        (see loading_screen.h).
 *
 * Rendered into an offscreen RGBA8 target rather than a swapchain: a swapchain image is created
 * COLOR_ATTACHMENT-only and cannot be read back at all, and the splash's real target is exactly
 * a colour attachment with the UI pass over it either way.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <string>

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

/** @brief Pixels in the box (fractions of the frame) noticeably brighter than `bg`. */
long long count_bright_in_box(const Frame& f, glm::ivec3 bg, int over,
                              float x0, float x1, float y0, float y1) {
    long long n = 0;
    const uint32_t xa = static_cast<uint32_t>(x0 * f.width), xb = static_cast<uint32_t>(x1 * f.width);
    const uint32_t ya = static_cast<uint32_t>(y0 * f.height), yb = static_cast<uint32_t>(y1 * f.height);
    for (uint32_t y = ya; y < yb && y < f.height; ++y) {
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
                  std::move(style)) {}

    ~SplashHarness() { ctx_.wait_idle(); }

    /** @brief Renders one splash frame into the target and reads it back. */
    Frame render() {
        // prepare() before any recording, exactly as the contract demands: it writes descriptor
        // sets and may regrow the geometry buffers, neither of which is legal inside a pass.
        splash_.prepare(0, kW, kH);
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

COOPA_TEST(the_splash_draws_the_background_and_the_cycling_mark) {
    SplashHarness splash;
    const Frame f = splash.render();
    dump_frame(f, "boot_splash");   // always: this screen is gone before a session can be paused
    expect(f.width == kW && f.height == kH, "splash: the frame came back at the target's size");

    // Background: a flat fill everywhere except the mark's own corner. The default
    // loading.background is itself pure black, so black_block_pixels() (elsewhere a signature of
    // a pipeline that never bound) cannot distinguish this intentional fill from nothing drawn --
    // skip it here and rely on the mark-presence checks below instead.
    const glm::ivec3 top_left = pixel(f, 4, 4);
    expect(top_left == glm::ivec3(0, 0, 0), "background: the default loading colour is pure black");

    // The mark: toyengine's default corner is LoadingCorner::BottomRight.
    const long long bright = count_bright_in_box(f, top_left, 60, 0.70f, 1.0f, 0.70f, 1.0f);
    expect(bright > 200, "mark: the cycling block is drawn in the bottom-right corner (" +
                             std::to_string(bright) + " px)");
    // Nowhere else -- the mark does not bleed over the whole screen.
    expect(count_bright_in_box(f, top_left, 60, 0.0f, 0.3f, 0.0f, 0.3f) == 0,
           "mark: the opposite (top-left) corner stays background-only");
}

COOPA_TEST(a_project_can_replace_the_background_and_the_mark_colour) {
    toy::core::BootSplashStyle style;
    style.loading.background = glm::vec3(0.6f, 0.1f, 0.1f);   // a project's own (red) branding
    style.loading.spinner_color = glm::vec3(0.1f, 0.1f, 1.0f);
    const Frame f = SplashHarness(style).render();

    const glm::ivec3 top_left = pixel(f, 4, 4);
    expect(top_left.r > top_left.b && top_left.r > top_left.g,
           "style: the background follows loading.background");

    const long long bright = count_bright_in_box(f, top_left, 40, 0.70f, 1.0f, 0.70f, 1.0f);
    expect(bright > 200, "style: the mark still draws over the replacement background");

    // spinner: false draws no mark at all -- only the background fill remains.
    style.loading.spinner = false;
    const Frame bare = SplashHarness(style).render();
    expect(count_bright_in_box(bare, pixel(bare, 4, 4), 40, 0.70f, 1.0f, 0.70f, 1.0f) == 0,
           "style: loading.spinner = false draws no mark");
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
        engine.boot_frame();
        expect(engine.booting(), "engine: a pump keeps the splash up");

        expect(engine.tick(), "engine: the first frame renders");
        expect(!engine.booting(), "engine: the first rendered frame drops the splash");
        expect(engine.tick(), "engine: the frame after it still renders");
        engine.boot_frame();   // a no-op once it is gone, not a crash
    }
    ::unsetenv("TOY_SPLASH");
}
