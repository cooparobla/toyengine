/**
 * @file compute_test.cpp
 * @brief pipeline/compute_pipeline.h + CommandBuffer compute recording: the device's compute
 *        limits meet the Vulkan spec minimums, a direct dispatch and a GPU-written indirect
 *        dispatch produce exactly the expected buffers, and a compute dispatch writes a
 *        compute_writable Mesh's vertex slot in place (the GPU-skinning shape), plus
 *        StorageBufferRing's slot arithmetic.
 */
#include <coopa/testing/test.h>

#include "support/gpu_fixture.h"

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/memory/storage_buffer.h>
#include <gfxcoopa/pipeline/compute_pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>

#include <iostream>
#include <vector>

COOPA_TEST_SUITE("compute");

using namespace coopa::gfx;

namespace {
/** @brief The push block both test kernels (test_compute_fill / test_compute_args) take. */
struct ComputeTestPush { uint32_t a; uint32_t b; };
}  // namespace

COOPA_TEST(device_compute_limits_meet_spec_minimums) {
    gfx_test::GpuFixture gpu;
    const auto& caps = gpu.device->compute_caps();
    EXPECT_TRUE(gpu.device->supports_compute());
    EXPECT_TRUE(caps.graphics_queue_compute);
    // Spec minimums (Vulkan 1.0 required limits) -- anything lower means the query is broken.
    EXPECT_GE(caps.max_storage_buffer_range, (1u << 27));
    EXPECT_GE(caps.max_per_stage_storage_buffers, 4u);
    EXPECT_GE(caps.max_work_group_invocations, 128u);
    EXPECT_GE(caps.max_work_group_size[0], 128u);
    EXPECT_GE(caps.max_work_group_count[0], 65535u);
    if (coopa::test::verbose()) {
        std::cout << "  storage range " << caps.max_storage_buffer_range
                  << ", per-stage SSBOs " << caps.max_per_stage_storage_buffers
                  << ", per-layout SSBOs " << caps.max_set_storage_buffers
                  << ", invocations " << caps.max_work_group_invocations
                  << ", shared " << caps.max_shared_memory
                  << ", ssbo align " << caps.min_storage_buffer_offset_alignment << std::endl;
    }
}

COOPA_TEST(direct_and_gpu_written_indirect_dispatches_fill_expected_values) {
    // Dispatches a buffer fill; then writes indirect args from a compute pass and runs a
    // dispatch_indirect() that consumes them; reads all three buffers back and checks them.
    gfx_test::GpuFixture gpu;

    auto layout = pipeline::DescriptorLayoutBuilder().storage_buffer(0, ShaderStage::Compute).build(*gpu.device);
    auto pool   = pipeline::DescriptorPoolBuilder().add_sets(layout, 3).build(*gpu.device);
    const std::vector<pipeline::PushConstantRange> push = {{ShaderStage::Compute, 0, sizeof(ComputeTestPush)}};
    pipeline::ComputePipeline fill(*gpu.device, gfx_test::k_fill_comp_spv, {&layout}, push);
    pipeline::ComputePipeline args_writer(*gpu.device, gfx_test::k_args_comp_spv, {&layout}, push);

    const uint32_t kCount = 1000, kLocal = 64, kIndirectItems = 150;  // 150 items -> 3 groups
    const uint64_t bytes = kCount * sizeof(uint32_t);
    auto direct   = memory::make_storage_buffer(*gpu.device, *gpu.allocator, bytes);
    auto indirect = memory::make_storage_buffer(*gpu.device, *gpu.allocator, bytes);
    auto args     = memory::make_storage_buffer(*gpu.device, *gpu.allocator, 8 * sizeof(uint32_t), BufferUsage::Indirect);
    auto readback = memory::Buffer(*gpu.device, *gpu.allocator, 2 * bytes + 8 * sizeof(uint32_t),
                                   BufferUsage::TransferDst, MemoryResidency::GpuToCpu);

    pipeline::DescriptorSet set_direct(*gpu.device, pool, layout), set_indirect(*gpu.device, pool, layout),
                            set_args(*gpu.device, pool, layout);
    set_direct.bind_storage_buffer(0, direct);
    set_indirect.bind_storage_buffer(0, indirect);
    set_args.bind_storage_buffer(0, args);

    gpu.cmd_pool->submit_once([&](command::CommandBuffer& cmd) {
        cmd.fill_buffer(indirect, 0xFFFFFFFFu);   // sentinel: anything the indirect dispatch skips
        cmd.fill_buffer(args, 0u);
        cmd.transfer_to_compute_barrier();

        // 1. A plain dispatch fills `direct`.
        cmd.bind_pipeline(fill);
        cmd.bind_descriptor_set(set_direct);
        cmd.push_constants(ShaderStage::Compute, ComputeTestPush{kCount, 7u});
        cmd.dispatch(pipeline::ComputePipeline::groups_for(kCount, kLocal));

        // 2. A compute pass writes the group count; dispatch_indirect() consumes it. The fill
        //    kernel is told the whole buffer is valid, so only the indirect group count limits
        //    how much of `indirect` gets written.
        cmd.bind_pipeline(args_writer);
        cmd.bind_descriptor_set(set_args);
        cmd.push_constants(ShaderStage::Compute, ComputeTestPush{kIndirectItems, kLocal});
        cmd.dispatch(1);
        cmd.compute_to_compute_barrier();

        cmd.bind_pipeline(fill);
        cmd.bind_descriptor_set(set_indirect);
        cmd.push_constants(ShaderStage::Compute, ComputeTestPush{kCount, 100u});
        cmd.dispatch_indirect(args);

        cmd.buffer_barrier(BufferAccess::ComputeWrite, BufferAccess::TransferRead);
        cmd.copy_buffer(direct, readback, bytes, 0, 0);
        cmd.copy_buffer(indirect, readback, bytes, 0, bytes);
        cmd.copy_buffer(args, readback, 8 * sizeof(uint32_t), 0, 2 * bytes);
        cmd.buffer_barrier(BufferAccess::TransferWrite, BufferAccess::HostRead);
    });

    std::vector<uint32_t> out(2 * kCount + 8);
    readback.download(out.data(), out.size() * sizeof(uint32_t));

    uint32_t direct_bad = 0, indirect_bad = 0;
    for (uint32_t i = 0; i < kCount; ++i) {
        if (out[i] != i * 3u + 7u) ++direct_bad;
        const uint32_t want = i < 3u * kLocal ? i * 3u + 100u : 0xFFFFFFFFu;
        if (out[kCount + i] != want) ++indirect_bad;
    }
    EXPECT_EQ(direct_bad, 0u);
    EXPECT_EQ(indirect_bad, 0u);
    const uint32_t* a = out.data() + 2 * kCount;
    EXPECT_EQ(a[0], 3u);
    EXPECT_EQ(a[1], 1u);
    EXPECT_EQ(a[2], 1u);
    EXPECT_EQ(a[4], kIndirectItems);   // the draw-indirect command written beside it
    EXPECT_EQ(a[5], 1u);
}

