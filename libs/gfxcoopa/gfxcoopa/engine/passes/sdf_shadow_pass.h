/**
 * @file sdf_shadow_pass.h
 * @brief Depth-only raymarch pipelines for directional and point-light
 *        cubemap shadow maps.
 *
 * Mirrors ShadowPipeline's shape (shadow_pipeline.h): two pipelines, drawn
 * inside ShadowMapTarget's existing begin_directional_pass()/
 * begin_cube_face_pass() brackets, right after the mesh loop -- see
 * ToyRenderPipeline::record_directional_shadow_()/record_point_shadow_().
 *
 * Unlike the main G-buffer/forward draws, a shadow draw is NOT scissored to
 * a light-space-projected rectangle -- computing one would need a second,
 * light-specific clip rect (and, for the cube map, one per face: six of
 * them), which the SDF system does not currently maintain. Instead each
 * shadow draw covers the ENTIRE shadow map, relying on
 * gfx_sdf_aabb_intersect()'s ray/AABB slab test (against the SAME world AABB
 * the main camera's clip rect was derived from) to make an out-of-bounds
 * pixel a single failed test rather than a full march. Shadow maps are
 * small (2048^2 directional, 512^2 x6 cube by default) and the slab-test
 * reject is cheap, so this trades a small, bounded amount of wasted
 * fragment-shader invocations for not having to maintain a second
 * projection's worth of bookkeeping per light.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_SDF_SHADOW_PASS_H
#define GFXCOOPA_ENGINE_PASSES_SDF_SHADOW_PASS_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <memory>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/command/command_buffer.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/// Push constants for the directional shadow draw (72 bytes: mat4 + 2 uints).
/// Byte-identical prefix to ShadowPipeline's DirectionalShadowPushConstants
/// through light_space_matrix, so a caller can share the same value.
struct SdfDirectionalShadowPushConstants {
    glm::mat4 light_space_matrix;
    uint32_t  renderer_index = 0;
    /// Caps the depth-only march below the renderer's own (main-pass) step
    /// budget -- see ToyRenderConfig::sdf_shadow_max_steps. A cheaper
    /// budget is safe here: the shadow map only needs a correct silhouette,
    /// not the fine surface detail the main G-buffer/forward draw does.
    uint32_t  shadow_max_steps = 32;
};

/// Push constants for one cube-face shadow draw (88 bytes: mat4 + vec4 + 2 uints).
struct SdfCubeShadowPushConstants {
    glm::mat4 light_space_matrix;
    glm::vec4 light_pos_range; // xyz = light pos, w = range
    uint32_t  renderer_index = 0;
    uint32_t  shadow_max_steps = 32; // see SdfDirectionalShadowPushConstants's doc
};

/**
 * @class SdfShadowPass
 * @brief Depth-only raymarch pipelines for directional and point-light shadows.
 *
 * Two pipelines, mirroring ShadowPipeline's shape, drawn inside the shadow
 * targets' existing brackets so SDF shapes cast the same shadows meshes do.
 */
class SdfShadowPass {
public:
    /**
     * @param device      Logical device.
     * @param dir_pass    ShadowMapTarget::dir_render_pass().
     * @param cube_pass   ShadowMapTarget::cube_render_pass().
     * @param sdf_layout  The SDF descriptor set (SdfData's layout), bound at set 0 in both
     *                    pipelines -- these depth-only passes need no camera/light set.
     * @param dir_vert_spv  sdf_shadow.vert.
     * @param dir_frag_spv  sdf_shadow.frag.
     * @param cube_vert_spv sdf_shadow_cube.vert.
     * @param cube_frag_spv sdf_shadow_cube.frag.
     */
    SdfShadowPass(core::Device& device,
                 pipeline::RenderPass& dir_pass,
                 pipeline::RenderPass& cube_pass,
                 const pipeline::DescriptorSetLayout& sdf_layout,
                 const std::string& dir_vert_spv,
                 const std::string& dir_frag_spv,
                 const std::string& cube_vert_spv,
                 const std::string& cube_frag_spv)
    {
        dir_vert_ = std::make_unique<pipeline::Shader>(device, dir_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        dir_frag_ = std::make_unique<pipeline::Shader>(device, dir_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        pipeline::PipelineDesc desc;
        desc.vertex = coopa::gfx::VertexLayout::none();
        desc.descriptor_layouts = {&sdf_layout};
        desc.raster.cull = coopa::gfx::CullMode::None;
        desc.depth.test  = true;
        desc.depth.write = true;

        desc.shaders = {dir_vert_.get(), dir_frag_.get()};
        desc.push_constants = {{coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment,
                                0, sizeof(SdfDirectionalShadowPushConstants)}};
        dir_pipeline_ = std::make_unique<pipeline::Pipeline>(device, dir_pass, desc);

        cube_vert_ = std::make_unique<pipeline::Shader>(device, cube_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        cube_frag_ = std::make_unique<pipeline::Shader>(device, cube_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        desc.shaders = {cube_vert_.get(), cube_frag_.get()};
        desc.push_constants = {{coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment,
                                0, sizeof(SdfCubeShadowPushConstants)}};
        cube_pipeline_ = std::make_unique<pipeline::Pipeline>(device, cube_pass, desc);
    }

    SdfShadowPass(const SdfShadowPass&) = delete;
    SdfShadowPass& operator=(const SdfShadowPass&) = delete;

    void bind_directional(command::CommandBuffer& cmd) const {
        cmd.bind_pipeline(*dir_pipeline_);
    }

    void push_directional(command::CommandBuffer& cmd, const SdfDirectionalShadowPushConstants& pc) const {
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
    }

    void bind_cube(command::CommandBuffer& cmd) const {
        cmd.bind_pipeline(*cube_pipeline_);
    }

    void push_cube(command::CommandBuffer& cmd, const SdfCubeShadowPushConstants& pc) const {
        cmd.push_constants(coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment, pc);
    }

private:
    std::unique_ptr<pipeline::Shader>   dir_vert_;
    std::unique_ptr<pipeline::Shader>   dir_frag_;
    std::unique_ptr<pipeline::Pipeline> dir_pipeline_;

    std::unique_ptr<pipeline::Shader>   cube_vert_;
    std::unique_ptr<pipeline::Shader>   cube_frag_;
    std::unique_ptr<pipeline::Pipeline> cube_pipeline_;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_SDF_SHADOW_PASS_H
