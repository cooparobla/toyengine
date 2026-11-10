/**
 * @file volumetrics_pass.h
 * @brief Raymarched LOCAL volumes (the caller's volumetrics_march.frag and
 *        volumetrics_composite.frag, e.g. toyengine's).
 *
 * Two fullscreen stages. The MARCH integrates the medium into a low-resolution
 * target (rgb = in-scatter, a = transmittance) -- the expensive part, so it runs at
 * a fraction of the render resolution. The COMPOSITE then upsamples that over the
 * full-resolution scene colour with a depth-aware (joint-bilateral) filter. The global
 * fog is not applied here (see FogPass, which fogs the opaque scene before translucency).
 * The composite writes into its own target: pipeline::RenderPass's hardcoded LOAD_OP_CLEAR
 * (gfxcoopa/pipeline/render_pass.h) means the target it reads scene colour from
 * can't be reopened and composited onto in place.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_VOLUMETRICS_PASS_H
#define GFXCOOPA_ENGINE_PASSES_VOLUMETRICS_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>

#include <memory>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/engine/util/sampler.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class VolumetricsPass
 * @brief Marches the local volumes at reduced resolution, then composites them over
 *        a scene colour image at full resolution.
 *
 * Usage: bind everything once at construction time (set_source_images,
 * set_march_result, set_shadow_images), then per frame record draw_march() into the
 * low-res target's render pass and draw_composite() into the full-res one.
 */
class VolumetricsPass {
public:
    /**
     * @brief Builds both pipelines and their descriptor sets.
     * @param device         Logical device.
     * @param march_pass     Render pass of the low-resolution march target
     *                       (RGBA16F: rgb = in-scatter, a = transmittance).
     * @param composite_pass Render pass of the full-resolution HDR target the composite
     *                       writes into.
     * @param vol_ubo        The VolumetricsData uniform buffer (engine/data/volumetrics_data.h).
     *                       Bound once here -- its VkBuffer never changes, only its
     *                       per-frame upload() repeats.
     * @param vert_spv       Fullscreen-triangle vertex shader (the shared fullscreen.vert).
     * @param march_frag_spv     volumetrics_march.frag.
     * @param composite_frag_spv volumetrics_composite.frag.
     */
    VolumetricsPass(coopa::gfx::core::Device& device,
             coopa::gfx::pipeline::RenderPass& march_pass,
             coopa::gfx::pipeline::RenderPass& composite_pass,
             const coopa::gfx::memory::Buffer& vol_ubo,
             const std::string& vert_spv,
             const std::string& march_frag_spv,
             const std::string& composite_frag_spv)
        : nearest_sampler_(coopa::gfx::engine::util::Sampler::nearest(device)),
          march_(device, march_pass, describe_march(vert_spv, march_frag_spv)),
          composite_(device, composite_pass, describe_composite(vert_spv, composite_frag_spv))
    {
        march_.set(1).bind_buffer(0, vol_ubo);
        composite_.set(1).bind_buffer(0, vol_ubo);
    }

    VolumetricsPass(const VolumetricsPass&) = delete;
    VolumetricsPass& operator=(const VolumetricsPass&) = delete;

    /**
     * @brief Binds the scene colour and G-buffer images. Call ONCE, at construction time.
     *
     * DescriptorSet::bind_image issues vkUpdateDescriptorSets immediately, so
     * rebinding mid-frame races this pipeline's in-flight command buffers.
     *
     * g_normal/g_position use the nearest sampler this pass owns, not linear:
     * linear filtering would blend world positions across silhouette edges,
     * producing a wrong ray-termination distance at every object outline -- the
     * same reasoning FogPass and StylizePass both document. The composite's
     * upsample also depends on it: it re-reads G2 at the march's texel centres to
     * recover each low-res sample's exact depth.
     */
    void set_source_images(coopa::gfx::TextureView scene_color, coopa::gfx::TextureView g_normal,
                           coopa::gfx::TextureView g_position,
                           const coopa::gfx::engine::util::Sampler& linear_sampler) {
        march_.set(0).bind_image(0, g_normal, nearest_sampler_);
        march_.set(0).bind_image(1, g_position, nearest_sampler_);
        composite_.set(0).bind_image(0, scene_color, linear_sampler);
        composite_.set(0).bind_image(1, g_normal, nearest_sampler_);
        composite_.set(0).bind_image(2, g_position, nearest_sampler_);
    }

