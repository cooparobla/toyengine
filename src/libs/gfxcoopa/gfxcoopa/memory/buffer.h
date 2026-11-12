/**
 * @file buffer.h
 * @brief Vulkan buffer and device memory management via VMA.
 *
 * Provides a general-purpose Buffer class backed by VMA allocations, with
 * convenience factory methods named in the style of OpenGL buffer objects:
 * Buffer::vertex(), Buffer::index(), Buffer::uniform(), Buffer::staging().
 *
 * Data upload is performed via upload(), which maps host-visible allocations
 * directly or via a staging buffer for device-local memory.
 */

#ifndef COOPA_GFX_MEMORY_BUFFER_H
#define COOPA_GFX_MEMORY_BUFFER_H

#include <volk/volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>

#include <cstring>
#include <stdexcept>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace memory {

/**
 * @class Buffer
 * @brief RAII Vulkan buffer backed by a VMA allocation.
 *
 * All buffer memory is managed by VMA. Host-visible buffers can be mapped
 * and written directly via upload(). For device-local buffers, a temporary
 * staging buffer is used internally.
 */
class Buffer {
public:
    /**
     * @brief Creates a Vulkan buffer with a VMA-managed allocation.
     *
     * @param device        The logical device.
     * @param allocator     The VMA allocator.
     * @param size          Buffer size in bytes.
     * @param usage         VkBufferUsageFlags (vertex, index, uniform, etc.).
     * @param memory_usage  VMA memory usage hint (e.g. VMA_MEMORY_USAGE_AUTO).
     * @param flags         VMA allocation create flags (e.g. VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT).
     */
    Buffer(core::Device& device,
           Allocator&    allocator,
           VkDeviceSize  size,
           VkBufferUsageFlags        usage,
           VmaMemoryUsage            memory_usage = VMA_MEMORY_USAGE_AUTO,
           VmaAllocationCreateFlags  flags        = 0)
        : device_(device), allocator_(allocator.handle()), size_(size)
    {
        VkBufferCreateInfo buffer_info{};
        buffer_info.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size        = size_;
        buffer_info.usage       = usage;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo alloc_info{};
        alloc_info.usage = memory_usage;
        alloc_info.flags = flags;

        VmaAllocationInfo info{};
        VkResult result = vmaCreateBuffer(allocator_, &buffer_info, &alloc_info,
                                          &buffer_, &allocation_, &info);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("[gfxcoopa] vmaCreateBuffer failed: " +
                                     util::vk_result_string(result));
        }
        // Non-null only for VMA_ALLOCATION_CREATE_MAPPED_BIT allocations (every host-visible
        // factory below): upload() then copies straight in, with no per-call map/unmap.
        mapped_ = info.pMappedData;
    }

    /**
     * @brief Creates a Vulkan buffer using gfxcoopa's sealed BufferUsage/
     * MemoryResidency vocabulary instead of raw VkBufferUsageFlags/
     * VmaMemoryUsage/VmaAllocationCreateFlags.
     *
     * The sibling of the raw-typed constructor above for callers who need a
     * custom usage/residency combination not covered by the named factories
     * (vertex()/index()/uniform()/staging()/storage()) below -- e.g. a
     * buffer that is both TransferDst (upload target) and Storage.
     *
     * @param device    The logical device.
     * @param allocator The VMA allocator.
     * @param size      Buffer size in bytes.
     * @param usage     What the buffer will be bound as (bitmask).
     * @param residency Where the memory lives and how the CPU may access it.
     */
    Buffer(core::Device& device, Allocator& allocator, VkDeviceSize size,
           BufferUsage usage, MemoryResidency residency = MemoryResidency::GpuOnly)
        : Buffer(device, allocator, size, detail::to_vk(usage),
                 detail::to_vma(residency).usage, detail::to_vma(residency).flags)
    {}

    /**
     * @brief Destroys the buffer and frees its VMA allocation.
     */
    ~Buffer() {
        if (buffer_ != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator_, buffer_, allocation_);
        }
    }

    /// @brief Non-copyable.
    Buffer(const Buffer&) = delete;
    /// @brief Non-copyable.
    Buffer& operator=(const Buffer&) = delete;

    /**
     * @brief Move constructor: transfers ownership of the VMA allocation.
     *
     * After the move, the source Buffer is in a hollow state (null handles)
     * and its destructor is a no-op.
     *
     * @param other The Buffer to move from.
     */
    Buffer(Buffer&& other) noexcept
        : device_(other.device_), allocator_(other.allocator_),
          buffer_(other.buffer_), allocation_(other.allocation_), size_(other.size_),
          mapped_(other.mapped_)
    {
        other.buffer_     = VK_NULL_HANDLE;
        other.allocation_ = VK_NULL_HANDLE;
        other.mapped_     = nullptr;
        other.size_        = 0;
    }


    // --- Data upload ---

    /**
     * @brief Uploads data into a host-visible buffer (analogous to glBufferSubData).
     *
     * Copies into the persistent mapping when the buffer was created mapped (every
     * host-visible factory below does that); otherwise maps, copies, then unmaps. Only
     * valid for host-visible (CPU-accessible) allocations such as staging or uniform buffers.
     *
     * @param data   Pointer to the source data.
     * @param size   Number of bytes to copy.
     * @param offset Byte offset into the buffer destination.
     * @throws std::runtime_error if the mapping fails.
     */
    void upload(const void* data, VkDeviceSize size, VkDeviceSize offset = 0) {
        if (mapped_) {
            std::memcpy(static_cast<uint8_t*>(mapped_) + offset, data, static_cast<size_t>(size));
            return;
        }
        void* mapped = nullptr;
        VkResult result = vmaMapMemory(allocator_, allocation_, &mapped);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("[gfxcoopa] vmaMapMemory failed: " +
                                     util::vk_result_string(result));
        }
        std::memcpy(static_cast<uint8_t*>(mapped) + offset, data, static_cast<size_t>(size));
        vmaUnmapMemory(allocator_, allocation_);
    }

    /**
     * @brief Reads bytes back out of a host-visible buffer (the mirror of upload()).
     *
     * Invalidates the range first, so a GpuToCpu (cached, possibly non-coherent) buffer
     * returns what the GPU wrote rather than a stale cache line. The caller must already have
     * waited for the GPU work that wrote it (a fence) -- this does not synchronise.
     *
     * @param data   Destination.
     * @param size   Number of bytes to copy.
     * @param offset Byte offset into this buffer.
     * @throws std::runtime_error if the mapping fails.
     */
    void download(void* data, VkDeviceSize size, VkDeviceSize offset = 0) const {
        vmaInvalidateAllocation(allocator_, allocation_, offset, size);
        if (mapped_) {
            std::memcpy(data, static_cast<const uint8_t*>(mapped_) + offset, static_cast<size_t>(size));
            return;
        }
        void* mapped = nullptr;
        VkResult result = vmaMapMemory(allocator_, allocation_, &mapped);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("[gfxcoopa] vmaMapMemory failed: " +
                                     util::vk_result_string(result));
        }
        std::memcpy(data, static_cast<const uint8_t*>(mapped) + offset, static_cast<size_t>(size));
        vmaUnmapMemory(allocator_, allocation_);
    }

    // --- Accessors ---

    /**
     * @brief Returns the underlying VkBuffer handle.
     * @return Raw VkBuffer.
     */
    VkBuffer handle() const { return buffer_; }

    /**
     * @brief Returns the size of this buffer in bytes.
     * @return Buffer byte size.
     */
    VkDeviceSize size() const { return size_; }

    /**
     * @brief Returns the underlying VMA allocation handle.
     * @return Raw VmaAllocation.
     */
    VmaAllocation allocation() const { return allocation_; }

    // --- Factory methods (OpenGL-style names) ---

    /**
     * @brief Creates a host-visible vertex buffer (analogous to GL_ARRAY_BUFFER).
     *
     * @param device    The logical device.
     * @param allocator The VMA allocator.
     * @param size      Buffer size in bytes.
     * @return A new Buffer configured for vertex data upload.
     */
    static Buffer vertex(core::Device& device, Allocator& allocator, VkDeviceSize size) {
        return Buffer(device, allocator, size,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

    /**
     * @brief Creates a host-visible index buffer (analogous to GL_ELEMENT_ARRAY_BUFFER).
     *
     * @param device    The logical device.
     * @param allocator The VMA allocator.
     * @param size      Buffer size in bytes.
     * @return A new Buffer configured for index data upload.
     */
    static Buffer index(core::Device& device, Allocator& allocator, VkDeviceSize size) {
        return Buffer(device, allocator, size,
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

    /**
     * @brief Creates a host-visible uniform buffer (analogous to GL_UNIFORM_BUFFER).
     *
     * @param device    The logical device.
     * @param allocator The VMA allocator.
     * @param size      Buffer size in bytes.
     * @return A new Buffer configured for uniform data upload.
     */
    static Buffer uniform(core::Device& device, Allocator& allocator, VkDeviceSize size) {
        return Buffer(device, allocator, size,
                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

    /**
     * @brief Creates a host-visible staging buffer for transfers to device-local memory.
     *
     * @param device    The logical device.
     * @param allocator The VMA allocator.
     * @param size      Buffer size in bytes.
     * @return A new Buffer configured as a CPU-side transfer source.
     */
    static Buffer staging(core::Device& device, Allocator& allocator, VkDeviceSize size) {
        return Buffer(device, allocator, size,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

    /**
     * @brief Creates a host-visible storage buffer (analogous to SSBO).
     */
    static Buffer storage(core::Device& device, Allocator& allocator, VkDeviceSize size) {
        return Buffer(device, allocator, size,
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VMA_MEMORY_USAGE_AUTO,
                      VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT);
    }

private:
    core::Device& device_;                          /**< Owning logical device (not owned). */
    VmaAllocator  allocator_ = VK_NULL_HANDLE;      /**< VMA allocator handle (not owned). */
    VkBuffer      buffer_    = VK_NULL_HANDLE;      /**< The Vulkan buffer handle. */
    VmaAllocation allocation_ = VK_NULL_HANDLE;     /**< VMA allocation tracking this buffer's memory. */
    VkDeviceSize  size_      = 0;                   /**< Buffer size in bytes. */
    void*         mapped_    = nullptr;             /**< Persistent mapping (MAPPED_BIT allocations), else null. */
};

} // namespace memory
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_MEMORY_BUFFER_H
