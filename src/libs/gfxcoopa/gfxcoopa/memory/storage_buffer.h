/**
 * @file storage_buffer.h
 * @brief Storage buffers (SSBOs) for compute: single buffers and per-frame-slot rings.
 *
 * make_storage_buffer() is a named constructor over Buffer's sealed one: always Storage plus
 * TransferSrc/TransferDst (so fill_buffer(), copies and readback work), plus whatever
 * `extra` usage lets the same memory be drawn from -- BufferUsage::Vertex for compute-written
 * vertices, Index, or Indirect for compute-written draw/dispatch arguments.
 *
 * StorageBufferRing is one such buffer per frame in flight. Use it for data the CPU rewrites
 * every frame (a bone palette, per-frame emitter parameters): the CPU writes the slot the GPU
 * is not reading -- the same reason Mesh::from_arrays() keeps a vertex buffer per frame --
 * and the previous slot still holds last frame's contents, which is what a motion-vector
 * path that needs "where was it last frame" reads.
 */

#ifndef COOPA_GFX_MEMORY_STORAGE_BUFFER_H
#define COOPA_GFX_MEMORY_STORAGE_BUFFER_H

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/types/enums.h>

namespace coopa {
namespace gfx {
namespace memory {

/**
 * @brief Creates a storage buffer usable by compute and by copies.
 * @param device    The logical device.
 * @param allocator The VMA allocator.
 * @param size      Bytes.
 * @param extra     Additional bindings for the same memory (Vertex, Index, Indirect, Uniform).
 * @param residency GpuOnly (the default) for GPU-produced data; CpuToGpu for data the CPU
 *                  writes through upload() each frame; GpuToCpu for readback targets.
 */
inline Buffer make_storage_buffer(core::Device& device, Allocator& allocator, uint64_t size,
                                  BufferUsage extra = BufferUsage::None,
                                  MemoryResidency residency = MemoryResidency::GpuOnly) {
    return Buffer(device, allocator, size,
                  BufferUsage::Storage | BufferUsage::TransferSrc | BufferUsage::TransferDst | extra,
                  residency);
}

/**
 * @class StorageBufferRing
 * @brief One storage buffer per frame-in-flight slot, all the same size and usage.
 *
 * @code
 * StorageBufferRing palette(device, allocator, bones * sizeof(glm::mat4),
 *                           ctx.frames_in_flight(), BufferUsage::None, MemoryResidency::CpuToGpu);
 * palette.upload(frame_slot, matrices.data(), bytes);     // this frame's slot only
 * set[frame_slot].bind_storage_buffer(1, palette.current(frame_slot));
 * @endcode
 */
class StorageBufferRing {
public:
    /**
     * @param slots Buffers to allocate -- Context::frames_in_flight() (clamped to at least 1).
     *              Other parameters as make_storage_buffer().
     */
    StorageBufferRing(core::Device& device, Allocator& allocator, uint64_t size, uint32_t slots,
                      BufferUsage extra = BufferUsage::None,
                      MemoryResidency residency = MemoryResidency::GpuOnly)
        : size_(size)
    {
        const uint32_t n = slots < 1u ? 1u : slots;
        buffers_.reserve(n);
        for (uint32_t i = 0; i < n; ++i) buffers_.push_back(make_storage_buffer(device, allocator, size, extra, residency));
    }

    /** @brief The buffer for `frame_slot` (taken modulo the slot count). */
    Buffer&       current(uint32_t frame_slot)       { return buffers_[frame_slot % slot_count()]; }
    const Buffer& current(uint32_t frame_slot) const { return buffers_[frame_slot % slot_count()]; }

    /** @brief The buffer the PREVIOUS frame used -- last frame's contents while the current
     *         frame's slot is being rewritten. With one slot it is the current buffer. */
    Buffer&       previous(uint32_t frame_slot)       { return buffers_[(frame_slot + slot_count() - 1u) % slot_count()]; }
    const Buffer& previous(uint32_t frame_slot) const { return buffers_[(frame_slot + slot_count() - 1u) % slot_count()]; }

    /** @brief Writes into `frame_slot`'s buffer through its mapping (CpuToGpu rings only). */
    void upload(uint32_t frame_slot, const void* data, uint64_t bytes, uint64_t offset = 0) {
        if (offset + bytes > size_) throw std::runtime_error("[gfxcoopa] StorageBufferRing::upload past the end of the buffer.");
        current(frame_slot).upload(data, bytes, offset);
    }

    uint32_t slot_count() const { return static_cast<uint32_t>(buffers_.size()); }
    uint64_t size() const { return size_; }

private:
    std::vector<Buffer> buffers_;
    uint64_t            size_ = 0;
};

} // namespace memory
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_MEMORY_STORAGE_BUFFER_H
