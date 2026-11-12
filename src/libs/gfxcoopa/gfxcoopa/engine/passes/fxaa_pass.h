/**
 * @file fxaa_pass.h
 * @brief Standalone LDR FXAA 3.11 (Quality Preset 39) post-processing pass.
 *
 * Follows FogPass's shape line for line -- a fullscreen triangle, one combined-image-sampler
 * descriptor, writing into a separate target the caller owns begin()/end() for (see
 * pipeline::RenderPass's hardcoded LOAD_OP_CLEAR, documented on FogPass itself, for why that
 * target can't be the same image this pass reads from).
 *
 * This pass expects an already-tonemapped LDR source and does no exposure/ACES work itself -- see toyengine's
 * fxaa.frag file doc for why it reads LDR rather than re-tonemapping every tap.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_FXAA_PASS_H
#define GFXCOOPA_ENGINE_PASSES_FXAA_PASS_H

#include <volk/volk.h>
#include <memory>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/util/sampler.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class FxaaPass
 * @brief Resolves FXAA 3.11 over an already-tonemapped LDR image.
 *
 * Does no exposure or ACES work of its own -- feed it LDR. Writes into a target
 * separate from the one it samples, which the caller begins and ends.
 */
class FxaaPass {
public:
    /**
     * @brief Push-constant layout for fxaa.frag -- fragment stage only.
     */
    struct PushConstants {
        float screen_width;
        float screen_height;
        float subpixel_quality      = 0.75f;   /**< Blend weight of the subpixel-aliasing term. */
        float edge_threshold        = 0.166f;  /**< Local contrast (fraction of lumaMax) below which no AA is applied. */
        float edge_threshold_min    = 0.0312f; /**< Absolute contrast floor -- avoids AA-ing near-black noise. */
    };

    /**
     * @brief Builds the FXAA pipeline and its descriptor set.
     * @param device      Logical device.
     * @param target_pass Render pass of the (separate) LDR target this pass writes into.
     * @param vert_spv    Fullscreen-triangle vertex shader.
     * @param frag_spv    fxaa.frag.
     */
    FxaaPass(coopa::gfx::core::Device& device,
             coopa::gfx::pipeline::RenderPass& target_pass,
             const std::string& vert_spv,
             const std::string& frag_spv)
        : stage_(device, target_pass, describe(vert_spv, frag_spv))
    {}

    FxaaPass(const FxaaPass&) = delete;
    FxaaPass& operator=(const FxaaPass&) = delete;

    /** @brief Rebinds the LDR source image (linear-filtered, matching FXAA's own edge-search taps). */
    void set_source_image(coopa::gfx::TextureView color_view, const coopa::gfx::engine::util::Sampler& linear_sampler) {
        stage_.set().bind_image(0, color_view, linear_sampler);
    }

    /**
     * @brief Records the fullscreen FXAA resolve.
     * @param cmd        Command buffer, inside the target render pass.
     * @param pc         Screen size and the three FXAA thresholds.
     * @param viewport_w Target width in pixels.
     * @param viewport_h Target height in pixels.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd, const PushConstants& pc,
              uint32_t viewport_w, uint32_t viewport_h) const {
        stage_.draw(cmd, viewport_w, viewport_h, coopa::gfx::ShaderStage::Fragment, pc);
    }

private:
    /// @brief One sampled LDR image at binding 0, plus the fragment push constants.
    static FullscreenStageDesc describe(const std::string& vert_spv, const std::string& frag_spv) {
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {{{0, coopa::gfx::DescriptorType::CombinedImageSampler,
                          coopa::gfx::ShaderStage::Fragment, 1}}};
        d.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        return d;
    }

    FullscreenStage stage_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_FXAA_PASS_H
