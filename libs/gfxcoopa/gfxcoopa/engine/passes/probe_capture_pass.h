/**
 * @file probe_capture_pass.h
 * @brief Renders real scene geometry into one reflection-probe cubemap face.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_PROBE_CAPTURE_PASS_H
#define GFXCOOPA_ENGINE_PASSES_PROBE_CAPTURE_PASS_H

#include <volk/volk.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/shader_library.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/model_ubo.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/// Two pipelines, both built against a depth-inclusive render pass (in
/// practice targets::CubemapTarget::render_pass()), used together within a single
/// begin_face_pass()/end_face_pass() instance:
///   1. A sky-background fullscreen triangle (depth off), drawn FIRST to fill
///      every texel with the analytic sky.
///   2. Real scene geometry (depth on), drawn on top -- RenderPass hardcodes
///      LOAD_OP_CLEAR, so the sky fill can't be a separate pass; it has to be
///      the first draw inside the same instance.
///
/// Uses its own probe_capture.frag rather than a general lighting shader: a shader
/// that samples the reflection cubemap would read the very image this pass is
/// writing, and a shadowed one would need a shadow descriptor set this pass doesn't
/// bind (see probe_capture.frag -- no shadow sampling).
class ProbeCapturePass {
public:
    /// 32 bytes -- byte-identical to the first 32 bytes of GBufferPipeline::PushConstants
    /// (80 bytes total there) / TransparentPass::PushConstants. The model matrix is streamed
    /// per instance (data::InstanceData), so this block is pushed once per instanced batch,
    /// not per object. Probe capture is a forward path,
    /// so it doesn't carry the deferred-only `emissive` field either.
    struct PushConstants {
        glm::vec4 albedo       = {0.8f, 0.8f, 0.8f, 1.0f};
        float     metallic     = 0.0f;
        float     roughness    = 0.5f;
        float     ao           = 1.0f;
        float     alpha_cutoff = 0.0f;
    };

    struct SkyPushConstants {
        int32_t face;
    };

    /**
     * @param device        Logical device.
     * @param face_pass     Depth-inclusive render pass (targets::CubemapTarget::render_pass()).
     * @param camera_layout Set 0.
     * @param light_layout  Set 1.
     * @param brdf_layout   Set 2 (BRDF LUT).
     * @param shaders       Resolves this pass's own .spv paths.
     * @param material_layout Optional material set (see engine::util::MaterialTextureCache),
     *                      appended as set 3. Null (the default) omits the set entirely, same
     *                      "declared iff bound" contract GBufferPipeline's material_layout
     *                      already follows. See bind_material()/probe_capture.frag's set 3.
     */
    ProbeCapturePass(coopa::gfx::core::Device& device,
                     coopa::gfx::pipeline::RenderPass& face_pass,
                     const coopa::gfx::pipeline::DescriptorSetLayout& camera_layout,
                     const coopa::gfx::pipeline::DescriptorSetLayout& light_layout,
                     const coopa::gfx::pipeline::DescriptorSetLayout& brdf_layout,
                     const coopa::gfx::pipeline::ShaderLibrary& shaders,
                     const coopa::gfx::pipeline::DescriptorSetLayout* material_layout = nullptr)
    {
        // --- Sky background pipeline ---
        sky_vert_ = std::make_unique<coopa::gfx::pipeline::Shader>(
            device, shaders("env_prefilter.vert"), VK_SHADER_STAGE_VERTEX_BIT);
        sky_frag_ = std::make_unique<coopa::gfx::pipeline::Shader>(
            device, shaders("probe_sky_background.frag"), VK_SHADER_STAGE_FRAGMENT_BIT);

        coopa::gfx::pipeline::PipelineDesc sky_desc;
        sky_desc.shaders = {sky_vert_.get(), sky_frag_.get()};
        sky_desc.vertex  = coopa::gfx::VertexLayout::none();
        sky_desc.raster.cull = coopa::gfx::CullMode::None;
        sky_desc.depth.test  = false;
        sky_desc.depth.write = false;
        sky_desc.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(SkyPushConstants)}};

        sky_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(device, face_pass, sky_desc);

        // --- Geometry pipeline ---
        geom_vert_ = std::make_unique<coopa::gfx::pipeline::Shader>(
            device, shaders("pbr.vert"), VK_SHADER_STAGE_VERTEX_BIT);
        geom_frag_ = std::make_unique<coopa::gfx::pipeline::Shader>(
            device, shaders("probe_capture.frag"), VK_SHADER_STAGE_FRAGMENT_BIT);

        coopa::gfx::pipeline::PipelineDesc geom_desc;
        geom_desc.shaders = {geom_vert_.get(), geom_frag_.get()};
        geom_desc.vertex  = coopa::gfx::engine::data::Vertex::layout().append(
            coopa::gfx::engine::data::InstanceData::layout());
        // CULL_MODE_NONE, not the main pass's back-face/CCW convention: this
        // render pass's projection deliberately omits the Vulkan Y-flip (see
        // targets::CubemapTarget::get_face_projection()), which mirrors framebuffer-
        // space winding relative to the main pass.
        geom_desc.raster.cull = coopa::gfx::CullMode::None;
        geom_desc.depth.test  = true;
        geom_desc.depth.write = true;
        geom_desc.descriptor_layouts = {&camera_layout, &light_layout, &brdf_layout};
        material_set_index_ = static_cast<uint32_t>(geom_desc.descriptor_layouts.size());
        if (material_layout != nullptr) {
            geom_desc.descriptor_layouts.push_back(material_layout);
        }
        geom_desc.push_constants = {{coopa::gfx::ShaderStage::Fragment, 0, sizeof(PushConstants)}};

        geom_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(device, face_pass, geom_desc);
    }

    /// Binds the sky pipeline and draws a fullscreen triangle for one face.
    /// Must be called first within a begin_face_pass()/end_face_pass()
    /// instance (depth off, so it doesn't block the geometry draw after it).
    void draw_sky_background(coopa::gfx::command::CommandBuffer& cmd, uint32_t face) const {
        cmd.bind_pipeline(*sky_pipeline_);
        SkyPushConstants pc{ static_cast<int32_t>(face) };
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pc);
        cmd.draw(3);
    }

    /// Binds the geometry pipeline. Caller then binds the camera/light/brdf
    /// descriptor sets (sets 0/1/2) and issues push()/mesh draws per object.
    void bind_geometry(coopa::gfx::command::CommandBuffer& cmd) const {
        cmd.bind_pipeline(*geom_pipeline_);
    }

    void push(coopa::gfx::command::CommandBuffer& cmd, const PushConstants& pc) const {
        cmd.push_constants(coopa::gfx::ShaderStage::Fragment, pc);
    }

    /// Binds `set` at the material set index (see ctor's material_layout doc) -- call after
    /// bind_geometry(), alongside sets 0-2, before push()/draws. Only meaningful when a
    /// non-null material_layout was passed to the ctor.
    void bind_material(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::DescriptorSet& set) const {
        cmd.bind_descriptor_set(set, material_set_index_);
    }

    VkPipelineLayout geometry_layout() const { return geom_pipeline_->layout(); }

private:
    std::unique_ptr<coopa::gfx::pipeline::Shader> sky_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> sky_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> sky_pipeline_;

    std::unique_ptr<coopa::gfx::pipeline::Shader> geom_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> geom_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> geom_pipeline_;
    uint32_t material_set_index_ = 0;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_PROBE_CAPTURE_PASS_H