    /**
     * @brief Binds the march target's colour image as the composite's input. Call ONCE,
     *        at construction time (same rule as set_source_images). The composite reads
     *        it with texelFetch, so the sampler choice is immaterial.
     */
    void set_march_result(coopa::gfx::TextureView march_result) {
        composite_.set(0).bind_image(3, march_result, nearest_sampler_);
    }

    /**
     * @brief Binds the directional and spot shadow maps the march's in-scatter
     *        terms sample. Call ONCE, at construction time (same rule as
     *        set_source_images above).
     *
     * @param dir_shadow    The directional shadow depth map.
     * @param spot_shadow   The local-light (point/spot) shadow atlas -- sampled by
     *                      gfx/local_shadow.glsl through VolumetricsUBO::local_shadows.
     * @param shadow_sampler A compare-enabled sampler (util::Sampler::shadow()) --
     *                      volumetrics_march.frag declares both bindings as
     *                      sampler2DShadow, so each march tap is one hardware
     *                      depth-compare read.
     *
     * The bindings are non-optional in the pipeline layout; a consumer that never
     * shadows its volumes still binds real maps and gates the taps off through
     * VolumetricsUBO::shadow_params instead (a runtime flag, unlike this binding).
     */
    void set_shadow_images(coopa::gfx::TextureView dir_shadow, coopa::gfx::TextureView spot_shadow,
                           const coopa::gfx::engine::util::Sampler& shadow_sampler) {
        march_.set(2).bind_image(0, dir_shadow, shadow_sampler);
        march_.set(2).bind_image(1, spot_shadow, shadow_sampler);
    }

    /**
     * @brief Binds an app's cloud shadow map at set 2, binding 4 (a plain sampler2D of the sun's
     *        transmittance -- toyengine's cloud layer; VolumetricsUBO::cloud_shadow says where it
     *        lies). Call ONCE, at construction time, when the march shader samples it; a shader
     *        that does not leaves the binding unused.
     */
    void set_cloud_shadow_image(coopa::gfx::TextureView cloud_shadow, const coopa::gfx::engine::util::Sampler& sampler) {
        march_.set(2).bind_image(4, cloud_shadow, sampler);
    }

    /**
     * @brief Records the low-resolution march into the caller's open render pass
     *        (the march target's). Sizes are the MARCH target's.
     *
     * Sets a POSITIVE-height viewport, overriding the negative-height one
     * OffscreenTarget::begin() installs for geometry passes -- a fullscreen
     * triangle must undo it (the same trap TiltShiftPass::begin_stage_ documents).
     */
    void draw_march(coopa::gfx::command::CommandBuffer& cmd, uint32_t viewport_w, uint32_t viewport_h) const {
        march_.bind(cmd, viewport_w, viewport_h);
        march_.draw(cmd);
    }

    /**
     * @brief Records the full-resolution composite into the caller's open render pass.
     *        Must follow draw_march() in the same frame, after that render pass ended
     *        (its final layout, SHADER_READ_ONLY_OPTIMAL, is what the composite reads).
     */
    void draw_composite(coopa::gfx::command::CommandBuffer& cmd, uint32_t viewport_w, uint32_t viewport_h) const {
        composite_.bind(cmd, viewport_w, viewport_h);
        composite_.draw(cmd);
    }

private:
    /// @brief Set 0: G-buffer normal/position. Set 1: the volumetrics UBO.
    ///        Set 2: directional map + local-light shadow atlas (see set_shadow_images), and
    ///        an app's cloud shadow map at binding 4 (set_cloud_shadow_image).
    static FullscreenStageDesc describe_march(const std::string& vert_spv, const std::string& frag_spv) {
        using coopa::gfx::DescriptorType;
        using coopa::gfx::ShaderStage;
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
            {{0, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {4, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
        };
        return d;
    }

    /// @brief Set 0: scene colour, G-buffer normal/position, march result.
    ///        Set 1: the volumetrics UBO.
    static FullscreenStageDesc describe_composite(const std::string& vert_spv, const std::string& frag_spv) {
        using coopa::gfx::DescriptorType;
        using coopa::gfx::ShaderStage;
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {3, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
            {{0, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},
        };
        return d;
    }

    coopa::gfx::engine::util::Sampler nearest_sampler_;
    FullscreenStage                   march_;
    FullscreenStage                   composite_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_VOLUMETRICS_PASS_H
