/**
 * @file ssr_pass.h
 * @brief Screen-space reflections + screen-space diffuse bounce ("SSGI").
 *
 * A structural port of gfxcoopa's SsrPass (engine/passes/ssr_pass.h) with:
 *  - the `gi_layout` constructor parameter and `gi::GiSystem*` execute()
 *    parameter removed, and the composite pipeline's descriptor set 4
 *    (reflection-probe cubemaps + BRDF LUT + SH-probe UBO) dropped entirely
 *    -- toyengine has neither, and constructing a GiSystem just to bind
 *    nothing would pull in the exact reflection-probe machinery it doesn't
 *    want.
 *  - `half_res` removed -- toyengine already renders at the pipeline's low
 *    internal resolution, so there is no separate "trace at half of that"
 *    tier worth the added bilateral-upsample code path.
 *  - the composite gains a second scene-colour binding (the prefiltered mip
 *    chain, already available for the raymarch) and three new push-constant
 *    fields (sky_intensity, ssgi_intensity, ssgi_distance) so
 *    ssr_composite.frag can add a screen-space diffuse-bounce term
 *    alongside the specular swap -- see that shader's header comment.
 *
 * The raymarch (ssr.frag) and temporal resolve (ssr_resolve.frag) shaders
 * are unmodified copies of gfxcoopa's ssr.frag/ssr_resolve.frag -- neither
 * has any GI coupling to remove.
 *
 * `ssr_enabled` gates whether this pass (and its prerequisites, HiZPass and
 * SceneColorMipPass) are constructed at all -- see PixelRenderPipeline.
 */

#ifndef TOYENGINE_RENDER_PASSES_SSR_PASS_H
#define TOYENGINE_RENDER_PASSES_SSR_PASS_H

#include <volk/volk.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/shader.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/targets/gbuffer_target.h>
#include <gfxcoopa/engine/util/sampler.h>

namespace toy {
namespace render {
namespace passes {

/**
 * @class SsrPass
 * @brief Hi-Z raymarched SSR with temporal resolve, composited with both a
 *        specular swap (replacing the sky-gradient indirect specular
 *        pixel_lighting.frag wrote) and a screen-space diffuse bounce.
 */
class SsrPass {
public:
    /** @brief Matches ssr.frag's SsrPushConstants. */
    struct SsrPushConstants {
        glm::mat4 inv_proj;
        float max_distance;
        float bias_texels;
        float thickness_min;
        float thickness_scale;
        float roughness_cutoff;
        int   max_iterations;
        int   max_hiz_mip;
        int   start_mip;
        int   min_mip0_steps;
        int   max_color_mip = 0;
    };

    /** @brief Matches ssr_resolve.frag's PushConstants. */
    struct ResolvePushConstants {
        glm::mat4 prev_view_proj;
        glm::vec2 resolution;
        float     blend_factor;
        int       history_valid;
    };

    /** @brief Matches ssr_composite.frag's CompositePushConstants (24 bytes). */
    struct CompositePushConstants {
        glm::vec2 screen_resolution;
        float     sky_intensity  = 1.0f;
        float     ssgi_intensity = 0.6f;
        float     ssgi_distance  = 0.5f;
        int       max_color_mip  = 0;
    };

    /// Per-frame parameters for execute(). See SsrPass::Params (gfxcoopa) for why this is a
    /// struct rather than a long positional argument list.
    struct Params {
        glm::mat4 proj;
        int   max_iterations   = 64;
        float thickness_min    = 0.05f;
        float thickness_scale  = 0.01f;
        float max_distance     = 15.0f;
        float bias_texels      = 3.5f;
        float roughness_cutoff = 1.0f;
        int   max_hiz_mip      = 0;      // hiz_pass.max_mip_level(), filled by the caller
        int   start_mip        = 0;
        int   min_mip0_steps   = 1;
        int   max_color_mip    = 0;      // scene_color_mip_pass.max_mip_level(), filled by caller
        bool  temporal_enabled = true;
        float temporal_blend   = 0.85f;
        float sky_intensity    = 1.0f;   // must match PixelLightingPass::PushConstants::sky_intensity
        float ssgi_intensity   = 0.6f;
        float ssgi_distance    = 0.5f;
        // Reprojection: previous frame's proj * view, and whether it (and the history buffer)
        // actually exist yet. False for the first frame and right after construction.
        glm::mat4 prev_view_proj       = glm::mat4(1.0f);
        bool      prev_view_proj_valid = false;
    };

