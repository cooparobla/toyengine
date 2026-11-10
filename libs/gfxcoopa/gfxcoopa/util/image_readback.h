/**
 * @file image_readback.h
 * @brief GPU image readback (download to host memory / PNG), the mirror of
 * memory/image_upload.h's upload path.
 *
 * One place for the staging-buffer + barrier + vkCmdCopyImageToBuffer +
 * vkMapMemory recipe a debug screenshot or exit-time render capture needs,
 * built on the sealed CommandBuffer::transition()/copy_image_to_buffer().
 * A caller names no Vk* type and writes no barrier of its own.
 */

#ifndef COOPA_GFX_UTIL_IMAGE_READBACK_H
#define COOPA_GFX_UTIL_IMAGE_READBACK_H

#include <volk/volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>

// Declarations only -- the implementation (STB_IMAGE_WRITE_IMPLEMENTATION)
// is compiled exactly once, in src/gfx_impl.cpp (see CMakeLists.txt).
#include <stb/stb_image_write.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/clear.h>

namespace coopa {
namespace gfx {
namespace util {

/// @brief Creates `path`'s parent directory (recursively) if it doesn't
/// already exist. stbi_write_png does not do this itself and fails on a
/// missing directory, so every write path here goes through this first.
inline void ensure_parent_dir(const std::string& path) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::filesystem::create_directories(p.parent_path());
    }
}

/// @brief Raw pixel bytes read back from a GPU image, tightly packed and row-major.
struct ImageData {
    std::vector<uint8_t> pixels;
    uint32_t width  = 0;
    uint32_t height = 0;
    uint32_t channels = 0; /**< Bytes per texel; e.g. 4 for an RGBA8 color image. */
};

/**
 * @brief Reads an image's full extent back into host memory.
 *
 * Intended for color images (Format::RGBA8_Unorm/_Srgb or
 * Format::BGRA8_Unorm/_Srgb, both 4 bytes/texel, are the only formats
 * exercised by any caller today; other uncompressed formats will still
 * copy their raw bytes correctly via format_byte_size(), but no channel/
 * swizzle handling beyond BGRA<->RGBA is applied).
 *
 * `src` is transitioned to TextureUsage::TransferSrc for the copy and, if
 * it was in a restorable usage beforehand (ColorAttachment, DepthAttachment,
 * ShaderRead, or TransferDst), transitioned back afterward -- so calling
 * this mid-frame on a target you'll keep using does not leave it stranded
 * in TransferSrc. If it was Undefined or Present beforehand, it is left in
 * TransferSrc, since transitioning TO Undefined is invalid in Vulkan and
 * transitioning back to Present makes no sense outside a swapchain image
 * (this function is not intended to be called on one).
 *
 * @param device    Logical device.
 * @param allocator VMA allocator.
 * @param cmd_pool  Command pool for the one-shot copy command buffer.
 * @param src       The image to read back.
 * @return The image's pixel bytes, width, height, and bytes-per-texel.
 */
inline ImageData read_image(core::Device& device, memory::Allocator& allocator,
                            command::CommandPool& cmd_pool, memory::Image& src)
{
    Format format = src.format_typed();
    uint32_t bytes_per_pixel = format_byte_size(format);
    if (bytes_per_pixel == 0) {
        throw std::runtime_error("[gfxcoopa] read_image: unsupported/unknown format");
    }

    uint32_t width  = src.width();
    uint32_t height = src.height();
    VkDeviceSize byte_size = static_cast<VkDeviceSize>(width) * height * bytes_per_pixel;

    memory::Buffer readback_buf(device, allocator, byte_size,
                                BufferUsage::TransferDst, MemoryResidency::GpuToCpu);

    TextureUsage original = src.current_usage();
    // A reading of TransferSrc here is always stale bookkeeping, never the image's real
    // layout. Only CommandBuffer::transition() writes Image::current_usage(); a render pass's
    // automatic attachment transition does not. So a render target captured once, then
    // rendered into again, still reports the TransferSrc this function left behind while its
    // actual layout has moved on to ShaderRead via its owning render pass. Trusting the stale
    // value would make the barrier below a no-op and leave the copy reading an image in the
    // wrong layout. Re-point the bookkeeping at ShaderRead before it is used, so repeated
    // capture of the same target is self-correcting.
    if (original == TextureUsage::TransferSrc) {
        src.mark_transitioned(TextureUsage::ShaderRead);
        original = TextureUsage::ShaderRead;
    }
    bool restorable = original == TextureUsage::ColorAttachment ||
                      original == TextureUsage::DepthAttachment ||
                      original == TextureUsage::ShaderRead ||
                      original == TextureUsage::TransferDst;

    cmd_pool.submit_once([&](command::CommandBuffer& cmd) {
        cmd.transition(src, TextureUsage::TransferSrc);
        cmd.copy_image_to_buffer(src, readback_buf, Extent2D{width, height});
        if (restorable) {
            cmd.transition(src, original);
        }
    });

    ImageData result;
    result.width    = width;
    result.height   = height;
    result.channels = bytes_per_pixel;
    result.pixels.resize(static_cast<size_t>(byte_size));

    void* mapped = nullptr;
    VkResult map_result = vmaMapMemory(allocator.handle(), readback_buf.allocation(), &mapped);
    if (map_result != VK_SUCCESS) {
        throw std::runtime_error("[gfxcoopa] read_image: vmaMapMemory failed");
    }
    std::memcpy(result.pixels.data(), mapped, static_cast<size_t>(byte_size));
    vmaUnmapMemory(allocator.handle(), readback_buf.allocation());

    // BGRA -> RGBA swizzle: stb_image_write and every other consumer of
    // ImageData expects RGBA channel order, but a swapchain image is
    // commonly B8G8R8A8. Swizzle in place so ImageData's contract holds
    // regardless of the source format.
    if (is_bgra(format) && bytes_per_pixel == 4) {
        for (size_t i = 0; i + 3 < result.pixels.size(); i += 4) {
            std::swap(result.pixels[i], result.pixels[i + 2]);
        }
    }

    return result;
}

/**
 * @brief Reads an image back and writes it to disk as a PNG.
 * @param device    Logical device.
 * @param allocator VMA allocator.
 * @param cmd_pool  Command pool for the one-shot copy command buffer.
 * @param src       The image to capture. Should be a 4-byte-per-texel
 *   color format (RGBA8/BGRA8, either _Unorm or _Srgb); PNG has no
 *   meaningful representation for e.g. a raw depth format.
 * @param path      Output file path, including the .png extension.
 * @throws std::runtime_error if readback or the PNG write fails.
 */
inline void save_image_png(core::Device& device, memory::Allocator& allocator,
                           command::CommandPool& cmd_pool, memory::Image& src,
                           const std::string& path)
{
    ImageData data = read_image(device, allocator, cmd_pool, src);
    ensure_parent_dir(path);
    int ok = stbi_write_png(path.c_str(), static_cast<int>(data.width), static_cast<int>(data.height),
                            static_cast<int>(data.channels), data.pixels.data(),
                            static_cast<int>(data.width * data.channels));
    if (!ok) {
        throw std::runtime_error("[gfxcoopa] save_image_png: stbi_write_png failed for " + path);
    }
}

/**
 * @brief Writes already-read-back ImageData to disk as a PNG, without a
 * device/allocator/cmd_pool -- for callers that already have an ImageData
 * (e.g. from read_image(), or read once and written to multiple paths).
 * @param data Pixel data to write.
 * @param path Output file path, including the .png extension.
 * @throws std::runtime_error if the PNG write fails.
 */
inline void save_image_png(const ImageData& data, const std::string& path) {
    ensure_parent_dir(path);
    int ok = stbi_write_png(path.c_str(), static_cast<int>(data.width), static_cast<int>(data.height),
                            static_cast<int>(data.channels), data.pixels.data(),
                            static_cast<int>(data.width * data.channels));
    if (!ok) {
        throw std::runtime_error("[gfxcoopa] save_image_png: stbi_write_png failed for " + path);
    }
}

} // namespace util
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_UTIL_IMAGE_READBACK_H
