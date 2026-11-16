/**
 * @file separable_blur_pass.h
 * @brief A uniform horizontal-then-vertical blur over the whole image, at a resolution the
 *        caller chooses -- the backdrop a glass UI panel samples (toyengine's
 *        ui_backdrop_blur.frag) is the first consumer.
 *
 * TiltShiftPass's skeleton with the focus band deleted: a uniform blur has no screen-position
 * structure to stair-step, so unlike that pass this one carries no opinion about resolution --
 * the caller sizes it however it likes (toyengine sizes it from render_extent_ / downscale,
 * which is what makes ITS instance startup-fixed rather than resize-aware; this class itself
 * has no notion of "startup-fixed", it just blurs whatever source it is given at whatever
 * out_width/out_height it was built with). set_source() lets the caller rebind which image is
 * read without a rebuild, e.g. to follow another pass's on/off toggle.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_SEPARABLE_BLUR_PASS_H
#define GFXCOOPA_ENGINE_PASSES_SEPARABLE_BLUR_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/types/texture_view.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class SeparableBlurPass
 * @brief A two-stage (horizontal, vertical) uniform blur, sized and sourced by the caller.
 *
 * Owns both OffscreenTargets and one pipeline shared by the two stages -- the exact shape
 * TiltShiftPass and BloomPass already use, which is what lets both draws share one pipeline
 * (their render passes are Vulkan render-pass-compatible: same format, same sample count,
 * resolution is not part of the rule).
 */
class SeparableBlurPass {
public:
    /// Per-frame tunable. A push constant -- unlike this pass's descriptor bindings (fixed
    /// at construction) free to change every frame with no rebuild.
    struct Params {
        float radius = 8.0f; ///< Furthest tap, in DESTINATION (out_width/out_height) pixels.
    };

    struct PushConstants {
        glm::vec2 texel_step; // offset 0: per-stage tap direction, in UV, pre-scaled by 1/dst_size
        float     radius;     // offset 8: furthest tap, in destination pixels
        float     _pad = 0.0f;
    };
    static_assert(sizeof(PushConstants) == 16, "must match ui_backdrop_blur.frag's Push block byte-for-byte");

    /**
     * @param out_width/out_height Output size in pixels -- whatever the caller wants; this
     *                             class has no opinion about resolution.
     * @param source               The image to blur.
     * @param source_sampler       Sampler the horizontal stage uses on `source`.
     * @param vert_spv             Fullscreen-triangle vertex shader.
     * @param frag_spv             ui_backdrop_blur.frag (or any shader matching PushConstants).
     */
    SeparableBlurPass(coopa::gfx::core::Device& device,
                      coopa::gfx::memory::Allocator& allocator,
                      uint32_t out_width, uint32_t out_height,
                      coopa::gfx::TextureView source,
                      const coopa::gfx::engine::util::Sampler& source_sampler,
                      const std::string& vert_spv,
                      const std::string& frag_spv)
        : out_width_(out_width), out_height_(out_height),
          linear_sampler_(coopa::gfx::engine::util::Sampler::linear(device)),
          h_target_(device, allocator, out_width, out_height, coopa::gfx::Format::RGBA8_Unorm, targets::kColorOnly),
          v_target_(device, allocator, out_width, out_height, coopa::gfx::Format::RGBA8_Unorm, targets::kColorOnly)
    {
        FullscreenStageDesc sd;
        sd.vert_spv = vert_spv;
        sd.frag_spv = frag_spv;
        sd.owned_sets = {{{0, coopa::gfx::DescriptorType::CombinedImageSampler,
                           coopa::gfx::ShaderStage::Fragment, 1}}};
        sd.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        sd.instances = 2;
        stage_ = std::make_unique<FullscreenStage>(device, h_target_.render_pass_object(), sd);

        stage_->set(0, kHorizontal).bind_image(0, source, source_sampler);
        // Instance 1 samples h_target_'s already-full-resolution output with LINEAR -- exact
        // at a texel centre, same reasoning as TiltShiftPass's vertical stage.
        stage_->set(0, kVertical).bind_image(0, h_target_.color_view_typed(), linear_sampler_);
    }

    SeparableBlurPass(const SeparableBlurPass&) = delete;
    SeparableBlurPass& operator=(const SeparableBlurPass&) = delete;

    /**
     * @brief Rebinds the horizontal stage's input. Outside a frame only -- bind_image()
     *        updates the descriptor immediately, which is unsafe once a render pass has
     *        begun (the same contract register_textures()/UiCompositePass::set_base_image()
     *        already carry).
     */
    void set_source(coopa::gfx::TextureView source, const coopa::gfx::engine::util::Sampler& source_sampler) {
        stage_->set(0, kHorizontal).bind_image(0, source, source_sampler);
    }

    /// @brief Runs the horizontal-then-vertical blur. Leaves the result in result_view_typed().
    ///
    /// No manual barrier between stages -- both targets are OffscreenTargets whose render
    /// passes end in SHADER_READ_ONLY_OPTIMAL with an exit subpass dependency, exactly the
    /// write-then-read hazard between the two stages here (same as TiltShiftPass/BloomPass).
    void execute(coopa::gfx::command::CommandBuffer& cmd, const Params& params) const {
        PushConstants pc{};
        pc.radius = params.radius;

        pc.texel_step = glm::vec2(1.0f / static_cast<float>(out_width_), 0.0f);
        begin_stage_(cmd, h_target_, kHorizontal);
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pc);
        stage_->draw(cmd);
        h_target_.end(cmd);

        pc.texel_step = glm::vec2(0.0f, 1.0f / static_cast<float>(out_height_));
        begin_stage_(cmd, v_target_, kVertical);
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pc);
        stage_->draw(cmd);
        v_target_.end(cmd);
    }

    /// @brief The finished blurred image, at (out_width, out_height).
    coopa::gfx::TextureView result_view_typed() const { return v_target_.color_view_typed(); }

    uint32_t width()  const { return out_width_; }
    uint32_t height() const { return out_height_; }

private:
    /// begin() + bind + POSITIVE-height viewport -- OffscreenTarget::begin() sets a NEGATIVE
    /// height (its Vulkan-NDC Y-flip for geometry passes), which would flip a fullscreen-
    /// triangle pass's output vertically. Same override TiltShiftPass/BloomPass use.
    void begin_stage_(coopa::gfx::command::CommandBuffer& cmd, const targets::OffscreenTarget& target,
                      uint32_t instance) const {
        target.begin(cmd);
        stage_->bind(cmd, target.width(), target.height(), instance);
    }

    static constexpr uint32_t kHorizontal = 0;
    static constexpr uint32_t kVertical   = 1;

    uint32_t out_width_;
    uint32_t out_height_;

    coopa::gfx::engine::util::Sampler linear_sampler_;

    targets::OffscreenTarget h_target_;
    targets::OffscreenTarget v_target_;

    std::unique_ptr<FullscreenStage> stage_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_SEPARABLE_BLUR_PASS_H
