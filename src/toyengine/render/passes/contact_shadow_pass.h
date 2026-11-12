/**
 * @file contact_shadow_pass.h
 * @brief Screen-space contact shadows as their own pass, with a converging temporal resolve.
 */

#ifndef TOYENGINE_RENDER_PASSES_CONTACT_SHADOW_PASS_H
#define TOYENGINE_RENDER_PASSES_CONTACT_SHADOW_PASS_H

#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/engine/passes/ssr_pass.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class ContactShadowPass
 * @brief Marches the screen-space contact shadow into its own buffer, then averages that buffer
 *        over frames the way the reflection and AO chains average theirs.
 *
 * Unity HDRP computes contact shadows into a dedicated texture (`_ContactShadowTexture`) rather
 * than inline in its lighting shader, and this follows that shape for the same reason plus one
 * more: inline, the term had no buffer of its own, so its only temporal filter was the
 * whole-frame TAA — whose variance clip rejects history precisely on thin, high-contrast
 * features under motion, which is exactly what a contact shadow is.
 *
 * Three stages, mirroring SsaoPass's raw → resolve shape:
 *  - **March** (`contact_shadow.frag`) writes raw occlusion: one ray toward the sun per pixel,
 *    one scene-depth fetch per step (`contact_shadow_body.glsl`, Unreal's screen-space contact
 *    shadow shape).
 *  - **Resolve** (`contact_shadow_resolve.frag`) is the scalar counterpart of gfxcoopa's
 *    `ssr_resolve.frag`: same push constants and bindings, same converging `1/N` schedule against
 *    the shared per-pixel count (TemporalHistoryPass), minus the vec4/YCoCg machinery.
 *  - **History copy**: one resolve target plus one history image, copied at the end of every
 *    execute(). SsrPass and TemporalHistoryPass ping-pong instead (one consumer set per
 *    parity, selected per frame); this pass's lone consumer, the lighting draw, binds its
 *    output once, and the copy is a small full-res R8-class transfer, so it keeps the copy.
 *
 * The consumer (`toy_lighting.frag`, and `debug_view.frag`'s contact_shadows channel) samples
 * `output_view_typed()` and applies strength and per-light darkness itself, exactly as it did
 * with the inline call: this pass emits the RAW geometric occlusion.
 */
class ContactShadowPass {
public:
    /// Format of the march and resolve targets: one occlusion scalar per pixel. The dedicated
    /// scalar resolve (contact_shadow_resolve.frag) is what lets this be R16F rather than the
    /// RGBA16F the shared vec4 SSR resolve required -- a quarter of the bandwidth for the march
    /// write, the resolve and the history copy alike.
    static constexpr coopa::gfx::Format kFormat = coopa::gfx::Format::R16_Sfloat;

    /// Per-frame parameters. Mirrors the temporal half of SsrPass::Params, since the resolve is
    /// literally the same shader.
    struct Params {
        /// Current clip space -> previous frame's clip space, composed in double precision by the
        /// caller. See SsrPass::Params::reproject for why the composition order matters.
        glm::mat4 reproject       = glm::mat4(1.0f);
        bool      reproject_valid = false;
        /// Accumulation depth (contact_shadow_temporal_frames). 0 makes the resolve a passthrough.
        int       temporal_frames = 16;
        /// Variance-clipping width, in standard deviations of the 3x3 neighbourhood.
        float     temporal_gamma  = 2.0f;
        /// True once the camera has been still long enough for the average to top up; accepted
        /// history is then held verbatim, which is what keeps a resting image byte-static.
        bool      frozen          = false;
        /// Which of the two count images update_descriptors() bound holds THIS frame's counts
        /// (TemporalHistoryPass::current_parity()).
        uint32_t  count_parity    = 0;
    };

    /**
     * @param device        Vulkan logical device.
     * @param allocator     VMA allocator for the three targets.
     * @param camera_layout The camera UBO set layout (set 0), owned by the caller.
     * @param light_layout  The light UBO set layout (set 1), owned by the caller.
     * @param width         Render width in pixels.
     * @param height        Render height in pixels.
     * @param vert_spv      Fullscreen-triangle vertex shader (fullscreen.vert).
     * @param march_spv     contact_shadow.frag.spv.
     * @param resolve_spv   contact_shadow_resolve.frag.spv (SsrPass::ResolvePushConstants layout).
     */
    ContactShadowPass(coopa::gfx::core::Device& device,
                      coopa::gfx::memory::Allocator& allocator,
                      const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                      const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                      uint32_t width, uint32_t height,
                      const std::string& vert_spv,
                      const std::string& march_spv,
                      const std::string& resolve_spv);

    /// Binds the live G-buffer, the scene depth the resolve reprojects from, and both parities
    /// of the shared accumulation count (TemporalHistoryPass::count_view_typed(0/1); execute()
    /// selects with Params::count_parity). Called once at construction (and after any resize):
    /// rebinding per frame is unsafe under an overlapped-frame pipeline.
    void update_descriptors(coopa::gfx::TextureView g0, coopa::gfx::TextureView g1,
                            coopa::gfx::TextureView g2,
                            coopa::gfx::TextureView depth,
                            coopa::gfx::TextureView count0, coopa::gfx::TextureView count1,
                            const coopa::gfx::engine::util::Sampler& count_sampler);

    void execute(coopa::gfx::command::CommandBuffer& cmd,
                 const coopa::gfx::pipeline::DescriptorSet& camera_set,
                 const coopa::gfx::pipeline::DescriptorSet& light_set,
                 const Params& params);

    /// Clears the output to 0 (no occlusion) without marching -- for a caller that skips
    /// execute() while the feature is off. Leaves the image in SHADER_READ_ONLY_OPTIMAL, the
    /// layout its consumers' descriptors expect, and holding exactly what a disabled march
    /// would resolve to. Also drops the history, which no longer describes the output.
    void clear_output(coopa::gfx::command::CommandBuffer& cmd);

    /// @brief The resolved occlusion buffer. One image for the pass's lifetime, so a consumer
    /// binds it once at setup and always reads this frame's result.
    coopa::gfx::TextureView output_view_typed() const {
        return resolved_target_->color_view_typed();
    }

    /// @brief NEAREST/ClampToEdge sampler consumers should read the output through.
    const coopa::gfx::engine::util::Sampler& sampler() const { return *nearest_sampler_; }

    /// Drops the accumulated history. Call on any frame execute() is skipped, so the next
    /// enabled frame does not average against occlusion captured under an arbitrary old pose.
    void invalidate_history() { history_initialized_ = false; }

private:
    /// Copies the resolve into the history image for next frame. Structurally identical to
    /// SsrPass::copy_history_(), over this pass's single image pair.
    void copy_history_(coopa::gfx::command::CommandBuffer& cmd);

    coopa::gfx::core::Device& device_;
    uint32_t width_;
    uint32_t height_;

    std::unique_ptr<coopa::gfx::engine::util::Sampler> nearest_sampler_;
    std::unique_ptr<coopa::gfx::engine::util::Sampler> linear_sampler_;

    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> march_target_;
    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> resolved_target_;
    /// Last frame's copy of the resolve, the surface the reprojection samples.
    std::unique_ptr<coopa::gfx::memory::Image> history_image_;
    bool history_initialized_ = false;

    std::unique_ptr<coopa::gfx::pipeline::Shader> vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> march_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> resolve_shader_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> gbuf_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> resolve_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      desc_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       gbuf_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       resolve_sets_[2];   ///< [count parity]

    std::unique_ptr<coopa::gfx::pipeline::Pipeline> march_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> resolve_pipeline_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_CONTACT_SHADOW_PASS_H
