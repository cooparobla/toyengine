/**
 * @file taa_pass.h
 * @brief Temporal Anti-Aliasing (TAA) pass.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_TAA_PASS_H
#define GFXCOOPA_ENGINE_PASSES_TAA_PASS_H

#include <volk/volk.h>
#include <string>
#include <memory>
#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/memory/image.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class TaaPass
 * @brief Temporal anti-aliasing: reprojects an accumulation history through the camera's
 *        frame-to-frame motion and blends it with the current frame.
 *
 * Two internal RGBA16F targets ping-pong as the accumulation buffer: each frame the resolve
 * renders into one while sampling the other as history, then a passthrough draw copies the
 * result into the caller's shared AA output target (RGBA8, alpha forced opaque -- the 16F
 * buffer's alpha carries the per-pixel accumulation age and its colour carries the sub-8-bit
 * increments late accumulation needs, neither of which may leak into the screenshot path).
 * The ping-pong needs no copy-based history update; the render passes' own layout
 * transitions do all the synchronisation.
 *
 * Reprojection uses the caller's velocity image when set_source_images() is given one (the
 * G-buffer's per-object motion vectors, so moving objects are followed exactly), and
 * otherwise falls back to camera-only reprojection from the scene depth buffer and the
 * caller-supplied reprojection matrix (see Params::reproject), where dynamic objects rely on
 * the resolve shader's variance clip alone. recreate() drops the accumulation, since it no
 * longer matches a new resolution.
 *
 * An optional REACTIVE mask (set_source_images()'s fourth image; R8, render extent) marks
 * pixels whose content moves in ways no motion vector describes -- particles, mostly. The
 * resolve trusts the current frame there in proportion to the mask (Unreal's "responsive AA",
 * FSR2's reactive mask), so a raindrop is drawn at full strength where it is and its old
 * position is rejected next frame instead of lingering as a faint streak.
 */
class TaaPass {
public:
    /// @brief Fragment push constants; must match taa.frag's block exactly.
    struct PushConstants {
        glm::mat4 reproject;
        glm::vec2 resolution;
        glm::vec2 jitter_ndc;
        float     feedback_still;
        float     feedback_motion;
        float     velocity_scale;
        float     sharpness;
        float     variance_gamma;
        int32_t   history_valid;
        int32_t   use_velocity;   ///< 1 when binding 3 holds a velocity image (set_source_images).
        int32_t   use_reactive;   ///< 1 when binding 4 holds a reactive mask (set_source_images).
    };
    static_assert(sizeof(PushConstants) == 112, "taa.frag's PushConstants block must match this layout byte-for-byte");

    /// @brief Per-frame resolve inputs, filled by the caller each draw.
    struct Params {
        /** Previous frame's UNJITTERED view-projection times the inverse of the current
         *  JITTERED one: maps current clip space to last frame's unjittered clip space. */
        glm::mat4 reproject       = glm::mat4(1.0f);
        /** False until `reproject` describes a real previous frame; the resolve outputs the
         *  current frame unblended while false (and while the history image is uninitialised
         *  -- the two validity conditions are ANDed, same as SsrPass's history gate). */
        bool      reproject_valid = false;
        glm::vec2 jitter_ndc      = glm::vec2(0.0f); /**< This frame's jitter as an NDC displacement. */
        float     feedback_still  = 0.98f; /**< History weight the accumulation converges to at rest. */
        float     feedback_motion = 0.85f; /**< History weight floor under fast motion. */
        float     velocity_scale  = 30.0f; /**< Feedback hits its floor at ~100/scale px of velocity. */
        float     sharpness       = 0.25f; /**< Motion-gated high-frequency restore; 0 disables. */
        float     variance_gamma  = 1.0f;  /**< History clip width in standard deviations. */
    };

    /**
     * @param output_render_pass The shared AA output target's render pass (RGBA8), which the
     *   present stage draws into. The resolve stage renders into the pass's own internal
     *   RGBA16F targets and is independent of it.
     * @param linear_sampler  For the scene, history and resolved-colour fetches.
     * @param nearest_sampler For the depth fetch.
     */
    TaaPass(coopa::gfx::core::Device& device,
            coopa::gfx::memory::Allocator& allocator,
            coopa::gfx::pipeline::RenderPass& output_render_pass,
            util::Sampler& linear_sampler,
            util::Sampler& nearest_sampler,
            uint32_t width,
            uint32_t height,
            const std::string& vert_path,
            const std::string& resolve_frag_path,
            const std::string& present_frag_path)
        : device_(device), allocator_(allocator),
          linear_sampler_(linear_sampler), nearest_sampler_(nearest_sampler),
          width_(width), height_(height)
    {
        for (int i = 0; i < 2; ++i) {
            accum_targets_[i] = std::make_unique<targets::OffscreenTarget>(
                device, allocator, width, height,
                coopa::gfx::Format::RGBA16_Sfloat, targets::kColorOnly);
        }

        resolve_stage_ = std::make_unique<FullscreenStage>(
            device, accum_targets_[0]->render_pass_object(),
            describe_resolve(vert_path, resolve_frag_path));
        present_stage_ = std::make_unique<FullscreenStage>(
            device, output_render_pass,
            describe_present(vert_path, present_frag_path));
    }

