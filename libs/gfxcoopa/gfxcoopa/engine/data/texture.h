/**
 * @file texture.h
 * @brief GPU-resident 2D texture: an Image plus its own Sampler, decoded from an image file.
 *
 * Loaded through coopa::asset::TextureLoader; it is what PBRMaterial's
 * texture_albedo/texture_normal/texture_metallic_roughness/texture_alpha_mask
 * fields resolve to.
 */

#ifndef GFXCOOPA_ENGINE_DATA_TEXTURE_H
#define GFXCOOPA_ENGINE_DATA_TEXTURE_H

#include <volk/volk.h>
#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/memory/image_upload.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/sampler_desc.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/detail/vk_convert.h>

#include <memory>
#include <cstdint>
#include <vector>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

/**
 * @class Texture
 * @brief GPU-resident 2D texture: an Image (RGBA8) plus a Sampler.
 *
 * Move-only, like every other GPU-owning type in gfxcoopa. Constructed via
 * upload() from already-decoded RGBA8 pixel bytes — decoding itself
 * (stb_image) happens in coopa::asset::TextureLoader::decode_typed(), off
 * the main thread when loaded via AssetManager::load_async(); this class
 * only ever touches the GPU.
 */
class Texture {
public:
    /**
     * @brief Uploads decoded RGBA8 pixel data into a new GPU texture.
     *
     * @param device     Logical device.
     * @param allocator  VMA allocator.
     * @param cmd_pool   Command pool for the one-shot upload command buffer.
     * @param pixels     Tightly packed RGBA8 pixel data, width * height * 4 bytes.
     * @param width      Texture width in pixels.
     * @param height     Texture height in pixels.
     * @param srgb       True to interpret the data as sRGB-encoded (albedo/base color maps);
     *                   false for linear data (normal maps, metallic/roughness, masks).
     * @param filter     Sampler min/mag filter. Defaults to VK_FILTER_LINEAR (every prior
     *                   caller's behavior); pass VK_FILTER_NEAREST for pixel-art atlases,
     *                   where bilinear filtering blurs texel edges.
     * @param address_mode Sampler UV wrap mode. Defaults to VK_SAMPLER_ADDRESS_MODE_REPEAT
     *                   (every prior caller's behavior); pass CLAMP_TO_EDGE for an atlas,
     *                   where wrapping across unrelated packed frames is never wanted.
     * @return A new GPU-resident Texture, ready to sample.
     */
    static Texture upload(core::Device& device,
                          memory::Allocator& allocator,
                          command::CommandPool& cmd_pool,
                          const uint8_t* pixels,
                          uint32_t width, uint32_t height,
                          bool srgb,
                          VkFilter filter = VK_FILTER_LINEAR,
                          VkSamplerAddressMode address_mode = VK_SAMPLER_ADDRESS_MODE_REPEAT)
    {
        VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        auto image = memory::upload_image_2d(device, allocator, cmd_pool, pixels, width, height, format, 4);
        auto sampler = std::make_unique<util::Sampler>(device, filter, address_mode);
        return Texture(std::move(image), std::move(sampler));
    }

    /**
     * @brief Uploads decoded pixel data into a new GPU texture, using
     * gfxcoopa's sealed Format and SamplerDesc instead of a hardcoded
     * RGBA8 format (via a `bool srgb` flag) and raw VkFilter/
     * VkSamplerAddressMode.
     *
     * Required for single-channel textures (Format::R8_Unorm, e.g. a font
     * atlas), which the raw overload above cannot express at all -- it is
     * hardcoded to RGBA8.
     *
     * @param device       Logical device.
     * @param allocator    VMA allocator.
     * @param cmd_pool     Command pool for the one-shot upload command buffer.
     * @param pixels       Tightly packed pixel data, width * height *
     *                     format_byte_size(format) bytes.
     * @param width        Texture width in pixels.
     * @param height       Texture height in pixels.
     * @param format       Pixel format of `pixels`.
     * @param sampler_desc Sampler configuration. Defaults to bilinear + repeat.
     * @return A new GPU-resident Texture, ready to sample.
     */
    static Texture upload(core::Device& device,
                          memory::Allocator& allocator,
                          command::CommandPool& cmd_pool,
                          const uint8_t* pixels,
                          uint32_t width, uint32_t height,
                          Format format,
                          const SamplerDesc& sampler_desc = SamplerDesc::linear_repeat())
    {
        uint32_t bytes_per_pixel = format_byte_size(format);
        // sampler_desc.mipmapped: the image gets a generated full mip chain and a sampler whose
        // LOD range covers it (see SamplerDesc::mipmapped); otherwise the single-mip upload
        // every pre-existing texture uses, byte for byte.
        auto image = sampler_desc.mipmapped
            ? memory::upload_image_2d_mipped(device, allocator, cmd_pool, pixels, width, height,
                                             format, bytes_per_pixel)
            : memory::upload_image_2d(device, allocator, cmd_pool, pixels, width, height,
                                      detail::to_vk(format), bytes_per_pixel);
        auto sampler = std::make_unique<util::Sampler>(
            device, sampler_desc, static_cast<float>(image->mip_levels()));
        return Texture(std::move(image), std::move(sampler));
    }

