/**
 * @file cloud_shadow_pass.h
 * @brief The cloud layer's shadow map (render cloud_shadows): the directional light's
 *        transmittance through the layer, on the layer's base plane, over a square around the
 *        camera -- rebuilt every frame from the same field the clouds are drawn with, so the
 *        shadows move, grow and thin exactly as the clouds do.
 *
 * One kSize^2 R16F target, two fragment stages:
 *   - volumetric (cloud_shadow_volumetric.frag): a march toward the light through the slab over
 *     SkyCloudPass's density field, reading its per-frame CloudFrame buffer;
 *   - flat (cloud_shadow_flat.frag): the toon layer's field (flat_cloud_field.glsl), reading
 *     CloudOverlayPass's per-frame FlatCloudFrame buffer.
 * The map's square is snapped to whole texels so it does not swim as the camera moves, and
 * nothing in it is jittered, so the shadows it casts cannot flicker. cloud_shadow.glsl samples it
 * for every lit surface, the water, the particles and the volumetric fog; the pipeline binds
 * view() once, at binding 4 of the shared shadow set.
 *
 * The target exists from construction (cleared to "no shadow" once by initialize()), so
 * render cloud_shadows switches live; while it is off nothing is recorded and the light UBO's
 * strength of 0 skips the lookup.
 */

#ifndef TOYENGINE_RENDER_PASSES_CLOUD_SHADOW_PASS_H
#define TOYENGINE_RENDER_PASSES_CLOUD_SHADOW_PASS_H

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class CloudShadowPass
 * @brief See the file doc.
 */
class CloudShadowPass {
public:
    static constexpr uint32_t kSize = 512;
    static constexpr int kVolumetricSteps = 16;

    /// Both shadow shaders' push constants.
    struct alignas(16) Push {
        glm::vec4 map{0.0f};     ///< xy the map's corner (world), z its side (world m), w march steps
        glm::vec4 light{0.0f, 0.0f, 1.0f, 0.0f};   ///< xyz unit direction TO the light, w the map's resolution
    };
    static_assert(sizeof(Push) == 32, "CloudShadowPass::Push must match the cloud shadow shaders' push constants");

    /// Where the map lies this frame: its corner and side (world), snapped to whole texels.
    struct Placement {
        glm::vec2 corner{0.0f};
        float side = 1.0f;
    };

    CloudShadowPass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator, uint32_t frames_in_flight,
                    const std::string& vert_spv, const std::string& volumetric_spv, const std::string& flat_spv)
        : frames_(std::max(frames_in_flight, 1u)),
          linear_(coopa::gfx::engine::util::Sampler::linear(device)),
          target_(device, allocator, kSize, kSize, coopa::gfx::Format::R16_Sfloat, coopa::gfx::engine::targets::kColorOnly),
          volumetric_stage_(device, target_.render_pass_object(), describe_volumetric_(vert_spv, volumetric_spv, frames_)),
          flat_stage_(device, target_.render_pass_object(), describe_flat_(vert_spv, flat_spv, frames_)) {}

    CloudShadowPass(const CloudShadowPass&) = delete;
    CloudShadowPass& operator=(const CloudShadowPass&) = delete;

    /**
     * @brief Binds the cloud noise and both per-frame parameter buffers (once, at construction):
     *        `cloud_frames[i]` is SkyCloudPass's CloudFrame buffer for frame slot i,
     *        `flat_frames[i]` CloudOverlayPass's FlatCloudFrame buffer.
     */
    void set_inputs(VkImageView weather, VkImageView shape, VkImageView detail, VkSampler noise_sampler,
                    const std::vector<const coopa::gfx::memory::Buffer*>& cloud_frames,
                    const std::vector<const coopa::gfx::memory::Buffer*>& flat_frames);

    coopa::gfx::TextureView view() const { return target_.color_view_typed(); }
    /// The sampler to bind view() with: bilinear, clamped (the lookup fades out before the edge).
    const coopa::gfx::engine::util::Sampler& sampler() const { return linear_; }

    /** @brief Clears the map to "no shadow" once, so the bound image is laid out. */
    void initialize(coopa::gfx::command::CommandBuffer& cmd);

    /**
     * @brief The map's square for a camera at `camera_xy`: `side` metres, centred on the camera,
     *        its corner snapped to whole texels so the shadows hold still under a moving camera.
     */
    static Placement place(glm::vec2 camera_xy, float side);

    /** @brief Renders the map from the volumetric clouds (their CloudFrame for `frame_slot` already uploaded). Outside any render pass. */
    void render_volumetric(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot, const Placement& pl, glm::vec3 light_to) {
        render_(cmd, volumetric_stage_, frame_slot, pl, light_to, kVolumetricSteps);
    }

    /** @brief Renders the map from the flat clouds (their FlatCloudFrame for `frame_slot` already uploaded). Outside any render pass. */
    void render_flat(coopa::gfx::command::CommandBuffer& cmd, uint32_t frame_slot, const Placement& pl, glm::vec3 light_to) {
        render_(cmd, flat_stage_, frame_slot, pl, light_to, 0);
    }

    uint64_t renders() const { return renders_; }   ///< How many times the map was rendered (tests).

private:
    void render_(coopa::gfx::command::CommandBuffer& cmd, coopa::gfx::engine::passes::FullscreenStage& stage,
                 uint32_t frame_slot, const Placement& pl, glm::vec3 light_to, int steps);

    static coopa::gfx::engine::passes::FullscreenStageDesc describe_volumetric_(
            const std::string& vert_spv, const std::string& frag_spv, uint32_t frames);
    static coopa::gfx::engine::passes::FullscreenStageDesc describe_flat_(
            const std::string& vert_spv, const std::string& frag_spv, uint32_t frames);

    uint32_t                                     frames_;
    coopa::gfx::engine::util::Sampler            linear_;
    coopa::gfx::engine::targets::OffscreenTarget target_;
    coopa::gfx::engine::passes::FullscreenStage  volumetric_stage_;
    coopa::gfx::engine::passes::FullscreenStage  flat_stage_;
    uint64_t renders_ = 0;
    bool initialized_ = false;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_CLOUD_SHADOW_PASS_H
