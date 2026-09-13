/**
 * @file forward_globals.h
 * @brief Per-frame-in-flight UBO for the forward MESH transparent pass's
 *        lighting/indirect/SSR/refraction tuning.
 *
 * A UBO rather than a push constant because the per-object refraction fields
 * (TransparentRefractionPushConstants) and this block together would put transparent.frag's
 * push-constant block over Vulkan's guaranteed 128-byte minimum. gfxcoopa's SdfGlobals hit
 * the same wall on the SDF forward path and resolved it the same way.
 *
 * Per-slot buffers, plus one descriptor set per slot bound ONCE at construction, for the
 * reason every other per-frame-in-flight buffer in this engine is per-slot: the pipeline
 * overlaps MAX_FRAMES_IN_FLIGHT command buffers with no wait, so a single shared UBO would
 * let frame N's upload race frame N-1's still-in-flight draw.
 *
 * Mesh-only: the SDF forward path keeps reading its own SdfGlobals UBO, since SDF glass is
 * deliberately excluded from refraction.
 */

#ifndef TOYENGINE_RENDER_FORWARD_GLOBALS_H
#define TOYENGINE_RENDER_FORWARD_GLOBALS_H

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/presentation/renderer.h>

namespace toy {
namespace render {

/**
 * @struct ForwardGlobals
 * @brief std140-aligned per-frame globals for transparent.frag: the lighting and indirect/SSR
 *        terms, plus two refraction vec4s.
 */
struct alignas(16) ForwardGlobals {
    glm::vec4  lighting0 = glm::vec4(4.0f, 0.55f, 0.0f, 0.0f); /**< x=light_bands, y=spec_threshold, z=soft_lighting, w=rim_strength. */
    glm::vec4  lighting1 = glm::vec4(1.0f, 1.0f, 0.0f, 0.0f);  /**< x=ambient_intensity, y=sky_intensity, z=ssr_enabled, w=ssgi_intensity. */
    glm::vec4  ssr0       = glm::vec4(0.5f, 15.0f, 3.5f, 0.05f); /**< x=ssgi_distance, y=ssr_max_distance, z=ssr_bias_texels, w=ssr_thickness_min. */
    glm::vec4  ssr1       = glm::vec4(0.01f, 1.0f, 0.0f, 0.0f);  /**< x=ssr_thickness_scale, y=ssr_roughness_cutoff, zw unused. */
    glm::ivec4 ssr_steps  = glm::ivec4(64, 0, 0, 1);              /**< x=ssr_max_iterations, y=ssr_max_hiz_mip, z=ssr_start_mip, w=ssr_min_mip0_steps. */
    glm::ivec4 ssr_mip    = glm::ivec4(0, 0, 0, 0);               /**< x=ssr_max_color_mip, yzw unused. */
    glm::vec4  refract0   = glm::vec4(0.0f, 1.0f, 0.08f, 0.0f);   /**< x=enabled, y=strength, z=max_offset, w=chromatic. */
    glm::vec4  refract1   = glm::vec4(1.0f, 1.0f, 1.0f, 0.0f);    /**< x=blur, y=density, z=fresnel_enabled, w unused. */
};

/**
 * @class ForwardGlobalsData
 * @brief Owns, per frame-in-flight slot, the ForwardGlobals UBO and its
 * bound-once descriptor set.
 *
 * Usage: fill globals() then upload(frame_index) once per frame, before
 * record_transparent_() binds set(frame_index) as transparent_pass_'s
 * trailing extra set.
 */
class ForwardGlobalsData {
public:
    static constexpr uint32_t kFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;

    ForwardGlobalsData(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator) {
        buffers_.reserve(kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            buffers_.push_back(coopa::gfx::memory::Buffer::uniform(device, allocator, sizeof(ForwardGlobals)));
        }

        layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .uniform_buffer(0, coopa::gfx::ShaderStage::Fragment)
                .build(device));

        pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*layout_, kFrames).build(device));

        sets_.reserve(kFrames);
        for (uint32_t i = 0; i < kFrames; ++i) {
            sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, *layout_));
            sets_[i]->bind_buffer(0, buffers_[i]);
        }
    }

    ForwardGlobalsData(const ForwardGlobalsData&) = delete;
    ForwardGlobalsData& operator=(const ForwardGlobalsData&) = delete;

    /** @brief Records this frame's slot -- call once per frame before globals()/upload(). */
    void begin(uint32_t frame_index) { frame_index_ = frame_index; }

    /** @brief Mutable access to this frame's globals -- fill then upload(). */
    ForwardGlobals& globals() { return globals_; }

    /** @brief Uploads this frame's globals to the slot most recently begin()'d. */
    void upload() {
        buffers_[frame_index_].upload(&globals_, sizeof(ForwardGlobals));
    }

    /** @brief The descriptor set -- bound once at construction -- for the slot most recently
     *         begin()'d. Bound at every mesh transparent draw. */
    coopa::gfx::pipeline::DescriptorSet& current_set() const { return *sets_[frame_index_]; }

    /** @brief The layout every set() shares -- for building a pipeline's descriptor_layouts list. */
    const coopa::gfx::pipeline::DescriptorSetLayout& layout() const { return *layout_; }

private:
    uint32_t frame_index_ = 0;
    ForwardGlobals globals_;

    std::vector<coopa::gfx::memory::Buffer> buffers_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      pool_;
    std::vector<std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> sets_;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_FORWARD_GLOBALS_H
