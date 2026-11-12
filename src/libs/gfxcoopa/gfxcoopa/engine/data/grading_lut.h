/**
 * @file grading_lut.h
 * @brief Loads a colour-grading strip LUT PNG into a bilinear-sampled texture.
 *
 * The layout is Unity's (and Photoshop/Resolve's export default): a 3D cube of
 * side N flattened into one N*N wide by N tall strip, blue varying across the
 * slices left to right, red across each slice, green down. A 32-cube is
 * therefore a 1024x32 image. Pairs with gfx/grading.glsl's gfx_apply_grading_lut(),
 * which reconstructs the trilinear 3D lookup from two strip taps.
 *
 * Sampled LINEAR (unlike PaletteLut, which is deliberately NEAREST): the whole
 * point of a grading LUT is that it interpolates between its entries, and the
 * hardware's bilinear filter is what supplies the red/green interpolation for
 * free -- the shader only has to lerp the two blue slices itself.
 */

#ifndef GFXCOOPA_ENGINE_DATA_GRADING_LUT_H
#define GFXCOOPA_ENGINE_DATA_GRADING_LUT_H

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
 * @class GradingLut
 * @brief An N*N by N strip LUT texture plus its cube side length.
 *
 * size() == 0 means "no grading" -- callers pass that through to a shader's
 * grading_size push constant, which disables the lookup entirely. The texture
 * is still a valid 1x1 dummy in that case, so the descriptor binding is never
 * left pointing at an unbound image (the same contract PaletteLut documents).
 */
class GradingLut {
public:
    GradingLut(const GradingLut&) = delete;
    GradingLut& operator=(const GradingLut&) = delete;
    GradingLut(GradingLut&&) = default;
    GradingLut& operator=(GradingLut&&) = default;

    /**
     * @brief Loads a strip LUT from a PNG, or builds a 1x1 dummy if path is empty.
     *
     * Rejects (with a warning, falling back to the dummy) any image whose
     * dimensions are not `height * height` by `height`, since that is the one
     * property the shader's slice arithmetic depends on -- a mis-sized LUT would
     * otherwise grade every pixel through garbage rather than fail visibly.
     *
     * @param path Absolute path to a strip LUT PNG. Empty disables grading.
     */
    static GradingLut load(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                           coopa::gfx::command::CommandPool& cmd_pool, const std::string& path) {
        coopa::gfx::SamplerDesc desc;
        desc.min = desc.mag = coopa::gfx::Filter::Linear;
        desc.mipmap  = coopa::gfx::MipmapMode::Nearest;
        desc.address = coopa::gfx::AddressMode::ClampToEdge;

        if (path.empty()) {
            uint8_t dummy[4] = {255, 255, 255, 255};
            return GradingLut(
                coopa::gfx::engine::data::Texture::upload(device, allocator, cmd_pool, dummy, 1, 1,
                                                          coopa::gfx::Format::RGBA8_Unorm, desc),
                0);
        }

        int w = 0, h = 0, channels = 0;
        uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
        if (!pixels) {
            std::cerr << "[gfxcoopa] Failed to load grading LUT '" << path << "'; grading disabled.\n";
            return load(device, allocator, cmd_pool, "");
        }
        if (h <= 1 || w != h * h) {
            std::cerr << "[gfxcoopa] Grading LUT '" << path << "' is " << w << "x" << h
                      << "; expected a strip of N*N by N (e.g. 1024x32). Grading disabled.\n";
            stbi_image_free(pixels);
            return load(device, allocator, cmd_pool, "");
        }

        auto texture = coopa::gfx::engine::data::Texture::upload(
            device, allocator, cmd_pool, pixels, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            coopa::gfx::Format::RGBA8_Unorm, desc);
        stbi_image_free(pixels);
        return GradingLut(std::move(texture), static_cast<uint32_t>(h));
    }

    VkImageView view() const { return texture_.view(); }
    coopa::gfx::TextureView view_typed() const { return texture_.view_typed(); }
    VkSampler   sampler() const { return texture_.sampler(); }
    const coopa::gfx::engine::util::Sampler& sampler_object() const { return texture_.sampler_object(); }
    /** @brief Cube side length N, or 0 when no LUT is loaded. */
    uint32_t    size() const { return size_; }

private:
    GradingLut(coopa::gfx::engine::data::Texture texture, uint32_t size)
        : texture_(std::move(texture)), size_(size) {}

    coopa::gfx::engine::data::Texture texture_;
    uint32_t                          size_;
};

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_DATA_GRADING_LUT_H
