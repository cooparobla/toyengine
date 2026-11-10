/**
 * @file texture_factory.h
 * @brief uicoopa-specific convenience wrappers around gfxcoopa's
 * engine::data::Texture -- a solid white default texture, and a synchronous
 * load-from-file helper for the interactive demo/tests.
 *
 * Textures themselves are gfxcoopa's Texture, which supports arbitrary formats
 * (including the R8_Unorm FontAtlas needs) via its sealed upload() overload.
 * These two free functions live here (not in gfxcoopa) since "make a white default texture"
 * and "synchronously decode+upload one file" are demo/UI conveniences, not
 * something every gfxcoopa consumer needs (gfxcoopa's own asset-loader path,
 * coopa::asset::TextureLoader, already covers the general case).
 */

#ifndef UICOOPA_RENDER_TEXTURE_FACTORY_H
#define UICOOPA_RENDER_TEXTURE_FACTORY_H

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/sampler_desc.h>

#include <stb/stb_image.h>

namespace coopa {
namespace ui {

/**
 * @brief Creates a 1x1 opaque white texture -- the default for untextured/
 * solid-color quads, so every Graphic (textured or not) can share the same
 * sprite pipeline and batch by clip rect alone when nothing else differs.
 */
inline std::unique_ptr<coopa::gfx::engine::data::Texture> make_white_texture(
    coopa::gfx::core::Device& device,
    coopa::gfx::memory::Allocator& allocator,
    coopa::gfx::command::CommandPool& cmd_pool)
{
    std::array<uint8_t, 4> white_px = { 255, 255, 255, 255 };
    return std::make_unique<coopa::gfx::engine::data::Texture>(
        coopa::gfx::engine::data::Texture::upload(
            device, allocator, cmd_pool, white_px.data(), 1, 1,
            coopa::gfx::Format::RGBA8_Unorm));
}

/**
 * @brief Loads a PNG/JPG/etc. file via stb_image and uploads it as an RGBA8 texture.
 * @throws std::runtime_error if the file cannot be found or decoded.
 */
inline std::unique_ptr<coopa::gfx::engine::data::Texture> load_texture_from_file(
    coopa::gfx::core::Device& device,
    coopa::gfx::memory::Allocator& allocator,
    coopa::gfx::command::CommandPool& cmd_pool,
    const std::string& path)
{
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("[uicoopa] load_texture_from_file: failed to load '" + path + "'.");
    }
    auto tex = std::make_unique<coopa::gfx::engine::data::Texture>(
        coopa::gfx::engine::data::Texture::upload(
            device, allocator, cmd_pool, pixels,
            static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            coopa::gfx::Format::RGBA8_Unorm));
    stbi_image_free(pixels);
    return tex;
}

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_TEXTURE_FACTORY_H
