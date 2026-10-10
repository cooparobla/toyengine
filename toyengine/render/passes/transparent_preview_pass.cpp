#include <toyengine/render/passes/transparent_preview_pass.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/types/enums.h>

namespace toy {
namespace render {
namespace passes {

TransparentPreviewPass::TransparentPreviewPass(coopa::gfx::core::Device& device, coopa::gfx::pipeline::RenderPass& target_pass,
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

void TransparentPreviewPass::set_scene_depth(coopa::gfx::TextureView depth, const coopa::gfx::engine::util::Sampler& nearest,
                     uint32_t width, uint32_t height) {
    depth_set_->bind_image(0, depth, nearest);
    inv_size_ = glm::vec2(1.0f / std::max(1u, width), 1.0f / std::max(1u, height));
}

void TransparentPreviewPass::begin(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::DescriptorSet& camera_set,
           const LetterboxRect& rect) const {
    cmd.bind_pipeline(*pipeline_);
    cmd.set_viewport(static_cast<float>(rect.x), static_cast<float>(rect.y + static_cast<int32_t>(rect.h)),
                     static_cast<float>(rect.w), -static_cast<float>(rect.h));
    cmd.set_scissor(rect.x, rect.y, rect.w, rect.h);
    cmd.bind_descriptor_set(camera_set, 0);
    cmd.bind_descriptor_set(*depth_set_, 1);
}

void TransparentPreviewPass::push(coopa::gfx::command::CommandBuffer& cmd, PushConstants pc,
          const coopa::gfx::pipeline::DescriptorSet& material_set) const {
    pc.view.y = inv_size_.x;
    pc.view.z = inv_size_.y;
    cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
    cmd.bind_descriptor_set(material_set, 2);
}

} // namespace passes
} // namespace render
} // namespace toy