    SsrPass(coopa::gfx::core::Device& device,
                    coopa::gfx::memory::Allocator& allocator,
                    VkDescriptorSetLayout camera_layout,
                    uint32_t width,
                    uint32_t height,
                    const std::string& ssr_vert_spv,
                    const std::string& ssr_frag_spv,
                    const std::string& comp_vert_spv,
                    const std::string& comp_frag_spv,
                    const std::string& resolve_vert_spv,
                    const std::string& resolve_frag_spv)
        : device_(device), allocator_(allocator), width_(width), height_(height)
    {
        // Nearest-filtered sampler for the in-march G-buffer point-lookups (self-hit
        // rejection, backface test) -- those sample at an arbitrary marched UV, not a texel
        // center, so a LINEAR sampler would bilinearly blend world positions/normals across
        // silhouette edges into values that exist on no real surface. See ssr.frag's port.
        nearest_sampler_ = std::make_unique<coopa::gfx::engine::util::Sampler>(
            device, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

        target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
            device, allocator, width_, height_, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT);
        composite_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
            device, allocator, width_, height_, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT);
        resolved_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
            device, allocator, width_, height_, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT);

        history_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device, allocator, width_, height_, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO);
        history_initialized_ = false;

        ssr_vert_     = std::make_unique<coopa::gfx::pipeline::Shader>(device, ssr_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        ssr_frag_     = std::make_unique<coopa::gfx::pipeline::Shader>(device, ssr_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        comp_vert_    = std::make_unique<coopa::gfx::pipeline::Shader>(device, comp_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        comp_frag_    = std::make_unique<coopa::gfx::pipeline::Shader>(device, comp_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);
        resolve_vert_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, resolve_vert_spv, VK_SHADER_STAGE_VERTEX_BIT);
        resolve_frag_ = std::make_unique<coopa::gfx::pipeline::Shader>(device, resolve_frag_spv, VK_SHADER_STAGE_FRAGMENT_BIT);

        std::vector<VkDescriptorSetLayoutBinding> gbuf3_bindings;
        for (uint32_t i = 0; i < 3; ++i) {
            VkDescriptorSetLayoutBinding b{};
            b.binding         = i;
            b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            gbuf3_bindings.push_back(b);
        }
        gbuf3_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, gbuf3_bindings);

        // Composite G-buffer layout: G0-G2 plus SSAO (binding 3), matching
        // PixelLightingPass's convention so both passes attenuate by the identical
        // ao * ssao term (see ssr_composite.frag).
        std::vector<VkDescriptorSetLayoutBinding> comp_gbuf_bindings = gbuf3_bindings;
        {
            VkDescriptorSetLayoutBinding b{};
            b.binding         = 3;
            b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
            comp_gbuf_bindings.push_back(b);
        }
        comp_gbuf_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, comp_gbuf_bindings);

