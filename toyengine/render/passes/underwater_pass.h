/**
 * @file underwater_pass.h
 * @brief The underwater look: fog, colour absorption, caustics and a slight shimmer for every
 *        pixel whose view ray starts below a water surface.
 *
 * Runs in linear HDR right after the transparent pass and ahead of global fog, reading the
 * post chain's source image and writing its own target (pipeline::RenderPass clears on load, so
 * it cannot composite in place). It runs EVERY frame once constructed -- the views downstream
 * read are bound once (ToyRenderPipeline's frame-overlap rule) -- and is a plain copy when the
 * camera is above water (`enabled` = 0 in the push constants).
 *
 * Per pixel (see underwater.frag): the view ray starts at the near plane. If that point is above
 * the water level the pixel is untouched, which is what splits the image at the waterline when
 * the camera straddles the surface. Otherwise the ray travels through water until it hits
 * geometry (G-buffer position) or leaves through the surface plane, and only that in-water length
 * is fogged and absorbed. So the surface seen from below, and whatever is above it, reads through
 * just the water between, rather than vanishing into fog as the sky behind it would.
 *
 * Every parameter rides in one 128-byte push-constant block (exactly Vulkan's guaranteed
 * minimum), so there is no per-frame UBO to go stale.
 */

#ifndef TOYENGINE_RENDER_PASSES_UNDERWATER_PASS_H
#define TOYENGINE_RENDER_PASSES_UNDERWATER_PASS_H

#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/passes/fullscreen_stage.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class UnderwaterPass
 * @brief See the file doc.
 */
class UnderwaterPass {
public:
    /// Matches underwater.frag's push-constant block byte for byte.
    struct alignas(16) Params {
        glm::mat4 inv_view_proj{1.0f};                  ///< Unjittered.
        glm::vec4 camera_time{0.0f};                     ///< xyz = camera position (or, water.w == 2, the surface's up), w = time (s).
        glm::vec4 water{0.0f};                           ///< x = surface level, y = density (1/m), z = caustics, w = enabled (2: a planet's water -- underwater_planet.frag, level along camera_time.xyz).
        glm::vec4 fog_color{0.05f, 0.24f, 0.28f, 1.0f};  ///< rgb = in-scatter colour, w = shimmer amount.
        glm::vec4 absorption{0.35f, 0.09f, 0.06f, 1.0f}; ///< rgb = per-metre extinction, w = light intensity.
    };
    static_assert(sizeof(Params) == 128, "UnderwaterPass::Params must fill exactly the 128-byte push range");

    /** @param planet_frag_spv underwater_planet.frag: drawn instead when the params say the
     *         surface tilts (water.w == 2, a planet's water). */
    UnderwaterPass(coopa::gfx::core::Device& device, coopa::gfx::pipeline::RenderPass& target_pass,
                   const std::string& vert_spv, const std::string& frag_spv, const std::string& planet_frag_spv)
        : nearest_sampler_(coopa::gfx::engine::util::Sampler::nearest(device)),
          stage_(device, target_pass, describe_(vert_spv, frag_spv)),
          planet_stage_(device, target_pass, describe_(vert_spv, planet_frag_spv)) {}

    UnderwaterPass(const UnderwaterPass&) = delete;
    UnderwaterPass& operator=(const UnderwaterPass&) = delete;

    /** @brief Binds the scene colour (linear, for the shimmer offset) and G-buffer normal/position
     *         (nearest -- see FogPass on why filtered world positions are wrong at silhouettes). */
    void set_source_images(coopa::gfx::TextureView scene_color, coopa::gfx::TextureView g_normal,
                           coopa::gfx::TextureView g_position,
                           const coopa::gfx::engine::util::Sampler& linear_sampler);

    void draw(coopa::gfx::command::CommandBuffer& cmd, uint32_t width, uint32_t height, const Params& params) const {
        (params.water.w > 1.5f ? planet_stage_ : stage_).draw(cmd, width, height, coopa::gfx::ShaderStage::Fragment, params);
    }

private:
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_(const std::string& vert_spv,
                                                                     const std::string& frag_spv);

    coopa::gfx::engine::util::Sampler           nearest_sampler_;
    coopa::gfx::engine::passes::FullscreenStage stage_;
    coopa::gfx::engine::passes::FullscreenStage planet_stage_;   ///< underwater_planet.frag
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_UNDERWATER_PASS_H
