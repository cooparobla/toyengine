/**
 * @file screenshot.h
 * @brief Copies a Vulkan color image to a PNG file on disk.
 *
 * Extracted from blendy/test.cpp's inline exit-time screenshot block (~90
 * lines inline in main()) into a reusable function, since toy::core::Engine
 * needs the same copy-to-staging-buffer-then-stbi_write_png sequence for both
 * the low-resolution LDR buffer and the swapchain-resolution upscaled output.
 */

#ifndef TOYENGINE_UTIL_SCREENSHOT_H
#define TOYENGINE_UTIL_SCREENSHOT_H

#include <volk/volk.h>
#include <vma/vk_mem_alloc.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include <stb_image_write.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/command/command_pool.h>

namespace toy {
namespace util {

/**
 * @brief Synchronously copies an RGBA8 color image to a PNG file.
 *
 * Transitions the image from `current_layout` to TRANSFER_SRC_OPTIMAL, copies
 * it into a host-visible staging buffer, transitions it back, and writes the
 * result with stb_image_write. Blocking (vkQueueWaitIdle) -- intended for
 * exit-time or headless (ONESHOT) capture, never a per-frame call.
 *
 * @param device         Logical device.
 * @param allocator      VMA allocator.
 * @param cmd_pool       Command pool to allocate the one-shot copy command from.
 * @param image          Source image, must be VK_FORMAT_R8G8B8A8_* (4 bytes/px).
 * @param current_layout  The image's layout at the time of the call; restored afterward.
 * @param width          Image width in pixels.
 * @param height         Image height in pixels.
 * @param path           Destination PNG path.
 * @return True if the file was written successfully.
 */
inline bool save_image_png(coopa::gfx::core::Device& device,
                           coopa::gfx::memory::Allocator& allocator,
                           coopa::gfx::command::CommandPool& cmd_pool,
                           VkImage image, VkImageLayout current_layout,
                           uint32_t width, uint32_t height,
                           const std::string& path) {
    VkDeviceSize buffer_size = static_cast<VkDeviceSize>(width) * height * 4;
    coopa::gfx::memory::Buffer dst_buffer(
        device, allocator, buffer_size,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT
    );

    VkCommandBuffer copy_cmd = cmd_pool.begin_single_use();

    VkImageMemoryBarrier barrier{};
    barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout           = current_layout;
    barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image               = image;
    barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(copy_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent      = {width, height, 1};

    vkCmdCopyImageToBuffer(copy_cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst_buffer.handle(), 1, &region);

    barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout     = current_layout;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(copy_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    cmd_pool.end_single_use(copy_cmd, device.graphics_queue());

    std::filesystem::path out_path(path);
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path());
    }

    void* mapped = nullptr;
    vmaMapMemory(allocator.handle(), dst_buffer.allocation(), &mapped);
    bool ok = stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height),
                             4, mapped, static_cast<int>(width * 4)) != 0;
    vmaUnmapMemory(allocator.handle(), dst_buffer.allocation());

    if (ok) {
        std::cout << "[toyengine] Saved " << path << " (" << width << "x" << height << ")\n";
    } else {
        std::cerr << "[toyengine] Failed to save " << path << "\n";
    }
    return ok;
}

} // namespace util
} // namespace toy

#endif // TOYENGINE_UTIL_SCREENSHOT_H
