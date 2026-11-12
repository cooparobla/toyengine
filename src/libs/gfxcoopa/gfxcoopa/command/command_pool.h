/**
 * @file command_pool.h
 * @brief Command pool and single-use command buffer helpers.
 *
 * CommandPool is the allocator for VkCommandBuffer objects. It provides
 * allocate() for multi-buffer batches and begin_single_use() /
 * end_single_use() for fire-and-forget commands like buffer copies and
 * image layout transitions (analogous to glBegin for one-shot GPU ops).
 */

#ifndef COOPA_GFX_COMMAND_COMMAND_POOL_H
#define COOPA_GFX_COMMAND_COMMAND_POOL_H

#include <volk/volk.h>
#include <vector>
#include <stdexcept>
#include <functional>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/command/command_buffer.h>

namespace coopa {
namespace gfx {
namespace command {

/**
 * @class CommandPool
 * @brief RAII wrapper around VkCommandPool.
 *
 * Provides both bulk allocation and a convenient single-use pattern.
 * All command buffers allocated from this pool must be freed or destroyed
 * before the pool itself is destroyed.
 */
class CommandPool {
public:
    /**
     * @brief Creates a VkCommandPool on the specified queue family.
     * @param device             The logical device.
     * @param queue_family_index Queue family this pool allocates for (typically the graphics family).
     * @param transient          If true, hints that buffers will be short-lived (VK_COMMAND_POOL_CREATE_TRANSIENT_BIT).
     * @throws std::runtime_error if pool creation fails.
     */
    CommandPool(core::Device& device, uint32_t queue_family_index, bool transient = false)
        : device_(device)
    {
        VkCommandPoolCreateInfo info{};
        info.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        info.queueFamilyIndex = queue_family_index;
        info.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (transient) {
            info.flags |= VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        }
        GFX_VK_CHECK(vkCreateCommandPool(device_.handle(), &info, nullptr, &pool_));
    }

    /**
     * @brief Destroys the VkCommandPool and all command buffers allocated from it.
     */
    ~CommandPool() {
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device_.handle(), pool_, nullptr);
        }
    }

    /// @brief Non-copyable.
    CommandPool(const CommandPool&) = delete;
    /// @brief Non-copyable.
    CommandPool& operator=(const CommandPool&) = delete;

    // --- Allocation ---

    /**
     * @brief Allocates a batch of primary command buffers from this pool.
     *
     * @param count Number of command buffers to allocate.
     * @return A vector of raw VkCommandBuffer handles.
     * @throws std::runtime_error if allocation fails.
     */
    std::vector<VkCommandBuffer> allocate(uint32_t count) const {
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool        = pool_;
        alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = count;

        std::vector<VkCommandBuffer> cmds(count);
        GFX_VK_CHECK(vkAllocateCommandBuffers(device_.handle(), &alloc_info, cmds.data()));
        return cmds;
    }

    /**
     * @brief Returns the raw VkCommandPool handle.
     * @return Raw VkCommandPool.
     */
    VkCommandPool handle() const { return pool_; }

    // --- Single-use helpers ---

    /**
     * @brief Allocates a transient primary command buffer and begins recording.
     *
     * Intended for one-shot operations such as buffer copies and image layout
     * transitions. Must be paired with end_single_use().
     *
     * @return A VkCommandBuffer in the recording state.
     */
    VkCommandBuffer begin_single_use() const {
        VkCommandBufferAllocateInfo alloc_info{};
        alloc_info.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool        = pool_;
        alloc_info.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;

        VkCommandBuffer cmd = VK_NULL_HANDLE;
        GFX_VK_CHECK(vkAllocateCommandBuffers(device_.handle(), &alloc_info, &cmd));

        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        GFX_VK_CHECK(vkBeginCommandBuffer(cmd, &begin_info));

        return cmd;
    }

    /**
     * @brief Ends recording, submits the command buffer to the queue, waits for completion,
     *        then frees the command buffer.
     *
     * This is a blocking synchronous operation — the calling thread waits until
     * the GPU finishes executing the command buffer.
     *
     * @param cmd   The command buffer returned by begin_single_use().
     * @param queue The queue to submit the command buffer to.
     */
    void end_single_use(VkCommandBuffer cmd, VkQueue queue) const {
        GFX_VK_CHECK(vkEndCommandBuffer(cmd));

        VkSubmitInfo submit_info{};
        submit_info.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers    = &cmd;

        GFX_VK_CHECK(vkQueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE));
        GFX_VK_CHECK(vkQueueWaitIdle(queue));

        vkFreeCommandBuffers(device_.handle(), pool_, 1, &cmd);
    }

    /**
     * @brief Allocates a transient command buffer, records `record` into
     * it via the CommandBuffer wrapper, then submits and blocks until the
     * GPU finishes -- the whole begin_single_use()/end_single_use() dance
     * in one call, using CommandBuffer rather than a raw VkCommandBuffer.
     *
     * Use this for any one-shot GPU work -- a texture upload, a readback
     * copy, a GI bake step -- rather than pairing the two methods above by
     * hand, which leaks the buffer if `record` throws.
     *
     * @param record A callable that records commands into the CommandBuffer.
     *   Do not call begin()/end() on it yourself -- submit_once() does both.
     *   Submits to this pool's device's graphics queue, which is correct
     *   for every use in this codebase today (uploads, readbacks, and bakes
     *   are all graphics-queue work here; there is no separate transfer or
     *   compute queue in use).
     */
    void submit_once(std::function<void(CommandBuffer&)> record) const {
        VkCommandBuffer raw = begin_single_use();
        CommandBuffer cmd(raw);
        record(cmd);
        end_single_use(raw, device_.graphics_queue());
    }

private:
    core::Device& device_;              /**< Owning logical device (not owned). */
    VkCommandPool pool_ = VK_NULL_HANDLE;/**< The Vulkan command pool. */
};

} // namespace command
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_COMMAND_COMMAND_POOL_H
