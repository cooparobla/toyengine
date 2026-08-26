/**
 * @file pixel_texture_loader.h
 * @brief coopa::asset loader for gfx::data::Texture, forcing NEAREST filtering.
 *
 * A near-verbatim fork of gfxcoopa's TextureLoader
 * (gfxcoopa/engine/loaders/texture_loader.h), which defaults to bilinear
 * filtering -- that blurs every pixel-art texture, which is exactly what
 * this engine exists to avoid. The only change from the original is the
 * upload() call's sampler (SamplerDesc::pixel_art() instead of the default).
 */

#ifndef TOYENGINE_LOADERS_PIXEL_TEXTURE_LOADER_H
#define TOYENGINE_LOADERS_PIXEL_TEXTURE_LOADER_H

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_source.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/sampler_desc.h>

#include <stb/stb_image.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace toy {
namespace loaders {

/**
 * @struct DecodedImage
 * @brief CPU-side decoded RGBA8 pixel buffer -- the intermediate between decode_typed() and finalize_typed().
 */
struct DecodedImage {
    std::vector<uint8_t> pixels; /**< Tightly packed RGBA8, width * height * 4 bytes. */
    uint32_t width  = 0;
    uint32_t height = 0;
};

/**
 * @class PixelTextureLoader
 * @brief Registers as the coopa::asset loader for gfx::data::Texture, NEAREST-filtered.
 *
 * @code
 * assets.register_loader<coopa::gfx::engine::data::Texture>(
 *     std::make_unique<toy::loaders::PixelTextureLoader>(device, allocator, cmd_pool));
 * auto tex = assets.load<coopa::gfx::engine::data::Texture>("textures/brick.png");
 * @endcode
 */
class PixelTextureLoader : public coopa::asset::TypedAssetLoader<coopa::gfx::engine::data::Texture, DecodedImage> {
public:
    PixelTextureLoader(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                       coopa::gfx::command::CommandPool& cmd_pool)
        : device_(device), allocator_(allocator), cmd_pool_(cmd_pool) {}

    std::shared_ptr<DecodedImage> decode_typed(const coopa::asset::AssetId& id,
                                               const coopa::asset::LoadContext& ctx) override {
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load(ctx.resolved_path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels) {
            throw std::runtime_error("[PixelTextureLoader] Failed to decode '" + id.path() + "': " +
                                     (stbi_failure_reason() ? stbi_failure_reason() : "unknown error"));
        }

        auto decoded = std::make_shared<DecodedImage>();
        decoded->width  = static_cast<uint32_t>(w);
        decoded->height = static_cast<uint32_t>(h);
        decoded->pixels.assign(pixels, pixels + (static_cast<size_t>(w) * h * 4));
        stbi_image_free(pixels);
        return decoded;
    }

    std::shared_ptr<coopa::gfx::engine::data::Texture> finalize_typed(
        std::shared_ptr<DecodedImage> decoded, const coopa::asset::AssetId&,
        const coopa::asset::LoadContext&) override {
        return std::make_shared<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(
                device_, allocator_, cmd_pool_, decoded->pixels.data(), decoded->width, decoded->height,
                coopa::gfx::Format::RGBA8_Unorm, coopa::gfx::SamplerDesc::pixel_art()));
    }

    const char* type_name() const override { return "Texture"; }

private:
    coopa::gfx::core::Device&        device_;
    coopa::gfx::memory::Allocator&    allocator_;
    coopa::gfx::command::CommandPool& cmd_pool_;
};

} // namespace loaders
} // namespace toy

#endif // TOYENGINE_LOADERS_PIXEL_TEXTURE_LOADER_H
