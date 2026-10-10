/**
 * @file instance_stream.h
 * @brief Per-frame-in-flight instance transform stream for the G-buffer pass.
 *
 * gfxcoopa's engine::util::InstanceBatcher is documented safe only when the whole frame is
 * submitted and vkQueueWaitIdle'd before the next frame's instance data could be rewritten --
 * exactly the stall ToyRenderPipeline avoids, since it records everything into one
 * double-buffered command buffer. Reusing it there would race a write against a
 * still-in-flight read.
 *
 * This is a small equivalent sized for MAX_FRAMES_IN_FLIGHT slots instead of one shared
 * buffer. Batching happens in the caller: ToyRenderPipeline sorts each view's visible
 * renderers and appends every batch's transforms contiguously (add_range), so one instanced
 * draw covers a whole run of identical mesh + material. Transforms repeat once per view an
 * object is visible in (camera, each shadow cascade/face) -- 128 bytes each: this frame's
 * world matrix and last frame's (data::InstanceData), the latter for the G-buffer's
 * per-object motion vectors.
 *
 * The buffer GROWS instead of dropping instances: a slot is only rewritten after render()
 * has waited on that slot's fence, so replacing its buffer there races nothing.
 */

#ifndef TOYENGINE_RENDER_INSTANCE_STREAM_H
#define TOYENGINE_RENDER_INSTANCE_STREAM_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/presentation/renderer.h>

namespace toy {
namespace render {

/**
 * @class InstanceStream
 * @brief Uploads one data::InstanceData (current + previous world matrix) per renderable
 *        into a per-frame-in-flight vertex buffer, bound at the G-buffer pipeline's
 *        instance slot (1).
 */
class InstanceStream {
public:
    /// @brief References gfxcoopa's own constant directly, rather than a
    /// hardcoded duplicate that could silently drift out of sync with it.
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;

    /// @brief Sentinel for "no instance" in callers' per-renderer index tables (a renderer
    /// that is not ready, or not visible in the view the table describes).
    static constexpr uint32_t kInvalidIndex = UINT32_MAX;

    InstanceStream(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                   uint32_t capacity = 256);

    InstanceStream(const InstanceStream&) = delete;
    InstanceStream& operator=(const InstanceStream&) = delete;

    /** @brief Drops last frame's pending transforms. Call once per frame before any add(). */
    void begin(uint32_t frame_index);

    /**
     * @brief Appends one instance transform together with the one it had last frame.
     * @return Its index -- pass as `first_instance` to an instanced draw.
     */
    uint32_t add(const glm::mat4& model, const glm::mat4& prev_model) { return add(model, prev_model, model); }

    /**
     * @brief As above, with the instance's snow anchor -- the pose its snow pattern is laid out
     *        in (data::InstanceData::snow_anchor): its current one unless it has moved.
     */
    uint32_t add(const glm::mat4& model, const glm::mat4& prev_model, const glm::mat4& snow_anchor);

    /** @brief Appends an instance with no previous pose (prev_model == model: zero object motion). */
    uint32_t add(const glm::mat4& model) { return add(model, model, model); }

    /// @brief Number of transforms added since begin() -- the next add()'s index.
    uint32_t size() const { return static_cast<uint32_t>(pending_.size()); }

    /**
     * @brief Uploads all instances added since begin() to this frame's buffer slot, first
     *        growing the slot's buffer (to the next power of two) if they no longer fit.
     *        Call after render() has waited on this slot's fence, before recording.
     */
    void upload();

    /** @brief The current frame slot's buffer -- bind at slot 1 before drawing. */
    const coopa::gfx::memory::Buffer& buffer() const { return *buffers_[frame_index_]; }

private:
    std::unique_ptr<coopa::gfx::memory::Buffer> make_buffer_(uint32_t capacity);

    coopa::gfx::core::Device&               device_;
    coopa::gfx::memory::Allocator&          allocator_;
    std::vector<uint32_t>                   capacities_;   ///< Per slot, in transforms.
    uint32_t                                frame_index_ = 0;
    std::vector<std::unique_ptr<coopa::gfx::memory::Buffer>> buffers_;
    std::vector<coopa::gfx::engine::data::InstanceData> pending_;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_INSTANCE_STREAM_H
