/**
 * @file gpu_profiler.h
 * @brief GPU timestamp queries for profiling mode -- how long each feature's passes take.
 *
 * One VkQueryPool, one block of timestamps per frame-in-flight slot. Each frame:
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

#include <volk/volk.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/presentation/renderer.h>

#include <toyengine/render/frame_profile.h>

namespace toy {
namespace render {

class GpuProfiler {
public:
    static constexpr uint32_t kFrames         = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;
    static constexpr uint32_t kQueriesPerSlot = 64;

    GpuProfiler(coopa::gfx::core::Device& device, FrameProfile& profile)
        : device_(device), profile_(profile)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(device.physical(), &props);
        period_ns_ = props.limits.timestampPeriod;

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &family_count, families.data());
        const uint32_t gfx = device.graphics_family();
        const uint32_t valid_bits = gfx < family_count ? families[gfx].timestampValidBits : 0;

        if (!props.limits.timestampComputeAndGraphics || valid_bits == 0 || period_ns_ <= 0.0f) {
            std::fprintf(stderr, "[profile] GPU timestamps unsupported on this device; GPU columns will be 0\n");
            return;
        }
        valid_mask_ = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1ull);

        VkQueryPoolCreateInfo info{};
        info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = kQueriesPerSlot * kFrames;
        if (vkCreateQueryPool(device.handle(), &info, nullptr, &pool_) != VK_SUCCESS) {
            pool_ = VK_NULL_HANDLE;
            std::fprintf(stderr, "[profile] vkCreateQueryPool failed; GPU columns will be 0\n");
        }
    }

    ~GpuProfiler() {
        if (pool_ != VK_NULL_HANDLE) vkDestroyQueryPool(device_.handle(), pool_, nullptr);
    }

    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    bool enabled() const { return pool_ != VK_NULL_HANDLE; }

    /**
     * @brief Reads back `slot`'s previous frame, if it recorded one. Call once the slot's fence
     *        has signalled (PixelRenderPipeline::render(), right after wait_for_current_frame()),
     *        and before begin_frame() reuses the slot.
     */
    void collect(uint32_t slot) {
        Slot& s = slots_[slot];
        if (!enabled() || !s.pending || s.count < 2) return;
        s.pending = false;

        std::array<uint64_t, kQueriesPerSlot> ticks{};
        const VkResult r = vkGetQueryPoolResults(device_.handle(), pool_, slot * kQueriesPerSlot, s.count,
                                                 sizeof(uint64_t) * s.count, ticks.data(), sizeof(uint64_t),
                                                 VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS) return;   // VK_NOT_READY cannot happen after the fence, but be safe

        std::array<double, kGpuScopeCount> scopes{};
        auto to_ms = [this](uint64_t a, uint64_t b) {
            const uint64_t d = ((b & valid_mask_) - (a & valid_mask_)) & valid_mask_;
            return static_cast<double>(d) * static_cast<double>(period_ns_) * 1e-6;
        };
        for (uint32_t q = 1; q < s.count; ++q) {
            scopes[static_cast<size_t>(s.scopes[q])] += to_ms(ticks[q - 1], ticks[q]);
        }
        profile_.set_gpu(s.frame, scopes, to_ms(ticks[0], ticks[s.count - 1]));
    }

    /// Starts `slot`'s timestamps for profile frame `frame`. First thing in the command buffer.
    void begin_frame(coopa::gfx::command::CommandBuffer& cmd, uint32_t slot, uint64_t frame) {
        if (!enabled()) return;
        Slot& s   = slots_[slot];
        s.frame   = frame;
        s.count   = 0;
        s.pending = true;
        current_  = slot;
        vkCmdResetQueryPool(cmd.handle(), pool_, slot * kQueriesPerSlot, kQueriesPerSlot);
        write_(cmd, GpuScope::Count);   // the start marker; its "scope" is never read
    }

    /// Closes a scope: the GPU time since the previous mark is attributed to `scope`.
    /// Must be recorded outside any render pass.
    void mark(coopa::gfx::command::CommandBuffer& cmd, GpuScope scope) {
        if (!enabled()) return;
        write_(cmd, scope);
    }

private:
    struct Slot {
        uint64_t frame   = 0;
        uint32_t count   = 0;
        bool     pending = false;
        std::array<GpuScope, kQueriesPerSlot> scopes{};
    };

    void write_(coopa::gfx::command::CommandBuffer& cmd, GpuScope scope) {
        Slot& s = slots_[current_];
        if (s.count >= kQueriesPerSlot) return;
        s.scopes[s.count] = scope;
        vkCmdWriteTimestamp(cmd.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_,
                            current_ * kQueriesPerSlot + s.count);
        ++s.count;
    }

    coopa::gfx::core::Device& device_;
    FrameProfile&             profile_;
    VkQueryPool               pool_       = VK_NULL_HANDLE;
    float                     period_ns_  = 0.0f;
    uint64_t                  valid_mask_ = ~0ull;
    std::array<Slot, kFrames> slots_{};
    uint32_t                  current_ = 0;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_GPU_PROFILER_H
