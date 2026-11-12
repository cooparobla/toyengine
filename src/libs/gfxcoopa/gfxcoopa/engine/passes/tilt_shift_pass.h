/**
 * @file tilt_shift_pass.h
 * @brief Diorama-style tilt-shift blur (the caller's tilt_shift.frag, e.g. toyengine's):
 *        a horizontal sharp band with the top and bottom progressively blurred,
 *        driven purely by screen position (no depth sampling).
 *
 * Modelled on BloomPass's shape (owns its own OffscreenTargets, one Pipeline reused
 * across render-pass-compatible stages, every descriptor bound once at construction --
 * so unlike HiZPass/SceneColorMipPass this needs no per-frame device_.wait_idle()) but
 * with a fixed two-stage chain instead of a pyramid: horizontal separable blur, then
 * vertical. Because the circle-of-confusion field this pass reads is a smooth function
 * of screen position (not depth), the separable two-pass Gaussian is exact -- there is
 * no depth-discontinuity edge for a horizontal-then-vertical blur to leak across.
 *
 * Deliberately built to run at DISPLAY resolution, after the pixel-art upscale, not at
 * the low internal render resolution: tilt shift is a lens effect layered on top of the
 * rendered image, not a pixel-grid effect, and running it at 720x480 would quantize the
 * blur kernel to 2x2 (or larger) display blocks and the focus ramp to a few hundred
 * vertical steps -- visibly stair-stepped compared to the reference. The horizontal
 * stage folds the existing nearest-neighbour upscale into itself for free: it samples
 * `source` (the low-res post_target_) at DESTINATION-resolution UVs with the caller's
 * nearest sampler, so wherever the circle of confusion is ~0 its single tap is
 * bit-identical to what UpscalePass/upscale.frag already produce (see tilt_shift.frag's
 * own doc). Downstream, UpscalePass changes from an upscale to a 1:1 letterbox blit.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_TILT_SHIFT_PASS_H
#define GFXCOOPA_ENGINE_PASSES_TILT_SHIFT_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>

#include <cmath>
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
 * @class TiltShiftPass
 * @brief Diorama-style tilt-shift blur: a horizontal then a vertical pass over
 *        a screen-space focus band, with the upscale folded in.
 *
 * Owns both OffscreenTargets and one pipeline shared by the two stages, which
 * differ only in their push constants and which set they bind.
 */
class TiltShiftPass {
public:
    /// Per-frame tunables. All are push constants -- unlike this pass's descriptor
    /// bindings (fixed at construction, see the file doc) these are free to change
    /// every frame with no rebuild.
    struct Params {
        float focus_center  = 0.55f; ///< 0..1 screen position of the sharp band's centre (0 = top, at angle 0).
        float focus_width   = 0.18f; ///< 0..1 half-height of the fully-sharp band.
        float ramp_width    = 0.22f; ///< 0..1 distance the blur ramps in over, smoothstepped.
        float max_radius    = 6.0f;  ///< Blur radius in DISPLAY pixels at full strength.
        float blur_top      = 1.0f;  ///< Strength multiplier on the "far" side of the band.
        float blur_bottom   = 0.7f;  ///< Strength multiplier on the "near" side of the band.
        float angle_degrees = 0.0f;  ///< Rotates the focus band off horizontal.
    };

    struct PushConstants {
        glm::vec4 band;       // offset  0: focus_center, focus_width, ramp_width, max_radius
        glm::vec4 shape;      // offset 16: blur_top, blur_bottom, axis.x, axis.y
        glm::vec2 texel_step; // offset 32: per-stage tap direction, in UV, pre-scaled by 1/dst_size
    };
    static_assert(sizeof(PushConstants) == 40,
                 "PushConstants must match tilt_shift.frag's TiltShiftPush byte-for-byte");

