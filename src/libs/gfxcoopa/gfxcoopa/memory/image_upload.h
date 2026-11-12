/**
 * @file image_upload.h
 * @brief Free helper that uploads raw pixel data into a new 2D Image via a staging buffer.
 *
 * One place for the staging-buffer -> layout transition -> copy -> layout
 * transition recipe. The returned Image is left in
 * TextureUsage::ShaderRead and ready to bind; the staging buffer does not
 * outlive the call.
 */

#ifndef COOPA_GFX_MEMORY_IMAGE_UPLOAD_H
#define COOPA_GFX_MEMORY_IMAGE_UPLOAD_H

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/clear.h>
#include <gfxcoopa/detail/vk_convert.h>

#include <memory>
#include <cstdint>

namespace coopa {
namespace gfx {
namespace memory {

/**
 * @brief Uploads tightly-packed, row-major pixel data into a newly created 2D Image.
 *
 * Records and submits a one-shot command buffer (via CommandPool::submit_once()) that
 * transitions the image Undefined -> TransferDst, copies the staging buffer into it, then
 * transitions TransferDst -> ShaderRead, and blocks until the graphics queue is idle before
 * returning, so the image is ready to sample immediately.
 *
 * @param device          Logical device.
 * @param allocator       VMA allocator.
 * @param cmd_pool        Command pool the one-shot upload command buffer is allocated from.
 * @param data            Raw pixel bytes, tightly packed, row-major, `width * height * bytes_per_pixel` long.
 * @param width           Image width in pixels.
 * @param height          Image height in pixels.
 * @param format          Pixel format; must match bytes_per_pixel.
 * @param bytes_per_pixel Bytes per texel (e.g. 1 for R8_Unorm, 4 for RGBA8_Unorm).
 * @return A new Image, already transitioned to TextureUsage::ShaderRead.
 */
inline std::unique_ptr<Image> upload_image_2d(core::Device&         device,
                                              Allocator&            allocator,
                                              command::CommandPool& cmd_pool,
                                              const void*           data,
                                              uint32_t              width,
                                              uint32_t              height,
                                              Format                format,
                                              uint32_t              bytes_per_pixel)
{
    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * bytes_per_pixel;

    Buffer staging = Buffer::staging(device, allocator, size);
    staging.upload(data, size);

    auto image = std::make_unique<Image>(
        device, allocator, width, height, format,
        ImageUsage::TransferDst | ImageUsage::Sampled
    );

    cmd_pool.submit_once([&](command::CommandBuffer& cmd) {
        cmd.transition(*image, TextureUsage::TransferDst);
        cmd.copy_buffer_to_image(staging, *image, Extent2D{width, height});
        cmd.transition(*image, TextureUsage::ShaderRead);
    });

    return image;
}

/**
 * @brief Uploads pixel data like upload_image_2d() and then generates a full mip chain.
 *
 * Level 0 is copied from the staging buffer; every further level is blitted (linear filter)
 * from the one above, with per-level barriers staging each source level through TransferSrc.
 * The whole chain is left in TextureUsage::ShaderRead, recorded via mark_transitioned() so
 * CommandBuffer::transition() keeps working on the image afterwards.
 *
 * Requires a format whose blit supports linear filtering (every 8-bit UNORM/SRGB color format
 * does on desktop hardware); the caller owns that contract the same way it owns
 * bytes_per_pixel matching the format.
 *
 * @param device          Logical device.
 * @param allocator       VMA allocator.
 * @param cmd_pool        Command pool for the one-shot upload command buffer.
 * @param data            Level-0 pixel bytes, tightly packed, row-major.
 * @param width           Level-0 width in pixels.
 * @param height          Level-0 height in pixels.
 * @param format          Pixel format.
 * @param bytes_per_pixel Bytes per texel.
 * @return A new Image with Image::full_mip_count() levels, ready to sample.
 */
inline std::unique_ptr<Image> upload_image_2d_mipped(core::Device&         device,
                                                     Allocator&            allocator,
                                                     command::CommandPool& cmd_pool,
                                                     const void*           data,
                                                     uint32_t              width,
                                                     uint32_t              height,
                                                     Format                format,
                                                     uint32_t              bytes_per_pixel)
{
    const uint32_t mips = Image::full_mip_count(width, height);
    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * bytes_per_pixel;

    Buffer staging = Buffer::staging(device, allocator, size);
    staging.upload(data, size);

    auto image = std::make_unique<Image>(
        device, allocator, width, height, detail::to_vk(format),
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO, VK_SAMPLE_COUNT_1_BIT, mips);

    cmd_pool.submit_once([&](command::CommandBuffer& cmd) {
        VkCommandBuffer vk = cmd.handle();

        // Raw per-level barriers: CommandBuffer::transition() moves the WHOLE chain at once,
        // while generation needs each level in TransferDst until written, then TransferSrc
        // while the next level reads it -- see transition()'s own comment.
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image                           = image->handle();
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;

        // The whole chain Undefined -> TransferDst, then level 0 gets the staging copy.
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount   = mips;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(vk, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent                 = {width, height, 1};
        vkCmdCopyBufferToImage(vk, staging.handle(), image->handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        barrier.subresourceRange.levelCount = 1;
        int32_t src_w = static_cast<int32_t>(width);
        int32_t src_h = static_cast<int32_t>(height);
        for (uint32_t level = 1; level < mips; ++level) {
            // The level above: TransferDst -> TransferSrc, its write finished.
            barrier.subresourceRange.baseMipLevel = level - 1;
            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(vk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            const int32_t dst_w = src_w > 1 ? src_w / 2 : 1;
            const int32_t dst_h = src_h > 1 ? src_h / 2 : 1;
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel   = level - 1;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1]             = {src_w, src_h, 1};
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel   = level;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1]             = {dst_w, dst_h, 1};
            vkCmdBlitImage(vk, image->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, VK_FILTER_LINEAR);

            // And on to ShaderRead -- it is never read or written as a transfer target again.
            barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(vk, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);

            src_w = dst_w;
            src_h = dst_h;
        }

        // The last level was written but never used as a blit source: TransferDst -> ShaderRead.
        barrier.subresourceRange.baseMipLevel = mips - 1;
        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(vk, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    });
    image->mark_transitioned(TextureUsage::ShaderRead);

    return image;
}

/**
 * @brief Raw-`VkFormat` overload of the above, for internal callers that hold a `VkFormat`
 * rather than the sealed `Format` enum. Prefer the `Format`-taking overload in new code.
 */
inline std::unique_ptr<Image> upload_image_2d(core::Device&         device,
                                              Allocator&            allocator,
                                              command::CommandPool& cmd_pool,
                                              const void*           data,
                                              uint32_t              width,
                                              uint32_t              height,
                                              VkFormat              format,
                                              uint32_t              bytes_per_pixel)
{
    return upload_image_2d(device, allocator, cmd_pool, data, width, height,
                           detail::from_vk(format), bytes_per_pixel);
}

} // namespace memory
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_MEMORY_IMAGE_UPLOAD_H