        std::vector<VkDescriptorSetLayoutBinding> single_img = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}
        };
        hiz_layout_         = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, single_img);
        scene_color_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, single_img);
        raw_ssr_layout_     = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, single_img);

        // Composite scene-colour layout: binding 0 = raw lit scene colour (compositing
        // target), binding 1 = its prefiltered mip chain (SSGI diffuse-bounce tap).
        std::vector<VkDescriptorSetLayoutBinding> comp_scene_bindings = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}
        };
        comp_scene_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, comp_scene_bindings);

        std::vector<VkDescriptorSetLayoutBinding> resolve_bindings = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}
        };
        resolve_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(device, resolve_bindings);

        desc_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            device, 8,
            std::vector<VkDescriptorPoolSize>{{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}});

        ssr_gbuf_set_    = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *gbuf3_layout_);
        hiz_set_         = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *hiz_layout_);
        scene_color_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *scene_color_layout_);
        resolve_set_     = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *resolve_layout_);

        comp_gbuf3_set_   = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *comp_gbuf_layout_);
        comp_raw_ssr_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *raw_ssr_layout_);
        comp_scene_set_   = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *desc_pool_, *comp_scene_layout_);

        coopa::gfx::pipeline::PipelineConfig cfg{};
        cfg.cull_mode   = VK_CULL_MODE_NONE;
        cfg.depth_test  = false;
        cfg.depth_write = false;

        VkPushConstantRange ssr_pc_range{};
        ssr_pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        ssr_pc_range.offset     = 0;
        ssr_pc_range.size       = sizeof(SsrPushConstants);

        ssr_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, target_->render_pass_object(),
            std::vector<coopa::gfx::pipeline::Shader*>{ssr_vert_.get(), ssr_frag_.get()},
            std::vector<VkVertexInputBindingDescription>{},
            std::vector<VkVertexInputAttributeDescription>{},
            std::vector<VkDescriptorSetLayout>{
                camera_layout, gbuf3_layout_->handle(), hiz_layout_->handle(), scene_color_layout_->handle()},
            cfg,
            std::vector<VkPushConstantRange>{ssr_pc_range});

        VkPushConstantRange comp_pc_range{};
        comp_pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        comp_pc_range.offset     = 0;
        comp_pc_range.size       = sizeof(CompositePushConstants);

        comp_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, composite_target_->render_pass_object(),
            std::vector<coopa::gfx::pipeline::Shader*>{comp_vert_.get(), comp_frag_.get()},
            std::vector<VkVertexInputBindingDescription>{},
            std::vector<VkVertexInputAttributeDescription>{},
            std::vector<VkDescriptorSetLayout>{
                camera_layout, comp_gbuf_layout_->handle(), raw_ssr_layout_->handle(), comp_scene_layout_->handle()},
            cfg,
            std::vector<VkPushConstantRange>{comp_pc_range});

        VkPushConstantRange resolve_pc_range{};
        resolve_pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        resolve_pc_range.offset     = 0;
        resolve_pc_range.size       = sizeof(ResolvePushConstants);

        resolve_pipeline_ = std::make_unique<coopa::gfx::pipeline::Pipeline>(
            device, resolved_target_->render_pass_object(),
            std::vector<coopa::gfx::pipeline::Shader*>{resolve_vert_.get(), resolve_frag_.get()},
            std::vector<VkVertexInputBindingDescription>{},
            std::vector<VkVertexInputAttributeDescription>{},
            std::vector<VkDescriptorSetLayout>{resolve_layout_->handle()},
            cfg,
            std::vector<VkPushConstantRange>{resolve_pc_range});
    }

    SsrPass(const SsrPass&) = delete;
    SsrPass& operator=(const SsrPass&) = delete;

    void recreate(uint32_t width, uint32_t height) {
        width_ = width;
        height_ = height;

        target_->recreate(width_, height_);
        composite_target_->recreate(width_, height_);
        resolved_target_->recreate(width_, height_);

        // History no longer matches the new resolution -- drop it and start fresh.
        history_image_ = std::make_unique<coopa::gfx::memory::Image>(
            device_, allocator_, width_, height_, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT, VMA_MEMORY_USAGE_AUTO);
        history_initialized_ = false;
    }

    void update_descriptors(const coopa::gfx::engine::targets::GBufferTarget& gbuffer,
                            VkImageView hiz_view,
                            VkSampler hiz_sampler,
                            VkImageView scene_color_mip_view,
                            VkSampler scene_color_mip_sampler,
                            VkImageView scene_color_view,
                            const coopa::gfx::engine::util::Sampler& linear_sampler) {
        ssr_gbuf_set_->bind_image(0, gbuffer.g0_view(), nearest_sampler_->handle());
        ssr_gbuf_set_->bind_image(1, gbuffer.g1_view(), nearest_sampler_->handle());
        ssr_gbuf_set_->bind_image(2, gbuffer.g2_view(), nearest_sampler_->handle());

        hiz_set_->bind_image(0, hiz_view, hiz_sampler);
        // The march samples the PREFILTERED chain (cone footprint -> textureLod); the
        // composite's own scene-colour binding below samples the raw full-res colour it is
        // compositing INTO. Same layout, deliberately different images.
        scene_color_set_->bind_image(0, scene_color_mip_view, scene_color_mip_sampler);

        resolve_set_->bind_image(0, target_->color_view(), nearest_sampler_->handle());
        // LINEAR, not nearest: reprojected UVs are not texel-centred.
        resolve_set_->bind_image(1, history_image_->view(), linear_sampler.handle());
        resolve_set_->bind_image(2, gbuffer.g2_view(), nearest_sampler_->handle());

        comp_gbuf3_set_->bind_image(0, gbuffer.g0_view(), linear_sampler.handle());
        comp_gbuf3_set_->bind_image(1, gbuffer.g1_view(), linear_sampler.handle());
        comp_gbuf3_set_->bind_image(2, gbuffer.g2_view(), linear_sampler.handle());

        comp_raw_ssr_set_->bind_image(0, resolved_target_->color_view(), linear_sampler.handle());
        comp_scene_set_->bind_image(0, scene_color_view, linear_sampler.handle());
        comp_scene_set_->bind_image(1, scene_color_mip_view, scene_color_mip_sampler);
    }

    /// Binding 3 of the composite G-buffer set must be rebound every frame -- pass
    /// SsaoPass::output_view() when enabled or SsaoPass::neutral_view() when disabled,
    /// mirroring PixelLightingPass::set_ssao_image() so both passes agree on ao * ssao.
    void set_ssao_image(VkImageView ssao_view, VkSampler ssao_sampler) {
        comp_gbuf3_set_->bind_image(3, ssao_view, ssao_sampler);
    }

    void execute(coopa::gfx::command::CommandBuffer& cmd,
                 const coopa::gfx::pipeline::DescriptorSet& camera_set,
                 const Params& params) {
        if (!history_initialized_) {
            VkImageMemoryBarrier barrier{};
            barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
            barrier.image                           = history_image_->handle();
            barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel   = 0;
            barrier.subresourceRange.levelCount     = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount     = 1;
            barrier.srcAccessMask                   = 0;
            barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

            vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
        }

        // 1. Raymarch.
        target_->begin(cmd);
        cmd.bind_pipeline(*ssr_pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);

        SsrPushConstants pc{};
        pc.inv_proj         = glm::inverse(params.proj);
        pc.max_distance     = params.max_distance;
        pc.bias_texels      = params.bias_texels;
        pc.thickness_min    = params.thickness_min;
        pc.thickness_scale  = params.thickness_scale;
        pc.roughness_cutoff = params.roughness_cutoff;
        pc.max_iterations   = params.max_iterations;
        pc.max_hiz_mip      = params.max_hiz_mip;
        pc.start_mip        = params.start_mip;
        pc.min_mip0_steps   = params.min_mip0_steps;
        pc.max_color_mip    = params.max_color_mip;

        cmd.push_constants(ssr_pipeline_->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SsrPushConstants), &pc);

        cmd.bind_descriptor_set(ssr_pipeline_->layout(), camera_set, 0);
        cmd.bind_descriptor_set(ssr_pipeline_->layout(), *ssr_gbuf_set_, 1);
        cmd.bind_descriptor_set(ssr_pipeline_->layout(), *hiz_set_, 2);
        cmd.bind_descriptor_set(ssr_pipeline_->layout(), *scene_color_set_, 3);

        cmd.draw(3);
        target_->end(cmd);

        // 2. Temporal resolve.
        resolved_target_->begin(cmd);
        cmd.bind_pipeline(*resolve_pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);

        ResolvePushConstants rpc{};
        rpc.prev_view_proj = params.prev_view_proj;
        rpc.resolution     = glm::vec2(static_cast<float>(width_), static_cast<float>(height_));
        rpc.blend_factor   = params.temporal_enabled ? params.temporal_blend : 0.0f;
        rpc.history_valid  = (history_initialized_ && params.prev_view_proj_valid) ? 1 : 0;

        cmd.push_constants(resolve_pipeline_->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ResolvePushConstants), &rpc);
        cmd.bind_descriptor_set(resolve_pipeline_->layout(), *resolve_set_, 0);

        cmd.draw(3);
        resolved_target_->end(cmd);

        // 3. Composite: specular swap + SSGI diffuse bounce.
        composite_target_->begin(cmd);
        cmd.bind_pipeline(*comp_pipeline_);
        cmd.set_viewport(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
        cmd.set_scissor(0, 0, width_, height_);

        CompositePushConstants cpc{};
        cpc.screen_resolution = glm::vec2(static_cast<float>(width_), static_cast<float>(height_));
        cpc.sky_intensity     = params.sky_intensity;
        cpc.ssgi_intensity    = params.ssgi_intensity;
        cpc.ssgi_distance     = params.ssgi_distance;
        cpc.max_color_mip     = params.max_color_mip;
        cmd.push_constants(comp_pipeline_->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CompositePushConstants), &cpc);

        cmd.bind_descriptor_set(comp_pipeline_->layout(), camera_set, 0);
        cmd.bind_descriptor_set(comp_pipeline_->layout(), *comp_gbuf3_set_, 1);
        cmd.bind_descriptor_set(comp_pipeline_->layout(), *comp_raw_ssr_set_, 2);
        cmd.bind_descriptor_set(comp_pipeline_->layout(), *comp_scene_set_, 3);

        cmd.draw(3);
        composite_target_->end(cmd);

        // 4. Copy resolved_target_ into history_image_ for next frame's resolve pass.
        update_ssr_history_(cmd);
    }

    VkImageView output_view() const { return composite_target_->color_view(); }

