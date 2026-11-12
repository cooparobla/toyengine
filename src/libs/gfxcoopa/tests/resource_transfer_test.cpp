/**
 * @file resource_transfer_test.cpp
 * @brief The sealed resource/command API against a live device with validation layers:
 *        descriptor layout/pool builders, a PipelineDesc pipeline, CommandBuffer's "no pipeline
 *        bound" guard; image transitions + buffer<->image copies preserving bytes in both
 *        barrier directions; and util/image_readback.h's save_image_png() verified through an
 *        independent PNG decoder.
 *
 * These prove the barrier access/stage masks in detail::barrier_masks_for() and the pool sizing
 * in DescriptorPoolBuilder are correct at runtime, not merely type-correct. Raw-Vk wrappers
 * (Buffer::vertex/uniform, Image, Shader, Fence/Semaphore, DescriptorSet) are not tested one by
 * one: every test here and in the other GPU suites constructs them.
 */
#include <coopa/testing/test.h>

#include "support/gpu_fixture.h"

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/util/image_readback.h>

// Declarations only -- STB_IMAGE_IMPLEMENTATION is compiled once in src/gfx_impl.cpp.
#include <stb/stb_image.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

COOPA_TEST_SUITE("resource_transfer");

using namespace coopa::gfx;

COOPA_TEST(descriptor_binding_requires_a_bound_pipeline) {
    gfx_test::GpuFixture gpu;

    memory::Buffer uniform(*gpu.device, *gpu.allocator, 256, BufferUsage::Uniform, MemoryResidency::CpuToGpu);
    ASSERT_TRUE(uniform.handle() != VK_NULL_HANDLE);

    pipeline::DescriptorSetLayout layout =
        pipeline::DescriptorLayoutBuilder()
            .uniform_buffer(0, ShaderStage::Vertex | ShaderStage::Fragment)
            .build(*gpu.device);
    EXPECT_EQ(layout.bindings().size(), 1u);
    pipeline::DescriptorPool pool = pipeline::DescriptorPoolBuilder().add_sets(layout, 1).build(*gpu.device);
    pipeline::DescriptorSet set(*gpu.device, pool, layout);
    set.bind_buffer(0, uniform);

    pipeline::Shader vert(*gpu.device, gfx_test::k_vert_spv, ShaderStage::Vertex);
    pipeline::Shader frag(*gpu.device, gfx_test::k_frag_spv, ShaderStage::Fragment);
    pipeline::PipelineDesc desc;
    desc.shaders = {&vert, &frag};
    desc.descriptor_layouts = {&layout};
    desc.push_constants = {{ShaderStage::Vertex, 0, sizeof(float) * 4}};
    desc.raster.cull = CullMode::None;
    pipeline::Pipeline pipeline(*gpu.device, *gpu.render_pass, desc);
    ASSERT_TRUE(pipeline.handle() != VK_NULL_HANDLE);

    // bind_descriptor_set(set) / push_constants(stage, value) both need the bound pipeline's
    // layout -- the "no pipeline bound" guard throws, then the happy path records cleanly.
    VkCommandBuffer raw = gpu.cmd_pool->begin_single_use();
    command::CommandBuffer cmd(raw);
    bool threw = false;
    try {
        cmd.bind_descriptor_set(set);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);

    cmd.bind_pipeline(pipeline);
    cmd.bind_descriptor_set(set);  // must not throw now
    float push_data[4] = {1, 2, 3, 4};
    cmd.push_constants(ShaderStage::Vertex, push_data);  // template overload
    gpu.cmd_pool->end_single_use(raw, gpu.device->graphics_queue());
}

