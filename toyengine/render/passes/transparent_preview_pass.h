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

#include <toyengine/render/toy_render_math.h>

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
                           const std::string& vert_spv, const std::string& frag_spv);

    TransparentPreviewPass(const TransparentPreviewPass&) = delete;
    TransparentPreviewPass& operator=(const TransparentPreviewPass&) = delete;

    /** @brief The scene depth to test against (bound once; the G-buffer is never recreated). */
    void set_scene_depth(coopa::gfx::TextureView depth, const coopa::gfx::engine::util::Sampler& nearest,
                         uint32_t width, uint32_t height);

    /** @brief Binds the pipeline, viewport (Y-flipped like DebugLinePass) and sets 0 / 1. */
    void begin(coopa::gfx::command::CommandBuffer& cmd, const coopa::gfx::pipeline::DescriptorSet& camera_set,
               const LetterboxRect& rect) const;

    /** @brief Per-object push constants + material set (call between begin() and the draw). */
    void push(coopa::gfx::command::CommandBuffer& cmd, PushConstants pc,
              const coopa::gfx::pipeline::DescriptorSet& material_set) const;

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
