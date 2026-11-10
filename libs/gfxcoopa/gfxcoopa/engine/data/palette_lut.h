/**
 * @file palette_lut.h
 * @brief Loads a palette PNG into an Nx1 nearest-filtered lookup texture.
 *
 * The extraction rule (unique non-transparent RGBA8 colors, first-seen
 * order) matches coopixel's own `extract_palette_from_image()`
 * (coopixel/ui/color_panel.py), so a palette authored or exported from
 * coopixel's swatch picker (e.g. coopixel/src/coopixel/default-palette.png,
 * PICO-8-style) loads directly. Pairs with StylizePass/stylize.frag
 * (or toyengine's own stylize.frag) for palette quantization.
 */

#ifndef GFXCOOPA_ENGINE_DATA_PALETTE_LUT_H
#define GFXCOOPA_ENGINE_DATA_PALETTE_LUT_H

#include <volk/volk.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <stb/stb_image.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/engine/data/texture.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

/**
 * @class PaletteLut
 * @brief An Nx1 RGBA8 texture of unique palette colors, sampled with NEAREST.
 *
 * count() == 0 means "no palette" -- callers pass that through to a
 * quantization shader's palette_count push constant, which disables
 * quantization entirely. The texture itself is still a valid 1x1 dummy in
 * that case, so the descriptor binding is never left pointing at an unbound
 * image.
 */
class PaletteLut {
public:
    PaletteLut(const PaletteLut&) = delete;
    PaletteLut& operator=(const PaletteLut&) = delete;
    PaletteLut(PaletteLut&&) = default;
    PaletteLut& operator=(PaletteLut&&) = default;

    /**
     * @brief Loads a palette from a PNG, or builds a 1x1 dummy if path is empty.
     * @param path Absolute path to a palette PNG. Empty disables quantization.
     */
    static PaletteLut load(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                           coopa::gfx::command::CommandPool& cmd_pool, const std::string& path) {
        if (path.empty()) {
            uint8_t dummy[4] = {255, 255, 255, 255};
            return PaletteLut(
                coopa::gfx::engine::data::Texture::upload(device, allocator, cmd_pool, dummy, 1, 1,
                                                          coopa::gfx::Format::RGBA8_Unorm,
                                                          coopa::gfx::SamplerDesc::pixel_art()),
                0);
        }

        int w = 0, h = 0, channels = 0;
        uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
        if (!pixels) {
            std::cerr << "[gfxcoopa] Failed to load palette '" << path << "'; quantization disabled.\n";
            return load(device, allocator, cmd_pool, "");
        }

        std::vector<uint8_t> entries; // RGBA8, unique, first-seen order
        std::vector<uint64_t> seen;
        entries.reserve(256 * 4);
        for (int i = 0; i < w * h && entries.size() < 256 * 4; ++i) {
            uint8_t r = pixels[i * 4 + 0], g = pixels[i * 4 + 1], b = pixels[i * 4 + 2], a = pixels[i * 4 + 3];
            if (a == 0) continue; // transparent pixels are not palette entries
            uint64_t key = (static_cast<uint64_t>(r) << 24) | (static_cast<uint64_t>(g) << 16) |
                          (static_cast<uint64_t>(b) << 8) | a;
            bool duplicate = false;
            for (uint64_t s : seen) {
                if (s == key) { duplicate = true; break; }
            }
            if (duplicate) continue;
            seen.push_back(key);
            entries.push_back(r);
            entries.push_back(g);
            entries.push_back(b);
            entries.push_back(a);
        }
        stbi_image_free(pixels);

        if (entries.empty()) {
            std::cerr << "[gfxcoopa] Palette '" << path << "' had no opaque pixels; quantization disabled.\n";
            return load(device, allocator, cmd_pool, "");
        }

        uint32_t count = static_cast<uint32_t>(entries.size() / 4);
        auto texture = coopa::gfx::engine::data::Texture::upload(
            device, allocator, cmd_pool, entries.data(), count, 1,
            coopa::gfx::Format::RGBA8_Unorm, coopa::gfx::SamplerDesc::pixel_art());
        return PaletteLut(std::move(texture), count);
    }

    VkImageView view() const { return texture_.view(); }
    coopa::gfx::TextureView view_typed() const { return texture_.view_typed(); }
    VkSampler   sampler() const { return texture_.sampler(); }
    const coopa::gfx::engine::util::Sampler& sampler_object() const { return texture_.sampler_object(); }
    uint32_t    count() const { return count_; }

private:
    PaletteLut(coopa::gfx::engine::data::Texture texture, uint32_t count)
        : texture_(std::move(texture)), count_(count) {}

    coopa::gfx::engine::data::Texture texture_;
    uint32_t                          count_;
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_DATA_PALETTE_LUT_H
