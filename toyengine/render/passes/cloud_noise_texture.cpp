#include <toyengine/render/passes/cloud_noise_texture.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/compute_pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/util/error.h>
#include <volk/volk.h>

namespace toy {
namespace render {
namespace passes {

CloudNoiseTexture::CloudNoiseTexture(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                  CloudNoiseGenerators& gens, uint32_t size, bool volume)
    : device_(device), allocator_(allocator.handle()), size_(size), volume_(volume) {
    levels_ = 1;
    while ((size_ >> levels_) > 0) ++levels_;

    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = volume_ ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.extent        = {size_, size_, volume_ ? size_ : 1u};
    info.mipLevels     = levels_;
    info.arrayLayers   = 1;
    info.format        = VK_FORMAT_R8G8B8A8_UNORM;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(allocator_, &info, &alloc, &image_, &allocation_, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("[toyengine] CloudNoiseTexture: vmaCreateImage failed");
    }

    sampled_view_ = make_view_(0, levels_);
    for (uint32_t m = 0; m < levels_; ++m) mip_views_.push_back(make_view_(m, 1));

    coopa::gfx::pipeline::DescriptorPoolBuilder pb;
    pb.add_sets(gens.gen_layout(), 1);
    if (levels_ > 1) pb.add_sets(gens.mip_layout(), levels_ - 1);
    pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(pb.build(device));
    gen_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, gens.gen_layout());
    gen_set_->bind_storage_image(0, mip_views_[0]);
    for (uint32_t m = 1; m < levels_; ++m) {
        mip_sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, gens.mip_layout()));
        mip_sets_.back()->bind_storage_image(0, mip_views_[m - 1]);
        mip_sets_.back()->bind_storage_image(1, mip_views_[m]);
    }
}

CloudNoiseTexture::~CloudNoiseTexture() {
    mip_sets_.clear();
    gen_set_.reset();
    pool_.reset();
    for (VkImageView v : mip_views_) vkDestroyImageView(device_.handle(), v, nullptr);
    if (sampled_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_.handle(), sampled_view_, nullptr);
    if (image_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, image_, allocation_);
}

void CloudNoiseTexture::generate(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::ComputePipeline& generator,
              const CloudNoiseGenerators& gens) {
    VkCommandBuffer cb = cmd.handle();
    barrier_(cb, 0, levels_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    const uint32_t local = volume_ ? 4u : 8u;
    cmd.bind_pipeline(generator);
    cmd.bind_descriptor_set(*gen_set_);
    const uint32_t push = size_;
    cmd.push_constants(coopa::gfx::ShaderStage::Compute, 0, sizeof(push), &push);
    const uint32_t g = (size_ + local - 1) / local;
    cmd.dispatch(g, g, volume_ ? g : 1u);

    const coopa::gfx::pipeline::ComputePipeline& mip = gens.mip(volume_);
    for (uint32_t m = 1; m < levels_; ++m) {
        barrier_(cb, m - 1, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        cmd.bind_pipeline(mip);
        cmd.bind_descriptor_set(*mip_sets_[m - 1]);
        const uint32_t s = std::max(1u, size_ >> m);
        const uint32_t gm = (s + local - 1) / local;
        cmd.dispatch(gm, gm, volume_ ? gm : 1u);
    }
    barrier_(cb, 0, levels_, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
             VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

VkImageView CloudNoiseTexture::make_view_(uint32_t base, uint32_t count) const {
    VkImageViewCreateInfo v{};
    v.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    v.image    = image_;
    v.viewType = volume_ ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    v.format   = VK_FORMAT_R8G8B8A8_UNORM;
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    GFX_VK_CHECK(vkCreateImageView(device_.handle(), &v, nullptr, &view));
    return view;
}

void CloudNoiseTexture::barrier_(VkCommandBuffer cb, uint32_t base, uint32_t count, VkImageLayout from, VkImageLayout to,
              VkAccessFlags src_access, VkAccessFlags dst_access,
              VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) const {
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcAccessMask       = src_access;
    b.dstAccessMask       = dst_access;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image_;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

} // namespace passes
} // namespace render
} // namespace toy
