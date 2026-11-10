/**
 * @file presentation_test.cpp
 * @brief The frame cycle on a hidden window: app::Context's full bring-up and a bounded run()
 *        loop (the end-to-end smoke for the whole stack), presentation::Renderer drawing the
 *        test triangle into the swapchain, and Swapchain::recreate()'s destroy/create cycle.
 *
 * Not covered one by one: Window/Instance/Surface/Device/Swapchain/RenderPass/CommandPool
 * construction -- GpuFixture builds all of them for every GPU test.
 */
#include <coopa/testing/test.h>

#include "support/gpu_fixture.h"

#include <gfxcoopa/app/context.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/presentation/renderer.h>

COOPA_TEST_SUITE("presentation");

using namespace coopa::gfx;

COOPA_TEST(context_brings_up_and_runs_a_bounded_frame_loop) {
    // Context owns its own Window/Instance/Surface/Device/Allocator/Swapchain/CommandPool/
    // RenderPass/Renderer, entirely separate from GpuFixture.
    app::ContextConfig config;
    config.title      = "gfxcoopa_context_test";
    config.width      = 320;
    config.height     = 240;
    config.validation = true;
    config.max_frames = 3;      // bound the loop regardless of ONESHOT/MAX_FRAMES env
    config.visible    = false;  // never put a window on screen from a test

    app::Context ctx(config);

#ifdef __APPLE__
    // Retina: the swapchain is sized in framebuffer pixels, an integer multiple
    // (the backing scale factor) of the 320x240 window points requested.
    EXPECT_TRUE(ctx.extent().width % 320 == 0 && ctx.extent().width >= 320);
    EXPECT_TRUE(ctx.extent().height * 320 == ctx.extent().width * 240);
#else
    EXPECT_EQ(ctx.extent().width, 320u);
    EXPECT_EQ(ctx.extent().height, 240u);
#endif
    EXPECT_TRUE(ctx.color_format() != Format::Undefined);
    EXPECT_EQ(ctx.frames_in_flight(), presentation::MAX_FRAMES_IN_FLIGHT);
    EXPECT_FALSE(ctx.should_close());

    uint32_t frames_recorded = 0;
    float last_dt = -1.0f;
    ctx.run(
        [&](float dt) { last_dt = dt; },
        app::FrameCallbacks{
            [&](command::CommandBuffer& cmd) { (void)cmd; ++frames_recorded; },
            nullptr,
            nullptr,
            ClearColor{0.1f, 0.2f, 0.3f, 1.0f},
        });

    EXPECT_EQ(frames_recorded, 3u);
    EXPECT_EQ(ctx.frame_index(), 3u);  // poll() calls time_.update() once per iteration
    EXPECT_GE(last_dt, 0.0f);

    // A single frame() call driven manually (not via run()) should also work.
    ctx.poll();
    EXPECT_TRUE(ctx.frame([](command::CommandBuffer&) {}));
}

COOPA_TEST(renderer_draws_the_test_triangle_into_the_swapchain) {
    gfx_test::GpuFixture gpu;
    pipeline::Shader vert(*gpu.device, gfx_test::k_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
    pipeline::Shader frag(*gpu.device, gfx_test::k_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
    // No vertex input bindings -- the vertex shader hard-codes the triangle.
    pipeline::Pipeline pipeline(*gpu.device, *gpu.render_pass, {&vert, &frag}, {}, {});
    ASSERT_TRUE(pipeline.handle() != VK_NULL_HANDLE);

    presentation::Renderer renderer(*gpu.device, *gpu.swapchain, *gpu.render_pass, *gpu.cmd_pool);
    VkExtent2D ext = gpu.swapchain->extent();
    // begin_frame() returns false only for a minimized/zero-size window, which a headless run
    // may legitimately report -- so its result is not asserted; recording and submitting the
    // draw without a throw or a device loss is the check.
    (void)renderer.begin_frame([&](command::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipeline);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(ext.width), static_cast<float>(ext.height));
        cmd.set_scissor(0, 0, ext.width, ext.height);
        cmd.draw(3);
    });
    gpu.device->wait_idle();
}

COOPA_TEST(swapchain_recreates_at_the_same_size) {
    gfx_test::GpuFixture gpu;
    auto [w, h] = gpu.window->framebuffer_size();
    gpu.swapchain->recreate(w, h);  // validates the destroy/create cycle
    EXPECT_TRUE(gpu.swapchain->handle() != VK_NULL_HANDLE);
    EXPECT_GT(gpu.swapchain->image_count(), 0u);
    EXPECT_EQ(gpu.swapchain->extent().width, w);
    EXPECT_EQ(gpu.swapchain->extent().height, h);
}
