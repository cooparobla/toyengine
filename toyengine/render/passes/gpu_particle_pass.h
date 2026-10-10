/**
 * @file gpu_particle_pass.h
 * @brief `simulation: gpu` particle systems: per-system storage buffers and the compute passes
 *        that emit, simulate, compact and sort them, recorded at the start of the frame; the
 *        quads then draw through ParticlePass's own pipeline with draw_indirect().
 *
 * ## Per system (keyed by GpuParticleJob::id; sized once by `max_particles`)
 *
 * - `particles`: the pool, one 80-byte Particle per slot (a free slot has life <= 0).
 * - `lists`: a 32-byte header -- a VkDrawIndirectCommand {6, alive, 0, 0} whose instance count
 *   IS the alive list's atomic counter, plus the free list's count -- then the free list.
 * - `instances`: the alive list as toy::render::ParticleInstance, bound as ParticlePass's
 *   instance-rate vertex buffer, so particle.vert / particle.frag are untouched (soft fade, HDR
 *   glow, lighting, shadows, atlas, the TAA reactive mask all just work).
 * - Sorted (blended) systems also have `unsorted` instances and (key, index) pairs padded to a
 *   power of two >= 1024.
 * - Per frame slot: the params buffer (GpuParticleParams, CPU-written), the descriptor set and
 *   a 32-byte readback of the header (the alive count Engine feeds back to the system).
 *
 * ## One frame (record(), outside any render pass, before the shadow pass)
 *
 *   barrier (last frame's draws / copies -> this frame's writes)
 *   fill instance_count = 0 (and the sort keys = ~0)            -> transfer->compute barrier
 *   init (new or reset systems)                                  -> barrier
 *   simulate: every slot; deaths push the free list, survivors append to the alive list
 *   emit: births pop the free list and append                   -> barrier
 *   bitonic sort (sorted systems only), then gather in key order -> compute->draw barrier
 *   copy each header to this slot's readback
 *
 * Every dispatch of a phase is recorded for all systems before the phase's single barrier.
 *
 * ## Determinism
 *
 * Births draw from per-particle PCG hashes of (job seed, birth index), and a particle's values
 * never depend on which slot it landed in, so the set of particles is deterministic. Their order
 * in the alive list (atomics) is not: sorted systems are re-ordered by key, additive ones are
 * order-independent up to blend rounding.
 */

#ifndef TOYENGINE_RENDER_PASSES_GPU_PARTICLE_PASS_H
#define TOYENGINE_RENDER_PASSES_GPU_PARTICLE_PASS_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/memory/storage_buffer.h>
#include <gfxcoopa/pipeline/compute_pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/types/enums.h>

#include <toyengine/render/particle_types.h>

namespace toy {
namespace render {
namespace passes {

class GpuParticlePass {
public:
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;
    static constexpr uint32_t kHeaderBytes = 32;
    static constexpr uint32_t kParticleBytes = 80;

    struct Push { uint32_t mode = 0, k = 0, j = 0, pad = 0; };

    /** @brief `shader_path` maps a compute shader's name (particles_emit.comp) to its .spv. */
    template <typename ShaderPathFn>
    GpuParticlePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                    const ShaderPathFn& shader_path)
        : device_(device), allocator_(allocator)
    {
        using namespace coopa::gfx;
        pipeline::DescriptorLayoutBuilder b;
        for (uint32_t i = 0; i < 7; ++i) b.storage_buffer(i, ShaderStage::Compute);
        layout_ = std::make_unique<pipeline::DescriptorSetLayout>(b.build(device));
        const std::vector<pipeline::PushConstantRange> push = {{ShaderStage::Compute, 0, sizeof(Push)}};
        auto make = [&](const char* name) {
            return std::make_unique<pipeline::ComputePipeline>(device, shader_path(name),
                std::vector<const pipeline::DescriptorSetLayout*>{layout_.get()}, push);
        };
        init_ = make("particles_init.comp");
        simulate_ = make("particles_simulate.comp");
        emit_ = make("particles_emit.comp");
        sort_ = make("particles_sort.comp");
        gather_ = make("particles_gather.comp");
        pool_ = std::make_unique<pipeline::DescriptorPool>(
            pipeline::DescriptorPoolBuilder().add_sets(*layout_, kMaxSystems * kFrames).build(device));
    }

