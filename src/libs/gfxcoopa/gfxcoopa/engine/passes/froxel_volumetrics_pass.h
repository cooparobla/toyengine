/**
 * @file froxel_volumetrics_pass.h
 * @brief Froxel-grid volumetrics for the LOCAL volumes -- the Unreal / HDRP "volumetric fog"
 *        technique, as an alternative to VolumetricsPass's per-pixel raymarch.
 *
 * Instead of marching every pixel, the medium is evaluated on a camera-aligned grid of
 * "froxels" (frustum voxels: W x H per slice at 1/tile of the render resolution, D slices
 * along the view ray, exponentially spaced) and integrated once per froxel column:
 *
 *   1. inject     density + in-scatter once per froxel at a jittered point, pre-integrated
 *                 over the slice, blended with last frame's grid reprojected (temporal)
 *   2. integrate  front-to-back accumulation along each column -> (scatter, T) per froxel,
 *                 in two passes (within groups of 8 slices, then across groups) so each
 *                 froxel reads ~9 entries instead of walking its whole column
 *   3. apply      per pixel: one lookup at the pixel's ray distance, composited over the
 *                 scene, into the caller's HDR target
 *
 * Cost scales with the grid (~W*H*D froxels, D accumulation taps per froxel), not with the
 * screen or a step count. gfxcoopa has no compute pipelines or 3D images, so every stage is
 * a fullscreen fragment pass and the grid is a 2D atlas: slice s is a W x H tile at column
 * s % cols, row s / cols (see gfx/volumetrics_froxel.glsl).
 *
 * The injected grid is ping-ponged (A/B by frame parity): one is written while the other is
 * the history being reprojected. Both parities' descriptor sets are bound at construction.
 */

#ifndef GFXCOOPA_ENGINE_PASSES_FROXEL_VOLUMETRICS_PASS_H
#define GFXCOOPA_ENGINE_PASSES_FROXEL_VOLUMETRICS_PASS_H

#include <volk/volk.h>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/engine/passes/fullscreen_stage.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/types/texture_view.h>
#include <gfxcoopa/engine/util/sampler.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace passes {

class FroxelVolumetricsPass {
public:
    /// Slices per atlas row. Fixed: the shaders read it from the UBO (froxel_grid.w).
    static constexpr uint32_t kAtlasColumns = 8;

    enum class Stage { Inject, Integrate };

    struct Desc {
        uint32_t render_width  = 0;   ///< Render resolution the grid covers.
        uint32_t render_height = 0;
        uint32_t tile          = 8;   ///< Render pixels per froxel (each axis).
        uint32_t slices        = 64;  ///< Depth slices (D).
        std::string vert_spv;         ///< fullscreen.vert
        std::string inject_frag_spv;
        std::string integrate_partial_frag_spv;
        std::string integrate_frag_spv;
        std::string apply_frag_spv;
    };

