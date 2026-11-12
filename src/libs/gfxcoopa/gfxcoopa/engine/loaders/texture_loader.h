/**
 * @file texture_loader.h
 * @brief coopa::asset loader for gfx::data::Texture — decodes PNG/JPG/etc via stb_image.
 *
 * decode_typed() runs off-thread (when loaded via AssetManager::load_async())
 * and only ever touches CPU memory: it reads the file's bytes and decodes
 * pixels with stb_image. finalize_typed() runs on the main thread and is
 * the only place that touches the GPU (uploads the decoded pixels via
 * gfx::memory::upload_image_2d).
 *
 * stb_image's implementation is compiled once, in the gfxcoopa_impl target
 * (STB_IMAGE_IMPLEMENTATION is defined in gfxcoopa's own CMakeLists.txt), so
 * gfxcoopa and every library above it share one decoder.
 *
 * Color space: every upload declares one via declare_color_space() (see below) rather than
 * guessing. An albedo map uploaded as if it were linear renders visibly washed out, because
 * sRGB gamma-encoded bytes get interpreted as linear intensities.
 */

#ifndef GFXCOOPA_ENGINE_LOADERS_TEXTURE_LOADER_H
#define GFXCOOPA_ENGINE_LOADERS_TEXTURE_LOADER_H

#include <optional>
#include <coopa/asset/asset_loader.h>
#include <coopa/asset/asset_source.h>
#include <coopa/asset/asset_id.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/sampler_desc.h>

#include <stb/stb_image.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace coopa {
namespace gfx {
namespace engine {
namespace loaders {

/**
 * @struct DecodedImage
 * @brief CPU-side decoded RGBA8 pixel buffer — the intermediate between decode_typed() and finalize_typed().
 */
struct DecodedImage {
    std::vector<uint8_t> pixels; /**< Tightly packed RGBA8, width * height * 4 bytes. */
    uint32_t width  = 0;
    uint32_t height = 0;
};

/**
 * @class TextureLoader
 * @brief Registers as the coopa::asset loader for gfx::data::Texture.
 *
 * Always decodes to RGBA8. The upload *format* (RGBA8_Srgb vs. RGBA8_Unorm) is chosen per
 * asset by declare_color_space() — call it with a path's ColorSpace before the matching
 * load()/load_async() call for that path; finalize_typed() looks up what was declared (default
 * ColorSpace::Linear when nothing was) and uploads accordingly. A caller that never declares
 * anything gets every texture uploaded as linear.
 *
 * Both declare_color_space() and finalize_typed() run on the main thread — the former from
 * scene/material parsing, the latter from AssetManager::update() (or a synchronous load()'s
 * caller) — so the color-space map needs no locking despite decode_typed() itself running off
 * thread via load_async().
 *
 * @code
 * assets.register_loader<gfx::data::Texture>(
 *     std::make_unique<TextureLoader>(device, allocator, cmd_pool));
 *
 * // declare_color_space() takes the RESOLVED path (see its own doc) -- reach the registered
 * // loader back through the AssetManager rather than holding onto a separate reference, so
 * // this works whether the caller declared the loader inline (as above) or received it from
 * // elsewhere.
 * auto* loader = dynamic_cast<TextureLoader*>(assets.loader<gfx::data::Texture>());
 * std::string resolved = assets.source().resolve("textures/brick_albedo.png");
 * loader->declare_color_space(resolved, ColorSpace::Srgb);
 *
 * auto tex = assets.load<gfx::data::Texture>("textures/brick_albedo.png");
 * @endcode
 */
class TextureLoader : public coopa::asset::TypedAssetLoader<data::Texture, DecodedImage> {
public:
    /**
     * @param device        Logical device.
     * @param allocator     VMA allocator.
     * @param cmd_pool      Command pool for the one-shot upload command buffer.
     * @param sampler_desc  Sampler every texture this loader uploads is given. Defaults to
     *   bilinear + repeat (every prior caller's behavior); pass SamplerDesc::pixel_art() for a
     *   pixel-art engine, where bilinear filtering blurs texel edges (see toyengine's
     *   registration of this loader in core/engine.h).
     */
    TextureLoader(core::Device& device, memory::Allocator& allocator, command::CommandPool& cmd_pool,
                 SamplerDesc sampler_desc = SamplerDesc::linear_repeat())
        : device_(device), allocator_(allocator), cmd_pool_(cmd_pool), sampler_desc_(sampler_desc) {}

