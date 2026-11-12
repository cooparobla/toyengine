/**
 * @file stylize_pass.h
 * @brief Optional bloom + tonemap + outline + ordered dither + palette
 *        quantization overlay (the caller's stylize.frag, e.g. toyengine's).
 *
 * The tonemap step is optional (PushConstants::exposure <= 0 disables it):
 * a full PBR renderer with its own tonemap/AA chain (e.g. blendy) feeds
 * already-tonemapped LDR input and composites the retro-look effects as a
 * final overlay, with no double-tonemapping. A consumer with no tonemap
 * step of its own (e.g. toyengine) sets exposure > 0 and feeds raw HDR
 * scene color instead.
 *
 * Reads scene color plus the G-buffer's depth and normal (for the outline
 * edge detector), a palette LUT, and (optionally) a pre-blurred bloom image --
 * see set_source_images()'s bloom_result param. Bloom itself is NOT computed
 * here: it's a separate BloomPass pyramid (bright-pass threshold -> multi-tap
 * downsample -> tent-filter upsample+combine), and this pass just adds its
 * finished result before the tonemap. Writes into its own target --
 * pipeline::RenderPass's hardcoded LOAD_OP_CLEAR (gfxcoopa/pipeline/render_pass.h)
 * means the target it reads from can't be reopened and composited onto in place.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_STYLIZE_PASS_H
#define GFXCOOPA_ENGINE_PASSES_STYLIZE_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/util/sampler.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class StylizePass
 * @brief Composites bloom, an optional tonemap, depth/normal outlines, ordered
 *        dither and palette quantization in one fullscreen draw.
 *
 * Samples scene colour, the G-buffer depth and normal, a palette LUT and an
 * optional pre-blurred bloom image. Set PushConstants::exposure <= 0 when the
 * source is already tonemapped.
 */
class StylizePass {
public:
    /**
     * @brief Matches stylize.frag's StylizePushConstants block.
     *
     * outline_color (vec4) is listed first so its GLSL std430 16-byte
     * alignment lands at offset 0 for free -- everywhere else in gfxcoopa
     * that mixes a vec4 with scalars in a push-constant block (e.g.
     * SsrPass::ResolvePushConstants, GBufferPipeline::PushConstants) follows
     * the same ordering, since a vec4 placed mid-struct forces the GLSL
     * side to insert padding the plain C++ struct below doesn't replicate.
     */
    struct PushConstants {
        glm::vec4 outline_color      = glm::vec4(0.05f, 0.04f, 0.08f, 1.0f);
        glm::vec2 inv_render_size;
        float     outline_thickness  = 0.0f;  ///< In texels; <= 0 disables.
        float     depth_threshold    = 0.02f;
        float     normal_threshold   = 0.75f;
        float     dither_strength    = 0.0f;  ///< <= 0 disables.
        float     palette_count      = 0.0f;  ///< <= 0 disables palette quantization.
        float     camera_near           = 0.1f;
        float     camera_far            = 1000.0f;
        float     camera_is_perspective = 1.0f;  ///< >= 0.5 => perspective, else orthographic.
        float     exposure              = 0.0f;  ///< <= 0 disables the tonemap step (input is already LDR).
        /// Final multiplier on the pre-blurred bloom image bound at binding 4 (see
        /// set_source_images()'s bloom_result param and BloomPass). <= 0 disables it.
        /// No threshold/LOD fields here: BloomPass's own bright-pass shader
        /// thresholds once, per source texel, before any blurring -- re-thresholding
        /// the finished blurred result here would eat the halo falloff the pyramid
        /// exists to produce, and there is no mip chain left to pick an LOD from.
        float     bloom_intensity        = 0.0f;
        /// != 0 multiplies `exposure` by the adapting value in the 1x1 texture at
        /// binding 5 (see set_source_images()'s exposure_result param and ExposurePass).
        /// Harmless to leave 0: the binding then points at a neutral 1.0 texture anyway,
        /// so this only saves the read.
        float     auto_exposure          = 0.0f;
        /// Colour-grading LUT cube side length (GradingLut::size()); <= 1 disables the
        /// lookup, matching the same "off at its neutral value" convention every other
        /// field in this block follows.
        float     grading_size           = 0.0f;
        /// Which surfaces the outline draws on. 0 (the default) outlines every depth/normal
        /// discontinuity in the frame. 1 outlines only where the per-material mask at
        /// binding 7 says so -- see set_source_images()'s outline_mask param. Unread when
        /// outline_thickness <= 0, like every other outline field here.
        float     outline_mode           = 0.0f;
    };
    static_assert(sizeof(PushConstants) == 76,
                 "PushConstants must match stylize.frag's StylizePushConstants byte-for-byte");

    StylizePass(coopa::gfx::core::Device& device,
                     coopa::gfx::pipeline::RenderPass& target_pass,
                     const std::string& vert_spv,
                     const std::string& frag_spv)
        : stage_(device, target_pass, describe(vert_spv, frag_spv))
    {}