    /**
     * @param composite_pass Render pass of the caller's full-resolution HDR target that the
     *                       apply stage draws into (the same one VolumetricsPass's composite uses).
     * @param vol_ubo        VolumetricsData's uniform buffer (with its froxel fields filled).
     */
    FroxelVolumetricsPass(core::Device& device, memory::Allocator& allocator, const Desc& desc,
                          pipeline::RenderPass& composite_pass,
                          const memory::Buffer& vol_ubo)
        : grid_w_(std::max(1u, (desc.render_width  + desc.tile - 1) / std::max(1u, desc.tile))),
          grid_h_(std::max(1u, (desc.render_height + desc.tile - 1) / std::max(1u, desc.tile))),
          slices_(std::max(1u, desc.slices)),
          nearest_sampler_(util::Sampler::nearest(device)),
          linear_sampler_(util::Sampler::linear(device))
    {
        const uint32_t rows = (slices_ + kAtlasColumns - 1) / kAtlasColumns;
        atlas_w_ = grid_w_ * kAtlasColumns;
        atlas_h_ = grid_h_ * rows;
        for (auto& t : injected_) {
            t = std::make_unique<targets::OffscreenTarget>(device, allocator, atlas_w_, atlas_h_,
                                                           Format::RGBA16_Sfloat, targets::kColorOnly);
        }
        partial_    = std::make_unique<targets::OffscreenTarget>(device, allocator, atlas_w_, atlas_h_,
                                                                 Format::RGBA16_Sfloat, targets::kColorOnly);
        integrated_ = std::make_unique<targets::OffscreenTarget>(device, allocator, atlas_w_, atlas_h_,
                                                                 Format::RGBA16_Sfloat, targets::kColorOnly);

        inject_    = std::make_unique<FullscreenStage>(device, injected_[0]->render_pass_object(),
                                                       describe_inject(desc.vert_spv, desc.inject_frag_spv));
        partial_stage_ = std::make_unique<FullscreenStage>(device, partial_->render_pass_object(),
                                                       describe_integrate(desc.vert_spv, desc.integrate_partial_frag_spv, 2));
        integrate_ = std::make_unique<FullscreenStage>(device, integrated_->render_pass_object(),
                                                       describe_integrate(desc.vert_spv, desc.integrate_frag_spv, 1));
        apply_     = std::make_unique<FullscreenStage>(device, composite_pass,
                                                       describe_apply(desc.vert_spv, desc.apply_frag_spv));

        // Parity p writes injected_[p] and reprojects injected_[1-p]; the partial pass reads
        // injected_[p]; the final integrate pass reads the (single) partial grid.
        for (uint32_t p = 0; p < 2; ++p) {
            inject_->set(0, p).bind_image(0, injected_[1 - p]->color_view_typed(), linear_sampler_);
            inject_->set(1, p).bind_buffer(0, vol_ubo);
            partial_stage_->set(0, p).bind_image(0, injected_[p]->color_view_typed(), nearest_sampler_);
            partial_stage_->set(1, p).bind_buffer(0, vol_ubo);
        }
        integrate_->set(0).bind_image(0, partial_->color_view_typed(), nearest_sampler_);
        integrate_->set(1).bind_buffer(0, vol_ubo);
        apply_->set(0).bind_image(3, integrated_->color_view_typed(), linear_sampler_);
        apply_->set(1).bind_buffer(0, vol_ubo);
    }

    FroxelVolumetricsPass(const FroxelVolumetricsPass&) = delete;
    FroxelVolumetricsPass& operator=(const FroxelVolumetricsPass&) = delete;

    uint32_t grid_width()  const { return grid_w_; }
    uint32_t grid_height() const { return grid_h_; }
    uint32_t slices()      const { return slices_; }

    /// Scene colour + G-buffer for the apply stage. Call ONCE, at construction time.
    void set_source_images(TextureView scene_color, TextureView g_normal, TextureView g_position,
                           const util::Sampler& linear_sampler) {
        apply_->set(0).bind_image(0, scene_color, linear_sampler);
        apply_->set(0).bind_image(1, g_normal, nearest_sampler_);
        apply_->set(0).bind_image(2, g_position, nearest_sampler_);
    }

    /// Directional map + local-light (point/spot) shadow atlas (compare sampler), for both
    /// parities' inject sets.
    /// Call ONCE, at construction time.
    void set_shadow_images(TextureView dir_shadow, TextureView spot_shadow, const util::Sampler& shadow_sampler) {
        for (uint32_t p = 0; p < 2; ++p) {
            inject_->set(2, p).bind_image(0, dir_shadow, shadow_sampler);
            inject_->set(2, p).bind_image(1, spot_shadow, shadow_sampler);
        }
    }

    /// An app's cloud shadow map at set 2, binding 4 (VolumetricsPass::set_cloud_shadow_image),
    /// for both parities' inject sets. Call ONCE, at construction time.
    void set_cloud_shadow_image(TextureView cloud_shadow, const util::Sampler& sampler) {
        for (uint32_t p = 0; p < 2; ++p) inject_->set(2, p).bind_image(4, cloud_shadow, sampler);
    }

