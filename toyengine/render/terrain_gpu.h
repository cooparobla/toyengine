/**
 * @file terrain_gpu.h
 * @brief GPU pieces of the heightmap terrain the generic material path has no room for: a
 *        mipmapped 2D texture ARRAY (one layer per paint layer), and the registry that owns
 *        G-buffer set 3 -- the splat maps and layer arrays a `heightmap_terrain` chunk samples.
 *
 * ### Why a set of its own
 *
 * The material set (MaterialTextureCache) has five sampler2D slots and every pass shares its
 * layout. A terrain needs two splat maps and two layer arrays on top of its height grid (which
 * does ride in the material set, as the displacement map, so the shadow passes see it too). Set 3
 * exists only in the G-buffer pipelines' layout; stock shaders never declare it and the shadow
 * passes never need it, so nothing else pays for it.
 *
 * ### Lifetime
 *
 * The registry is the renderer's and is rebuilt with it; the textures belong to
 * toy::world::HeightmapTerrainSystem. Each frame the engine hands the renderer the live
 * TerrainFrameBinding list (SurfaceFrameState::terrains); update() makes / reuses one set per
 * binding and holds shared_ptrs to everything a set references. A binding that stops appearing
 * (terrain removed, textures replaced) has its set parked for a few frames -- command buffers in
 * flight may still use it -- and only then are the set and its texture references dropped.
 */

#ifndef TOYENGINE_RENDER_TERRAIN_GPU_H
#define TOYENGINE_RENDER_TERRAIN_GPU_H

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/descriptor.h>

#include <toyengine/render/surface_world.h>

namespace toy {
namespace render {

/**
 * @class TextureArray
 * @brief An RGBA8 2D array texture with a full mip chain, sampled trilinear + anisotropic, repeat.
 *
 * gfxcoopa's memory::Image is single-mip, single-layer; this builds against Vulkan directly, the
 * way render/passes/cloud_noise_texture.cpp does. Mips are box-filtered on the CPU at upload
 * (no format-feature dependence on blits), so construction is synchronous: it blocks on one
 * staging copy.
 */
class TextureArray {
public:
    /**
     * @param layers  `count` layers, each size * size * 4 bytes (RGBA8, row-major).
     * @param srgb    Albedo data (sRGB) vs linear data (normal maps).
     */
    TextureArray(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                 coopa::gfx::command::CommandPool& cmd_pool, uint32_t size, uint32_t count,
                 const std::vector<std::vector<uint8_t>>& layers, bool srgb);
    ~TextureArray();

    TextureArray(const TextureArray&) = delete;
    TextureArray& operator=(const TextureArray&) = delete;

    VkImageView view() const { return view_; }
    VkSampler sampler() const { return sampler_; }
    uint32_t size() const { return size_; }
    uint32_t layers() const { return count_; }
    uint32_t levels() const { return levels_; }

    /** @brief Mip level count for a square texture of `size` texels (down to 1x1). */
    static uint32_t mip_count(uint32_t size);
    /** @brief The box-filtered mip chain of one RGBA8 level, level 0 first (tests reuse it). */
    static std::vector<std::vector<uint8_t>> build_mips(const std::vector<uint8_t>& level0, uint32_t size, bool srgb);

private:
    coopa::gfx::core::Device& device_;
    VmaAllocator allocator_;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    uint32_t size_ = 0, count_ = 0, levels_ = 1;
};

/**
 * @class TerrainGpuRegistry
 * @brief Owns G-buffer set 3 for heightmap terrains; see the file comment.
 *
 *   binding 0  sampler2D       splat0 (layers 0-3 weights, RGBA8 linear)
 *   binding 1  sampler2D       splat1 (layers 4-7)
 *   binding 2  sampler2DArray  layer albedo (sRGB)
 *   binding 3  sampler2DArray  layer normal maps (linear)
 */
class TerrainGpuRegistry {
public:
    static constexpr uint32_t kMaxSets = 32;
    static constexpr uint32_t kRetireFrames = 4;

    TerrainGpuRegistry(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                       coopa::gfx::command::CommandPool& cmd_pool);

    const coopa::gfx::pipeline::DescriptorSetLayout& layout() const { return *layout_; }

    /** @brief Brings the sets in line with this frame's bindings; ages released ones. */
    void update(const std::vector<TerrainFrameBinding>& bindings);

    /** @brief The set for chunks displaced by `height` (a neutral fallback when unknown). */
    const coopa::gfx::pipeline::DescriptorSet& set_for(const coopa::gfx::engine::data::Texture* height) const;

    /** @brief Height textures whose sets were retired by the last update() (their material sets
     *         should be released from the MaterialTextureCache too). */
    const std::vector<const coopa::gfx::engine::data::Texture*>& released() const { return released_; }

private:
    struct Entry {
        TerrainFrameBinding binding;   // keeps the textures alive while the set references them
        std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> set;
    };
    struct Retired {
        uint32_t frames_left = 0;
        Entry entry;
    };

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> make_set_(const TerrainFrameBinding& b) const;

    coopa::gfx::core::Device& device_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool> pool_;
    std::unique_ptr<coopa::gfx::engine::data::Texture> white_;
    std::unique_ptr<TextureArray> white_array_, flat_array_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> fallback_;
    std::map<const coopa::gfx::engine::data::Texture*, Entry> entries_;
    std::vector<Retired> graveyard_;
    std::vector<const coopa::gfx::engine::data::Texture*> released_;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_TERRAIN_GPU_H
