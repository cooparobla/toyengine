#include <toyengine/render/gpu_profiler.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <volk/volk.h>

namespace toy {
namespace render {

GpuProfiler::GpuProfiler(coopa::gfx::core::Device& device, FrameProfile& profile)
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

void GpuProfiler::collect(uint32_t slot) {
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

void GpuProfiler::begin_frame(coopa::gfx::command::CommandBuffer& cmd, uint32_t slot, uint64_t frame) {
    if (!enabled()) return;
    Slot& s   = slots_[slot];
    s.frame   = frame;
    s.count   = 0;
    s.pending = true;
    current_  = slot;
    vkCmdResetQueryPool(cmd.handle(), pool_, slot * kQueriesPerSlot, kQueriesPerSlot);
    write_(cmd, GpuScope::Count);   // the start marker; its "scope" is never read
}

void GpuProfiler::mark(coopa::gfx::command::CommandBuffer& cmd, GpuScope scope) {
    if (!enabled()) return;
    write_(cmd, scope);
}

void GpuProfiler::write_(coopa::gfx::command::CommandBuffer& cmd, GpuScope scope) {
    Slot& s = slots_[current_];
    if (s.count >= kQueriesPerSlot) return;
    s.scopes[s.count] = scope;
    vkCmdWriteTimestamp(cmd.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_,
                        current_ * kQueriesPerSlot + s.count);
    ++s.count;
}

} // namespace render
} // namespace toy
