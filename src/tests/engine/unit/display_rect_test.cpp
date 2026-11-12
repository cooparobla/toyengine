/**
 * @file display_rect_test.cpp
 * @brief toyengine/render/toy_render_math.h's presentation maths: how the internal render extent is
 *        chosen (fixed / divisor) and how it is fitted into the window (integer letterbox vs
 *        fractional fit). Exact integer answers.
 */

#include <coopa/testing/test.h>

#include <toyengine/render/toy_render_math.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("display_rect");

COOPA_TEST(letterbox_uses_the_largest_integer_scale_and_centres) {
    {
        // 1920x1080 window, 480x270 buffer -> scale 4, exact fit, no bars.
        auto rect = toy::render::compute_letterbox(1920, 1080, 480, 270);
        expect(rect.scale == 4.0f, "letterbox: 1920x1080 / 480x270 -> scale 4");
        expect(rect.w == 1920 && rect.h == 1080, "letterbox: 1920x1080 / 480x270 -> exact fit");
        expect(rect.x == 0 && rect.y == 0, "letterbox: 1920x1080 / 480x270 -> no offset");
    }
    {
        // 1600x900 window, 480x270 buffer -> scale 3, 1440x810, centred with bars.
        auto rect = toy::render::compute_letterbox(1600, 900, 480, 270);
        expect(rect.scale == 3.0f, "letterbox: 1600x900 / 480x270 -> scale 3");
        expect(rect.w == 1440 && rect.h == 810, "letterbox: 1600x900 / 480x270 -> 1440x810");
        expect(rect.x == 80 && rect.y == 45, "letterbox: 1600x900 / 480x270 -> centred at (80,45)");
    }
    {
        auto rect = toy::render::compute_letterbox(100, 100, 480, 270);
        expect(rect.scale == 1.0f, "letterbox: window smaller than buffer -> scale clamps to 1");
    }
}

COOPA_TEST(fit_uses_a_fractional_scale_and_bars_one_axis_only) {
    {
        // 1920x1080 window, 720x480 buffer (3:2 into 16:9) -> scale 2.25, 1620x1080,
        // pillarboxed left/right only -- the exact case a floored integer scale (2 ->
        // 1440x960) would letterbox on all four sides instead.
        auto rect = toy::render::compute_fit(1920, 1080, 720, 480);
        expect_near(rect.scale, 2.25f, 1e-6f, "fit: 1920x1080 / 720x480 -> scale 2.25");
        expect(rect.w == 1620 && rect.h == 1080, "fit: 1920x1080 / 720x480 -> 1620x1080");
        expect(rect.x == 150 && rect.y == 0, "fit: 1920x1080 / 720x480 -> pillarboxed at (150,0), no top/bottom bars");
    }
    {
        // Matching aspect (16:9 into 16:9) -> fills the window exactly, same as integer mode.
        auto rect = toy::render::compute_fit(1920, 1080, 480, 270);
        expect(rect.w == 1920 && rect.h == 1080 && rect.x == 0 && rect.y == 0,
              "fit: 1920x1080 / 480x270 -> exact fill, no bars");
    }
    {
        // Same input compute_letterbox's test_letterbox_with_bars uses (scale 3 -> 1440x810,
        // 80/45px bars) -- fit uses the full fractional scale (3.333) and fills completely.
        auto rect = toy::render::compute_fit(1600, 900, 480, 270);
        expect(rect.w == 1600 && rect.h == 900 && rect.x == 0 && rect.y == 0,
              "fit: 1600x900 / 480x270 -> fills completely, unlike integer mode's 1440x810");
    }
    {
        auto rect = toy::render::compute_fit(100, 100, 480, 270);
        expect(rect.w == 100 && rect.h == 56, "fit: window smaller than buffer -> scales down, fills width");
        expect(rect.x == 0 && rect.y == 22, "fit: window smaller than buffer -> letterboxed top/bottom at (0,22)");
    }
}

COOPA_TEST(display_rect_dispatches_on_upscale_mode) {
    toy::render::ToyRenderConfig integer_cfg;
    integer_cfg.upscale_mode = "integer";
    auto integer_rect = toy::render::compute_display_rect(integer_cfg, 1920, 1080, 720, 480);
    expect(integer_rect.w == 1440 && integer_rect.h == 960,
          "display_rect: upscale_mode=integer dispatches to compute_letterbox");

    toy::render::ToyRenderConfig fit_cfg;
    fit_cfg.upscale_mode = "fit";
    auto fit_rect = toy::render::compute_display_rect(fit_cfg, 1920, 1080, 720, 480);
    expect(fit_rect.w == 1620 && fit_rect.h == 1080,
          "display_rect: upscale_mode=fit dispatches to compute_fit");
}

COOPA_TEST(render_extent_follows_fixed_and_divisor_modes) {
    {
        toy::render::ToyRenderConfig cfg;
        cfg.resolution_mode = "fixed";
        cfg.render_width = 480;
        cfg.render_height = 270;
        toy::render::RenderExtent e = toy::render::compute_render_extent(cfg, 1920, 1080);
        expect(e.width == 480 && e.height == 270, "render_resolution: fixed mode ignores swapchain size");
    }
    {
        toy::render::ToyRenderConfig cfg;
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
}
