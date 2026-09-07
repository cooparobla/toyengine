/**
 * @file material_texture_cache.h
 * @brief Owns the "material" descriptor set (a single combined-image-sampler binding) shared
 * by the G-buffer and shadow pipelines, and lazily allocates one DescriptorSet per distinct
 * alpha mask texture a CUTOUT (AlphaMode::Mask) material references.
 *
 * This is the first material descriptor set either repo has ever bound -- PBRMaterial's
 * albedo/normal/metallic_roughness handles are still parsed-but-unread (see mesh_renderer.h);
 * only the alpha mask goes through this path. Every non-masked material (Opaque, Blend, or a
 * Mask material with no texture) binds a permanent 1x1 white fallback instead, which makes the
 * fragment-shader alpha test `albedo.a * texture(mask, uv).a` collapse back to plain
 * `albedo.a` -- so wiring this in changes nothing for existing materials.
 */

#ifndef TOYENGINE_RENDER_MATERIAL_TEXTURE_CACHE_H
#define TOYENGINE_RENDER_MATERIAL_TEXTURE_CACHE_H

#include <volk/volk.h>
#include <memory>
#include <unordered_map>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/sampler_desc.h>
#include <gfxcoopa/types/enums.h>

namespace toy {
namespace render {

/**
 * @class MaterialTextureCache
 * @brief Lazily allocates and caches one DescriptorSet per alpha-mask Texture, keyed on the
 * Texture's address.
 *
 * Descriptor sets are allocated (and their one binding written via bind_image(), which does an
 * immediate vkUpdateDescriptorSets) the first time a given material's texture is seen, never
 * re-written after that. This is what makes lazy allocation safe under
 * MAX_FRAMES_IN_FLIGHT-overlapped command buffers, the same hazard
 * PixelRenderPipeline's own construction-time comments describe for ssao_pass_: a *newly
 * allocated* descriptor set is by construction referenced by no in-flight command buffer, and
 * an already-cached set is never rebound, so no command buffer ever observes a set update while
 * still executing.
 */
class MaterialTextureCache {
public:
    /// Sets allocated for distinct alpha-mask textures. Sized well above the pixel_demo scene's
    /// needs (one masked material today) -- one set per unique mask Texture*, not per material
    /// instance, so this only grows with authored texture variety, not object count.
    static constexpr uint32_t kMaxMaskSets = 64;

    MaterialTextureCache(coopa::gfx::core::Device& device,
                         coopa::gfx::memory::Allocator& allocator,
                         coopa::gfx::command::CommandPool& cmd_pool)
        : device_(device)
    {
        layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .build(device));

        // +1 for white_set_ below, allocated from the same pool.
        pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*layout_, kMaxMaskSets + 1).build(device));

        const uint8_t white_pixel[4] = {255, 255, 255, 255};
        white_texture_ = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(
                device, allocator, cmd_pool, white_pixel, 1, 1,
                coopa::gfx::Format::RGBA8_Unorm, coopa::gfx::SamplerDesc::pixel_art()));

        white_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, *layout_);
        white_set_->bind_image(0, white_texture_->view_typed(), white_texture_->sampler_object());
    }

    /** @brief The layout shared by every set this cache hands out -- pass to a pipeline's ctor. */
    VkDescriptorSetLayout layout() const { return layout_->handle(); }

    /** @brief Same layout as layout(), as the sealed DescriptorSetLayout object -- for ctors
     * (e.g. ShadowPipeline's) that take `const DescriptorSetLayout*` instead of a raw handle. */
    const coopa::gfx::pipeline::DescriptorSetLayout& layout_object() const { return *layout_; }

    /**
     * @brief Returns the descriptor set to bind at the material set index for `material`.
     *
     * The white 1x1 fallback for anything other than a Mask material with a loaded
     * texture_alpha_mask; otherwise the (lazily allocated, then cached) set for that texture.
     *
     * @param material The renderer's material.
     * @return The DescriptorSet to bind. Owned by this cache; valid as long as it is.
     */
    const coopa::gfx::pipeline::DescriptorSet& set_for(const coopa::gfx::engine::components::PBRMaterial& material) {
        if (!material.has_alpha_mask()) {
            return *white_set_;
        }
        const auto* texture = material.alpha_mask_handle.get();
        auto it = mask_sets_.find(texture);
        if (it != mask_sets_.end()) {
            return *it->second;
        }
        auto set = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *pool_, *layout_);
        set->bind_image(0, texture->view_typed(), texture->sampler_object());
        auto [inserted, _] = mask_sets_.emplace(texture, std::move(set));
        return *inserted->second;
    }

private:
    coopa::gfx::core::Device& device_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      pool_;
    std::unique_ptr<coopa::gfx::engine::data::Texture>         white_texture_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       white_set_;
    std::unordered_map<const coopa::gfx::engine::data::Texture*,
                       std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> mask_sets_;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_MATERIAL_TEXTURE_CACHE_H