    /**
     * @brief Builds the two-stage blur chain and its pipeline.
     * @param device         Logical device.
     * @param allocator      VMA allocator, for the two owned OffscreenTargets.
     * @param out_width      Output width in pixels -- the DISPLAY (letterbox) size, not
     *                       the low internal render resolution.
     * @param out_height     Output height in pixels.
     * @param source         The low-res image to read (e.g. post_target_'s color view).
     * @param source_sampler Sampler the horizontal stage uses on `source`. That stage reads
     *                       `source` at DESTINATION resolution, so it is the scene-to-display
     *                       resample: pass NEAREST only when the destination is at least as
     *                       large as the source (folding a crisp upscale in for free), and
     *                       LINEAR when it is smaller, where NEAREST decimates and flickers.
     * @param vert_spv       Fullscreen-triangle vertex shader.
     * @param frag_spv       tilt_shift.frag.
     */
    TiltShiftPass(coopa::gfx::core::Device& device,
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
        // Instance 0 is the horizontal stage, instance 1 the vertical: one layout and
        // one pipeline, two sets.
        //
        // h_target_ and v_target_ share format/size/sample-count, so their render passes
        // are Vulkan render-pass-compatible (resolution and the specific VkRenderPass
        // handle are not part of the compatibility rule -- see bloom_pass.h's identical
        // reasoning). One pipeline therefore serves both draws in execute() below.
        FullscreenStageDesc sd;
        sd.vert_spv = vert_spv;
        sd.frag_spv = frag_spv;
        sd.owned_sets = {{{0, coopa::gfx::DescriptorType::CombinedImageSampler,
                           coopa::gfx::ShaderStage::Fragment, 1}}};
        sd.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        sd.instances = 2;
        stage_ = std::make_unique<FullscreenStage>(device, h_target_.render_pass_object(), sd);

        // Instance 0 samples the SOURCE at destination UVs -- this stage IS the scene-to-
        // display resample, so the caller's sampler choice decides how it is filtered. It is
        // NEAREST for a magnifying fit (crisp blocks, the folded-upscale property) but must
        // be LINEAR when the destination is SMALLER than the source, where nearest is a point
        // decimation: see toyengine's ToyRenderConfig::upscale_filter.
        // Instance 1 samples h_target_'s already-full-resolution output with LINEAR, which is
        // exact (not an approximation) at coc == 0: a bilinear tap sampled precisely at a
        // texel centre returns that texel unchanged, so the sharp band survives both stages
        // bit-identical to a plain nearest upscale -- see tilt_shift.frag's early-out.
        stage_->set(0, kHorizontal).bind_image(0, source, source_sampler);
        stage_->set(0, kVertical).bind_image(0, h_target_.color_view_typed(), linear_sampler_);
    }

    TiltShiftPass(const TiltShiftPass&) = delete;
    TiltShiftPass& operator=(const TiltShiftPass&) = delete;

    /// @brief Runs the horizontal-then-vertical blur. Leaves the finished result in
    /// result_view_typed()/result_image().
    ///
    /// No manual VkImageMemoryBarrier between the two stages: both targets are
    /// OffscreenTargets whose render passes end in SHADER_READ_ONLY_OPTIMAL and emit an
    /// exit subpass dependency (COLOR_ATTACHMENT_OUTPUT/WRITE -> FRAGMENT_SHADER/READ --
    /// see render_pass.h's needs_exit_dependency block), exactly the write-then-read
    /// hazard between the two stages here. BloomPass and SmaaPass chain OffscreenTargets
    /// the same way with no manual barriers.
    void execute(coopa::gfx::command::CommandBuffer& cmd, const Params& params) const {
        // axis is NOT unit length: the aspect factor is baked in on purpose so
        // focus_width/ramp_width stay in consistent screen-HEIGHT-fraction units
        // regardless of angle_degrees, matching the angle == 0 case (band == in_uv.y)
        // exactly rather than distorting the ramp width as the band rotates.
        float theta  = glm::radians(params.angle_degrees);
        float aspect = out_height_ > 0
            ? static_cast<float>(out_width_) / static_cast<float>(out_height_) : 1.0f;
        glm::vec2 axis(aspect * std::sin(theta), std::cos(theta));

        PushConstants pc{};
        pc.band  = glm::vec4(params.focus_center, params.focus_width, params.ramp_width, params.max_radius);
        pc.shape = glm::vec4(params.blur_top, params.blur_bottom, axis.x, axis.y);

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

    /// @brief The finished blurred+upscaled image, at (out_width, out_height).
    coopa::gfx::TextureView result_view_typed() const { return v_target_.color_view_typed(); }
    /// @brief Sealed sibling of result_view_typed(), for PNG readback.
    coopa::gfx::memory::Image& result_image() const { return *v_target_.color_image_object(); }

    uint32_t width()  const { return out_width_; }
    uint32_t height() const { return out_height_; }

private:
    /// begin() + bind + POSITIVE-height viewport. The positive height is not redundant:
    /// OffscreenTarget::begin() sets a NEGATIVE-height viewport (its Vulkan-NDC Y-flip
    /// for geometry passes), which would flip a fullscreen-triangle pass's output
    /// vertically -- see bloom_pass.h's begin_stage_() for the same override.
    void begin_stage_(coopa::gfx::command::CommandBuffer& cmd, const targets::OffscreenTarget& target,
                      uint32_t instance) const {
        target.begin(cmd);
        stage_->bind(cmd, target.width(), target.height(), instance);
    }

    static constexpr uint32_t kHorizontal = 0;  ///< stage_ instance for the horizontal blur.
    static constexpr uint32_t kVertical   = 1;  ///< stage_ instance for the vertical blur.

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

#endif // GFXCOOPA_ENGINE_PASSES_TILT_SHIFT_PASS_H
