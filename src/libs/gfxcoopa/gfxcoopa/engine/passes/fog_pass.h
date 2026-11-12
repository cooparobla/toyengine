/**
 * @file fog_pass.h
 * @brief Fullscreen global-fog composite over the OPAQUE scene, in place (the caller supplies
 *        fog.frag; see toyengine's assets/shaders/fog.frag).
 *
 * The Unreal/HDRP split: this pass fogs opaque geometry and sky by the G-buffer distance, and
 * every forward (translucent) shader fogs its own fragment at its own distance afterwards.
 * So it must run after lighting and BEFORE translucency is drawn.
 *
 * Draws in place: the fragment shader outputs premultiplied (in-scatter, 1 - transmittance) and
 * the pipeline blends src + dst * (1 - srcA) = dst * T + in-scatter. That needs a render pass that
 * LOADS the existing colour, which pipeline::RenderPass (hardcoded LOAD_OP_CLEAR) cannot give, so
 * the stage is built against a caller-supplied raw VkRenderPass -- in toyengine,
 * TransparentPass's own (LOAD on colour and depth), which is how the pass reopens the live HDR
 * image with no extra target or copy.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_FOG_PASS_H
#define GFXCOOPA_ENGINE_PASSES_FOG_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>

#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/detail/vk_convert.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/engine/util/sampler.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class FogPass
 * @brief Blends global fog in place over the opaque scene colour.
 *
 * Set 0 (owned): G-buffer normal and position. Set 1: the caller's light set, which carries
 * the fog parameters (LightUBO's fog block) -- per frame-in-flight, so the fog never reads a
 * stale camera or a half-written block. Push constants: the camera ray reconstruction.
 */
class FogPass {
public:
    /// Fragment push constants: clip -> world for the per-pixel view ray, and the camera.
    struct PushConstants {
        glm::mat4 inv_view_proj = glm::mat4(1.0f);
        glm::vec4 camera_pos    = glm::vec4(0.0f);  ///< xyz = world-space camera position.
    };

    /**
     * @brief Builds the fog pipeline.
     * @param device       Logical device.
     * @param render_pass  A render pass that LOADS the HDR colour it draws into (one colour
     *                     attachment; any depth attachment is ignored -- no depth test).
     * @param light_layout Layout of the caller's light set, bound at set 1.
     * @param vert_spv     Fullscreen-triangle vertex shader (e.g. toyengine's fullscreen.vert).
     * @param frag_spv     fog.frag.
     */
    FogPass(coopa::gfx::core::Device& device,
            VkRenderPass render_pass,
            const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
            const std::string& vert_spv,
            const std::string& frag_spv)
        : nearest_sampler_(coopa::gfx::engine::util::Sampler::nearest(device)),
          stage_(device, coopa::gfx::detail::RawRenderPass{render_pass}, describe(light_layout, vert_spv, frag_spv))
    {}

    FogPass(const FogPass&) = delete;
    FogPass& operator=(const FogPass&) = delete;

    /**
     * @brief Binds the G-buffer normal (sky test) and world position (fog distance). Nearest,
     * not linear: linear filtering would blend world positions across silhouette edges and
     * produce a wrong fog distance at every object outline.
     */
    void set_source_images(coopa::gfx::TextureView g_normal, coopa::gfx::TextureView g_position) {
        stage_.set(0).bind_image(0, g_normal, nearest_sampler_);
        stage_.set(0).bind_image(1, g_position, nearest_sampler_);
    }

    /// @brief Records the fog draw. Must be inside the render pass given at construction.
    void draw(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::DescriptorSet& light_set,
              const PushConstants& pc, uint32_t viewport_w, uint32_t viewport_h) const {
        stage_.bind(cmd, viewport_w, viewport_h);
        cmd.bind_descriptor_set(light_set, 1);
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pc);
        stage_.draw(cmd);
    }

private:
    static FullscreenStageDesc describe(const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                                        const std::string& vert_spv, const std::string& frag_spv) {
        using coopa::gfx::DescriptorType;
        using coopa::gfx::ShaderStage;
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
        };
        d.extra_layouts  = {&light_layout};
        d.push_constants = {{ShaderStage::Fragment, 0, static_cast<uint32_t>(sizeof(PushConstants))}};
        d.blend          = coopa::gfx::pipeline::BlendMode::PremultipliedAlpha;
        return d;
    }

    coopa::gfx::engine::util::Sampler nearest_sampler_;
    FullscreenStage                   stage_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_FOG_PASS_H
