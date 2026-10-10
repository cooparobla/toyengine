/**
 * @file cloud_overlay_pass.h
 * @brief The cloud layer drawn over the finished scene's geometry: one fullscreen draw after the
 *        translucent pass (so clouds below a high camera also cover water and other BLEND
 *        surfaces), blended premultiplied. Two looks (render cloud_type):
 *          - volumetric (cloud_composite.frag): SkyCloudPass's reconstructed layer upsampled over
 *            the geometry pixels, depth-tested against each surface (the lighting pass already
 *            composited it over the sky);
 *          - flat (flat_clouds.frag): the toon layer, every pixel -- a heightfield from above, a
 *            deck from below -- over the weather map and shape volume SkyCloudPass bakes.
 *
 * Owns the flat layer's per-frame parameter buffer (FlatFrame, one per frame in flight), which
 * CloudShadowPass's flat stage reads too, so the shadows are the clouds drawn. Built against the
 * translucent pass's render pass; the flat look is lit from the camera and light sets (sets 0
 * and 1: the scene's sun and sky colours), so it works with either sky model.
 */

#ifndef TOYENGINE_RENDER_PASSES_CLOUD_OVERLAY_PASS_H
#define TOYENGINE_RENDER_PASSES_CLOUD_OVERLAY_PASS_H

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/passes/fullscreen_stage.h>

#include <toyengine/render/sky_state.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class CloudOverlayPass
 * @brief See the file doc.
 */
class CloudOverlayPass {
public:
    /// flat_cloud_field.glsl's FlatCloudFrame block (std140).
    struct alignas(16) FlatFrame {
        glm::vec4 layer{0.0f};    ///< base height, puff height, puff size, opacity
        glm::vec4 drift0{0.0f};   ///< xy the groups' drift, zw the warp's (m, each wrapped to its period)
        glm::vec4 drift1{0.0f};   ///< xy the first puff octave's drift, zw the second's
        glm::vec4 look{0.0f};     ///< coverage, light bands, outline, camera fade
        glm::vec4 flow{0.0f};     ///< turbulence, evolution phase (0..1), time
    };
    static_assert(sizeof(FlatFrame) == 80, "CloudOverlayPass::FlatFrame must match flat_cloud_field.glsl's FlatCloudFrame");

    /// cloud_composite.frag's push constants.
    struct alignas(16) CompositePush {
        glm::vec4 camera{0.0f};   ///< xyz camera position, w cloud scale
        glm::vec4 region{1.0f, 0.0f, 0.0f, 0.0f};   ///< x the fraction of the cloud target the march filled
    };

    /// The periods (in puff sizes) of the flat field's reads -- flat_cloud_field.glsl's FLAT_*_PERIOD.
    static constexpr double kGroupPeriod = 18.0, kWarpPeriod = 14.0, kPuff0Period = 3.2, kPuff1Period = 1.7;
    /// Each read's drift speed relative to the wind: the warp lags, the finer puffs run ahead.
    static constexpr double kWarpSpeed = 0.6, kPuff1Speed = 1.35;

    /**
     * @brief The flat layer's parameters for a frame. Every drift offset is the wind's
     *        accumulated drift (double precision) times its read's speed, wrapped to that read's
     *        own period, so the field is seamless however long the wind blows.
     */
    static FlatFrame flat_frame_of(const CloudFrameState& st);

    CloudOverlayPass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator, VkRenderPass render_pass,
                     uint32_t frames_in_flight, const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                     const coopa::gfx::pipeline::DescriptorSetLayout& light_layout, const std::string& vert_spv,
                     const std::string& flat_spv, const std::string& composite_spv);

    CloudOverlayPass(const CloudOverlayPass&) = delete;
    CloudOverlayPass& operator=(const CloudOverlayPass&) = delete;

    /** @brief Binds the G-buffer, the cloud noise and the volumetric layer (once, at construction). */
    void set_inputs(coopa::gfx::TextureView g_normal, coopa::gfx::TextureView g_position,
                    VkImageView weather, VkImageView shape, VkSampler noise_sampler,
                    coopa::gfx::TextureView volumetric_layer);

    /// The flat layer's per-frame-slot buffer (CloudShadowPass's flat stage reads it too).
    const coopa::gfx::memory::Buffer& flat_buffer(uint32_t slot) const { return *flat_ubos_[slot % frames_]; }

    /** @brief Uploads the flat layer's parameters for `frame_slot` (before its shadow map or draw). */
    void upload_flat(uint32_t frame_slot, const FlatFrame& f) { flat_ubos_[frame_slot % frames_]->upload(&f, sizeof(f)); }

    /// @brief Records the flat layer. Must be inside the render pass given at construction.
    void draw_flat(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot,
                   const coopa::gfx::pipeline::DescriptorSet& camera_set,
                   const coopa::gfx::pipeline::DescriptorSet& light_set, const glm::mat4& inv_view_proj,
                   uint32_t width, uint32_t height) const;

    /// @brief Records the volumetric layer over geometry. Must be inside the render pass given at construction.
    void draw_composite(coopa::gfx::command::CommandBuffer& cmd, const CompositePush& p, uint32_t width, uint32_t height) const;

private:
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_flat_(
            const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
            const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
            const std::string& vert_spv, const std::string& frag_spv, uint32_t frames);
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_composite_(const std::string& vert_spv,
                                                                               const std::string& frag_spv);

    uint32_t                                    frames_;
    coopa::gfx::engine::util::Sampler           nearest_;
    coopa::gfx::engine::passes::FullscreenStage flat_stage_;
    coopa::gfx::engine::passes::FullscreenStage composite_stage_;
    std::vector<std::unique_ptr<coopa::gfx::memory::Buffer>> flat_ubos_;   ///< FlatFrame, one per frame in flight
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_CLOUD_OVERLAY_PASS_H