private:
    void update_ssr_history_(coopa::gfx::command::CommandBuffer& cmd) {
        VkImage src_image = resolved_target_->color_image_object()->handle();
        VkImage dst_image = history_image_->handle();

        VkImageMemoryBarrier barriers[2]{};

        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = src_image;
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.baseMipLevel = 0;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.baseArrayLayer = 0;
        barriers[0].subresourceRange.layerCount = 1;
        barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = dst_image;
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.baseMipLevel = 0;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.baseArrayLayer = 0;
        barriers[1].subresourceRange.layerCount = 1;
        barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageCopy copy_region{};
        copy_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.srcSubresource.layerCount = 1;
        copy_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy_region.dstSubresource.layerCount = 1;
        copy_region.extent = { width_, height_, 1 };

        vkCmdCopyImage(cmd.handle(), src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);

        history_initialized_ = true;
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;

    uint32_t width_;
    uint32_t height_;

    std::unique_ptr<coopa::gfx::engine::util::Sampler> nearest_sampler_;

    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> target_;
    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> composite_target_;
    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> resolved_target_;

    std::unique_ptr<coopa::gfx::memory::Image> history_image_;
    bool history_initialized_ = false;

    std::unique_ptr<coopa::gfx::pipeline::Shader> ssr_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> ssr_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> comp_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> comp_frag_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> resolve_vert_;
    std::unique_ptr<coopa::gfx::pipeline::Shader> resolve_frag_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> gbuf3_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> comp_gbuf_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> hiz_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> scene_color_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> raw_ssr_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> comp_scene_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> resolve_layout_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool> desc_pool_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> ssr_gbuf_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> hiz_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> scene_color_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> resolve_set_;

    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> comp_gbuf3_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> comp_raw_ssr_set_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> comp_scene_set_;

    std::unique_ptr<coopa::gfx::pipeline::Pipeline> ssr_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> comp_pipeline_;
    std::unique_ptr<coopa::gfx::pipeline::Pipeline> resolve_pipeline_;
};

} // namespace passes
} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PASSES_SSR_PASS_H
