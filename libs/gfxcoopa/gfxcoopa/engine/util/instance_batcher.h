/**
 * @file instance_batcher.h
 * @brief Groups per-frame draw items into instanced batches and streams their transforms to the GPU.
 *
 * Not templated on item type: RenderableItem (blendy) and CaptureItem
 * (gfxcoopa's GiSystem) are unrelated types in different repos, so the
 * caller runs its own loop over its own list and feeds this class plain
 * data (a mesh, a world matrix, a continue-batch flag) instead.
 */

#ifndef GFXCOOPA_ENGINE_UTIL_INSTANCE_BATCHER_H
#define GFXCOOPA_ENGINE_UTIL_INSTANCE_BATCHER_H

#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/data/mesh.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace coopa {
namespace gfx {
namespace engine {
namespace util {

/**
 * @class InstanceBatcher
 * @brief Accumulates per-frame instances into contiguous batches and streams them via one shared vertex buffer.
 *
 * Usage per frame:
 * @code
 * batcher.begin();
 * for (each renderable in a draw list, in whatever order that list requires) {
 *     bool continue_batch = (i > 0) && same_key(list[i], list[i-1]);   // caller-defined
 *     batcher.add(mesh_ptr, continue_batch, world_matrix, i);
 * }
 * Range my_list_range{ first_batch, batcher.batch_count() - first_batch };
 * // ... build other lists the same way, appending more batches ...
 * batcher.upload();
 * // ... later, per pass:
 * batcher.bind(cmd);
 * for (uint32_t i = range.first; i < range.first + range.count; ++i) {
 *     const Batch& b = batcher.batches()[i];
 *     b.mesh->bind(cmd);
 *     b.mesh->draw(cmd, b.instance_count, b.first_instance);
 * }
 * @endcode
 *
 * Buffering is intentionally a single, non-double-buffered coopa::gfx::memory::Buffer,
 * grown by destroy-and-recreate (mirroring uicoopa's UiPass::ensure_capacity_ pattern) and
 * fully re-uploaded on every upload() call. This is safe ONLY because every
 * consumer of this class in this codebase records into a command buffer
 * that is submitted and vkQueueWaitIdle'd (blendy's pre_frame_cmd_,
 * gfxcoopa's GiSystem bake, both single-submit) before the instance data for
 * the next frame/bake could be rewritten. If a future caller records
 * instanced draws into a double-buffered swapchain-frame command buffer
 * instead, this class must grow a kFrames-deep ring like UiPass::vbo_[]
 * first — reusing one buffer across frames-in-flight without that would be
 * a write racing a still-in-flight read.
 */
class InstanceBatcher {
public:
    /** @brief One instanced draw: a mesh, a contiguous slice of the instance buffer, and the item that supplied the batch's shared (e.g. material) state. */
    struct Batch {
        const data::Mesh* mesh           = nullptr;
        uint32_t          first_instance = 0; /**< Index into the instance array; passed straight to Mesh::draw()'s first_instance. */
        uint32_t          instance_count = 0;
        uint32_t          item_index     = 0; /**< Caller's index for the item that opened this batch — e.g. where a color pass reads its per-batch material push constant from. */
    };

    /** @brief Half-open slice of batches(), so several draw lists can share one buffer/frame. */
    struct Range {
        uint32_t first = 0;
        uint32_t count = 0;
    };

    /**
     * @param device            Logical device.
     * @param allocator         VMA allocator.
     * @param initial_capacity  Initial instance-buffer capacity, in instances.
     */
    InstanceBatcher(core::Device& device, memory::Allocator& allocator, size_t initial_capacity = 256)
        : device_(device), allocator_(allocator)
    {
        ensure_capacity_(initial_capacity);
    }

    /** @brief Drops last frame's batches and instances. Call once per frame/bake before any add(). */
    void begin() {
        batches_.clear();
        instances_.clear();
    }

    /**
     * @brief Appends one instance, extending the currently open batch or opening a new one.
     *
     * @param mesh           Mesh payload resolved from its AssetHandle THIS
     *   call — never a pointer cached across frames (see coopa::asset::AssetHandle's
     *   contract: reload/eviction change what the handle resolves to).
     * @param continue_batch True iff this item shares its batch key with the
     *   immediately preceding add() call. Always pass false for the first
     *   item of a draw list, so lists never merge across a list boundary.
     *   A caller-computed exact predicate, not a hashed key — a hash
     *   collision on material would silently apply the wrong material to a
     *   merged batch with no failure signal, so each pass compares its own
     *   handful of fields exactly instead.
     * @param model          World matrix, streamed as this instance's vertex data.
     * @param item_index     Caller's index into its own item vector, recorded on new-batch opens.
     */
    void add(const data::Mesh* mesh, bool continue_batch, const glm::mat4& model, uint32_t item_index) {
        if (continue_batch && !batches_.empty() && batches_.back().mesh == mesh) {
            ++batches_.back().instance_count;
        } else {
            Batch b;
            b.mesh           = mesh;
            b.first_instance = static_cast<uint32_t>(instances_.size());
            b.instance_count = 1;
            b.item_index     = item_index;
            batches_.push_back(b);
        }
        // No previous-frame pose is tracked here: prev_model == model, i.e. zero object motion.
        instances_.push_back(data::InstanceData{model, model, model});
    }

    /** @brief Number of batches recorded so far — bracket a draw list's add() calls with this to compute its Range. */
    uint32_t batch_count() const { return static_cast<uint32_t>(batches_.size()); }

    /** @brief Grows the buffer if needed and uploads every instance recorded since begin(). Call once, after the last add(), before recording any draws. */
    void upload() {
        if (instances_.empty()) return;
        ensure_capacity_(instances_.size());
        buffer_->upload(instances_.data(), instances_.size() * sizeof(data::InstanceData));
    }

    const std::vector<Batch>& batches() const { return batches_; }

    /** @brief Binds the instance stream at binding slot 1. Call once per pass, after bind_pipeline(), before the pass's batch loop. */
    void bind(command::CommandBuffer& cmd) const {
        cmd.bind_vertex_buffer(*buffer_, 0, /*binding=*/1);
    }

private:
    /** @brief Destroy-and-recreate growth, mirroring uicoopa's UiPass::ensure_capacity_ — safe because the whole stream is re-uploaded every use (see class doc). */
    void ensure_capacity_(size_t needed_instances) {
        if (buffer_ && needed_instances <= capacity_) return;
        capacity_ = std::max(needed_instances, capacity_ * 2);
        capacity_ = std::max(capacity_, needed_instances);
        buffer_ = std::make_unique<memory::Buffer>(
            memory::Buffer::vertex(device_, allocator_, capacity_ * sizeof(data::InstanceData)));
    }

    core::Device&       device_;
    memory::Allocator&  allocator_;

    std::unique_ptr<memory::Buffer>   buffer_;
    size_t                            capacity_ = 0;

    std::vector<Batch>              batches_;
    std::vector<data::InstanceData> instances_;
};

} // namespace util
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_UTIL_INSTANCE_BATCHER_H
