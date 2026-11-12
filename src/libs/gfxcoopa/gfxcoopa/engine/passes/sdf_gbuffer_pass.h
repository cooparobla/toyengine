/**
 * @file sdf_gbuffer_pass.h
 * @brief Raymarches every OPAQUE/MASK SdfRenderer into the opaque G-buffer.
 *
 * Drawn inside GBufferTarget's existing begin()/end() bracket, right after
 * the mesh loop -- see ToyRenderPipeline::record_gbuffer_(). Each draw is a
 * single 6-vertex, vertex-buffer-less quad (VertexLayout::none(), see
 * sdf_quad.vert) scissored to the renderer's screen-space (pre-upscale)
 * rectangle by the caller (cmd.set_scissor(), a plain dynamic pipeline
 * state); this pass owns no per-object geometry at all. Writes gl_FragDepth
 * from the marched hit position, so opaque SDF geometry interleaves
 * correctly with rasterized mesh depth and is picked up by SSAO, the outline
 * detector, deferred lighting, SSR and fog with no further work -- the whole
 * reason opaque SDFs go through the G-buffer rather than a forward pass (see
 * the SDF system's design plan).
 */

#ifndef GFXCOOPA_ENGINE_PASSES_SDF_GBUFFER_PASS_H
#define GFXCOOPA_ENGINE_PASSES_SDF_GBUFFER_PASS_H

#include <volk/volk.h>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class SdfGBufferPass
 * @brief Raymarches every OPAQUE/MASK SdfRenderer into the opaque G-buffer.
 *
 * Drawn inside GBufferTarget's existing begin()/end() bracket, after the mesh
 * G-buffer pass, so SDF shapes depth-test against rasterized geometry. Owns
 * only its pipeline.
 */
class SdfGBufferPass {
public:
    /// Vertex AND fragment stages both read this -- vertex to know nothing
    /// (the quad is index-only), fragment to index the SdfRenderer/SdfShape
    /// SSBOs. 4 bytes total: well under any push-constant budget concern.
    struct PushConstants {
        uint32_t renderer_index = 0;
    };

    /**
     * @param device        Logical device.
     * @param gbuffer_render_pass GBufferTarget's raw render pass (5 color + depth).
     * @param camera_layout Set 0.
     * @param sdf_layout    Set 1 -- SdfData's layout (globals UBO + renderer/shape SSBOs).
     * @param vert_spv      sdf_quad.vert.
     * @param frag_spv      sdf_gbuffer.frag.
     */
    SdfGBufferPass(coopa::gfx::core::Device& device,
                  VkRenderPass gbuffer_render_pass,
                  const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                  const coopa::gfx::pipeline::DescriptorSetLayout& sdf_layout,
                  const std::string& vert_spv,
                  const std::string& frag_spv)
    {
        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        coopa::gfx::pipeline::PipelineDesc desc;
        desc.shaders = {vert_shader_.get(), frag_shader_.get()};
        desc.vertex  = coopa::gfx::VertexLayout::none();
        desc.descriptor_layouts = {&camera_layout, &sdf_layout};
        desc.push_constants = {{coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment,
                                0, sizeof(PushConstants)}};
        // No backface concept for a raymarched volume behind a screen-aligned quad.
        desc.raster.cull  = coopa::gfx::CullMode::None;
        desc.depth.test   = true;
        desc.depth.write  = true;
        desc.depth.compare = coopa::gfx::CompareOp::Less;
        // detail::RawRenderPass's Pipeline ctor can't read color_attachment_count() from a
        // real RenderPass (see pipeline.h) -- GBufferTarget's render pass has 5 (G0-G3 plus
        // the G4 velocity attachment).
        desc.blend.color_attachment_count = 5;

        pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, coopa::gfx::detail::RawRenderPass{gbuffer_render_pass}, desc);
    }

    SdfGBufferPass(const SdfGBufferPass&) = delete;
    SdfGBufferPass& operator=(const SdfGBufferPass&) = delete;

    /** @brief Binds the pipeline. Call after GBufferTarget::begin(), before bind_descriptor_set()s. */
    void bind(coopa::gfx::command::CommandBuffer& cmd) const {
        cmd.bind_pipeline(*pipeline_);
    }

    /** @brief Uploads the renderer index for the next draw(6) call. */
    void push(coopa::gfx::command::CommandBuffer& cmd, uint32_t renderer_index) const {
        PushConstants pc{renderer_index};
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
    }

private:
    std::unique_ptr<coopa::gfx::pipeline::Shader>   vert_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Shader>   frag_shader_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> pipeline_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_SDF_GBUFFER_PASS_H