COOPA_TEST(dispatch_writes_a_mesh_vertex_slot_in_place) {
    // The GPU-skinning shape: a compute_writable dynamic Mesh's per-slot vertex buffer written
    // in place by a dispatch, then read back through its host mapping. Plus StorageBufferRing's
    // slot arithmetic.
    gfx_test::GpuFixture gpu;
    using engine::data::Mesh;
    using engine::data::Vertex;

    std::vector<Vertex> verts(37, Vertex{glm::vec3(0.0f), glm::vec3(0, 1, 0), glm::vec2(0.0f), glm::vec4(1, 0, 0, 1)});
    std::vector<uint32_t> indices = {0, 1, 2};
    Mesh mesh = Mesh::from_arrays(*gpu.device, *gpu.allocator, verts, indices, 2, /*compute_writable=*/true);
    ASSERT_EQ(mesh.vertex_buffer_count(), 2u);

    auto layout = pipeline::DescriptorLayoutBuilder().storage_buffer(0, ShaderStage::Compute).build(*gpu.device);
    auto pool   = pipeline::DescriptorPoolBuilder().add_sets(layout, 1).build(*gpu.device);
    pipeline::ComputePipeline fill(*gpu.device, gfx_test::k_fill_comp_spv, {&layout},
                                   {{ShaderStage::Compute, 0, sizeof(ComputeTestPush)}});
    pipeline::DescriptorSet set(*gpu.device, pool, layout);
    set.bind_storage_buffer(0, mesh.vertex_buffer(1));

    const uint32_t words = static_cast<uint32_t>(verts.size() * sizeof(Vertex) / sizeof(uint32_t));
    gpu.cmd_pool->submit_once([&](command::CommandBuffer& cmd) {
        cmd.bind_pipeline(fill);
        cmd.bind_descriptor_set(set);
        cmd.push_constants(ShaderStage::Compute, ComputeTestPush{words, 5u});
        cmd.dispatch(pipeline::ComputePipeline::groups_for(words, 64));
        cmd.compute_to_draw_barrier();
        cmd.buffer_barrier(BufferAccess::ComputeWrite, BufferAccess::HostRead);
    });
    mesh.mark_gpu_written(1, glm::vec3(-1.0f), glm::vec3(1.0f));
    EXPECT_EQ(mesh.active_slot(), 1u);
    EXPECT_TRUE(mesh.bounds_max() == glm::vec3(1.0f));

    std::vector<uint32_t> got(words);
    mesh.vertex_buffer(1).download(got.data(), words * sizeof(uint32_t));
    uint32_t bad = 0;
    for (uint32_t i = 0; i < words; ++i) if (got[i] != i * 3u + 5u) ++bad;
    EXPECT_EQ(bad, 0u);

    // Slot 0 was never dispatched to: it still holds the seeded vertices.
    Vertex v0{};
    mesh.vertex_buffer(0).download(&v0, sizeof(Vertex));
    EXPECT_TRUE(v0.normal == glm::vec3(0, 1, 0));

    memory::StorageBufferRing ring(*gpu.device, *gpu.allocator, 64, 2, BufferUsage::None, MemoryResidency::CpuToGpu);
    EXPECT_EQ(ring.slot_count(), 2u);
    EXPECT_TRUE(&ring.current(0) == &ring.current(2));
    EXPECT_TRUE(&ring.previous(1) == &ring.current(0));
    EXPECT_TRUE(&ring.previous(0) == &ring.current(1));
    const uint32_t word = 0xC0FFEEu;
    ring.upload(1, &word, sizeof(word));
    uint32_t back = 0;
    ring.current(1).download(&back, sizeof(back));
    EXPECT_EQ(back, word);
}
