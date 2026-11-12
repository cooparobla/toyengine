#ifndef GFXCOOPA_COMMAND_QUERY_POOL_H
#define GFXCOOPA_COMMAND_QUERY_POOL_H

/**
 * @file query_pool.h
 * @brief GPU timestamp queries: a pool of slots the GPU writes tick counts into.
 *
 * Named for what it measures rather than mirroring VkQueryPool's other query types (occlusion,
 * pipeline statistics), none of which any consumer uses -- the same narrowing the rest of this
 * library applies to Vulkan's surface.
 */

#include <volk/volk.h>

#include <cstdint>
#include <vector>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>

namespace coopa {
namespace gfx {
namespace command {

/**
 * @class TimestampPool
 * @brief RAII timestamp query pool: `capacity` slots the GPU writes a tick count into, read back
 *        once the submitting fence has signalled.
 *
 * Absorbs the three pieces of arithmetic every caller would otherwise repeat: the device's tick
 * period (`timestampPeriod`, nanoseconds per tick), the queue family's `timestampValidBits` mask
 * (a device reporting fewer than 64 valid bits wraps, and subtracting unmasked ticks there gives a
 * wildly wrong duration), and the support probe itself.
 *
 * On a device without usable timestamps -- no `timestampComputeAndGraphics`, zero valid bits on
 * the graphics family, a zero tick period, or a failed pool creation -- supported() is false and
 * every method is a no-op returning false. A profiler therefore needs no `if (enabled())` guard
 * around each call.
 */
class TimestampPool {
public:
    /**
     * @brief Creates a pool of `capacity` timestamp slots.
     * @param device   The logical device; its graphics family decides the valid-bit mask.
     * @param capacity How many timestamps the pool holds.
     */
    TimestampPool(core::Device& device, uint32_t capacity)
        : device_(device), capacity_(capacity)
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

        if (!props.limits.timestampComputeAndGraphics || valid_bits == 0 || period_ns_ <= 0.0f) return;
        valid_mask_ = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1ull);

        VkQueryPoolCreateInfo info{};
        info.sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType  = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = capacity_;
        if (vkCreateQueryPool(device.handle(), &info, nullptr, &pool_) != VK_SUCCESS) {
            pool_ = VK_NULL_HANDLE;
        }
    }

    ~TimestampPool() {
        if (pool_ != VK_NULL_HANDLE) vkDestroyQueryPool(device_.handle(), pool_, nullptr);
    }

    /// @brief Non-copyable: it owns a VkQueryPool.
    TimestampPool(const TimestampPool&) = delete;
    /// @brief Non-copyable.
    TimestampPool& operator=(const TimestampPool&) = delete;

    /** @brief Whether this device can time GPU work at all. False makes every method a no-op. */
    bool supported() const { return pool_ != VK_NULL_HANDLE; }

    /** @brief How many timestamp slots the pool holds. */
    uint32_t capacity() const { return capacity_; }

    /**
     * @brief Invalidates `count` queries from `first`, so they can be written again.
     *
     * Record outside any render pass, before the writes that follow it.
     */
    void reset(CommandBuffer& cmd, uint32_t first, uint32_t count) {
        if (!supported()) return;
        vkCmdResetQueryPool(cmd.handle(), pool_, first, count);
    }

    /**
     * @brief Writes the tick count at the bottom of the pipe into query `index`.
     *
     * Bottom-of-pipe, so the stamp lands after everything recorded before it has completed --
     * which is what makes two consecutive stamps bracket the work between them.
     */
    void write(CommandBuffer& cmd, uint32_t index) {
        if (!supported()) return;
        vkCmdWriteTimestamp(cmd.handle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, index);
    }

    /**
     * @brief Reads `count` raw ticks from `first` into `out` without waiting.
     *
     * Call only after the fence of the submission that wrote them; the results are not available
     * before that and this does not block for them.
     *
     * @return False, with `out` untouched, if any query is unavailable or the device cannot time.
     */
    bool read(uint32_t first, uint32_t count, uint64_t* out) const {
        if (!supported() || count == 0) return false;
        return vkGetQueryPoolResults(device_.handle(), pool_, first, count,
                                     sizeof(uint64_t) * count, out, sizeof(uint64_t),
                                     VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    }

    /**
     * @brief Elapsed milliseconds between two ticks from this pool.
     *
     * Applies the device's tick period and its valid-bit mask, so a device with fewer than 64
     * valid timestamp bits still gives a correct duration across a wrap.
     */
    double elapsed_ms(uint64_t first_tick, uint64_t second_tick) const {
        const uint64_t d = ((second_tick & valid_mask_) - (first_tick & valid_mask_)) & valid_mask_;
        return static_cast<double>(d) * static_cast<double>(period_ns_) * 1e-6;
    }

private:
    core::Device& device_;
    uint32_t      capacity_   = 0;
    VkQueryPool   pool_       = VK_NULL_HANDLE;
    float         period_ns_  = 0.0f;   /**< Nanoseconds per tick (timestampPeriod). */
    uint64_t      valid_mask_ = ~0ull;  /**< Masks ticks to the queue's timestampValidBits. */
};

} // namespace command
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_COMMAND_QUERY_POOL_H