COOPA_TEST(image_bytes_survive_transitions_and_copies_both_ways) {
    // Uploads a known 4x4 RGBA8 pattern via a staging buffer, transitions to ShaderRead (the
    // steady state a texture lives in), then back to TransferSrc and reads it into a second
    // buffer -- the bytes must survive both barrier directions intact.
    gfx_test::GpuFixture gpu;

    memory::Image image(*gpu.device, *gpu.allocator, 4, 4, Format::RGBA8_Unorm,
                        ImageUsage::Sampled | ImageUsage::TransferDst | ImageUsage::TransferSrc);
    ASSERT_TRUE(image.view_typed().valid());
    EXPECT_TRUE(image.current_usage() == TextureUsage::Undefined);
    EXPECT_EQ(static_cast<int>(image.format_typed()), static_cast<int>(Format::RGBA8_Unorm));

    const uint32_t w = 4, h = 4, byte_size = w * h * 4;
    std::vector<uint8_t> pattern(byte_size);
    for (uint32_t i = 0; i < byte_size; ++i) pattern[i] = static_cast<uint8_t>(i * 7 + 3);

    memory::Buffer staging(*gpu.device, *gpu.allocator, byte_size, BufferUsage::TransferSrc, MemoryResidency::CpuToGpu);
    staging.upload(pattern.data(), byte_size);
    memory::Buffer readback(*gpu.device, *gpu.allocator, byte_size, BufferUsage::TransferDst, MemoryResidency::GpuToCpu);

    TextureUsage mid_usage = TextureUsage::Undefined;
    gpu.cmd_pool->submit_once([&](command::CommandBuffer& cmd) {
        cmd.transition(image, TextureUsage::TransferDst);
        cmd.copy_buffer_to_image(staging, image, Extent2D{w, h});
        cmd.transition(image, TextureUsage::ShaderRead);
        mid_usage = image.current_usage();
        cmd.transition(image, TextureUsage::TransferSrc);
        cmd.copy_image_to_buffer(image, readback, Extent2D{w, h});
    });
    EXPECT_TRUE(mid_usage == TextureUsage::ShaderRead);

    std::vector<uint8_t> got(byte_size);
    readback.download(got.data(), byte_size);
    EXPECT_TRUE(got == pattern);
}

COOPA_TEST(saved_png_matches_the_clear_color) {
    // Clears an offscreen image to a known colour, writes it via save_image_png(), and reads the
    // PNG back with stb_image -- a completely separate code path from the write side.
    gfx_test::GpuFixture gpu;

    // ColorAttachment is included because memory::Image unconditionally creates a VkImageView,
    // and the Vulkan spec requires at least one view-compatible usage bit -- a real render
    // target being screenshotted always has this anyway.
    memory::Image target(*gpu.device, *gpu.allocator, 8, 8, Format::RGBA8_Unorm,
                         ImageUsage::ColorAttachment | ImageUsage::TransferSrc | ImageUsage::TransferDst);

    ClearColor clear{0.25f, 0.5f, 0.75f, 1.0f};
    gpu.cmd_pool->submit_once([&](command::CommandBuffer& cmd) {
        cmd.transition(target, TextureUsage::TransferDst);
        cmd.clear_color(target, clear);
    });
    ASSERT_TRUE(target.current_usage() == TextureUsage::TransferDst);

    const std::string out_path = (coopa::test::scratch_dir() / "readback.png").string();
    util::save_image_png(*gpu.device, *gpu.allocator, *gpu.cmd_pool, target, out_path);

    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(out_path.c_str(), &w, &h, &channels, 4);
    ASSERT_TRUE(pixels != nullptr);
    EXPECT_EQ(w, 8);
    EXPECT_EQ(h, 8);

    auto to_u8 = [](float f) { return static_cast<int>(f * 255.0f + 0.5f); };
    const int er = to_u8(clear.r), eg = to_u8(clear.g), eb = to_u8(clear.b);
    int mismatched = 0;
    for (int i = 0; i < w * h; ++i) {
        int r = pixels[i * 4 + 0], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2];
        // +/-2 tolerance for float->unorm8 rounding through the GPU clear.
        if (std::abs(r - er) > 2 || std::abs(g - eg) > 2 || std::abs(b - eb) > 2) ++mismatched;
    }
    stbi_image_free(pixels);
    EXPECT_EQ(mismatched, 0);

    // read_image()'s "restore original usage" behaviour: TransferDst is restorable, so after
    // the read inside save_image_png() the image is back in TransferDst, not stranded in
    // TransferSrc.
    EXPECT_TRUE(target.current_usage() == TextureUsage::TransferDst);
}