    void recreate(uint32_t width, uint32_t height) {
        width_  = width;
        height_ = height;
        accum_targets_[0]->recreate(width, height);
        accum_targets_[1]->recreate(width, height);
        history_initialized_ = false;
        parity_ = 0;

        // The recreated targets keep their formats, so the resolve pipeline (built against the
        // old render pass) stays render-pass compatible; only the image bindings went stale.
        if (last_scene_view_ != coopa::gfx::TextureView::null()) {
            set_source_images(last_scene_view_, last_depth_view_, last_velocity_view_, last_reactive_view_);
        }
    }

    /**
     * @brief Binds the resolve inputs for both ping-pong instances.
     * @param scene_view    The post-processed LDR scene (this frame's jittered render).
     * @param depth_view    The scene depth buffer, at the same resolution.
     * @param velocity_view The G-buffer's velocity attachment (GBufferTarget::g4_view_typed(),
     *                      xy = uv motion since last frame, unjittered-to-unjittered), or null
     *                      for camera-only reprojection through Params::reproject.
     * @param reactive_view A reactive mask (r = 0..1, how much to trust the current frame over
     *                      history), written every frame before draw(), or null for none.
     */
    void set_source_images(coopa::gfx::TextureView scene_view,
                           coopa::gfx::TextureView depth_view,
                           coopa::gfx::TextureView velocity_view = coopa::gfx::TextureView::null(),
                           coopa::gfx::TextureView reactive_view = coopa::gfx::TextureView::null()) {
        last_scene_view_    = scene_view;
        last_depth_view_    = depth_view;
        last_velocity_view_ = velocity_view;
        last_reactive_view_ = reactive_view;
        const bool has_velocity = velocity_view != coopa::gfx::TextureView::null();
        const bool has_reactive = reactive_view != coopa::gfx::TextureView::null();
        for (uint32_t i = 0; i < 2; ++i) {
            // Instance i renders into accum_targets_[i] and reads the OTHER as history.
            resolve_stage_->set(0, i).bind_image(0, scene_view, linear_sampler_);
            resolve_stage_->set(0, i).bind_image(1, accum_targets_[1 - i]->color_view_typed(),
                                                 linear_sampler_);
            resolve_stage_->set(0, i).bind_image(2, depth_view, nearest_sampler_);
            // Binding 3 must hold a valid image even when the shader's use_velocity branch is
            // off; the depth view stands in then.
            resolve_stage_->set(0, i).bind_image(3, has_velocity ? velocity_view : depth_view,
                                                 nearest_sampler_);
            // Binding 4 likewise always holds an image; the depth view stands in without a mask.
            resolve_stage_->set(0, i).bind_image(4, has_reactive ? reactive_view : depth_view,
                                                 linear_sampler_);
            present_stage_->set(0, i).bind_image(0, accum_targets_[i]->color_view_typed(),
                                                 linear_sampler_);
        }
    }

