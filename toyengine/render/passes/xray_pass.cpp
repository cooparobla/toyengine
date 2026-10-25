#include <toyengine/render/passes/xray_pass.h>

#include <algorithm>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/texture_view.h>

namespace toy {
namespace render {
namespace passes {

XrayPass::XrayPass(coopa::gfx::core::Device& device,
                   coopa::gfx::memory::Allocator& allocator,
                   coopa::gfx::pipeline::RenderPass& composite_pass,
                   const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                   uint32_t width, uint32_t height,
                   const std::string& mesh_vert_spv,
                   const std::string& mask_frag_spv,
                   const std::string& fullscreen_vert_spv,
                   const std::string& composite_frag_spv)
    : mask_target_(device, allocator, std::max(1u, width), std::max(1u, height),
                   kMaskFormat, coopa::gfx::engine::targets::kColorOnly)
{
    using namespace coopa::gfx;

    inv_size_ = glm::vec2(1.0f / static_cast<float>(std::max(1u, width)),
                          1.0f / static_cast<float>(std::max(1u, height)));

    mesh_vert_       = std::make_unique<pipeline::Shader>(device, mesh_vert_spv, ShaderStage::Vertex);
    mask_frag_       = std::make_unique<pipeline::Shader>(device, mask_frag_spv, ShaderStage::Fragment);
    fullscreen_vert_ = std::make_unique<pipeline::Shader>(device, fullscreen_vert_spv, ShaderStage::Vertex);
    composite_frag_  = std::make_unique<pipeline::Shader>(device, composite_frag_spv, ShaderStage::Fragment);

    // --- Mask stage ---
    // Scene depth at set 1, matching transparent_preview.frag's arrangement so the vertex
    // shader (transparent.vert, which owns set 0) is reused untouched.
    depth_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
        pipeline::DescriptorLayoutBuilder().combined_sampler(0, ShaderStage::Fragment).build(device));
    depth_pool_ = std::make_unique<pipeline::DescriptorPool>(
        pipeline::DescriptorPoolBuilder().add_sets(*depth_layout_, 1).build(device));
    depth_set_ = std::make_unique<pipeline::DescriptorSet>(device, *depth_pool_, *depth_layout_);

    {
        pipeline::PipelineDesc desc;
        desc.shaders = {mesh_vert_.get(), mask_frag_.get()};
        desc.vertex  = engine::data::Vertex::layout().append(engine::data::InstanceData::layout());
        desc.descriptor_layouts = {&camera_layout, depth_layout_.get()};
        desc.push_constants = {{ShaderStage::Vertex | ShaderStage::Fragment, 0, sizeof(MaskPushConstants)}};
        // Two-sided: a subject seen from inside its own geometry (a camera clipping into the
        // player) still has to produce a silhouette, and back faces cost nothing on a mask.
        desc.raster.cull = CullMode::None;
        // No hardware depth test -- the target has no depth attachment and the compare is
        // per-fragment against the SAMPLED G-buffer depth. See the file doc.
        desc.depth.test  = false;
        desc.depth.write = false;
        // Max, so overlapping subject meshes union into one shape rather than summing or
        // fighting over draw order. See the file doc for why neither alternative works.
        desc.blend.mode = pipeline::BlendMode::Max;
        mask_pipeline_ = std::make_unique<pipeline::Pipeline>(device, mask_target_.render_pass_object(), desc);
    }

    // --- Composite stage ---
    mask_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
        pipeline::DescriptorLayoutBuilder().combined_sampler(0, ShaderStage::Fragment).build(device));
    mask_pool_ = std::make_unique<pipeline::DescriptorPool>(
        pipeline::DescriptorPoolBuilder().add_sets(*mask_layout_, 1).build(device));
    mask_set_ = std::make_unique<pipeline::DescriptorSet>(device, *mask_pool_, *mask_layout_);

    {
        pipeline::PipelineDesc desc;
        desc.shaders = {fullscreen_vert_.get(), composite_frag_.get()};
        desc.descriptor_layouts = {mask_layout_.get()};
        desc.push_constants = {{ShaderStage::Fragment, 0, sizeof(CompositePushConstants)}};
        desc.raster.cull = CullMode::None;
        desc.depth.test  = false;
        desc.depth.write = false;
        desc.blend.mode  = pipeline::BlendMode::Alpha;
        composite_pipeline_ = std::make_unique<pipeline::Pipeline>(device, composite_pass, desc);
    }
}

XrayPass::~XrayPass() = default;

void XrayPass::set_scene_depth(coopa::gfx::TextureView scene_depth,
                               const coopa::gfx::engine::util::Sampler& nearest) {
    depth_set_->bind_image(0, scene_depth, nearest);
    // The composite samples its own mask with the same NEAREST sampler the depth compare uses.
    // The outline's ring tap steps whole texels, so there is nothing for a filter to improve,
    // and a LINEAR tap would bleed coverage a texel outward past the subject's real edge.
    mask_set_->bind_image(0, mask_target_.color_view_typed(), nearest);
}

void XrayPass::begin_mask(coopa::gfx::command::CommandBuffer& cmd,
                          const coopa::gfx::pipeline::DescriptorSet& camera_set,
                          const LetterboxRect& rect) const {
    mask_target_.begin(cmd, coopa::gfx::ClearColor{0.0f, 0.0f, 0.0f, 0.0f});
    cmd.bind_pipeline(*mask_pipeline_);
    // Negative height, like DebugLinePass and TransparentPreviewPass: these vertices go through
    // a real view_proj, so a positive-height viewport would mirror the mask vertically. Set
    // explicitly rather than inherited from begin(), so correctness here does not depend on
    // what an unrelated target happened to leave behind.
    cmd.set_viewport(static_cast<float>(rect.x), static_cast<float>(rect.y + static_cast<int32_t>(rect.h)),
                     static_cast<float>(rect.w), -static_cast<float>(rect.h));
    cmd.set_scissor(rect.x, rect.y, rect.w, rect.h);
    cmd.bind_descriptor_set(camera_set, 0);
    cmd.bind_descriptor_set(*depth_set_, 1);
}

void XrayPass::push_mask(coopa::gfx::command::CommandBuffer& cmd, MaskPushConstants pc) const {
    pc.params.y = inv_size_.x;
    pc.params.z = inv_size_.y;
    cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
}

void XrayPass::end_mask(coopa::gfx::command::CommandBuffer& cmd) const {
    mask_target_.end(cmd);
}

void XrayPass::draw_composite(coopa::gfx::command::CommandBuffer& cmd, CompositePushConstants pc,
                              uint32_t width, uint32_t height) const {
    pc.inv_size = inv_size_;
    cmd.bind_pipeline(*composite_pipeline_);
    cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    cmd.set_scissor(0, 0, width, height);
    cmd.bind_descriptor_set(*mask_set_);
    cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pc);
    cmd.draw(3);   // fullscreen triangle
}

} // namespace passes
} // namespace render
} // namespace toy
