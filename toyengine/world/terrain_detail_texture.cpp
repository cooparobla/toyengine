#include <toyengine/world/terrain_detail_texture.h>

#include <cmath>

#include <glm/glm.hpp>

namespace toy {
namespace world {
namespace {

/// The shader's hash13, kept bit-compatible so the baked field is the one terrain_styled drew.
float hash13(glm::vec3 p) {
    p = glm::fract(p * 0.1031f);
    p += glm::dot(p, glm::vec3(p.z, p.y, p.x) + 31.32f);
    return glm::fract((p.x + p.y) * p.z);
}

/// 3D value noise in [-1, 1], quintic fade -- terrain_styled.glsl's noise_d() without the
/// analytic gradient, which the bake does not need (the mip chain carries the filtering).
float value_noise(glm::vec3 x) {
    const glm::vec3 i = glm::floor(x);
    const glm::vec3 f = x - i;
    const glm::vec3 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);

    const float a = hash13(i + glm::vec3(0, 0, 0)), b = hash13(i + glm::vec3(1, 0, 0));
    const float c = hash13(i + glm::vec3(0, 1, 0)), d = hash13(i + glm::vec3(1, 1, 0));
    const float e = hash13(i + glm::vec3(0, 0, 1)), g = hash13(i + glm::vec3(1, 0, 1));
    const float h = hash13(i + glm::vec3(0, 1, 1)), k = hash13(i + glm::vec3(1, 1, 1));

    const float x00 = glm::mix(a, b, u.x), x10 = glm::mix(c, d, u.x);
    const float x01 = glm::mix(e, g, u.x), x11 = glm::mix(h, k, u.x);
    const float y0 = glm::mix(x00, x10, u.y), y1 = glm::mix(x01, x11, u.y);
    return 2.0f * glm::mix(y0, y1, u.z) - 1.0f;
}

/**
 * @brief Value noise that TILES over `period` lattice cells on x and y.
 *
 * Evaluated on a torus: the 2D position is mapped onto two circles and the noise is read in the
 * 3D field along them, so the value at u and at u + period is the same sample, not merely a
 * similar one. The third noise axis carries the circles' second coordinate, which is why the
 * shader's 3D field is what gets baked rather than a 2D one.
 */
float tiling_noise(float x, float y, float freq, float period, float seed) {
    const float two_pi = 6.2831853f;
    const float r = period * freq / two_pi;
    const float ax = x / period * two_pi;
    const float ay = y / period * two_pi;
    // Two circles, one per axis, offset so they do not sample the same lattice region.
    const float v0 = value_noise(glm::vec3(std::cos(ax) * r, std::sin(ax) * r, seed) +
                                 glm::vec3(0.0f, 0.0f, std::cos(ay) * r));
    const float v1 = value_noise(glm::vec3(std::sin(ay) * r, seed + 11.0f,
                                           std::cos(ay) * r + std::sin(ax) * r));
    return 0.5f * (v0 + v1);
}

// The shader's octave frequencies, in cycles per world unit.
constexpr float k_freq_broad = 0.22f;
constexpr float k_freq_mid   = 1.7f;
constexpr float k_freq_fine  = 6.5f;
constexpr float k_freq_speck = 5.0f;

} // namespace

const char* terrain_detail_texture_id() { return "runtime/terrain_detail_noise"; }

coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> bake_terrain_detail_texture(
    coopa::gfx::core::Device& device,
    coopa::gfx::memory::Allocator& allocator,
    coopa::gfx::command::CommandPool& cmd_pool,
    coopa::asset::AssetManager& assets,
    uint32_t resolution,
    float world_period)
{
    // One upload for the whole world: ask first, so a second terrain (or a scene reload that
    // keeps the asset manager) rebinds the existing texture instead of re-baking it.
    if (auto existing = assets.get<coopa::gfx::engine::data::Texture>(terrain_detail_texture_id());
        existing.is_loaded()) {
        return existing;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(resolution) * resolution * 4);
    const float texel = world_period / static_cast<float>(resolution);

    for (uint32_t py = 0; py < resolution; ++py) {
        for (uint32_t px = 0; px < resolution; ++px) {
            // Texel centres, so the sample grid is symmetric and the tile's two edges carry the
            // same value (a corner-sampled grid would double one edge and drop the other).
            const float wx = (static_cast<float>(px) + 0.5f) * texel;
            const float wy = (static_cast<float>(py) + 0.5f) * texel;

            const float broad = tiling_noise(wx, wy, k_freq_broad, world_period, 17.0f);
            const float mid   = tiling_noise(wx, wy, k_freq_mid,   world_period, 3.7f);
            const float fine  = tiling_noise(wx, wy, k_freq_fine,  world_period, -11.0f);
            const float speck_field = tiling_noise(wx, wy, k_freq_speck, world_period, 41.0f);
            const float strata = tiling_noise(wx, wy, 3.0f, world_period, 7.0f);

            // Specks are thresholded HERE, not in the shader: a smoothstep over noise is a hard
            // edge of fixed world width, which no per-pixel fade can filter and which flickers
            // whenever the sampling point drifts. Thresholded at bake time, the mip chain turns
            // a fleck into coverage as it shrinks -- the filtering a threshold needs.
            const float speck = glm::smoothstep(0.45f, 0.75f, speck_field);

            auto encode = [](float signed_value) {
                return static_cast<uint8_t>(
                    glm::clamp(signed_value * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
            };
            const size_t i = (static_cast<size_t>(py) * resolution + px) * 4;
            // R carries broad+mid together: they are never used apart, and pairing them frees a
            // channel for the strata the walls need.
            pixels[i + 0] = encode(0.5f * (broad + mid));
            pixels[i + 1] = encode(fine);
            pixels[i + 2] = static_cast<uint8_t>(glm::clamp(speck, 0.0f, 1.0f) * 255.0f + 0.5f);
            pixels[i + 3] = encode(strata);
        }
    }

    // Linear, not sRGB: these are signed offsets and a coverage mask, not colours -- gamma
    // would bend the "0.5 means no change" midpoint the shader decodes around.
    auto texture = std::make_shared<coopa::gfx::engine::data::Texture>(
        coopa::gfx::engine::data::Texture::upload(
            device, allocator, cmd_pool, pixels.data(), resolution, resolution,
            coopa::gfx::Format::RGBA8_Unorm,
            coopa::gfx::SamplerDesc::trilinear_repeat()));

    return assets.create<coopa::gfx::engine::data::Texture>(terrain_detail_texture_id(),
                                                            std::move(texture));
}

} // namespace world
} // namespace toy
