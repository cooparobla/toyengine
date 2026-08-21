/**
 * @file debug_gradient_pass.h
 * @brief Draws a checkerboard/gradient test pattern into the low-resolution
 *        offscreen target -- a Phase 2 milestone diagnostic for verifying the
 *        integer upscale and nearest-neighbour filtering before any real
 *        geometry exists (see assets/shaders/debug_gradient.frag).
 *
 * No descriptor sets or push constants: the fragment shader derives its
 * output purely from gl_FragCoord/UV, so this pass is the simplest possible
 * fullscreen draw and a useful reference for the shape every other
 * fullscreen pass (pixel_lighting, pixel_post, upscale) follows.
 */

#ifndef TOYENGINE_RENDER_PASSES_DEBUG_GRADIENT_PASS_H
#define TOYENGINE_RENDER_PASSES_DEBUG_GRADIENT_PASS_H

#include <volk/volk.h>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/command/command_buffer.h>

namespace toy {
namespace render {
namespace passes {

class DebugGradientPass {
public:
    DebugGradientPass(coopa::gfx::core::Device& device,
                      coopa::gfx::pipeline::RenderPass& target_pass,
                      const std::string& vert_spv,
                      const std::string& frag_spv)
    {
        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        coopa::gfx::pipeline::PipelineConfig cfg{};
        cfg.cull_mode   = VK_CULL_MODE_NONE;
        cfg.depth_test  = false;
        cfg.depth_write = false;

        pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, target_pass,
            std::vector<coopa::gfx::pipeline::Shader*>{vert_shader_.get(), frag_shader_.get()},
            std::vector<VkVertexInputBindingDescription>{},
            std::vector<VkVertexInputAttributeDescription>{},
            std::vector<VkDescriptorSetLayout>{},
            cfg,
            std::vector<VkPushConstantRange>{});
    }

    DebugGradientPass(const DebugGradientPass&) = delete;
    DebugGradientPass& operator=(const DebugGradientPass&) = delete;

    void draw(coopa::gfx::command::CommandBuffer& cmd, uint32_t viewport_w, uint32_t viewport_h) const {
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(viewport_w), static_cast<float>(viewport_h));
        cmd.set_scissor(0, 0, viewport_w, viewport_h);
        cmd.draw(3);
    }

private:
    std::unique_ptr<coopa::gfx::pipeline::Shader>   vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader>   frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_DEBUG_GRADIENT_PASS_H
