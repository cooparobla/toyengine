/**
 * @file material_texture_cache.h
 * @brief Owns the "material" descriptor set (4 combined-image-sampler bindings: alpha mask,
 * albedo, normal, metallic-roughness) shared by every textured pass -- G-buffer, shadow,
 * forward transparent, probe capture -- and lazily allocates one
 * DescriptorSet per distinct 4-texture combination a material references.
 *
 * One cache serves all four texture slots across every consuming pass, so gfxcoopa's own
 * probe-capture path (gi/gi_system.h, passes/probe_capture_pass.h) and toyengine's G-buffer/shadow/transparent passes
 * share descriptor sets instead of each maintaining its own.
 *
 * Every material -- textured or not -- binds a full 4-tuple set. Untextured slots bind a
 * neutral fallback (white for alpha_mask/albedo/metallic_roughness, flat-up for normal) instead
 * of leaving a binding unwritten, which is what makes wiring this cache into a new pass a no-op
 * for every material that doesn't use it: `albedo.a * texture(mask, uv).a` collapses to
 * `albedo.a`, `albedo.rgb * texture(albedo_map, uv).rgb` collapses to `albedo.rgb`, and
 * `TBN * (texture(normal_map, uv).xyz * 2 - 1)` collapses to the interpolated geometric normal.
 */

#ifndef GFXCOOPA_ENGINE_UTIL_MATERIAL_TEXTURE_CACHE_H
#define GFXCOOPA_ENGINE_UTIL_MATERIAL_TEXTURE_CACHE_H

#include <volk/volk.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/types/format.h>
#include <gfxcoopa/types/sampler_desc.h>
#include <gfxcoopa/types/enums.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace util {

/**
 * @class MaterialTextureCache
 * @brief Lazily allocates and caches one DescriptorSet per distinct (alpha_mask, albedo,
 * normal, metallic_roughness) texture combination, keyed on the four Textures' addresses.
 *
 * Descriptor sets are allocated (and all four bindings written via bind_image(), which does an
 * immediate vkUpdateDescriptorSets) the first time a given material's texture combination is
 * seen, never re-written after that. This is what makes lazy allocation safe under
 * MAX_FRAMES_IN_FLIGHT-overlapped command buffers, the same hazard
 * ToyRenderPipeline's own construction-time comments describe for ssao_pass_: a *newly
 * allocated* descriptor set is by construction referenced by no in-flight command buffer, and
 * an already-cached set is never rebound, so no command buffer ever observes a set update while
 * still executing.
 */
class MaterialTextureCache {
public:
    /// Binding indices within the material set's layout, in the order every consuming shader
    /// (gbuffer_fs.glsl, transparent_fs.glsl, probe_capture.frag) declares
    /// them. alpha_mask is at binding 0 so gfx/surface/shadow_fs.glsl and shadow_cube_fs.glsl,
    /// which sample only the mask, can share this layout: a descriptor set layout may declare
    /// bindings a given shader never samples.
    static constexpr uint32_t kAlphaMaskBinding         = 0;
    static constexpr uint32_t kAlbedoBinding            = 1;
    static constexpr uint32_t kNormalBinding            = 2;
    static constexpr uint32_t kMetallicRoughnessBinding = 3;
    /// Height map a tessellated draw displaces along the normal (r * displacement_scale) --
    /// read in the tessellation EVALUATION stage only; black (no displacement) by default.
    static constexpr uint32_t kDisplacementBinding      = 4;
    static constexpr uint32_t kBindingCount             = 5;

    /// Sets allocated for distinct texture combinations. Sized well above any one scene's
    /// authored texture variety -- one set per unique 4-tuple of Texture*, not per material
    /// instance, so this only grows with authored texture variety, not object count.
    static constexpr uint32_t kMaxMaterialSets = 64;