    GpuParticlePass(const GpuParticlePass&) = delete;
    GpuParticlePass& operator=(const GpuParticlePass&) = delete;

    /**
     * @brief Records this frame's compute work for every job (see the file doc). `frame_slot`'s
     *        previous submission has completed (its fence was waited), so its readbacks are read
     *        first and its params buffers rewritten.
     */
    void record(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot, const std::vector<GpuParticleJob>& jobs);

    /** @brief Binds `id`'s instances as vertex buffer 0 and draws its alive list indirectly.
     *         Inside the transparent (or reactive) render pass; false if there is no such state. */
    bool draw(coopa::gfx::command::CommandBuffer& cmd, uint64_t id) const;

    /** @brief The last read-back alive count of `id` and the generation it belongs to. */
    bool alive(uint64_t id, uint32_t& count, uint32_t& generation) const;

    /** @brief Tests: copy `id`'s drawn instances to the host at the next record(). */
    void request_debug_readback(uint64_t id);
    /** @brief Tests: the instances copied by request_debug_readback(), once that frame has
     *         completed (two frames later). False until then. */
    bool debug_instances(uint64_t id, std::vector<ParticleInstance>& out) const;

    std::size_t system_count() const { return states_.size(); }

private:
    static constexpr uint32_t kMaxSystems = 256;

    struct State {
        uint64_t id = 0;
        uint32_t generation = 0;
        uint32_t capacity = 0;
        uint32_t sort_size = 0;   ///< Keys (power of two >= 1024); 0 = unsorted.
        const void* triangles_src = nullptr;
        bool needs_init = true;
        uint64_t last_used = 0;
        std::unique_ptr<coopa::gfx::memory::Buffer> particles, lists, instances, unsorted, keys, tris;
        std::array<std::unique_ptr<coopa::gfx::memory::Buffer>, kFrames> params;
        std::array<std::unique_ptr<coopa::gfx::memory::Buffer>, kFrames> readback;
        std::array<uint32_t, kFrames> readback_generation{};
        std::array<bool, kFrames> readback_written{};
        std::array<std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>, kFrames> sets;
        uint32_t alive = 0, alive_generation = 0;
        bool has_alive = false;
        bool debug_requested = false;
        uint64_t debug_frame = 0;
        std::unique_ptr<coopa::gfx::memory::Buffer> debug_instances, debug_header;
    };
    struct Active { State* st; const GpuParticleJob* job; };

    static uint32_t pow2_at_least_(uint32_t n);

    std::unique_ptr<coopa::gfx::memory::Buffer> storage_(uint64_t bytes, coopa::gfx::BufferUsage extra = coopa::gfx::BufferUsage::None,
                                                         coopa::gfx::MemoryResidency res = coopa::gfx::MemoryResidency::GpuOnly);

    /** @brief The job's state: created on first sight, recreated when its capacity, sort or
     *         mesh changes, re-initialised (same buffers) when its generation changes. */
    State* state_for_(const GpuParticleJob& job);

    void bind_(coopa::gfx::command::CommandBuffer& cmd, const State& st, uint32_t slot, const Push& push) const;

    void record_sort_(coopa::gfx::command::CommandBuffer& cmd, uint32_t slot);

    void read_back_(uint32_t slot);

    /** @brief Systems not seen for a while are dropped; dropped buffers live until every frame
     *         that could still use them has completed. */
    void retire_unused_();

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool> pool_;
    std::unique_ptr<coopa::gfx::pipeline::ComputePipeline> init_, simulate_, emit_, sort_, gather_;
    std::unordered_map<uint64_t, std::unique_ptr<State>> states_;
    std::vector<std::pair<uint64_t, std::unique_ptr<State>>> retired_;
    std::vector<Active> active_;
    uint64_t frame_counter_ = 0;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_GPU_PARTICLE_PASS_H
