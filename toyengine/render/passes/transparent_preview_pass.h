/**
 * @file transparent_preview_pass.h
 * @brief BLEND (transparent) meshes for the editor's viewport shading modes -- Solid,
 *        Material Preview and Wireframe -- drawn over the debug-view image.
 *
 * Those modes replace the stylized frame with debug_view_pass_'s G-buffer readout, and BLEND
 * meshes are never in the G-buffer (they are forward-drawn into the HDR image the readout
 * ignores), so without this pass glass simply vanished in the editor. It draws each BLEND
 * mesh, back-to-front, with the same shading maths as the opaque surfaces
 * (editor_shading.glsl) and the material's alpha.
 *
 * A guest inside post_target_'s already-open bracket, exactly like DebugLinePass (see its doc
 * for why this pipeline is built against that render pass rather than opening its own). That
 * target carries no scene depth, so occlusion by opaque geometry is a per-fragment compare
 * against the sampled G-buffer depth (set 1), as in debug_line.frag / ui_world_occlude.glsl.
 *
 * Vertex stage: transparent.vert, the forward-transparent backbone -- same vertex + per-instance
 * streams, camera set 0 and the first 64 push-constant bytes as TransparentPass.
 */

#ifndef TOYENGINE_RENDER_PASSES_TRANSPARENT_PREVIEW_PASS_H
#define TOYENGINE_RENDER_PASSES_TRANSPARENT_PREVIEW_PASS_H

#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/types/enums.h>

#include <toyengine/render/pixel_math.h>

namespace toy {
namespace render {
namespace passes {

class TransparentPreviewPass {
public:
    /** @brief transparent_preview.frag's push block; the first 64 bytes are transparent.vert's. */
    struct PushConstants {
        glm::vec4 albedo{1.0f};      ///< rgb albedo, a = material alpha
        float metallic = 0.0f;
        float roughness = 0.5f;
        float ao = 1.0f;
        float alpha_cutoff = 0.0f;   ///< unused (transparent.vert layout)
        glm::vec4 gfx_time{0.0f};
        glm::vec4 gfx_params{0.0f};
        glm::vec4 view{0.0f};        ///< x: mode (1 solid, 2 material preview, 3 wireframe), yz: 1/target size
        glm::vec4 emissive{0.0f};    ///< rgb pre-multiplied emissive
    };
    static_assert(sizeof(PushConstants) == 96, "must match transparent_preview.frag");

    TransparentPreviewPass(coopa::gfx::core::Device& device, coopa::gfx::pipeline::RenderPass& target_pass,
                           const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                           const coopa::gfx::pipeline::DescriptorSetLayout& material_layout,
                           const std::string& vert_spv, const std::string& frag_spv) {
        using namespace coopa::gfx;
        vert_ = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
        frag_ = std::make_unique<pipeline::Shader>(device, frag_spv, ShaderStage::Fragment);
        depth_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
            pipeline::DescriptorLayoutBuilder().combined_sampler(0, ShaderStage::Fragment).build(device));
        depth_pool_ = std::make_unique<pipeline::DescriptorPool>(
            pipeline::DescriptorPoolBuilder().add_sets(*depth_layout_, 1).build(device));
        depth_set_ = std::make_unique<pipeline::DescriptorSet>(device, *depth_pool_, *depth_layout_);

        pipeline::PipelineDesc desc;
        desc.shaders = {vert_.get(), frag_.get()};
        desc.vertex = engine::data::Vertex::layout().append(engine::data::InstanceData::layout());
        desc.descriptor_layouts = {&camera_layout, depth_layout_.get(), &material_layout};
        desc.push_constants = {{ShaderStage::Vertex | ShaderStage::Fragment, 0, sizeof(PushConstants)}};
        desc.raster.cull = CullMode::None;     // glass reads from both sides in the editor
        desc.depth.test = false;               // occlusion is tested in the shader (file doc)
        desc.depth.write = false;
        desc.blend.mode = pipeline::BlendMode::Alpha;
        pipeline_ = std::make_unique<pipeline::Pipeline>(device, target_pass, desc);
    }

    TransparentPreviewPass(const TransparentPreviewPass&) = delete;
    TransparentPreviewPass& operator=(const TransparentPreviewPass&) = delete;

    /** @brief The scene depth to test against (bound once; the G-buffer is never recreated). */
    void set_scene_depth(coopa::gfx::TextureView depth, const coopa::gfx::engine::util::Sampler& nearest,
                         uint32_t width, uint32_t height) {
        depth_set_->bind_image(0, depth, nearest);
        inv_size_ = glm::vec2(1.0f / std::max(1u, width), 1.0f / std::max(1u, height));
    }

    /** @brief Binds the pipeline, viewport (Y-flipped like DebugLinePass) and sets 0 / 1. */
    void begin(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::DescriptorSet& camera_set,
               const LetterboxRect& rect) const {
        cmd.bind_pipeline(*pipeline_);
        cmd.set_viewport(static_cast<float>(rect.x), static_cast<float>(rect.y + static_cast<int32_t>(rect.h)),
                         static_cast<float>(rect.w), -static_cast<float>(rect.h));
        cmd.set_scissor(rect.x, rect.y, rect.w, rect.h);
        cmd.bind_descriptor_set(camera_set, 0);
        cmd.bind_descriptor_set(*depth_set_, 1);
    }

    /** @brief Per-object push constants + material set (call between begin() and the draw). */
    void push(coopa::gfx::command::CommandBuffer& cmd, PushConstants pc,
              const coopa::gfx::pipeline::DescriptorSet& material_set) const {
        pc.view.y = inv_size_.x;
        pc.view.z = inv_size_.y;
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
        cmd.bind_descriptor_set(material_set, 2);
    }

private:
    std::unique_ptr<coopa::gfx::pipeline::Shader> vert_, frag_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> depth_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool> depth_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> depth_set_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline_;
    glm::vec2 inv_size_{1.0f};
};

}  // namespace passes
}  // namespace render
}  // namespace toy

#endif  // TOYENGINE_RENDER_PASSES_TRANSPARENT_PREVIEW_PASS_H
