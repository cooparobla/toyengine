/**
 * @file sdf_forward_pass.h
 * @brief Raymarches every BLEND SdfRenderer, lit and SSR-tracing, into the
 *        live HDR colour image.
 *
 * A second Pipeline built against TransparentPass's OWN render pass handle
 * (see TransparentPass::render_pass()) rather than a render pass of its own
 * -- render passes only need to be attachment-COMPATIBLE to share a
 * VkPipeline, and drawing both a BLEND mesh's pipeline and this one inside
 * the SAME begin()/end() bracket is what lets
 * ToyRenderPipeline::record_transparent_() walk one merged back-to-front
 * list of meshes and SDFs, switching pipelines per item, instead of drawing
 * all SDFs in a separate pass that would always land in front of or behind
 * every BLEND mesh regardless of true depth order.
 *
 * Shading matches TransparentPass's transparent.frag exactly (same direct
 * lighting formula, same gfx_ssr_trace() call against the same Hi-Z/scene-
 * colour chain via the same ExtraSets mechanism) -- see toyengine's
 * sdf_forward.frag, which includes the same toy_forward_shading.glsl
 * body transparent.frag includes, so the two can never silently diverge.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_SDF_FORWARD_PASS_H
#define GFXCOOPA_ENGINE_PASSES_SDF_FORWARD_PASS_H

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
#include <gfxcoopa/engine/passes/extra_sets.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class SdfForwardPass
 * @brief Raymarches every BLEND SdfRenderer, lit and SSR-tracing, into the live
 *        HDR frame.
 *
 * Built against TransparentPass's own render pass so SDF shapes and blended
 * meshes interleave in one bracket. Owns only its pipeline.
 */
class SdfForwardPass {
public:
    /// Vertex AND fragment both read this (same reasoning as
    /// SdfGBufferPass::PushConstants) -- everything else this pass needs
    /// (material, march params, per-frame lighting/SSR tuning) lives in the
    /// SdfData UBO/SSBOs, not a push constant, for the same 128-byte-budget
    /// reason toyengine's ForwardGlobals doc explains.
    struct PushConstants {
        uint32_t renderer_index = 0;
    };

    /**
     * @param device        Logical device.
     * @param color_format  Format of the HDR colour image (matches TransparentPass's own).
     * @param shared_render_pass TransparentPass::render_pass() -- see file doc.
     * @param camera_layout Set 0.
     * @param light_layout  Set 1.
     * @param shadow_layout Set 2.
     * @param sdf_layout    Set 3 -- SdfData's layout.
     * @param vert_spv      sdf_quad.vert.
     * @param frag_spv      sdf_forward.frag.
     * @param extra         Trailing sets (SSR's trace_gbuffer/hiz/scene_color sets), appended
     *                      after set 3 -- same ExtraSets mechanism TransparentPass itself uses.
     */
    SdfForwardPass(coopa::gfx::core::Device& device,
                  VkFormat color_format,
                  VkRenderPass shared_render_pass,
                  const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                  const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                  const coopa::gfx::pipeline::DescriptorSetLayout& shadow_layout,
                  const coopa::gfx::pipeline::DescriptorSetLayout& sdf_layout,
                  const std::string& vert_spv,
                  const std::string& frag_spv,
                  ExtraSets extra = {})
        : extra_(std::move(extra))
    {
        extra_.validate("SdfForwardPass");
        vert_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        frag_shader_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        std::vector<const coopa::gfx::pipeline::DescriptorSetLayout*> layouts = {
            &camera_layout, &light_layout, &shadow_layout, &sdf_layout
        };
        extra_first_set_ = static_cast<uint32_t>(layouts.size());
        layouts.insert(layouts.end(), extra_.layouts.begin(), extra_.layouts.end());

        coopa::gfx::pipeline::PipelineDesc desc;
        desc.shaders = {vert_shader_.get(), frag_shader_.get()};
        desc.vertex  = coopa::gfx::VertexLayout::none();
        desc.descriptor_layouts = layouts;
        desc.push_constants = {{coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment,
                                0, sizeof(PushConstants)}};
        desc.raster.cull  = coopa::gfx::CullMode::None;
        desc.depth.test    = true;                                    // test against the opaque G-Buffer depth
        desc.depth.write   = false;                                   // never occlude other transparents
        desc.depth.compare = coopa::gfx::CompareOp::Less;
        desc.blend.mode    = coopa::gfx::pipeline::BlendMode::Alpha;
        // Same RawRenderPass caveat TransparentPass's own ctor documents: no RenderPass to read
        // color_attachment_count()/samples() from -- single-color-attachment, single-sampled.
        desc.blend.color_attachment_count = 1;

        pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, coopa::gfx::detail::RawRenderPass{shared_render_pass}, desc);
    }

    SdfForwardPass(const SdfForwardPass&) = delete;
    SdfForwardPass& operator=(const SdfForwardPass&) = delete;

    /** @brief Binds the pipeline. Call after TransparentPass::begin() (see file doc). */
    void bind(coopa::gfx::command::CommandBuffer& cmd) const {
        cmd.bind_pipeline(*pipeline_);
    }

    /// Binds this pass's extra sets (e.g. SSR's trace inputs), if any were provided at
    /// construction. Call after bind(), alongside binding sets 0-3.
    void bind_extra(coopa::gfx::command::CommandBuffer& cmd) const {
        if (extra_.bind) {
            extra_.bind(cmd, extra_first_set_);
        }
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

    ExtraSets extra_;
    uint32_t  extra_first_set_ = 0;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_SDF_FORWARD_PASS_H
