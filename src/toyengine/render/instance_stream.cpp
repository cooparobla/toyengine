#include <toyengine/render/instance_stream.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/types/enums.h>

namespace toy {
namespace render {

InstanceStream::InstanceStream(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
               uint32_t capacity)
    : device_(device), allocator_(allocator)
{
    buffers_.reserve(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        buffers_.push_back(make_buffer_(capacity));
        capacities_.push_back(capacity);
    }
}

void InstanceStream::begin(uint32_t frame_index) {
    frame_index_ = frame_index;
    pending_.clear();
}

uint32_t InstanceStream::add(const glm::mat4& model, const glm::mat4& prev_model, const glm::mat4& snow_anchor) {
    uint32_t idx = static_cast<uint32_t>(pending_.size());
    coopa::gfx::engine::data::InstanceData inst;
    inst.model       = model;
    inst.prev_model  = prev_model;
    inst.snow_anchor = snow_anchor;
    pending_.push_back(inst);
    return idx;
}

void InstanceStream::upload() {
    if (pending_.empty()) return;
    if (pending_.size() > capacities_[frame_index_]) {
        uint32_t cap = capacities_[frame_index_];
        while (cap < pending_.size()) cap *= 2;
        buffers_[frame_index_] = make_buffer_(cap);
        capacities_[frame_index_] = cap;
    }
    buffers_[frame_index_]->upload(pending_.data(),
                                   sizeof(coopa::gfx::engine::data::InstanceData) * pending_.size());
}

std::unique_ptr<coopa::gfx::memory::Buffer> InstanceStream::make_buffer_(uint32_t capacity) {
    return std::make_unique<coopa::gfx::memory::Buffer>(
        device_, allocator_,
        sizeof(coopa::gfx::engine::data::InstanceData) * std::max<uint32_t>(capacity, 1u),
        coopa::gfx::BufferUsage::Vertex, coopa::gfx::MemoryResidency::CpuToGpu);
}

} // namespace render
} // namespace toy
