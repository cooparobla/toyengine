#include <toyengine/render/forward_globals.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>

namespace toy {
namespace render {

ForwardGlobalsData::ForwardGlobalsData(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator) {
    buffers_.reserve(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        buffers_.push_back(coopa::gfx::memory::Buffer::uniform(device, allocator, sizeof(ForwardGlobals)));
    }

    layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
        coopa::gfx::pipeline::DescriptorLayoutBuilder()
            .uniform_buffer(0, coopa::gfx::ShaderStage::Fragment)
            .build(device));

    pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
        coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*layout_, kFrames).build(device));

    sets_.reserve(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) {
        sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, *layout_));
        sets_[i]->bind_buffer(0, buffers_[i]);
    }
}

} // namespace render
} // namespace toy