    StylizePass(const StylizePass&) = delete;
    StylizePass& operator=(const StylizePass&) = delete;

    void set_source_images(coopa::gfx::TextureView scene_color, coopa::gfx::TextureView scene_depth,
                           coopa::gfx::TextureView scene_normal, coopa::gfx::TextureView palette_lut,
                           const coopa::gfx::engine::util::Sampler& linear_sampler,
                           const coopa::gfx::engine::util::Sampler& nearest_sampler,
                           coopa::gfx::TextureView bloom_result = {},
                           const coopa::gfx::engine::util::Sampler* bloom_sampler = nullptr,
                           coopa::gfx::TextureView exposure_result = {},
                           const coopa::gfx::engine::util::Sampler* exposure_sampler = nullptr,
                           coopa::gfx::TextureView grading_lut = {},
                           const coopa::gfx::engine::util::Sampler* grading_sampler = nullptr,
                           coopa::gfx::TextureView outline_mask = {},
                           const coopa::gfx::engine::util::Sampler* outline_mask_sampler = nullptr) {
        // scene_depth/scene_normal use the nearest sampler, not linear: the outline edge
        // detector's taps need exact texel values (linear filtering would blend across the
        // very discontinuities it's looking for), and linear filtering of a D32_SFLOAT depth
        // image is an optional Vulkan format feature that isn't queried anywhere in this engine.
        stage_.set().bind_image(0, scene_color, linear_sampler);
        stage_.set().bind_image(1, scene_depth, nearest_sampler);
        stage_.set().bind_image(2, scene_normal, nearest_sampler);
        stage_.set().bind_image(3, palette_lut, nearest_sampler);
        // bloom_result: the finished, pre-blurred output of a dedicated BloomPass pyramid
        // (bright-pass threshold -> multi-tap downsample -> tent-filter upsample+combine),
        // sampled with a plain texture() through a LINEAR sampler: one already-composited
        // image at half this pass's resolution, bilinearly upsampled for free, with no LOD
        // to select. Optional -- omitting it re-binds binding 0's own view/sampler, which is
        // harmless because bloom_intensity <= 0 makes the shader skip the read. Only pass a
        // real view once the BloomPass has executed at least once, or the descriptor points
        // at a target that has never been rendered into.
        stage_.set().bind_image(4, bloom_sampler ? bloom_result : scene_color,
                              bloom_sampler ? *bloom_sampler : linear_sampler);
        // exposure_result / grading_lut: same optional-with-harmless-fallback contract as
        // bloom_result above. Omitting either re-binds binding 0's own view/sampler, which
        // the matching push constant (auto_exposure 0 / grading_size 0) makes unread. A
        // consumer that DOES want them passes a real 1x1 neutral texture instead, so the
        // shader needs no branch at all.
        stage_.set().bind_image(5, exposure_sampler ? exposure_result : scene_color,
                              exposure_sampler ? *exposure_sampler : linear_sampler);
        stage_.set().bind_image(6, grading_sampler ? grading_lut : scene_color,
                              grading_sampler ? *grading_sampler : linear_sampler);
        // outline_mask: a per-surface mask the outline can be restricted to, read only when
        // PushConstants::outline_mode selects it. Same optional-with-harmless-fallback
        // contract as the three above. toyengine binds the G-buffer's emissive attachment,
        // whose alpha carries each material's packed toon code (PBRMaterial::gpu_toon_code()
        // -- bit 10 is "outline this material"); a consumer with a mask of its own binds
        // whatever single-channel image encodes the same decision. NEAREST, for the same
        // reason depth/normal above are: this is a packed integer code, and blending one
        // across a material boundary produces a value neither material asked for.
        stage_.set().bind_image(7, outline_mask_sampler ? outline_mask : scene_color,
                              outline_mask_sampler ? *outline_mask_sampler : nearest_sampler);
    }

    void draw(coopa::gfx::command::CommandBuffer& cmd, const PushConstants& params,
              uint32_t viewport_w, uint32_t viewport_h) const {
        stage_.draw(cmd, viewport_w, viewport_h, coopa::gfx::ShaderStage::Fragment, params);
    }

private:
    /// @brief Eight sampled images at bindings 0..7 (scene colour, depth, normal,
    /// palette LUT, bloom, auto-exposure, grading LUT, outline mask), plus the fragment
    /// push constants.
    static FullscreenStageDesc describe(const std::string& vert_spv, const std::string& frag_spv) {
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets.emplace_back();
        for (uint32_t i = 0; i < 8; ++i) {
            d.owned_sets[0].push_back({i, coopa::gfx::DescriptorType::CombinedImageSampler,
                                       coopa::gfx::ShaderStage::Fragment, 1});
        }
        d.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        return d;
    }

    FullscreenStage stage_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_STYLIZE_PASS_H
