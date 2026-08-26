/**
 * @file instance_stream.h
 * @brief Per-frame-in-flight instance transform stream for the G-buffer pass.
 *
 * gfxcoopa's engine::util::InstanceBatcher documents that it is only safe
 * when the whole frame is submitted and vkQueueWaitIdle'd before the next
 * frame's instance data could be rewritten (see
 * gfxcoopa/engine/util/instance_batcher.h) -- exactly the stall
 * PixelRenderPipeline avoids, since everything records into one
 * double-buffered swapchain-frame command buffer via
 * Renderer::begin_frame's pre_pass_fn (see gfxcoopa/presentation/renderer.h).
 * Reusing InstanceBatcher unmodified there would race a write against a
 * still-in-flight read.
 *
 * This is a small from-scratch equivalent sized for
 * Renderer::MAX_FRAMES_IN_FLIGHT slots instead of one shared buffer, which
 * removes that hazard. It intentionally skips InstanceBatcher's
 * same-mesh-run batching -- one draw call per renderable instead of grouped
 * instanced draws -- a correctness-over-throughput trade acceptable at
 * toyengine's target scene scale; batching can be reintroduced per-frame-slot
 * later without changing this class's public surface.
 */

#ifndef TOYENGINE_RENDER_INSTANCE_STREAM_H
#define TOYENGINE_RENDER_INSTANCE_STREAM_H

#include <glm/glm.hpp>

#include <cstdint>
#include <iostream>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/types/enums.h>

namespace toy {
namespace render {

/**
 * @class InstanceStream
 * @brief Uploads one glm::mat4 per renderable into a per-frame-in-flight
 *        vertex buffer, bound at the G-buffer pipeline's instance slot (1).
 */
class InstanceStream {
public:
    /// @brief References gfxcoopa's own constant directly, rather than a
    /// hardcoded duplicate that could silently drift out of sync with it.
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;

    InstanceStream(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                   uint32_t capacity = 256)
        : capacity_(capacity)
    {
        buffers_.reserve(kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            buffers_.push_back(coopa::gfx::memory::Buffer(
                device, allocator, sizeof(glm::mat4) * capacity_,
                coopa::gfx::BufferUsage::Vertex, coopa::gfx::MemoryResidency::CpuToGpu));
        }
    }

    InstanceStream(const InstanceStream&) = delete;
    InstanceStream& operator=(const InstanceStream&) = delete;

    /** @brief Drops last frame's pending transforms. Call once per frame before any add(). */
    void begin(uint32_t frame_index) {
        frame_index_ = frame_index;
        pending_.clear();
    }

    /**
     * @brief Appends one instance transform.
     * @return Its index -- pass as `first_instance` to Mesh::draw(cmd, 1, index).
     */
    uint32_t add(const glm::mat4& model) {
        if (pending_.size() >= capacity_) {
            std::cerr << "[toyengine] InstanceStream capacity (" << capacity_
                     << ") exceeded; dropping instance.\n";
            return 0;
        }
        uint32_t idx = static_cast<uint32_t>(pending_.size());
        pending_.push_back(model);
        return idx;
    }

    /** @brief Uploads all instances added since begin() to this frame's buffer slot. */
    void upload() {
        if (!pending_.empty()) {
            buffers_[frame_index_].upload(pending_.data(), sizeof(glm::mat4) * pending_.size());
        }
    }

    /** @brief The current frame slot's buffer -- bind at slot 1 before drawing. */
    const coopa::gfx::memory::Buffer& buffer() const { return buffers_[frame_index_]; }

private:
    uint32_t                                capacity_;
    uint32_t                                frame_index_ = 0;
    std::vector<coopa::gfx::memory::Buffer> buffers_;
    std::vector<glm::mat4>                  pending_;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_INSTANCE_STREAM_H