    /**
     * @brief Replaces a sub-rectangle of the texture's pixels in place (painting, tiled
     *        canvases), keeping the same image and view -- so descriptor sets that already
     *        reference it stay valid.
     *
     * Records and submits a one-shot command buffer (CommandPool::submit_once()): ShaderRead
     * -> TransferDst, copy from a staging buffer, -> ShaderRead, and blocks until it is done.
     * Work submitted earlier that samples the texture (previous frames) is ordered before the
     * copy by the barrier; call it between frames, not while recording one that samples it.
     *
     * @param device    Logical device.
     * @param allocator VMA allocator (for the staging buffer).
     * @param cmd_pool  Command pool for the one-shot command buffer.
     * @param pixels    The region's texels, tightly packed: w * h * bytes per texel.
     * @param x, y      Region origin in texels.
     * @param w, h      Region size in texels; the region must lie inside the texture.
     */
    void update(core::Device& device,
                memory::Allocator& allocator,
                command::CommandPool& cmd_pool,
                const void* pixels,
                uint32_t x, uint32_t y, uint32_t w, uint32_t h)
    {
        if (w == 0 || h == 0) return;
        const uint32_t bpp = format_byte_size(image_->format_typed());
        const VkDeviceSize size = static_cast<VkDeviceSize>(w) * h * bpp;
        memory::Buffer staging = memory::Buffer::staging(device, allocator, size);
        staging.upload(pixels, size);
        cmd_pool.submit_once([&](command::CommandBuffer& cmd) {
            cmd.transition(*image_, TextureUsage::TransferDst);
            cmd.copy_buffer_to_image_region(staging, *image_, static_cast<int32_t>(x), static_cast<int32_t>(y), Extent2D{w, h});
            cmd.transition(*image_, TextureUsage::ShaderRead);
        });
    }

    /** @brief The image view to bind into a descriptor set. */
    VkImageView view() const { return image_->view(); }

    /** @brief Sealed sibling of view(): the texture identity to bind into a
     * descriptor set via pipeline::DescriptorSet::bind_image(uint32_t,
     * TextureView, const Sampler&) or the const Texture& overload. */
    TextureView view_typed() const { return image_->view_typed(); }

    /** @brief The sampler to bind alongside view(). */
    VkSampler sampler() const { return sampler_->handle(); }

    /** @brief Sealed sibling of sampler(): the Sampler object itself, for
     * pipeline::DescriptorSet::bind_image(uint32_t, TextureView, const Sampler&). */
    const util::Sampler& sampler_object() const { return *sampler_; }

    uint32_t width()  const { return image_->width(); }
    uint32_t height() const { return image_->height(); }

    // Move only (Image/Sampler are not copyable).
    Texture(Texture&&) = default;
    Texture& operator=(Texture&&) = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

private:
    Texture(std::unique_ptr<memory::Image> image, std::unique_ptr<util::Sampler> sampler)
        : image_(std::move(image)), sampler_(std::move(sampler)) {}

    std::unique_ptr<memory::Image>   image_;
    std::unique_ptr<util::Sampler>   sampler_;
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_DATA_TEXTURE_H
