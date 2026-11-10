/**
 * @file smaa_pass.h
 * @brief Subpixel Morphological Anti-Aliasing (SMAA 1x) post-processing pass for gfxcoopa.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_SMAA_PASS_H
#define GFXCOOPA_ENGINE_PASSES_SMAA_PASS_H

#include <volk/volk.h>
#include <memory>
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/shader_library.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/util/smaa_textures.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

/**
 * @class SmaaPass
 * @brief Subpixel Morphological Anti-Aliasing (SMAA 1x) in three stages --
 *        edge detection, blending-weight calculation, neighbourhood blend.
 *
 * Owns an OffscreenTarget per stage and the Jimenez area/search textures. The
 * chained targets' render passes synchronize the stages, so execute() records
 * no manual barriers.
 */
class SmaaPass {
public:
    struct EdgePush {
        glm::vec4 rt_metrics;
        float     threshold = 0.1f;
    };

    struct BlendPush {
        glm::vec4 rt_metrics;
        int32_t   max_search_steps = 16;
    };

    struct NeighborhoodPush {
        glm::vec4 rt_metrics;
        float     exposure = 1.0f;
    };

    SmaaPass(coopa::gfx::core::Device& device,
             coopa::gfx::memory::Allocator& allocator,
             coopa::gfx::pipeline::RenderPass& output_render_pass,
             coopa::gfx::command::CommandPool& cmd_pool,
             uint32_t width, uint32_t height,
             const util::Sampler& linear_sampler,
             const coopa::gfx::pipeline::ShaderLibrary& shaders)
        : allocator_(allocator), width_(width), height_(height)
    {
        smaa_textures_ = std::make_unique<util::SmaaTextures>(device, allocator, cmd_pool);

        edges_target_ = std::make_unique<targets::OffscreenTarget>(
            device, allocator, width, height, coopa::gfx::Format::RG8_Unorm, targets::kColorOnly
        );

        blend_target_ = std::make_unique<targets::OffscreenTarget>(
            device, allocator, width, height, coopa::gfx::Format::RGBA8_Unorm, targets::kColorOnly
        );

        // One FullscreenStage per SMAA stage. All three share the same pipeline shape
        // (fullscreen triangle, no depth, no blending) and differ only in their shaders,
        // how many images they sample, and their push-constant block.
        using coopa::gfx::DescriptorType;
        using coopa::gfx::ShaderStage;
        const auto both_stages = ShaderStage::Vertex | ShaderStage::Fragment;

        auto sampled = [](uint32_t n) {
            std::vector<coopa::gfx::pipeline::DescriptorBinding> b;
            for (uint32_t i = 0; i < n; ++i) {
                b.push_back({i, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1});
            }
            return b;
        };

        FullscreenStageDesc edge_sd;
        edge_sd.vert_spv = shaders("smaa_edge.vert");
        edge_sd.frag_spv = shaders("smaa_edge.frag");
        edge_sd.owned_sets = {sampled(1)};                 // scene colour
        edge_sd.push_constants = {{both_stages, 0, sizeof(EdgePush)}};
        edge_ = std::make_unique<FullscreenStage>(device, edges_target_->render_pass_object(), edge_sd);

        FullscreenStageDesc blend_sd;
        blend_sd.vert_spv = shaders("smaa_blend.vert");
        blend_sd.frag_spv = shaders("smaa_blend.frag");
        blend_sd.owned_sets = {sampled(3)};                // edges, area LUT, search LUT
        blend_sd.push_constants = {{both_stages, 0, sizeof(BlendPush)}};
        blend_ = std::make_unique<FullscreenStage>(device, blend_target_->render_pass_object(), blend_sd);

        blend_->set().bind_image(0, edges_target_->color_view_typed(), linear_sampler);
        blend_->set().bind_image(1, smaa_textures_->area_view_typed(), smaa_textures_->sampler());
        blend_->set().bind_image(2, smaa_textures_->search_view_typed(), smaa_textures_->sampler());

        FullscreenStageDesc neigh_sd;
        neigh_sd.vert_spv = shaders("smaa_neighborhood.vert");
        neigh_sd.frag_spv = shaders("smaa_neighborhood.frag");
        neigh_sd.owned_sets = {sampled(2)};                // scene colour, blend weights
        neigh_sd.push_constants = {{both_stages, 0, sizeof(NeighborhoodPush)}};
        neigh_ = std::make_unique<FullscreenStage>(device, output_render_pass, neigh_sd);

        neigh_->set().bind_image(1, blend_target_->color_view_typed(), linear_sampler);
    }

    void set_source_image(coopa::gfx::TextureView color_view, const util::Sampler& linear_sampler) {
        edge_->set().bind_image(0, color_view, linear_sampler);
        neigh_->set().bind_image(0, color_view, linear_sampler);
    }

    void recreate(uint32_t width, uint32_t height, const util::Sampler& linear_sampler) {
        width_  = width;
        height_ = height;
        edges_target_->recreate(width, height);
        blend_target_->recreate(width, height);

        blend_->set().bind_image(0, edges_target_->color_view_typed(), linear_sampler);
        neigh_->set().bind_image(1, blend_target_->color_view_typed(), linear_sampler);
    }

    void draw(coopa::gfx::command::CommandBuffer& cmd,
              targets::OffscreenTarget& output_target,
              float exposure, float threshold, int max_search_steps,
              uint32_t viewport_w, uint32_t viewport_h)
    {
        glm::vec4 rt_metrics(1.0f / viewport_w, 1.0f / viewport_h, static_cast<float>(viewport_w), static_cast<float>(viewport_h));

        const auto both_stages = coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment;

        // Pass 1: Edge Detection
        edges_target_->begin(cmd);
        edge_->bind(cmd, viewport_w, viewport_h);

        EdgePush pc_edge{rt_metrics, threshold};
        cmd.push_constants(both_stages, pc_edge);
        edge_->draw(cmd);
        edges_target_->end(cmd);

        // Pass 2: Blending Weight Calculation
        blend_target_->begin(cmd);
        blend_->bind(cmd, viewport_w, viewport_h);

        BlendPush pc_blend{rt_metrics, max_search_steps};
        cmd.push_constants(both_stages, pc_blend);
        blend_->draw(cmd);
        blend_target_->end(cmd);

        // Pass 3: Neighborhood Blending into output render pass
        output_target.begin(cmd);
        neigh_->bind(cmd, viewport_w, viewport_h);

        NeighborhoodPush pc_neigh{rt_metrics, exposure};
        cmd.push_constants(both_stages, pc_neigh);
        neigh_->draw(cmd);
        output_target.end(cmd);
    }

private:
    coopa::gfx::memory::Allocator& allocator_;
    uint32_t width_;
    uint32_t height_;

    std::unique_ptr<util::SmaaTextures>                        smaa_textures_;
    std::unique_ptr<targets::OffscreenTarget> edges_target_;
    std::unique_ptr<targets::OffscreenTarget> blend_target_;

    std::unique_ptr<FullscreenStage> edge_;   ///< Stage 1: edge detection.
    std::unique_ptr<FullscreenStage> blend_;  ///< Stage 2: blending-weight calculation.
    std::unique_ptr<FullscreenStage> neigh_;  ///< Stage 3: neighbourhood blend.
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_SMAA_PASS_H