    /// Optional callback after each grid stage (e.g. a GPU timestamp), between render passes.
    void set_stage_hook(std::function<void(command::CommandBuffer&, Stage)> hook) { stage_hook_ = std::move(hook); }

    /**
     * @brief Records inject + integrate for this frame. `parity` alternates 0/1 frame to frame
     *        (it picks which injected grid is written and which is history). The UBO's
     *        froxel_params.w must be 0 on any frame whose history grid is not last frame's.
     */
    void record_grid(command::CommandBuffer& cmd, uint32_t parity) {
        const uint32_t p = parity & 1u;
        if (!initialized_) {
            // The history grid is bound (and so must be validly laid out) even on the first
            // frame, when the shader ignores it: clear it once to empty air.
            injected_[1 - p]->begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
            injected_[1 - p]->end(cmd);
            initialized_ = true;
        }
        injected_[p]->begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
        inject_->bind(cmd, atlas_w_, atlas_h_, p);
        inject_->draw(cmd);
        injected_[p]->end(cmd);
        if (stage_hook_) stage_hook_(cmd, Stage::Inject);

        partial_->begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
        partial_stage_->bind(cmd, atlas_w_, atlas_h_, p);
        partial_stage_->draw(cmd);
        partial_->end(cmd);

        integrated_->begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
        integrate_->bind(cmd, atlas_w_, atlas_h_);
        integrate_->draw(cmd);
        integrated_->end(cmd);
        if (stage_hook_) stage_hook_(cmd, Stage::Integrate);
    }

    /// Records the per-pixel apply into the caller's open render pass (the composite target).
    void draw_apply(command::CommandBuffer& cmd, uint32_t width, uint32_t height) const {
        apply_->bind(cmd, width, height);
        apply_->draw(cmd);
    }

private:
    static FullscreenStageDesc describe_inject(const std::string& vert, const std::string& frag) {
        FullscreenStageDesc d;
        d.vert_spv = vert;
        d.frag_spv = frag;
        d.owned_sets = {
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},   // history grid
            {{0, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},          // volumetrics UBO
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},    // dir shadow
             {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},    // spot shadow
             {4, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},   // an app's cloud shadow
        };
        d.instances = 2;
        return d;
    }

    static FullscreenStageDesc describe_integrate(const std::string& vert, const std::string& frag,
                                                  uint32_t instances) {
        FullscreenStageDesc d;
        d.vert_spv = vert;
        d.frag_spv = frag;
        d.owned_sets = {
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},   // source grid
            {{0, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},
        };
        d.instances = instances;
        return d;
    }

    /// Same layout as VolumetricsPass's composite: the apply shader is that composite's body
    /// with the froxel lookup in place of the upsample (gfx/volumetrics_composite_body.glsl).
    static FullscreenStageDesc describe_apply(const std::string& vert, const std::string& frag) {
        FullscreenStageDesc d;
        d.vert_spv = vert;
        d.frag_spv = frag;
        d.owned_sets = {
            {{0, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {1, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {2, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1},
             {3, DescriptorType::CombinedImageSampler, ShaderStage::Fragment, 1}},
            {{0, DescriptorType::UniformBuffer, ShaderStage::Fragment, 1}},
        };
        return d;
    }

    uint32_t grid_w_, grid_h_, slices_;
    uint32_t atlas_w_ = 0, atlas_h_ = 0;
    util::Sampler nearest_sampler_;
    util::Sampler linear_sampler_;
    std::array<std::unique_ptr<targets::OffscreenTarget>, 2> injected_;
    std::unique_ptr<targets::OffscreenTarget> partial_;
    std::unique_ptr<targets::OffscreenTarget> integrated_;
    std::unique_ptr<FullscreenStage> inject_, partial_stage_, integrate_, apply_;
    std::function<void(command::CommandBuffer&, Stage)> stage_hook_;
    bool initialized_ = false;
};

} // namespace passes
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_PASSES_FROXEL_VOLUMETRICS_PASS_H
