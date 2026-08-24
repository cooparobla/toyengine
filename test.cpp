// toyengine test suite -- registered with ctest via CMakeLists.txt's
// enable_testing()/add_test(). Volk/VMA implementations are compiled once in
// this translation unit, mirroring main.cpp (both are single-TU executables;
// see CMakeLists.txt's add_definitions() comment).
#include <volk/volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>

// See main.cpp's identical comment: each translation unit that needs
// stb_image.h/stb_image_write.h's implementation must trigger it exactly
// once, before anything else in this TU includes them undefined.
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>

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
    expect(rect.scale == 4, "letterbox: 1920x1080 / 480x270 -> scale 4");
    expect(rect.w == 1920 && rect.h == 1080, "letterbox: 1920x1080 / 480x270 -> exact fit");
    expect(rect.x == 0 && rect.y == 0, "letterbox: 1920x1080 / 480x270 -> no offset");
}

void test_letterbox_with_bars() {
    // 1600x900 window, 480x270 buffer -> scale 3, 1440x810, centred with bars.
    auto rect = toy::render::compute_letterbox(1600, 900, 480, 270);
    expect(rect.scale == 3, "letterbox: 1600x900 / 480x270 -> scale 3");
    expect(rect.w == 1440 && rect.h == 810, "letterbox: 1600x900 / 480x270 -> 1440x810");
    expect(rect.x == 80 && rect.y == 45, "letterbox: 1600x900 / 480x270 -> centred at (80,45)");
}

void test_letterbox_undersized_window_clamps_to_scale_1() {
    auto rect = toy::render::compute_letterbox(100, 100, 480, 270);
    expect(rect.scale == 1, "letterbox: window smaller than buffer -> scale clamps to 1");
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

/**
 * @brief Full headless render through a real Vulkan device: constructs an
 * Engine against the demo scene with a palette configured, ticks it a few
 * frames, and asserts the saved low-res buffer is exactly the configured
 * dimensions and every pixel is a member of the palette. This one assertion
 * validates the render path, the outline/dither/quantize post pass, and the
 * UNORM/gamma choice all at once -- see pixel_post_pass.h's file doc.
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
 * (outline, palette, dither, soft_shadows, ssao, ssr), mirroring
 * test_headless_render_matches_palette() but exercising the opposite branch of
 * every toggle at once: SsaoPass::invalidate_history() instead of execute(),
 * no HiZPass/SceneColorMipPass/SsrPass construction at all, the manual
 * gbuffer-depth transition instead of HiZPass's, and pixel_post_pass_ reading
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
    config.render.soft_shadows    = false;
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

} // namespace

int main() {
    test_letterbox_exact_fit();
    test_letterbox_with_bars();
    test_letterbox_undersized_window_clamps_to_scale_1();
    test_render_resolution_fixed_mode();
    test_render_resolution_divisor_mode();
    test_pixel_density_orthographic();
    test_pixel_density_perspective_disabled();
    test_headless_render_matches_palette();
    test_headless_render_with_all_toggles_off();

    if (g_failures > 0) {
        std::cerr << "\n" << g_failures << " test(s) failed.\n";
        return 1;
    }
    std::cout << "\nAll tests passed.\n";
    return 0;
}
