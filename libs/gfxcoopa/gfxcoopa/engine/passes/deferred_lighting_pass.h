/**
 * @file deferred_lighting_pass.h
 * @brief Deferred lighting pass header for gfxcoopa.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_DEFERRED_LIGHTING_PASS_H
#define GFXCOOPA_ENGINE_PASSES_DEFERRED_LIGHTING_PASS_H

#include <volk/volk.h>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/passes/extra_sets.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class DeferredLightingPass
 * @brief Shades the G-buffer into an HDR colour target, in one fullscreen draw.
 *
 * Owns the G-buffer/SSAO descriptor set (declared last in its pipeline layout,
 * after camera, light, shadow and any caller-supplied ExtraSets) and the
 * pipeline. The caller owns the camera/light/shadow sets and passes them to
 * draw(); G-buffer images are bound once via set_gbuffer_images(), and the SSAO
 * image is rebound every frame via set_ssao_image().
 */
class DeferredLightingPass {
public:
    DeferredLightingPass(coopa::gfx::core::Device& device,
                         coopa::gfx::pipeline::RenderPass& offscreen_pass,
                         const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                         const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                         const coopa::gfx::pipeline::DescriptorSetLayout& shadow_layout,
                         const util::Sampler& linear_sampler,
                         const std::string& vert_spv,
                         const std::string& frag_spv,
                         ExtraSets extra = {},
                         std::vector<coopa::gfx::pipeline::PushConstantRange> pc_ranges = {})
        : extra_(std::move(extra))
    {
        extra_.validate("DeferredLightingPass");

        // Layout order: camera, light, shadow, then any caller extras, then this pass's
        // own G-buffer/SSAO set LAST. Folding the first four into leading_layouts lets
        // FullscreenStage place the owned set after them, and first_owned_set() then is
        // the G-buffer set's index -- nothing here hardcodes either index.
        std::vector<const coopa::gfx::pipeline::DescriptorSetLayout*> leading = {
            &camera_layout, &light_layout, &shadow_layout
        };
        extra_first_set_ = static_cast<uint32_t>(leading.size());
        leading.insert(leading.end(), extra_.layouts.begin(), extra_.layouts.end());

        FullscreenStageDesc sd;
        sd.vert_spv = vert_spv;
        sd.frag_spv = frag_spv;
        sd.leading_layouts = leading;
        // 4 G-Buffer samplers (albedo/ao, normal/metallic, position/roughness, emissive)
        // + 1 SSAO sampler.
        sd.owned_sets.emplace_back();
        for (uint32_t i = 0; i < 5; ++i) {
            sd.owned_sets[0].push_back({i, coopa::gfx::DescriptorType::CombinedImageSampler,
                                        coopa::gfx::ShaderStage::Fragment, 1});
        }
        sd.push_constants = pc_ranges;

        stage_ = std::make_unique<FullscreenStage>(device, offscreen_pass, sd);

        (void)linear_sampler;
    }

    void set_gbuffer_images(coopa::gfx::TextureView g0_view,
                            coopa::gfx::TextureView g1_view,
                            coopa::gfx::TextureView g2_view,
                            coopa::gfx::TextureView g3_view,
                            const util::Sampler& linear_sampler) {
        stage_->set().bind_image(0, g0_view, linear_sampler);
        stage_->set().bind_image(1, g1_view, linear_sampler);
        stage_->set().bind_image(2, g2_view, linear_sampler);
        // Binding 4, not 3 -- SSAO keeps binding 3 (see set_ssao_image() below), so adding
        // emissive is a pure append in every consumer's fragment shader rather than a
        // renumber, and this set stays aligned with SsrPass's composite G-buffer set,
        // which also treats binding 3 as SSAO.
        stage_->set().bind_image(4, g3_view, linear_sampler);
    }

    /// Binding 3 must be rebound every frame -- callers pass the SSAO pass's blurred output when
    /// enabled, or its permanent neutral (fully-unoccluded) texture when disabled/absent, so this
    /// binding is never left pointing at an image still in VK_IMAGE_LAYOUT_UNDEFINED.
    ///
    /// Raw (VkImageView/VkSampler), unlike set_gbuffer_images() above: SsaoPass -- the only
    /// source of this argument -- has no TextureView-returning sibling of its output_view()/
    /// neutral_view() accessors, so a sealed parameter here would have no caller able to
    /// satisfy it.
    void set_ssao_image(VkImageView ssao_view, VkSampler ssao_sampler) {
        stage_->set().bind_image(3, ssao_view, ssao_sampler);
    }

    /// @brief Draws the fullscreen deferred-lighting pass, with no push-constant block
    /// (only valid if the ctor's pc_ranges was left empty).
    void draw(coopa::gfx::command::CommandBuffer& cmd,
              const coopa::gfx::pipeline::DescriptorSet& camera_set,
              const coopa::gfx::pipeline::DescriptorSet& light_set,
              const coopa::gfx::pipeline::DescriptorSet& shadow_set,
              uint32_t viewport_w, uint32_t viewport_h) const
    {
        stage_->bind(cmd, viewport_w, viewport_h);
        bind_sets_and_draw_(cmd, camera_set, light_set, shadow_set);
    }

    /// @brief Draws the fullscreen deferred-lighting pass, pushing `push_constants` to the
    /// fragment stage first -- the sealed replacement for a caller fetching layout() to push
    /// its own app-defined struct before calling the no-push-constant draw() above. `PushConstants`
    /// must match the byte layout the ctor's pc_ranges declared (this pass doesn't know that
    /// type -- it's app-defined, e.g. blendy's LightingPushConstants).
    template<typename PushConstants>
    void draw(coopa::gfx::command::CommandBuffer& cmd,
              const coopa::gfx::pipeline::DescriptorSet& camera_set,
              const coopa::gfx::pipeline::DescriptorSet& light_set,
              const coopa::gfx::pipeline::DescriptorSet& shadow_set,
              const PushConstants& push_constants,
              uint32_t viewport_w, uint32_t viewport_h) const
    {
        stage_->bind(cmd, viewport_w, viewport_h);
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, push_constants);

        bind_sets_and_draw_(cmd, camera_set, light_set, shadow_set);
    }

    VkPipelineLayout layout() const {
        return stage_->pipeline().layout();
    }

private:
    /// @brief Shared tail of both draw() overloads -- binds the pass's own 3 sets, any
    /// caller-supplied ExtraSets, the G-buffer/SSAO set, and issues the fullscreen draw.
    /// Relies on the caller having already called stage_->bind(), which binds the
    /// pipeline, the viewport/scissor, and the G-buffer set at its own index.
    void bind_sets_and_draw_(coopa::gfx::command::CommandBuffer& cmd,
                             const coopa::gfx::pipeline::DescriptorSet& camera_set,
                             const coopa::gfx::pipeline::DescriptorSet& light_set,
                             const coopa::gfx::pipeline::DescriptorSet& shadow_set) const
    {
        cmd.bind_descriptor_set(camera_set, 0);
        cmd.bind_descriptor_set(light_set, 1);
        cmd.bind_descriptor_set(shadow_set, 2);
        if (extra_.bind) {
            extra_.bind(cmd, extra_first_set_);
        }
        stage_->draw(cmd);
    }

    std::unique_ptr<FullscreenStage> stage_;

    ExtraSets extra_;
    uint32_t  extra_first_set_ = 0;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_DEFERRED_LIGHTING_PASS_H
