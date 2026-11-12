#include <toyengine/render/passes/cloud_noise_texture.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/compute_pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>

namespace toy {
namespace render {
namespace passes {

namespace {

using coopa::gfx::ImageDesc;
using coopa::gfx::ImageKind;
using coopa::gfx::ImageUsage;

/** @brief The image a cloud noise field of `size` texels per axis needs, 2D or 3D. */
ImageDesc noise_image_desc(uint32_t size, bool volume, uint32_t levels) {
    const auto usage = ImageUsage::Storage | ImageUsage::Sampled;
    ImageDesc d = volume ? ImageDesc::volume(size, coopa::gfx::Format::RGBA8_Unorm, usage, levels)
                         : ImageDesc::texture_2d(size, size, coopa::gfx::Format::RGBA8_Unorm, usage, levels);
    return d;
}

uint32_t level_count(uint32_t size) {
    uint32_t levels = 1;
    while ((size >> levels) > 0) ++levels;
    return levels;
}

} // namespace

CloudNoiseTexture::CloudNoiseTexture(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                  CloudNoiseGenerators& gens, uint32_t size, bool volume)
    : size_(size), volume_(volume), levels_(level_count(size)),
      image_(device, allocator, noise_image_desc(size, volume, levels_)) {
    const ImageKind kind = volume_ ? ImageKind::Image3D : ImageKind::Image2D;
    for (uint32_t m = 0; m < levels_; ++m) {
        mip_views_.push_back(image_.make_view(coopa::gfx::ImageViewDesc::mip(m, kind)));
    }

    coopa::gfx::pipeline::DescriptorPoolBuilder pb;
    pb.add_sets(gens.gen_layout(), 1);
    if (levels_ > 1) pb.add_sets(gens.mip_layout(), levels_ - 1);
    pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(pb.build(device));
    gen_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, gens.gen_layout());
    gen_set_->bind_storage_image(0, mip_views_[0].view_typed());
    for (uint32_t m = 1; m < levels_; ++m) {
        mip_sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, gens.mip_layout()));
        mip_sets_.back()->bind_storage_image(0, mip_views_[m - 1].view_typed());
        mip_sets_.back()->bind_storage_image(1, mip_views_[m].view_typed());
    }
}

void CloudNoiseTexture::generate(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::ComputePipeline& generator,
              const CloudNoiseGenerators& gens) {
    using coopa::gfx::ImageAccess;
    using coopa::gfx::SubresourceRange;
    using coopa::gfx::TextureUsage;

    const SubresourceRange whole = SubresourceRange::mips(0, levels_);
    barrier_(cmd, whole, TextureUsage::Undefined, TextureUsage::Storage,
             ImageAccess::None, ImageAccess::ComputeWrite);

    const uint32_t local = volume_ ? 4u : 8u;
    cmd.bind_pipeline(generator);
    cmd.bind_descriptor_set(*gen_set_);
    const uint32_t push = size_;
    cmd.push_constants(coopa::gfx::ShaderStage::Compute, 0, sizeof(push), &push);
    const uint32_t g = (size_ + local - 1) / local;
    cmd.dispatch(g, g, volume_ ? g : 1u);

    const coopa::gfx::pipeline::ComputePipeline& mip = gens.mip(volume_);
    for (uint32_t m = 1; m < levels_; ++m) {
        // The level just written becomes the level about to be read.
        barrier_(cmd, SubresourceRange::mip(m - 1), TextureUsage::Storage, TextureUsage::Storage,
                 ImageAccess::ComputeWrite, ImageAccess::ComputeRead);
        cmd.bind_pipeline(mip);
        cmd.bind_descriptor_set(*mip_sets_[m - 1]);
        const uint32_t s = std::max(1u, size_ >> m);
        const uint32_t gm = (s + local - 1) / local;
        cmd.dispatch(gm, gm, volume_ ? gm : 1u);
    }
    barrier_(cmd, whole, TextureUsage::Storage, TextureUsage::ShaderRead,
             ImageAccess::ComputeWrite, ImageAccess::FragmentShaderRead);
}

void CloudNoiseTexture::barrier_(coopa::gfx::command::CommandBuffer& cmd, coopa::gfx::SubresourceRange range,
              coopa::gfx::TextureUsage from, coopa::gfx::TextureUsage to,
              coopa::gfx::ImageAccess wait_for, coopa::gfx::ImageAccess make_visible_to) const {
    coopa::gfx::ImageBarrier b;
    b.from = from;
    b.to = to;
    b.range = range;
    b.wait_for = wait_for;
    b.make_visible_to = make_visible_to;
    cmd.barrier(image_.handle_typed(), b);
}

} // namespace passes
} // namespace render
} // namespace toy