    MaterialTextureCache(coopa::gfx::core::Device& device,
                         coopa::gfx::memory::Allocator& allocator,
                         coopa::gfx::command::CommandPool& cmd_pool)
        : device_(device)
    {
        // The displacement map is the evaluation stage's -- and, for surfaces that displace a
        // whole grid from it (toyengine's heightmap terrain), the vertex and control stages'
        // too: an untessellated draw displaces per vertex, and the control stage sizes edges
        // from the displaced endpoints. On a device without tessellation only the vertex and
        // fragment stages exist, but the binding stays so every layout matches.
        const coopa::gfx::ShaderStage disp_stages = device.supports_tessellation()
            ? coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::TessControl |
              coopa::gfx::ShaderStage::TessEval | coopa::gfx::ShaderStage::Fragment
            : coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment;
        layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(kAlphaMaskBinding, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(kAlbedoBinding, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(kNormalBinding, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(kMetallicRoughnessBinding, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(kDisplacementBinding, disp_stages)
                .build(device));

        // +1 for fallback_set_ below, allocated from the same pool.
        pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*layout_, kMaxMaterialSets + 1).build(device));

        const uint8_t white_pixel[4] = {255, 255, 255, 255};
        white_texture_ = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(
                device, allocator, cmd_pool, white_pixel, 1, 1,
                coopa::gfx::Format::RGBA8_Unorm, coopa::gfx::SamplerDesc::pixel_art()));

        // Tangent-space "no bump" normal: (0, 0, 1) encoded as unsigned [0,1] -> (0.5, 0.5, 1.0)
        // -> (128, 128, 255). Decodes to (0.0039, 0.0039, 1.0) rather than exactly (0, 0, 1) -- a
        // 0.32-degree tilt from 128/255 vs. the mathematically exact 127.5, far below the width
        // of a lighting band in toyengine's toy_lighting.frag. Documented, not branched around.
        const uint8_t flat_normal_pixel[4] = {128, 128, 255, 255};
        flat_normal_texture_ = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(
                device, allocator, cmd_pool, flat_normal_pixel, 1, 1,
                coopa::gfx::Format::RGBA8_Unorm, coopa::gfx::SamplerDesc::pixel_art()));

        const uint8_t black_pixel[4] = {0, 0, 0, 255};
        black_texture_ = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(
                device, allocator, cmd_pool, black_pixel, 1, 1,
                coopa::gfx::Format::RGBA8_Unorm, coopa::gfx::SamplerDesc::pixel_art()));

        fallback_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, *layout_);
        fallback_set_->bind_image(kDisplacementBinding, black_texture_->view_typed(), black_texture_->sampler_object());
        fallback_set_->bind_image(kAlphaMaskBinding, white_texture_->view_typed(), white_texture_->sampler_object());
        fallback_set_->bind_image(kAlbedoBinding, white_texture_->view_typed(), white_texture_->sampler_object());
        fallback_set_->bind_image(kNormalBinding, flat_normal_texture_->view_typed(), flat_normal_texture_->sampler_object());
        fallback_set_->bind_image(kMetallicRoughnessBinding, white_texture_->view_typed(), white_texture_->sampler_object());
    }

    /** @brief The layout shared by every set this cache hands out -- pass to a pipeline's ctor. */
    VkDescriptorSetLayout layout() const { return layout_->handle(); }

    /** @brief Same layout as layout(), as the sealed DescriptorSetLayout object -- for ctors
     * (e.g. ShadowPipeline's) that take `const DescriptorSetLayout*` instead of a raw handle. */
    const coopa::gfx::pipeline::DescriptorSetLayout& layout_object() const { return *layout_; }

    /**
     * @brief Returns the descriptor set to bind at the material set index for `material`.
     *
     * Each of the four slots binds its loaded texture if present, or the shared neutral
     * fallback (white, white, flat-normal, white) otherwise -- see has_alpha_mask()/
     * has_albedo_map()/has_normal_map()/has_metallic_roughness_map(). Note has_alpha_mask()
     * additionally gates on alpha_mode == Mask (a Blend or Opaque material's alpha_mask_handle,
     * if any, is ignored); the other three slots have no such gate.
     *
     * @param material The renderer's material.
     * @return The DescriptorSet to bind. Owned by this cache; valid as long as it is.
     */
    /** @brief The all-neutral set (white albedo / mask / metal-rough, flat normal): a material
     *         drawn as if it had no textures -- e.g. an editor's Solid shading. */
    const coopa::gfx::pipeline::DescriptorSet& untextured_set() const { return *fallback_set_; }

    /** @brief untextured_set() but keeping `material`'s displacement map: Solid shading drops the
     *         colour maps, not the shape a tessellated surface is displaced into. */
    const coopa::gfx::pipeline::DescriptorSet& untextured_set_for(const coopa::gfx::engine::components::PBRMaterial& material) {
        return set_for_key_({nullptr, nullptr, nullptr, nullptr,
                             material.has_displacement_map() ? material.displacement_handle.get() : nullptr});
    }

    const coopa::gfx::pipeline::DescriptorSet& set_for(const coopa::gfx::engine::components::PBRMaterial& material) {
        return set_for_key_({
            material.has_alpha_mask() ? material.alpha_mask_handle.get() : nullptr,
            material.has_albedo_map() ? material.albedo_handle.get() : nullptr,
            material.has_normal_map() ? material.normal_handle.get() : nullptr,
            material.has_metallic_roughness_map() ? material.metallic_roughness_handle.get() : nullptr,
            material.has_displacement_map() ? material.displacement_handle.get() : nullptr,
        });
    }

    /**
     * @brief Forgets every set that references `texture`, which its owner is about to destroy.
     *
     * Sets are keyed by Texture ADDRESS, so a texture freed and a new one later allocated at the
     * same address would otherwise be handed the old set -- with a dangling image view. A
     * released set may still be referenced by command buffers in flight, so it is parked and
     * freed only after kRetireFrames further tick() calls. Call this while `texture` is still
     * alive (the address must not have been reused yet).
     */
    void release(const coopa::gfx::engine::data::Texture* texture) {
        if (texture == nullptr) return;
        for (auto it = sets_.begin(); it != sets_.end();) {
            if (std::find(it->first.begin(), it->first.end(), texture) != it->first.end()) {
                graveyard_.push_back({kRetireFrames, std::move(it->second)});
                it = sets_.erase(it);
            } else {
                ++it;
            }
        }
    }

    /** @brief Advances the release() grace period; call once per rendered frame. */
    void tick() {
        for (auto it = graveyard_.begin(); it != graveyard_.end();) {
            if (it->frames_left == 0) {
                it = graveyard_.erase(it);
            } else {
                --it->frames_left;
                ++it;
            }
        }
    }

    /** @brief Frames a released set outlives its release() (frames in flight, with margin). */
    static constexpr uint32_t kRetireFrames = 4;