    /**
     * @brief On the first frame (and after recreate()), moves both ping-pong images out of
     *        UNDEFINED so their sampled-image descriptors are legal before either has been
     *        rendered to. The resolve shader never reads the garbage: history_valid stays 0
     *        until update at the end of the first draw().
     */
    void prepare_history(coopa::gfx::command::CommandBuffer& cmd) {
        if (history_initialized_) return;
        VkImageMemoryBarrier barriers[2]{};
        for (int i = 0; i < 2; ++i) {
            barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].image = accum_targets_[i]->color_image_object()->handle();
            barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barriers[i].subresourceRange.levelCount = 1;
            barriers[i].subresourceRange.layerCount = 1;
            barriers[i].srcAccessMask = 0;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);
    }

    /**
     * @brief Records the whole pass: the resolve into this frame's accumulation target, then
     *        the passthrough into `output`. Owns both render-pass brackets (like SmaaPass;
     *        the caller must NOT bracket `output` itself).
     *
     * @param cmd    Command buffer to record into.
     * @param output The shared AA output target the final colour lands in.
     * @param params This frame's resolve inputs.
     * @param width  Render width in pixels.
     * @param height Render height in pixels.
     */
    void draw(coopa::gfx::command::CommandBuffer& cmd,
              targets::OffscreenTarget& output,
              const Params& params,
              uint32_t width, uint32_t height) {
        PushConstants pc{};
        pc.reproject       = params.reproject;
        pc.resolution      = glm::vec2(static_cast<float>(width), static_cast<float>(height));
        pc.jitter_ndc      = params.jitter_ndc;
        pc.feedback_still  = params.feedback_still;
        pc.feedback_motion = params.feedback_motion;
        pc.velocity_scale  = params.velocity_scale;
        pc.sharpness       = params.sharpness;
        pc.variance_gamma  = params.variance_gamma;
        pc.history_valid   = (history_initialized_ && params.reproject_valid) ? 1 : 0;
        pc.use_velocity    = (last_velocity_view_ != coopa::gfx::TextureView::null()) ? 1 : 0;
        pc.use_reactive    = (last_reactive_view_ != coopa::gfx::TextureView::null()) ? 1 : 0;

        targets::OffscreenTarget& accum = *accum_targets_[parity_];
        accum.begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
        resolve_stage_->draw(cmd, width, height, coopa::gfx::ShaderStage::Fragment, pc, parity_);
        accum.end(cmd);

        output.begin(cmd);
        present_stage_->bind(cmd, width, height, parity_);
        present_stage_->draw(cmd);
        output.end(cmd);

        history_initialized_ = true;
        parity_ ^= 1u;
    }

private:
    /// @brief Scene + history + depth + velocity + reactive at bindings 0-4, two instances for the ping-pong.
    static FullscreenStageDesc describe_resolve(const std::string& vert_spv,
                                                const std::string& frag_spv) {
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {{{0, coopa::gfx::DescriptorType::CombinedImageSampler,
                          coopa::gfx::ShaderStage::Fragment, 1},
                         {1, coopa::gfx::DescriptorType::CombinedImageSampler,
                          coopa::gfx::ShaderStage::Fragment, 1},
                         {2, coopa::gfx::DescriptorType::CombinedImageSampler,
                          coopa::gfx::ShaderStage::Fragment, 1},
                         {3, coopa::gfx::DescriptorType::CombinedImageSampler,
                          coopa::gfx::ShaderStage::Fragment, 1},
                         {4, coopa::gfx::DescriptorType::CombinedImageSampler,
                          coopa::gfx::ShaderStage::Fragment, 1}}};
        d.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        d.instances = 2;
        return d;
    }

    /// @brief The resolved colour at binding 0, two instances for the ping-pong.
    static FullscreenStageDesc describe_present(const std::string& vert_spv,
                                                const std::string& frag_spv) {
        FullscreenStageDesc d;
        d.vert_spv = vert_spv;
        d.frag_spv = frag_spv;
        d.owned_sets = {{{0, coopa::gfx::DescriptorType::CombinedImageSampler,
                          coopa::gfx::ShaderStage::Fragment, 1}}};
        d.instances = 2;
        return d;
    }

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;
    util::Sampler& linear_sampler_;
    util::Sampler& nearest_sampler_;
    uint32_t width_;
    uint32_t height_;

    bool     history_initialized_ = false;
    /// @brief Which accumulation target this frame renders into; the other is history.
    uint32_t parity_ = 0;

    coopa::gfx::TextureView last_scene_view_    = coopa::gfx::TextureView::null();
    coopa::gfx::TextureView last_depth_view_    = coopa::gfx::TextureView::null();
    coopa::gfx::TextureView last_velocity_view_ = coopa::gfx::TextureView::null();
    coopa::gfx::TextureView last_reactive_view_ = coopa::gfx::TextureView::null();

    /**
     * The accumulation ping-pong. RGBA16F: rgb needs sub-8-bit increments (late accumulation
     * adds as little as 1/64th of an LSB per frame, which an RGBA8 buffer would round away and
     * stall), and alpha stores the integer sample age (0..63, exactly representable). Holds
     * sRGB-ENCODED values, matching post_target_/aa_target_'s convention -- the resolve blends
     * in encoded space, as the other AA modes filter in it.
     */
    std::unique_ptr<targets::OffscreenTarget> accum_targets_[2];

    std::unique_ptr<FullscreenStage> resolve_stage_;
    std::unique_ptr<FullscreenStage> present_stage_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_TAA_PASS_H
