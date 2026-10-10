#include <toyengine/render/passes/debug_line_pass.h>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/vertex_layout.h>

namespace toy {
namespace render {

uint32_t pack_gpu_color(uint32_t rrggbbaa) {
    uint32_t r = (rrggbbaa >> 24) & 0xFFu;
    uint32_t g = (rrggbbaa >> 16) & 0xFFu;
    uint32_t b = (rrggbbaa >> 8) & 0xFFu;
    uint32_t a = rrggbbaa & 0xFFu;
    return r | (g << 8) | (b << 16) | (a << 24);
}

} // namespace render
} // namespace toy

namespace toy {
namespace render {
namespace passes {

DebugLinePass::DebugLinePass(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
              coopa::gfx::pipeline::RenderPass& target_pass,
              const std::string& vert_spv, const std::string& frag_spv,
              uint32_t initial_capacity_vertices)
    : device_(device), allocator_(allocator)
{
    using namespace coopa::gfx;

    vert_shader_ = std::make_unique<pipeline::Shader>(device, vert_spv, ShaderStage::Vertex);
    frag_shader_ = std::make_unique<pipeline::Shader>(device, frag_spv, ShaderStage::Fragment);

    pipeline::PipelineDesc desc;
    desc.shaders = {vert_shader_.get(), frag_shader_.get()};
    desc.vertex = VertexLayout{}
        .binding(0, sizeof(Vertex))
        .attribute(0, Format::RGB32_Sfloat, static_cast<uint32_t>(offsetof(Vertex, pos)))
        .attribute(1, Format::RGBA8_Unorm, static_cast<uint32_t>(offsetof(Vertex, color)));
    desc.raster.topology = Topology::LineList;
    // polygon stays the default PolygonMode::Fill deliberately: Line (wireframe) mode
    // rasterizes the EDGES of a filled polygon and requires the fillModeNonSolid device
    // feature, which this app doesn't request -- pipeline creation fails validation
    // outright (VUID-VkPipelineRasterizationStateCreateInfo-polygonMode-01507) if set,
    // regardless of whether the pass ever draws. Neither applies to LineList topology in
    // the first place: there's no polygon to fill or outline, only Fill is meaningful/
    // universally supported, and Vulkan renders LineList primitives as thin lines under it.
    desc.raster.cull = CullMode::None;
    desc.raster.line_width = 1.0f; // >1.0 needs the unenabled wideLines device feature
    desc.depth.test = false;       // X-ray gizmo look; occluded lines test in the shader
    desc.depth.write = false;
    desc.blend.mode = pipeline::BlendMode::Alpha;   // faint lines (grid) fade
    depth_layout_ = std::make_unique<pipeline::DescriptorSetLayout>(
        pipeline::DescriptorLayoutBuilder().combined_sampler(0, ShaderStage::Fragment).build(device));
    depth_pool_ = std::make_unique<pipeline::DescriptorPool>(
        pipeline::DescriptorPoolBuilder().add_sets(*depth_layout_, 1).build(device));
    depth_set_ = std::make_unique<pipeline::DescriptorSet>(device, *depth_pool_, *depth_layout_);
    desc.descriptor_layouts = {depth_layout_.get()};
    desc.push_constants = {{ShaderStage::Vertex | ShaderStage::Fragment, 0, sizeof(PushConstants)}};

    pipeline_ = std::make_unique<pipeline::Pipeline>(device, target_pass, desc);

    for (uint32_t i = 0; i < kFrames; ++i) {
        capacity_[i] = initial_capacity_vertices;
        vbo_[i] = std::make_unique<memory::Buffer>(
            memory::Buffer::vertex(device_, allocator_, capacity_[i] * sizeof(Vertex)));
    }
}

void DebugLinePass::set_scene_depth(coopa::gfx::TextureView depth, const coopa::gfx::engine::util::Sampler& nearest,
                     uint32_t width, uint32_t height) {
    depth_set_->bind_image(0, depth, nearest);
    depth_bound_ = true;
    inv_size_ = glm::vec2(1.0f / std::max(1u, width), 1.0f / std::max(1u, height));
}

void DebugLinePass::upload(uint32_t frame_index, const std::vector<DebugLine>& lines) {
    frame_index_ = frame_index;
    vertex_count_ = static_cast<uint32_t>(lines.size()) * 2;
    xray_count_ = 0;
    if (lines.empty()) return;

    ensure_capacity_(frame_index, vertex_count_);

    scratch_.clear();
    scratch_.reserve(vertex_count_);
    for (int pass = 0; pass < 2; ++pass) {
        for (const DebugLine& line : lines) {
            if (line.occluded != (pass == 1)) continue;
            scratch_.push_back(Vertex{line.a, line.color});
            scratch_.push_back(Vertex{line.b, line.color});
        }
        if (pass == 0) xray_count_ = static_cast<uint32_t>(scratch_.size());
    }
    vbo_[frame_index]->upload(scratch_.data(), sizeof(Vertex) * scratch_.size());
}

void DebugLinePass::draw(coopa::gfx::command::CommandBuffer& cmd, const glm::mat4& view_proj, const LetterboxRect& rect) const {
    if (vertex_count_ == 0) return;
    cmd.bind_pipeline(*pipeline_);
    cmd.set_viewport(static_cast<float>(rect.x), static_cast<float>(rect.y + static_cast<int32_t>(rect.h)),
                     static_cast<float>(rect.w), -static_cast<float>(rect.h));
    cmd.set_scissor(rect.x, rect.y, rect.w, rect.h);
    cmd.bind_descriptor_set(*depth_set_, 0);
    cmd.bind_vertex_buffer(*vbo_[frame_index_]);
    const auto stages = coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment;
    // Occluded lines first, so X-ray gizmos and wireframes blend on top of the grid.
    const uint32_t occluded = vertex_count_ - xray_count_;
    if (occluded > 0 && depth_bound_) {
        cmd.push_constants(stages, PushConstants{view_proj, glm::vec4(1.0f, inv_size_, 0.003f)});
        cmd.draw(occluded, xray_count_);
    }
    if (xray_count_ > 0) {
        cmd.push_constants(stages, PushConstants{view_proj, glm::vec4(0.0f)});
        cmd.draw(xray_count_, 0);
    }
}

void DebugLinePass::ensure_capacity_(uint32_t frame_index, uint32_t needed_verts) {
    if (needed_verts <= capacity_[frame_index]) return;
    uint32_t new_capacity = std::max(needed_verts, capacity_[frame_index] * 2);
    vbo_[frame_index].reset(); // must be destroyed before the allocator creates the replacement
    vbo_[frame_index] = std::make_unique<coopa::gfx::memory::Buffer>(
        coopa::gfx::memory::Buffer::vertex(device_, allocator_, new_capacity * sizeof(Vertex)));
    capacity_[frame_index] = new_capacity;
}

} // namespace passes
} // namespace render
} // namespace toy