private:
    using Key = std::array<const coopa::gfx::engine::data::Texture*, kBindingCount>;

    struct Retired {
        uint32_t frames_left = 0;
        std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> set;
    };

    const coopa::gfx::pipeline::DescriptorSet& set_for_key_(const Key& key) {
        if (key[0] == nullptr && key[1] == nullptr && key[2] == nullptr && key[3] == nullptr && key[4] == nullptr) {
            return *fallback_set_;
        }
        auto it = sets_.find(key);
        if (it != sets_.end()) {
            return *it->second;
        }
        auto set = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *pool_, *layout_);
        bind_slot_(*set, kAlphaMaskBinding, key[0]);
        bind_slot_(*set, kAlbedoBinding, key[1]);
        bind_slot_(*set, kNormalBinding, key[2]);
        bind_slot_(*set, kMetallicRoughnessBinding, key[3]);
        bind_slot_(*set, kDisplacementBinding, key[4]);
        auto [inserted, _] = sets_.emplace(key, std::move(set));
        return *inserted->second;
    }

    /// Binds `texture` at `binding` if non-null, else this cache's fallback for that binding
    /// (flat-normal at kNormalBinding, white everywhere else).
    void bind_slot_(coopa::gfx::pipeline::DescriptorSet& set, uint32_t binding,
                    const coopa::gfx::engine::data::Texture* texture) {
        const coopa::gfx::engine::data::Texture& t =
            texture ? *texture : (binding == kNormalBinding ? *flat_normal_texture_
                                : binding == kDisplacementBinding ? *black_texture_ : *white_texture_);
        set.bind_image(binding, t.view_typed(), t.sampler_object());
    }

    coopa::gfx::core::Device& device_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      pool_;
    std::unique_ptr<coopa::gfx::engine::data::Texture>         white_texture_;
    std::unique_ptr<coopa::gfx::engine::data::Texture>         flat_normal_texture_;
    std::unique_ptr<coopa::gfx::engine::data::Texture>         black_texture_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       fallback_set_;
    std::map<Key, std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> sets_;
    std::vector<Retired> graveyard_;
};

} // namespace util
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_UTIL_MATERIAL_TEXTURE_CACHE_H
