/**
 * @file sprite_sheet_loader.h
 * @brief coopa::asset loader for SpriteSheet -- uicoopa's first coopa::asset registration,
 *        modeled directly on gfxcoopa's TextureLoader (engine/loaders/texture_loader.h).
 *
 * decode_typed() runs off-thread when loaded via AssetManager::load_async(): it reads and
 * parses a `.yaml`/`.yml` sheet descriptor (resolving its `image:` relative to the
 * descriptor's own directory) or, for a bare image path, synthesizes a one-entry
 * descriptor naming the file stem -- see sprite_sheet.h's whole_image_desc(). Either way
 * it then stbi_loads the pixels. finalize_typed() runs on the main thread and is the only
 * place that touches the GPU (Texture::upload).
 */

#ifndef UICOOPA_RENDER_SPRITE_SHEET_LOADER_H
#define UICOOPA_RENDER_SPRITE_SHEET_LOADER_H

#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_source.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/engine/loaders/texture_loader.h>
#include <gfxcoopa/types/format.h>

#include <uicoopa/render/sprite_sheet.h>

#include <stb/stb_image.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <coopa/yaml/document.h>

namespace coopa {
namespace ui {

/**
 * @struct DecodedSpriteSheet
 * @brief CPU-only intermediate between SpriteSheetLoader::decode_typed() and finalize_typed():
 *        decoded RGBA8 pixels plus the parsed (or synthesized) descriptor.
 */
struct DecodedSpriteSheet {
    coopa::gfx::engine::loaders::DecodedImage image; /**< Reuses gfxcoopa's decoded-pixel struct. */
    SpriteSheetDesc                           desc;
};

/**
 * @class SpriteSheetLoader
 * @brief Registers as the coopa::asset loader for SpriteSheet.
 *
 * @code
 * assets.register_loader<SpriteSheet>(
 *     std::make_unique<SpriteSheetLoader>(device, allocator, cmd_pool));
 * auto sheet = assets.load<SpriteSheet>("icons/icons.yaml");
 * @endcode
 */
class SpriteSheetLoader : public coopa::asset::TypedAssetLoader<SpriteSheet, DecodedSpriteSheet> {
public:
    SpriteSheetLoader(coopa::gfx::core::Device& device,
                      coopa::gfx::memory::Allocator& allocator,
                      coopa::gfx::command::CommandPool& cmd_pool)
        : device_(device), allocator_(allocator), cmd_pool_(cmd_pool) {}

    std::shared_ptr<DecodedSpriteSheet> decode_typed(const coopa::asset::AssetId& id,
                                                     const coopa::asset::LoadContext& ctx) override {
        auto result = std::make_shared<DecodedSpriteSheet>();

        std::filesystem::path resolved(ctx.resolved_path);
        std::string ext = resolved.extension().string();
        std::string image_path;

        if (coopa::yaml::is_document_ext(resolved)) {
            std::string yaml_text = coopa::yaml::read_text(resolved);
            result->desc = parse_sprite_sheet_desc(yaml_text);
            image_path = (resolved.parent_path() / result->desc.image).string();
            if (ctx.source) image_path = ctx.source->resolve(image_path);
        } else {
            image_path = ctx.resolved_path;
        }

        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load(image_path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels) {
            throw std::runtime_error("[SpriteSheetLoader] Failed to decode '" + image_path + "': " +
                                     (stbi_failure_reason() ? stbi_failure_reason() : "unknown error"));
        }
        result->image.width  = static_cast<uint32_t>(w);
        result->image.height = static_cast<uint32_t>(h);
        result->image.pixels.assign(pixels, pixels + (static_cast<size_t>(w) * h * 4));
        stbi_image_free(pixels);

        if (result->desc.sprites.empty()) {
            // Bare image, no sidecar descriptor -- one entry covering the whole texture,
            // named after the file's stem (see sprite_sheet.h's whole_image_desc()).
            result->desc = whole_image_desc(resolved.stem().string(), result->image.width, result->image.height);
        }
        if (result->desc.width == 0)  result->desc.width  = result->image.width;
        if (result->desc.height == 0) result->desc.height = result->image.height;

        return result;
    }

    std::shared_ptr<SpriteSheet> finalize_typed(std::shared_ptr<DecodedSpriteSheet> decoded,
                                                const coopa::asset::AssetId&,
                                                const coopa::asset::LoadContext&) override {
        auto texture = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(
                device_, allocator_, cmd_pool_,
                decoded->image.pixels.data(), decoded->image.width, decoded->image.height,
                coopa::gfx::Format::RGBA8_Unorm));
        return std::make_shared<SpriteSheet>(std::move(texture), decoded->desc);
    }

    const char* type_name() const override { return "SpriteSheet"; }

private:
    coopa::gfx::core::Device&         device_;
    coopa::gfx::memory::Allocator&    allocator_;
    coopa::gfx::command::CommandPool& cmd_pool_;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_SPRITE_SHEET_LOADER_H