    /**
     * @brief Declares the color space `virtual_path` should be uploaded as, ahead of loading it.
     *
     * Must be called before the matching load()/load_async() call for this path — finalize_typed()
     * consults this map once, when the texture is first uploaded, and the declaration is not
     * retroactive. Calling this again for a path whose texture has already been uploaded (or with
     * a conflicting ColorSpace before the first upload) logs a warning and keeps the
     * first-declared value, rather than silently reuploading or picking one arbitrarily.
     *
     * @param resolved_path The SAME path AssetManager::load()/load_async() will resolve this
     *   asset to (i.e. AssetSource::resolve()'s return value, not the raw virtual path a caller
     *   passes to load_async() directly) -- AssetManager builds its own AssetId from the
     *   resolved path (see asset_manager.h's load_async()), and this must agree with that or the
     *   declaration is never found. A caller working from a virtual_path/base_dir pair, like
     *   register.h's material parser, should call assets.source().resolve(virtual_path, base_dir)
     *   first and pass the result here.
     * @param color_space  ColorSpace::Srgb for albedo/base-color maps; ColorSpace::Linear
     *   (the default when nothing is declared) for normal maps, metallic/roughness maps, and masks.
     */
    void declare_color_space(const std::string& resolved_path, ColorSpace color_space) {
        auto id = coopa::asset::AssetId::from_path(resolved_path);
        auto it = color_spaces_.find(id);
        if (it != color_spaces_.end()) {
            if (it->second != color_space) {
                // Two materials disagree about what this one path means (e.g. reused as both
                // an albedo map and a mask) -- keep whichever was declared first rather than
                // guessing, and surface it loudly since a silent pick would be a hard-to-spot
                // rendering bug (washed-out or over-dark texture).
                fprintf(stderr,
                       "[TextureLoader] declare_color_space('%s') conflicts with an earlier "
                       "declaration -- keeping the first one. Use texture_color_space: to "
                       "resolve this explicitly if the same file is meant as both.\n",
                       resolved_path.c_str());
            }
            return;
        }
        color_spaces_.emplace(id, color_space);
    }

    /** @brief The color space declared for `resolved_path` so far, if any (first one wins). */
    std::optional<ColorSpace> declared_color_space(const std::string& resolved_path) const {
        auto it = color_spaces_.find(coopa::asset::AssetId::from_path(resolved_path));
        if (it == color_spaces_.end()) return std::nullopt;
        return it->second;
    }

    std::shared_ptr<DecodedImage> decode_typed(const coopa::asset::AssetId& id,
                                               const coopa::asset::LoadContext& ctx) override {
        int w = 0, h = 0, channels = 0;
        stbi_uc* pixels = stbi_load(ctx.resolved_path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
        if (!pixels) {
            throw std::runtime_error("[TextureLoader] Failed to decode '" + id.path() + "': " +
                                     (stbi_failure_reason() ? stbi_failure_reason() : "unknown error"));
        }

        auto decoded = std::make_shared<DecodedImage>();
        decoded->width  = static_cast<uint32_t>(w);
        decoded->height = static_cast<uint32_t>(h);
        decoded->pixels.assign(pixels, pixels + (static_cast<size_t>(w) * h * 4));
        stbi_image_free(pixels);
        return decoded;
    }

    std::shared_ptr<data::Texture> finalize_typed(std::shared_ptr<DecodedImage> decoded,
                                                  const coopa::asset::AssetId& id,
                                                  const coopa::asset::LoadContext&) override {
        auto it = color_spaces_.find(id);
        ColorSpace cs = (it != color_spaces_.end()) ? it->second : ColorSpace::Linear;
        Format format = format_from_channels(4, cs == ColorSpace::Srgb);
        return std::make_shared<data::Texture>(
            data::Texture::upload(device_, allocator_, cmd_pool_,
                                  decoded->pixels.data(), decoded->width, decoded->height,
                                  format, sampler_desc_));
    }

    const char* type_name() const override { return "Texture"; }

private:
    core::Device&         device_;
    memory::Allocator&     allocator_;
    command::CommandPool&  cmd_pool_;
    SamplerDesc            sampler_desc_;
    std::unordered_map<coopa::asset::AssetId, ColorSpace> color_spaces_;
};

} // namespace loaders
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_LOADERS_TEXTURE_LOADER_H
