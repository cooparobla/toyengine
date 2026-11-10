#pragma once

/**
 * @file gpu_fixture.h
 * @brief The Vulkan bring-up gfxcoopa's GPU suites draw on: a hidden window, an instance with
 *        validation layers, surface, device, allocator, swapchain, command pool and a swapchain
 *        render pass -- plus the paths of the test shaders (assets/shaders/test*).
 *
 * Each GPU test builds its own GpuFixture (RAII, torn down in reverse order at the end of the
 * test), so no test can inherit device state another test left behind and nothing Vulkan
 * outlives main(). Bring-up is cheap next to the GPU work, and ctest runs each suite as its own
 * process in parallel anyway.
 *
 * The window is ALWAYS hidden: a hidden GLFW window still backs a working surface and
 * swapchain, so these suites put nothing on screen regardless of the environment (the old
 * monolithic test only hid it when HEADLESS=1 was set).
 */

#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/instance.h>
#include <gfxcoopa/core/surface.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/presentation/window.h>

#include <memory>

namespace gfx_test {

// Shader .spv paths: next to the sources (relative to the repo root, the suites' working
// directory -- see CMakeLists.txt), or where a build that sets GFX_SHADER_OUTPUT_ROOT compiled
// them (see cmake/GfxShaders.cmake). test.vert hard-codes a triangle and takes no vertex input.
#ifdef GFX_TEST_SHADER_DIR
inline constexpr const char* k_vert_spv      = GFX_TEST_SHADER_DIR "/test.vert.spv";
inline constexpr const char* k_frag_spv      = GFX_TEST_SHADER_DIR "/test.frag.spv";
inline constexpr const char* k_fill_comp_spv = GFX_TEST_SHADER_DIR "/test_compute_fill.comp.spv";
inline constexpr const char* k_args_comp_spv = GFX_TEST_SHADER_DIR "/test_compute_args.comp.spv";
#else
inline constexpr const char* k_vert_spv      = "assets/shaders/test.vert.spv";
inline constexpr const char* k_frag_spv      = "assets/shaders/test.frag.spv";
inline constexpr const char* k_fill_comp_spv = "assets/shaders/test_compute_fill.comp.spv";
inline constexpr const char* k_args_comp_spv = "assets/shaders/test_compute_args.comp.spv";
#endif

/** @brief A complete hidden-window Vulkan bring-up; members destroy in reverse order. */
struct GpuFixture {
    std::unique_ptr<coopa::gfx::presentation::Window> window;
    std::unique_ptr<coopa::gfx::core::Instance>       instance;
    std::unique_ptr<coopa::gfx::core::Surface>        surface;
    std::unique_ptr<coopa::gfx::core::Device>         device;
    std::unique_ptr<coopa::gfx::memory::Allocator>    allocator;
    std::unique_ptr<coopa::gfx::core::Swapchain>      swapchain;
    std::unique_ptr<coopa::gfx::command::CommandPool> cmd_pool;
    std::unique_ptr<coopa::gfx::pipeline::RenderPass> render_pass;

    GpuFixture() {
        using namespace coopa::gfx;
        window    = std::make_unique<presentation::Window>("gfxcoopa test", 800, 600, /*resizable=*/false,
                                                           /*visible=*/false);
        instance  = std::make_unique<core::Instance>("gfxcoopa_test", /*validation=*/true);
        surface   = std::make_unique<core::Surface>(*instance, *window);
        device    = std::make_unique<core::Device>(*instance, *surface);
        allocator = std::make_unique<memory::Allocator>(*instance, *device);
        auto [w, h] = window->framebuffer_size();
        swapchain   = std::make_unique<core::Swapchain>(*device, *surface, w, h, /*vsync=*/true);
        cmd_pool    = std::make_unique<command::CommandPool>(*device, device->graphics_family());
        render_pass = std::make_unique<pipeline::RenderPass>(*device, swapchain->image_format(), VK_FORMAT_UNDEFINED);
    }

    ~GpuFixture() {
        if (device) device->wait_idle();
        render_pass.reset();
        cmd_pool.reset();
        swapchain.reset();
        allocator.reset();
        device.reset();
        surface.reset();
        instance.reset();
        window.reset();
    }

    GpuFixture(const GpuFixture&)            = delete;
    GpuFixture& operator=(const GpuFixture&) = delete;
};

}  // namespace gfx_test
