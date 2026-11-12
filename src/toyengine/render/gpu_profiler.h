/**
 * @file gpu_profiler.h
 * @brief GPU timestamp queries for profiling mode -- how long each feature's passes take.
 *
 * One TimestampPool, one block of timestamps per frame-in-flight slot. Each frame:
 *   begin_frame()  resets the slot's block and writes a starting timestamp;
 *   mark(scope)    writes a timestamp after a feature's passes -- the interval since the
 *                  previous timestamp is that feature's time;
 *   collect()      after the slot's fence has signalled (two frames later), reads the block
 *                  back without stalling and hands the per-scope times to FrameProfile.
 *
 * Marks are CONTIGUOUS -- every interval belongs to exactly one scope -- so the scopes sum to
 * the whole GPU frame, and anything unmarked (a barrier, a debug pass) is folded into the next
 * scope rather than lost.
 *
 * Callers write a feature's mark ONLY when that feature actually recorded GPU work (inside
 * its enabled branch). Under MoltenVK a timestamp written with no work since the previous one
 * does not read as zero -- it picks up time from a neighbouring encoder -- so an unconditional
 * mark after a skipped feature would show a disabled feature costing real milliseconds. A
 * feature that is off therefore records no mark and reads exactly 0.
 *
 * Timestamps are only ever written BETWEEN render passes. MoltenVK on Apple GPUs samples them
 * at encoder boundaries, which is exactly where these sit. Because the tile-based GPU overlaps
 * neighbouring encoders, the per-scope split is boundary-to-boundary attribution rather than
 * an exact per-pass cost; the frame total is exact.
 */

#ifndef TOYENGINE_RENDER_GPU_PROFILER_H
#define TOYENGINE_RENDER_GPU_PROFILER_H


#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <gfxcoopa/command/query_pool.h>
#include <gfxcoopa/presentation/renderer.h>

#include <toyengine/render/frame_profile.h>

namespace toy {
namespace render {

class GpuProfiler {
public:
    static constexpr uint32_t kFrames         = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;
    static constexpr uint32_t kQueriesPerSlot = 64;

    GpuProfiler(coopa::gfx::core::Device& device, FrameProfile& profile);

    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    /// @brief False on a device that cannot time GPU work; every method is then a no-op.
    bool enabled() const { return pool_.supported(); }

    /**
     * @brief Reads back `slot`'s previous frame, if it recorded one. Call once the slot's fence
     *        has signalled (ToyRenderPipeline::render(), right after wait_for_current_frame()),
     *        and before begin_frame() reuses the slot.
     */
    void collect(uint32_t slot);

    /// Starts `slot`'s timestamps for profile frame `frame`. First thing in the command buffer.
    void begin_frame(coopa::gfx::command::CommandBuffer& cmd, uint32_t slot, uint64_t frame);

    /// Closes a scope: the GPU time since the previous mark is attributed to `scope`.
    /// Must be recorded outside any render pass.
    void mark(coopa::gfx::command::CommandBuffer& cmd, GpuScope scope);

private:
    struct Slot {
        uint64_t frame   = 0;
        uint32_t count   = 0;
        bool     pending = false;
        std::array<GpuScope, kQueriesPerSlot> scopes{};
    };

    void write_(coopa::gfx::command::CommandBuffer& cmd, GpuScope scope);

    FrameProfile&                        profile_;
    coopa::gfx::command::TimestampPool   pool_;
    std::array<Slot, kFrames>            slots_{};
    uint32_t                             current_ = 0;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_GPU_PROFILER_H
