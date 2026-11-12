#include <toyengine/render/gpu_profiler.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>

namespace toy {
namespace render {

GpuProfiler::GpuProfiler(coopa::gfx::core::Device& device, FrameProfile& profile)
    : profile_(profile), pool_(device, kQueriesPerSlot * kFrames)
{
    if (!pool_.supported()) {
        std::fprintf(stderr, "[profile] GPU timestamps unavailable on this device; GPU columns will be 0\n");
    }
}

void GpuProfiler::collect(uint32_t slot) {
    Slot& s = slots_[slot];
    if (!enabled() || !s.pending || s.count < 2) return;
    s.pending = false;

    std::array<uint64_t, kQueriesPerSlot> ticks{};
    // Not ready cannot happen after the slot's fence, but a false return is still respected.
    if (!pool_.read(slot * kQueriesPerSlot, s.count, ticks.data())) return;

    std::array<double, kGpuScopeCount> scopes{};
    auto to_ms = [this](uint64_t a, uint64_t b) { return pool_.elapsed_ms(a, b); };
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
    pool_.reset(cmd, slot * kQueriesPerSlot, kQueriesPerSlot);
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
    pool_.write(cmd, current_ * kQueriesPerSlot + s.count);
    ++s.count;
}

} // namespace render
} // namespace toy
