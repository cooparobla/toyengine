/**
 * @file cloud_noise_texture.h
 * @brief A tiling noise texture for the cloud march -- 2D (the weather map) or 3D (the shape and
 *        detail volumes) -- with a full mip chain, generated once on the GPU by a compute shader.
 *
 * gfxcoopa's memory::Image is single-mip 2D only, so this builds the image against Vulkan
 * directly: RGBA8, STORAGE | SAMPLED, one storage view per mip (the generator writes mip 0, a
 * box-filter compute pass writes each further mip from the one above) and one sampled view over
 * the whole chain. The cloud march samples it trilinearly with a repeating sampler and picks the
 * mip from each sample's screen footprint, so distant clouds read pre-filtered noise instead of
 * aliasing into shimmer.
 *
 * The generator and downsample pipelines are shared (CloudNoiseGenerators) and take one storage
 * image (binding 0) -- the downsample a second (binding 1, the destination). Push constants: the
 * generator gets the texture size in x (uint), the downsample nothing.
 */

#ifndef TOYENGINE_RENDER_PASSES_CLOUD_NOISE_TEXTURE_H
#define TOYENGINE_RENDER_PASSES_CLOUD_NOISE_TEXTURE_H


#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gfxcoopa/command/command_buffer.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @brief The compute pipelines that fill CloudNoiseTextures: the downsamples (2D and 3D) and
 *        their set layouts. The per-texture generators are built by their owner against
 *        gen_layout().
 */
class CloudNoiseGenerators {
public:
    CloudNoiseGenerators(coopa::gfx::core::Device& device, const std::string& mip2d_spv, const std::string& mip3d_spv)
        : gen_layout_(coopa::gfx::pipeline::DescriptorLayoutBuilder()
                          .storage_image(0, coopa::gfx::ShaderStage::Compute)
                          .build(device)),
          mip_layout_(coopa::gfx::pipeline::DescriptorLayoutBuilder()
                          .storage_image(0, coopa::gfx::ShaderStage::Compute)
                          .storage_image(1, coopa::gfx::ShaderStage::Compute)
                          .build(device)),
          mip2d_(device, mip2d_spv, {&mip_layout_}),
          mip3d_(device, mip3d_spv, {&mip_layout_}) {}

    coopa::gfx::pipeline::DescriptorSetLayout& gen_layout() { return gen_layout_; }
    coopa::gfx::pipeline::DescriptorSetLayout& mip_layout() { return mip_layout_; }
    const coopa::gfx::pipeline::ComputePipeline& mip(bool volume) const { return volume ? mip3d_ : mip2d_; }

private:
    coopa::gfx::pipeline::DescriptorSetLayout gen_layout_;
    coopa::gfx::pipeline::DescriptorSetLayout mip_layout_;
    coopa::gfx::pipeline::ComputePipeline     mip2d_;
    coopa::gfx::pipeline::ComputePipeline     mip3d_;
};

class CloudNoiseTexture {
public:
    /**
     * @param size   Texels per axis (a power of two).
     * @param volume true: size^3 (3D); false: size^2 (2D).
     */
    CloudNoiseTexture(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                      CloudNoiseGenerators& gens, uint32_t size, bool volume);

    ~CloudNoiseTexture();

    CloudNoiseTexture(const CloudNoiseTexture&) = delete;
    CloudNoiseTexture& operator=(const CloudNoiseTexture&) = delete;

    /** @brief The whole mip chain, for sampling (SHADER_READ_ONLY_OPTIMAL after generate()). */
    VkImageView view() const { return sampled_view_; }
    uint32_t levels() const { return levels_; }

    /**
     * @brief Records the generator into mip 0, then every further mip, and leaves the image
     *        shader-readable. Outside any render pass. `generator` must take gen_layout() at set 0
     *        and a uint size push constant.
     */
    void generate(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::ComputePipeline& generator,
                  const CloudNoiseGenerators& gens);

private:
    VkImageView make_view_(uint32_t base, uint32_t count) const;

    void barrier_(VkCommandBuffer cb, uint32_t base, uint32_t count, VkImageLayout from, VkImageLayout to,
                  VkAccessFlags src_access, VkAccessFlags dst_access,
                  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) const;

    coopa::gfx::core::Device& device_;
    VmaAllocator              allocator_;
    uint32_t                  size_;
    bool                      volume_;
    uint32_t                  levels_ = 1;
    VkImage                   image_ = VK_NULL_HANDLE;
    VmaAllocation             allocation_ = VK_NULL_HANDLE;
    VkImageView               sampled_view_ = VK_NULL_HANDLE;
    std::vector<VkImageView>  mip_views_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>              pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>               gen_set_;
    std::vector<std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> mip_sets_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_CLOUD_NOISE_TEXTURE_H
