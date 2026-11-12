/**
 * @file sync.h
 * @brief Vulkan synchronization primitives: Fence and Semaphore.
 *
 * Provides RAII wrappers for the two most commonly used Vulkan sync objects.
 * Fence is used for CPU↔GPU synchronization (wait for a frame to finish),
 * while Semaphore is used for GPU↔GPU synchronization between queue submits.
 */

#ifndef COOPA_GFX_COMMAND_SYNC_H
#define COOPA_GFX_COMMAND_SYNC_H

#include <volk/volk.h>
#include <stdexcept>
#include <limits>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/util/error.h>

namespace coopa {
namespace gfx {
namespace command {

/**
 * @class Fence
 * @brief RAII wrapper around VkFence for CPU-GPU synchronization.
 *
 * A fence signals when the GPU completes a submitted command buffer.
 * Commonly used to throttle frame-in-flight count and prevent the CPU
 * from submitting work faster than the GPU can consume it.
 */
class Fence {
public:
    /**
     * @brief Creates a VkFence.
     * @param device   The logical device.
     * @param signaled If true, the fence starts in the signaled state
     *                 (useful for first-frame initialization so the first
     *                 wait() returns immediately).
     * @throws std::runtime_error if creation fails.
     */
    explicit Fence(core::Device& device, bool signaled = true)
        : device_(device)
    {
        VkFenceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        info.flags = signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0;
        GFX_VK_CHECK(vkCreateFence(device_.handle(), &info, nullptr, &fence_));
    }

    /**
     * @brief Destroys the VkFence.
     */
    ~Fence() {
        if (fence_ != VK_NULL_HANDLE) {
            vkDestroyFence(device_.handle(), fence_, nullptr);
        }
    }

    /// @brief Non-copyable.
    Fence(const Fence&) = delete;
    /// @brief Non-copyable.
    Fence& operator=(const Fence&) = delete;

    /**
     * @brief Blocks the calling thread until the fence is signaled.
     *
     * @param timeout_ns Timeout in nanoseconds. Defaults to infinite wait.
     * @throws std::runtime_error on device loss or timeout.
     */
    void wait(uint64_t timeout_ns = std::numeric_limits<uint64_t>::max()) const {
        GFX_VK_CHECK(vkWaitForFences(device_.handle(), 1, &fence_, VK_TRUE, timeout_ns));
    }

    /**
     * @brief Resets the fence to the unsignaled state.
     * @throws std::runtime_error if the reset fails.
     */
    void reset() {
        GFX_VK_CHECK(vkResetFences(device_.handle(), 1, &fence_));
    }

    /**
     * @brief Returns the underlying VkFence handle.
     * @return Raw VkFence.
     */
    VkFence handle() const { return fence_; }

private:
    core::Device& device_;              /**< Owning logical device (not owned). */
    VkFence       fence_ = VK_NULL_HANDLE; /**< The Vulkan fence handle. */
};

// ---------------------------------------------------------

/**
 * @class Semaphore
 * @brief RAII wrapper around VkSemaphore for GPU-GPU synchronization.
 *
 * A semaphore signals between queue submissions — for example, signaling
 * that a swapchain image is ready to be rendered into, or that rendering
 * is complete and the image can be presented.
 */
class Semaphore {
public:
    /**
     * @brief Creates a VkSemaphore.
     * @param device The logical device.
     * @throws std::runtime_error if creation fails.
     */
    explicit Semaphore(core::Device& device) : device_(device) {
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        GFX_VK_CHECK(vkCreateSemaphore(device_.handle(), &info, nullptr, &semaphore_));
    }

    /**
     * @brief Destroys the VkSemaphore.
     */
    ~Semaphore() {
        if (semaphore_ != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_.handle(), semaphore_, nullptr);
        }
    }

    /// @brief Non-copyable.
    Semaphore(const Semaphore&) = delete;
    /// @brief Non-copyable.
    Semaphore& operator=(const Semaphore&) = delete;

    /**
     * @brief Returns the underlying VkSemaphore handle.
     * @return Raw VkSemaphore.
     */
    VkSemaphore handle() const { return semaphore_; }

private:
    core::Device& device_;                    /**< Owning logical device (not owned). */
    VkSemaphore   semaphore_ = VK_NULL_HANDLE;/**< The Vulkan semaphore handle. */
};

} // namespace command
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_COMMAND_SYNC_H
