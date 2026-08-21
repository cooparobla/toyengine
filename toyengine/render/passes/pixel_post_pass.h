/**
 * @file pixel_post_pass.h
 * @brief Outline + exposure + ordered dither + palette quantization, all in
 *        one fullscreen fragment shader (see assets/shaders/pixel_post.frag).
 *
 * Reads the lit scene color plus the G-buffer's depth and normal (for the
 * outline edge detector), and a palette LUT. Writes into its own low-res LDR
 * target -- pipeline::RenderPass's hardcoded LOAD_OP_CLEAR
 * (gfxcoopa/pipeline/render_pass.h) means the lighting target it reads from
 * can't be reopened and composited onto in place.
 */

#ifndef TOYENGINE_RENDER_PASSES_PIXEL_POST_PASS_H
#define TOYENGINE_RENDER_PASSES_PIXEL_POST_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>

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

namespace toy {
namespace render {
namespace passes {

class PixelPostPass {
public:
    /** @brief Matches pixel_post.frag's PostParams push constant block. */
    struct PushConstants {
        glm::vec2 inv_render_size;
        float     exposure           = 1.0f;
        float     outline_thickness  = 1.0f;
        glm::vec4 outline_color      = glm::vec4(0.05f, 0.04f, 0.08f, 1.0f);
        float     depth_threshold    = 0.02f;
        float     normal_threshold   = 0.75f;
        float     dither_strength    = 0.0f;
        float     palette_count      = 0.0f;
    };
    static_assert(sizeof(PushConstants) == 48, "PushConstants must match pixel_post.frag's PostParams byte-for-byte");

    PixelPostPass(coopa::gfx::core::Device& device,
                 coopa::gfx::pipeline::RenderPass& target_pass,
                 const std::string& vert_spv,
                 const std::string& frag_spv)
    {
        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (uint32_t i = 0; i < 4; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding         = i;
            b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(b);
        }
        desc_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, bindings);
        desc_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            device, 1, std::vector<VkDescriptorPoolSize>{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}});
        desc_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *desc_layout_);

        coopa::gfx::pipeline::PipelineConfig cfg{};
        cfg.cull_mode   = VK_CULL_MODE_NONE;
        cfg.depth_test  = false;
        cfg.depth_write = false;

        VkPushConstantRange pc_range{};
        pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc_range.offset     = 0;
        pc_range.size       = sizeof(PushConstants);

        pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, target_pass,
            std::vector<coopa::gfx::pipeline::Shader*>{vert_shader_.get(), frag_shader_.get()},
            std::vector<VkVertexInputBindingDescription>{},
            std::vector<VkVertexInputAttributeDescription>{},
            std::vector<VkDescriptorSetLayout>{desc_layout_->handle()},
            cfg,
            std::vector<VkPushConstantRange>{pc_range});
    }

    PixelPostPass(const PixelPostPass&) = delete;
    PixelPostPass& operator=(const PixelPostPass&) = delete;

    void set_source_images(VkImageView scene_color, VkImageView scene_depth, VkImageView scene_normal,
                           VkImageView palette_lut, const coopa::gfx::engine::util::Sampler& linear_sampler,
                           const coopa::gfx::engine::util::Sampler& nearest_sampler) {
        desc_set_->bind_image(0, scene_color, linear_sampler.handle());
        desc_set_->bind_image(1, scene_depth, linear_sampler.handle());
        desc_set_->bind_image(2, scene_normal, linear_sampler.handle());
        desc_set_->bind_image(3, palette_lut, nearest_sampler.handle());
    }

    void draw(coopa::gfx::command::CommandBuffer& cmd, const PushConstants& params,
              uint32_t viewport_w, uint32_t viewport_h) const {
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(viewport_w), static_cast<float>(viewport_h));
        cmd.set_scissor(0, 0, viewport_w, viewport_h);
        cmd.bind_descriptor_set(pipeline_->layout(), *desc_set_, 0);
        cmd.push_constants(pipeline_->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &params);
        cmd.draw(3);
    }

private:
    std::unique_ptr<coopa::gfx::pipeline::Shader>              vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader>              frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> desc_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      desc_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       desc_set_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline>            pipeline_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_PIXEL_POST_PASS_H
