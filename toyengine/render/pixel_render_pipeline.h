/**
 * @file pixel_render_pipeline.h
 * @brief Orchestrates the low-resolution deferred pixel-art frame graph.
 *
 * Owns the G-buffer and low-resolution targets, the shared camera UBO
 * and descriptor sets, and every pass. Records the entire frame into a
 * single command buffer via Renderer::begin_frame's pre_pass_fn seam (see
 * gfxcoopa/presentation/renderer.h).
 *
 * Direct lighting has two looks, picked by `soft_lighting` (see
 * pixel_lighting.frag): the default banded-cel formula (diffuse quantized by
 * `light_bands`, specular hard-masked by `spec_threshold`), or a smooth
 * Cook-Torrance falloff when `soft_lighting` is set. `outline`, `palette`,
 * `dither`, `camera_pixel_snap`, `soft_lighting`,
 * `ssao_enabled`, `ssr_enabled` are all independently toggleable on top of
 * the G-buffer/deferred frame graph -- there used to be a separate
 * "enhanced" track selected by a `render.track` config key; that's gone,
 * replaced by these per-feature toggles (see PixelRenderConfig).
 * `ssao_pass_` is always constructed (cheap, and
 * pixel_lighting.frag always has a g_ssao binding to fill); `hiz_pass_`,
 * `scene_color_mip_pass_` and `ssr_pass_` exist to feed SSR/SSGI and are
 * ALSO always constructed (`ssr_enabled` is a runtime flag per
 * render_features.h's policy -- render() re-reads it every frame to decide
 * whether to execute/composite them, so it can be flipped with no pipeline
 * rebuild). Only one point light can cast a shadow at a time (gfxcoopa's ShadowMapTarget holds
 * exactly one cube map, not one per light -- see shadow_map_target.h);
 * every other point light still lights the scene, just without occlusion.
 * GBufferVisualizePass is retained as an unused standalone diagnostic (see
 * its own file doc). `bloom_pass_` (gfxcoopa's BloomPass) is likewise always
 * constructed, gated per-frame by `bloom_enabled` -- but unlike
 * scene_color_mip_pass_/hiz_pass_ below, every descriptor it owns is bound
 * once at construction and never rebinds, so it needs no per-frame wait.
 *
 * No per-frame vkQueueWaitIdle in general -- unlike blendy's
 * PbrRenderPipeline::record_offscreen_() -- *except* when `ssr_enabled` is
 * set: HiZPass/SceneColorMipPass (reused unmodified from gfxcoopa) rebind
 * their own descriptors on every execute() call, which is only safe under a
 * per-frame wait. See render()'s device_.wait_idle() call site.
 *
 * The camera UBO and its descriptor set are per-frame-in-flight
 * (camera_ubos_/camera_sets_), the same policy instance_stream_/sdf_data_
 * already follow, for the same reason: gfxcoopa's CameraUBO owns exactly one
 * buffer, so a single shared instance updated in place would let frame N's
 * write race frame N-1's still-executing command buffer. That used to put
 * mesh rasterization (reading the shared CameraUBO) one frame out of step
 * with the SDF raymarch (reading its own per-slot SdfGlobals.inv_view_proj)
 * -- and with the SDF pass's own depth write in sdf_gbuffer.frag, which also
 * reads CameraUBO -- visible as SDF surfaces sliding against mesh geometry
 * under fast camera motion.
 */

#ifndef TOYENGINE_RENDER_PIXEL_RENDER_PIPELINE_H
#define TOYENGINE_RENDER_PIXEL_RENDER_PIPELINE_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/presentation/renderer.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/targets/gbuffer_target.h>
#include <gfxcoopa/engine/targets/shadow_map_target.h>
#include <gfxcoopa/engine/passes/gbuffer_pipeline.h>
#include <gfxcoopa/engine/passes/shadow_pipeline.h>
#include <gfxcoopa/engine/passes/hiz_pass.h>
#include <gfxcoopa/engine/passes/scene_color_mip_pass.h>
#include <gfxcoopa/engine/passes/ssao_pass.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/data/camera_ubo.h>
#include <gfxcoopa/engine/data/light_data.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/engine/components/point_light.h>
#include <gfxcoopa/engine/passes/skybox_pass.h>
#include <gfxcoopa/engine/passes/transparent_pass.h>
#include <gfxcoopa/engine/targets/transparent_capture_target.h>
#include <gfxcoopa/engine/passes/transparent_capture_pass.h>
#include <gfxcoopa/engine/passes/fog_pass.h>
#include <gfxcoopa/engine/data/fog_data.h>
#include <gfxcoopa/engine/components/fog_volume.h>
#include <gfxcoopa/engine/components/sdf_renderer.h>
#include <gfxcoopa/engine/components/sdf_shape.h>
#include <gfxcoopa/engine/data/sdf_data.h>
#include <gfxcoopa/engine/passes/sdf_gbuffer_pass.h>
#include <gfxcoopa/engine/passes/sdf_forward_pass.h>
#include <gfxcoopa/engine/passes/sdf_shadow_pass.h>
#include <gfxcoopa/engine/passes/sdf_capture_pass.h>

#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/components/transform_component.h>

#include <toyengine/render/pixel_render_config.h>
#include <toyengine/render/pixel_math.h>
#include <toyengine/render/instance_stream.h>
#include <toyengine/render/forward_globals.h>
#include <toyengine/render/material_texture_cache.h>
#include <gfxcoopa/engine/data/palette_lut.h>
#include <gfxcoopa/engine/passes/deferred_lighting_pass.h>
#include <gfxcoopa/engine/passes/ssr_pass.h>
#include <toyengine/render/passes/gbuffer_visualize_pass.h>
#include <gfxcoopa/engine/passes/pixel_stylize_pass.h>
#include <gfxcoopa/engine/passes/bloom_pass.h>
#include <gfxcoopa/engine/passes/tilt_shift_pass.h>
#include <toyengine/render/passes/upscale_pass.h>

namespace toy {
namespace render {

/**
 * @struct PixelLightingPushConstants
 * @brief Matches pixel_lighting.frag's PixelParams push constant block (24 bytes).
 *
 * Toyengine's own concept (banded/cel-shading tuning) -- gfxcoopa's
 * DeferredLightingPass takes an optional trailing VkPushConstantRange list
 * so a consumer can declare exactly this block without a fork; the caller
 * pushes it directly via DeferredLightingPass::layout() before draw().
 */
struct PixelLightingPushConstants {
    float light_bands       = 4.0f;
    float spec_threshold    = 0.55f;
    float rim_strength      = 0.0f;
    float ambient_intensity = 1.0f;
    float sky_intensity     = 1.0f;
    // >0.5 = smooth Cook-Torrance direct lighting; else this engine's default banded/ramped
    // cel-shaded look (light_bands quantized diffuse + spec_threshold hard-masked specular).
    float soft_lighting     = 0.0f;
};

/**
 * @struct TransparentRefractionPushConstants
 * @brief Matches transparent.frag's PushConstants block's trailing [32, 64) region --
 *        appended after TransparentPass::PushConstants' own 32-byte material block (see
 *        that pass's extra_pc_bytes ctor param). Pushed once per MESH object in
 *        record_transparent_() (not per SDF item -- SDF glass doesn't refract, see the
 *        refraction plan), immediately after TransparentPass::push()'s own [0,32) push.
 *
 *        The frame-level lighting/indirect/SSR block that used to occupy this region
 *        (TransparentLightingPushConstants) moved to a UBO -- see ForwardGlobals
 *        (forward_globals.h) -- because adding these two per-object refraction vec4s
 *        alongside it would have put the combined push-constant block over the 128-byte
 *        guaranteed Vulkan minimum. gfxcoopa's SdfGlobals (sdf_data.h) hit the identical
 *        wall on the SDF forward path and was resolved the same way.
 */
struct TransparentRefractionPushConstants {
    glm::vec4 tint_thickness = {1.0f, 1.0f, 1.0f, 0.25f};  // rgb = refraction_tint, w = thickness
    glm::vec4 ior_flags      = {1.45f, 0.0f, 0.0f, 0.0f};  // x = ior, y = refraction on/off, zw reserved
};
// The actual combined push-constant range TransparentPass's pipeline layout declares
// (see its extra_pc_bytes ctor param) -- checked here, not just on
// TransparentPass::PushConstants alone, since that struct's own static_assert can't see
// what a caller adds on top.
static_assert(sizeof(coopa::gfx::engine::passes::TransparentPass::PushConstants) +
             sizeof(TransparentRefractionPushConstants) <= 128,
             "TransparentPass's combined push-constant range exceeds Vulkan's guaranteed "
             "maxPushConstantsSize (128 bytes) -- see the layered-shaders plan's "
             "push-constant budget table before growing either struct.");

/**
 * @struct TransparentCaptureLightingPushConstants
 * @brief Matches transparent_capture.frag's PushConstants block's trailing [32, 56) region --
 *        appended after TransparentCapturePass::PushConstants' own 32-byte material block.
 *        Deliberately smaller than ForwardGlobals: this capture never traces its own SSR
 *        (see transparent_capture.frag's file doc), so none of that struct's
 *        ssr_enabled/ssgi/GfxSsrParams/refraction fields apply here.
 */
struct TransparentCaptureLightingPushConstants {
    float light_bands       = 4.0f;
    float spec_threshold    = 0.55f;
    float soft_lighting     = 0.0f;
    float rim_strength      = 0.0f;
    float ambient_intensity = 1.0f;
    float sky_intensity     = 1.0f;
};
// See TransparentRefractionPushConstants' identical static_assert above.
static_assert(sizeof(coopa::gfx::engine::passes::TransparentCapturePass::PushConstants) +
             sizeof(TransparentCaptureLightingPushConstants) <= 128,
             "TransparentCapturePass's combined push-constant range exceeds Vulkan's "
             "guaranteed maxPushConstantsSize (128 bytes) -- see the layered-shaders "
             "plan's push-constant budget table before growing either struct.");

/**
 * @struct SdfDrawItem
 * @brief One SdfRenderer gathered this frame: its GPU buffer index (see
 *        gfxcoopa's SdfData), draw-list membership, and the world/screen
 *        bounds every recording site below needs.
 *
 * Built once per frame in render()'s SDF gather block and threaded through
 * record_gbuffer_()/record_directional_shadow_()/record_point_shadow_()/
 * record_transparent_capture_()/record_transparent_() -- the same shape
 * `renderers`/`world_matrices`/`instance_idx` already play for MeshRenderer.
 */
struct SdfDrawItem {
    coopa::gfx::engine::components::SdfRenderer* comp = nullptr;
    uint32_t  gpu_index    = 0;     /**< Index into this frame's SdfData renderer SSBO. */
    bool      is_blend     = false; /**< True -> forward/capture path; false -> G-buffer path. */
    bool      cast_shadows = true;  /**< Already folds in PixelRenderConfig::sdf_shadows_enabled. */
    glm::vec3 world_min{0.0f};
    glm::vec3 world_max{0.0f};
    glm::vec3 world_center{0.0f};   /**< For the shadow AABB fit and the back-to-front sort. */
    PixelRect px_rect{};            /**< Scissor rect in low-res render-target pixel space. */
};

/**
 * @class PixelRenderPipeline
 * @brief Renders a scene through the low-resolution deferred pixel-art
 *        frame graph and upscales it into the swapchain.
 */
class PixelRenderPipeline {
public:
    PixelRenderPipeline(coopa::gfx::core::Device& device,
                        coopa::gfx::memory::Allocator& allocator,
                        coopa::gfx::core::Swapchain& swapchain,
                        coopa::gfx::pipeline::RenderPass& swapchain_pass,
                        coopa::gfx::command::CommandPool& cmd_pool,
                        PixelRenderConfig config)
        : device_(device), allocator_(allocator), swapchain_(swapchain), config_(std::move(config)),
          render_extent_(compute_render_extent(config_, swapchain.extent().width, swapchain.extent().height)),
          upscaled_extent_(compute_letterbox(swapchain.extent().width, swapchain.extent().height,
                                             render_extent_.width, render_extent_.height)),
          gbuffer_target_(device, allocator, render_extent_.width, render_extent_.height),
          // HDR always -- the sky-based indirect lighting (see pixel_lighting.frag) can exceed
          // 1.0 regardless of whether SSR/SSAO are toggled, and pixel_stylize.frag's tonemap
          // step (gated on PushConstants::exposure) always applies before dither/palette to
          // bring it back down.
          offscreen_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat),
          post_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA8_Unorm),
          transparent_capture_target_(device, allocator, render_extent_.width, render_extent_.height),
          // Fog composite target -- HDR, same reasoning as offscreen_target_ above: fog belongs
          // in linear HDR (Unity applies it there too), ahead of pixel_stylize_pass_'s tonemap
          // step. A separate target is mandatory, not a style choice: pipeline::RenderPass
          // hardcodes LOAD_OP_CLEAR, so FogPass can't reopen and composite in place onto the
          // image it reads from.
          fog_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat),
          nearest_sampler_(coopa::gfx::engine::util::Sampler::nearest(device)),
          linear_sampler_(coopa::gfx::engine::util::Sampler::linear(device)),
          // Hardware compareEnable, not a plain linear sampler -- see gfx/shadow_sampling.glsl's
          // *Shadow-family doc (pixel_lighting.frag/transparent.frag's dir_shadow_map and
          // point_shadow_map are sampler2DShadow/samplerCubeShadow to match).
          shadow_sampler_(coopa::gfx::engine::util::Sampler::shadow(device)),
          light_data_(device, allocator),
          fog_data_(device, allocator),
          shadow_target_(device, allocator, config_.shadow_map_resolution, config_.cube_shadow_resolution),
          palette_lut_(coopa::gfx::engine::data::PaletteLut::load(device, allocator, cmd_pool, config_.palette_path)),
          instance_stream_(device, allocator),
          forward_globals_(device, allocator),
          sdf_data_(device, allocator, config_.sdf_max_renderers, config_.sdf_max_shapes)
    {
        camera_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .uniform_buffer(0, coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment)
                .build(device));

        // One CameraUBO + one descriptor set per frame-in-flight slot, mirroring sdf_data_'s
        // own per-slot policy (see sdf_data.h's file doc) -- camera_layout_ above stays a
        // single shared layout, since every set allocated from it is interchangeable at any
        // pipeline built from that layout (see the ~10 construction sites below). Every
        // bind_buffer() below still happens once at construction, not per frame, so this adds
        // no per-frame vkUpdateDescriptorSets and doesn't trip the VUID this file's doc warns
        // about elsewhere. std::unique_ptr is load-bearing, not stylistic: CameraUBO's deleted
        // copy ctor suppresses its implicit move ctor too, so it isn't MoveInsertable and
        // std::vector<CameraUBO> won't compile.
        camera_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*camera_layout_, kCameraFrames).build(device));

        camera_ubos_.reserve(kCameraFrames);
        camera_sets_.reserve(kCameraFrames);
        for (uint32_t i = 0; i < kCameraFrames; ++i) {
            camera_ubos_.push_back(std::make_unique<coopa::gfx::engine::data::CameraUBO>(device, allocator));
            camera_sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *camera_pool_, *camera_layout_));
            camera_sets_[i]->bind_buffer(0, camera_ubos_[i]->buffer());
        }

        light_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .uniform_buffer(0, coopa::gfx::ShaderStage::Fragment)
                .build(device));
        light_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*light_layout_, 1).build(device));
        light_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *light_pool_, *light_layout_);
        light_set_->bind_buffer(0, light_data_.buffer());

        shadow_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
                .build(device));
        shadow_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*shadow_layout_, 1).build(device));
        shadow_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *shadow_pool_, *shadow_layout_);
        shadow_set_->bind_image(0, shadow_target_.dir_shadow_view(), shadow_sampler_.handle());
        shadow_set_->bind_image(1, shadow_target_.cube_shadow_view(), shadow_sampler_.handle());

        // Built before shadow_pipeline_/gbuffer_pipeline_ below -- both need material_cache_'s
        // layout handle to append the CUTOUT alpha-mask sampler as their material set. See
        // MaterialTextureCache's own doc for why lazily allocating sets from it later (once a
        // scene's masked materials finish loading) is safe under overlapped command buffers.
        material_cache_ = std::make_unique<toy::render::MaterialTextureCache>(device, allocator, cmd_pool);

        shadow_pipeline_ = std::make_unique<coopa::gfx::engine::passes::ShadowPipeline>(
            device, shadow_target_.dir_render_pass(), shadow_target_.cube_render_pass(),
            config_.shaders("shadow_depth.vert"),
            config_.shaders("shadow_depth.frag"),
            config_.shaders("shadow_cube.vert"),
            config_.shaders("shadow_cube.frag"),
            &material_cache_->layout_object());

        gbuffer_pipeline_ = std::make_unique<coopa::gfx::engine::passes::GBufferPipeline>(
            device, gbuffer_target_.render_pass(), camera_layout_->handle(), material_cache_->layout(),
            config_.shaders("gbuffer.vert"),
            config_.shaders("gbuffer.frag"));

        // Register every Opaque-domain derived shader (e.g. foliage) as a named pipeline
        // variant on both the G-buffer and shadow passes -- see SurfaceShaderDesc's doc on
        // why an entry point left empty falls back to the STOCK logical name rather than
        // being skipped: a shader that only overrides, say, the fragment stage still needs
        // a real shadow entry point, or its shadow silently stops moving with it.
        for (const auto& sd : config_.surface_shaders.all()) {
            if (sd.domain != coopa::gfx::pipeline::SurfaceShaderDomain::Opaque) continue;
            gbuffer_pipeline_->add_variant(
                sd.name,
                config_.shaders(sd.vert.empty() ? "gbuffer.vert" : sd.vert),
                config_.shaders(sd.frag.empty() ? "gbuffer.frag" : sd.frag),
                sd.cull);
            shadow_pipeline_->add_variant(
                sd.name,
                config_.shaders(sd.shadow_vert.empty() ? "shadow_depth.vert" : sd.shadow_vert),
                config_.shaders(sd.shadow_frag.empty() ? "shadow_depth.frag" : sd.shadow_frag),
                config_.shaders(sd.shadow_cube_vert.empty() ? "shadow_cube.vert" : sd.shadow_cube_vert),
                config_.shaders(sd.shadow_cube_frag.empty() ? "shadow_cube.frag" : sd.shadow_cube_frag));
        }

        // --- SDF raymarching system ---
        // Three of the four SDF passes need nothing this pipeline hasn't already built by this
        // point (camera_layout_/shadow_target_/transparent_capture_target_/sdf_data_); the
        // fourth (sdf_forward_pass_) needs transparent_pass_'s render pass and ssr_pass_'s trace
        // sets, both constructed later, so it's built further down alongside transparent_pass_
        // itself (see that call site).
        sdf_gbuffer_pass_ = std::make_unique<coopa::gfx::engine::passes::SdfGBufferPass>(
            device, gbuffer_target_.render_pass(), *camera_layout_, sdf_data_.layout(),
            config_.shaders("sdf_quad.vert"),
            config_.shaders("sdf_gbuffer.frag"));

        sdf_shadow_pass_ = std::make_unique<coopa::gfx::engine::passes::SdfShadowPass>(
            device, shadow_target_.dir_render_pass(), shadow_target_.cube_render_pass(),
            sdf_data_.layout(),
            config_.shaders("sdf_shadow.vert"),
            config_.shaders("sdf_shadow.frag"),
            config_.shaders("sdf_shadow_cube.vert"),
            config_.shaders("sdf_shadow_cube.frag"));

        sdf_capture_pass_ = std::make_unique<coopa::gfx::engine::passes::SdfCapturePass>(
            device, transparent_capture_target_.render_pass(),
            *camera_layout_, *light_layout_, *shadow_layout_, sdf_data_.layout(),
            config_.shaders("sdf_quad.vert"),
            config_.shaders("sdf_capture.frag"));

        gbuffer_visualize_pass_ = std::make_unique<passes::GBufferVisualizePass>(
            device, offscreen_target_.render_pass_object(),
            config_.shaders("fullscreen.vert"),
            config_.shaders("gbuffer_visualize.frag"));
        gbuffer_visualize_pass_->set_albedo_image(gbuffer_target_.g0_view_typed(), linear_sampler_);

        skybox_pass_ = std::make_unique<coopa::gfx::engine::passes::SkyboxPass>(
            device, offscreen_target_.render_pass_object(), *camera_layout_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("skybox.frag"));
        skybox_pass_->set_gbuffer_normal_image(gbuffer_target_.g1_view_typed(), linear_sampler_);

        // SsaoPass is always constructed: pixel_lighting.frag (and the SSR composite, when
        // that's built below) always has a g_ssao binding to fill, toggle or not -- cheap
        // either way, since SsaoPass's ctor already builds a permanent 1x1 neutral texture.
        ssao_pass_ = std::make_unique<coopa::gfx::engine::passes::SsaoPass>(
            device, allocator, cmd_pool, *camera_layout_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("ssao.frag"),
            config_.shaders("ssao_resolve.frag"),
            config_.shaders("ssao_blur.frag"));
        ssao_pass_->recreate(render_extent_.width, render_extent_.height);
        // Bound once here, not per frame: g1/g2 never change (no resize support), and
        // SsaoPass::update_descriptors() calls bind_image(), which does an immediate
        // vkUpdateDescriptorSets -- illegal on a descriptor set any still-pending command
        // buffer references. This pipeline overlaps MAX_FRAMES_IN_FLIGHT command buffers with
        // no per-frame wait in the common case (see the class doc), so calling this from
        // inside render()'s per-frame lambda would intermittently hit exactly that validation
        // error under sustained multi-frame rendering.
        ssao_pass_->update_descriptors(gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed(), linear_sampler_);

        // ssao_enabled is a load-time config value (no live reload), so which image to bind is
        // decided once here rather than every frame -- see the update_descriptors comment above
        // for why a per-frame rebind would be unsafe anyway.
        VkImageView ssao_view = config_.ssao_enabled ? ssao_pass_->output_view() : ssao_pass_->neutral_view();

        // No ExtraSets (toyengine has neither GI nor reflection probes to plumb through), so
        // this collapses to the same {camera=0, light=1, shadow=2, gbuffer=3} layout the old
        // fork hardcoded -- gbuffer_set_index_ is derived, not hardcoded, so this is correct
        // whether or not extras are ever added later (see the fix in deferred_lighting_pass.h).
        pixel_lighting_pass_ = std::make_unique<coopa::gfx::engine::passes::DeferredLightingPass>(
            device, offscreen_target_.render_pass_object(), *camera_layout_, *light_layout_,
            *shadow_layout_, linear_sampler_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("pixel_lighting.frag"),
            coopa::gfx::engine::passes::ExtraSets{},
            std::vector<coopa::gfx::pipeline::PushConstantRange>{
                {coopa::gfx::ShaderStage::Fragment, 0, sizeof(PixelLightingPushConstants)}});
        pixel_lighting_pass_->set_gbuffer_images(
            gbuffer_target_.g0_view_typed(), gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed(),
            gbuffer_target_.g3_view_typed(), linear_sampler_);
        pixel_lighting_pass_->set_ssao_image(ssao_view, ssao_pass_->sampler().handle());

        // ssr_enabled is a RUNTIME flag (see render_features.h's policy doc): hiz_pass_/
        // scene_color_mip_pass_/ssr_pass_ are always constructed -- cheap, ~2MB of screen-sized
        // targets at this pipeline's low internal resolution -- and render() below re-reads
        // config_.ssr_enabled every frame to decide whether to execute/composite them, so the
        // flag can be flipped at runtime with no pipeline rebuild. (Previously this block was
        // itself gated on config_.ssr_enabled, making the flag startup-only; every downstream
        // read of it was already a fresh per-frame check, so unconditional construction is the
        // only change needed.)
        //
        // Hi-Z depth pyramid + prefiltered scene-colour mip chain: both consumed by ssr_pass_'s
        // raymarch/cone-trace. HiZPass::execute() also performs the gbuffer-depth
        // DEPTH_STENCIL_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL transition that
        // transition_gbuffer_depth_to_shader_read_() does when SSR is off for this frame --
        // render() must not call both (see that method's doc comment).
        hiz_pass_ = std::make_unique<coopa::gfx::engine::passes::HiZPass>(
            device, allocator,
            config_.shaders("fullscreen.vert"),
            config_.shaders("hiz_downsample.frag"));
        hiz_pass_->recreate(render_extent_.width, render_extent_.height);

        scene_color_mip_pass_ = std::make_unique<coopa::gfx::engine::passes::SceneColorMipPass>(
            device, allocator,
            config_.shaders("fullscreen.vert"),
            config_.shaders("scene_color_downsample.frag"));
        scene_color_mip_pass_->recreate(render_extent_.width, render_extent_.height);

        // No ExtraSets (toyengine has neither GI nor reflection probes to plumb through)
        // and half_res left at its false default -- toyengine already renders at the
        // pipeline's low internal resolution, so there is no separate "trace at half of
        // that" tier worth the bilateral-upsample cost.
        ssr_pass_ = std::make_unique<coopa::gfx::engine::passes::SsrPass>(
            device, allocator, *camera_layout_,
            render_extent_.width, render_extent_.height,
            config_.shaders("fullscreen.vert"),
            config_.shaders("ssr.frag"),
            config_.shaders("fullscreen.vert"),
            config_.shaders("ssr_composite.frag"),
            config_.shaders("fullscreen.vert"),
            config_.shaders("ssr_resolve.frag"),
            /*half_res=*/false,
            coopa::gfx::engine::passes::ExtraSets{},
            // Spatial denoise for the SSR buffer (bilateral blur, same technique as
            // SsaoPass's own ssao_blur.frag) -- addresses hit/miss noise at reflection
            // boundaries that temporal accumulation alone doesn't fully resolve, especially
            // under continuous camera motion (this scene's auto-rotating orbit camera).
            config_.shaders("fullscreen.vert"),
            config_.shaders("ssr_blur.frag"));
        ssr_pass_->update_descriptors(
            gbuffer_target_, hiz_pass_->full_hiz_view_typed(), hiz_pass_->sampler(),
            scene_color_mip_pass_->full_view_typed(), scene_color_mip_pass_->sampler(),
            offscreen_target_.color_view_typed(), linear_sampler_);
        ssr_pass_->set_ssao_image(ssao_view, ssao_pass_->sampler().handle());

        // --- ssr_reflect_transparent: opaque surfaces also reflect transparent geometry ---
        // Always constructed (same always-on-but-runtime-gated policy as hiz_pass_/
        // scene_color_mip_pass_/ssr_pass_ above); render() re-reads
        // config_.ssr_reflect_transparent every frame.
        transparent_capture_pass_ = std::make_unique<coopa::gfx::engine::passes::TransparentCapturePass>(
            device, transparent_capture_target_.render_pass(),
            camera_layout_->handle(), light_layout_->handle(), shadow_layout_->handle(),
            config_.shaders("pbr.vert"),
            config_.shaders("transparent_capture.frag"),
            static_cast<uint32_t>(sizeof(TransparentCaptureLightingPushConstants)));

        // Second, independent HiZPass/SceneColorMipPass instances over transparent_capture_
        // target_'s own depth/shaded-color images -- both classes are fully generic
        // (execute() takes a plain depth/colour image+view, no GBufferTarget reference held),
        // so this reuse needs no engine changes beyond what hiz_pass_/scene_color_mip_pass_
        // already prove works.
        transparent_hiz_pass_ = std::make_unique<coopa::gfx::engine::passes::HiZPass>(
            device, allocator,
            config_.shaders("fullscreen.vert"),
            config_.shaders("hiz_downsample.frag"));
        transparent_hiz_pass_->recreate(render_extent_.width, render_extent_.height);

        transparent_scene_color_mip_pass_ = std::make_unique<coopa::gfx::engine::passes::SceneColorMipPass>(
            device, allocator,
            config_.shaders("fullscreen.vert"),
            config_.shaders("scene_color_downsample.frag"));
        transparent_scene_color_mip_pass_->recreate(render_extent_.width, render_extent_.height);

        // Refraction's own post-SSR scene-colour chain (see refraction_scene_color_mip_pass_'s
        // own doc for why this must be a separate instance/set, not a second execute() on
        // scene_color_mip_pass_ itself). Always constructed, matching this pipeline's usual
        // policy elsewhere -- render() gates execute() per frame on config_.refraction_enabled
        // && config_.transparency_enabled; its image simply never gets sampled if
        // config_.refraction_enabled was false at construction (see the transparent_extra
        // bind lambda just below, which is the only thing that ever reads this set, and only
        // does so when that same condition held at startup).
        refraction_scene_color_mip_pass_ = std::make_unique<coopa::gfx::engine::passes::SceneColorMipPass>(
            device, allocator,
            config_.shaders("fullscreen.vert"),
            config_.shaders("scene_color_downsample.frag"));
        refraction_scene_color_mip_pass_->recreate(render_extent_.width, render_extent_.height);

        refraction_scene_color_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .build(device));
        refraction_scene_color_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*refraction_scene_color_layout_, 1).build(device));
        refraction_scene_color_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(
            device, *refraction_scene_color_pool_, *refraction_scene_color_layout_);
        refraction_scene_color_set_->bind_image(0, refraction_scene_color_mip_pass_->full_view(),
                                                refraction_scene_color_mip_pass_->sampler().handle());

        // ssr_pass_->set_secondary_source() is NOT called here, unlike update_descriptors()
        // above -- deliberately. transparent_hiz_pass_/transparent_scene_color_mip_pass_'s
        // images are freshly recreate()'d (VK_IMAGE_LAYOUT_UNDEFINED) at this point and stay
        // that way until their first execute(), which only happens on a frame where
        // config_.ssr_reflect_transparent is true (see record()). Binding the secondary
        // source to them here, at construction, would point ssr_pass_'s always-valid pipeline
        // layout at a descriptor that is NOT yet validly laid out on any frame before that
        // first execute() -- exactly the VK_IMAGE_LAYOUT_UNDEFINED validation error this
        // session already hit and fixed once for a different pass. So set_secondary_source()
        // is instead called from record(), gated on config_.ssr_reflect_transparent and
        // sequenced AFTER transparent_hiz_pass_/transparent_scene_color_mip_pass_'s execute()
        // for that same frame -- leaving sets 4-6 correctly bound to the permanent neutral
        // fallback SsrPass's own constructor already set up, on every frame before that.

        // Extra sets 3/4/5 = ssr_pass_'s own trace-input sets (G-buffer, Hi-Z, prefiltered
        // scene-colour mip chain), so transparent.frag can call gfx/ssr_trace_body.glsl's
        // gfx_ssr_trace() with the SAME descriptors ssr.frag itself traces against -- giving
        // BLEND geometry the same screen-space reflection + SSGI bounce opaque geometry gets
        // from ssr_composite_body.glsl, instead of the flat analytic sky fallback alone. Must
        // be constructed after ssr_pass_ (whose layouts/sets this borrows) and after
        // ssr_pass_->update_descriptors() above (so the sets are already populated -- they are
        // bound once for the run, never per-frame; see that call's own comment).
        //
        // Set 6 -- forward_globals_'s per-frame lighting/indirect/SSR/refraction UBO (see
        // forward_globals.h). Appended after ssr_pass_'s own three trace-input sets, mesh-only:
        // sdf_forward_extra below does NOT get this set, since SDF glass keeps reading its own
        // SdfGlobals UBO unchanged (see the refraction plan for why). This is where
        // soft_lighting/light_bands/spec_threshold plus the rim/indirect/SSR tuning
        // pixel_lighting.frag and ssr_pass_'s Params also carry now lives -- it used to be a
        // per-frame push constant here (TransparentLightingPushConstants) until per-object
        // refraction fields needed room in the same push-constant block that left none (see
        // TransparentRefractionPushConstants' own doc, and extra_pc_bytes below, for why).
        // u_scene_color (the third of these four sets) comes from refraction_scene_color_
        // mip_pass_ instead of ssr_pass_'s own scene_color_set() when refraction was enabled
        // at startup -- decided ONCE here, matching pre_fog_view_typed_'s own "startup value,
        // not re-selected per frame" policy (config_.refraction_enabled never changes at
        // runtime, so this is safe). When refraction_enabled is false, this is BYTE-IDENTICAL
        // to the pre-refraction binding, which is what keeps that config the regression guard
        // for this feature. See refraction_scene_color_mip_pass_'s own doc for why this can't
        // just be a second execute() on scene_color_mip_pass_ itself.
        const bool refraction_scene_color_active = config_.transparency_enabled && config_.refraction_enabled;
        coopa::gfx::engine::passes::ExtraSets transparent_extra;
        transparent_extra.layouts = {
            &ssr_pass_->trace_gbuffer_layout(), &ssr_pass_->hiz_layout(),
            refraction_scene_color_active ? refraction_scene_color_layout_.get() : &ssr_pass_->scene_color_layout(),
            &forward_globals_.layout()
        };
        transparent_extra.bind = [this, refraction_scene_color_active](coopa::gfx::command::CommandBuffer& cmd, uint32_t first_set) {
            cmd.bind_descriptor_set(ssr_pass_->trace_gbuffer_set(), first_set);
            cmd.bind_descriptor_set(ssr_pass_->hiz_set(), first_set + 1);
            if (refraction_scene_color_active) {
                cmd.bind_descriptor_set(*refraction_scene_color_set_, first_set + 2);
            } else {
                cmd.bind_descriptor_set(ssr_pass_->scene_color_set(), first_set + 2);
            }
            cmd.bind_descriptor_set(forward_globals_.current_set(), first_set + 3);
        };

        // transparent.frag reuses pbr.vert (byte-identical to gbuffer.vert -- see gfx/), the
        // same convention blendy uses for its own TransparentPass. extra_pc_bytes is now just
        // TransparentRefractionPushConstants -- the frame-level lighting/SSR block that used
        // to occupy this region moved to forward_globals_'s UBO (set 6 above); see that
        // struct's own doc for why.
        transparent_pass_ = std::make_unique<coopa::gfx::engine::passes::TransparentPass>(
            device, VK_FORMAT_R16G16B16A16_SFLOAT, *camera_layout_, *light_layout_,
            *shadow_layout_,
            config_.shaders("pbr.vert"),
            config_.shaders("transparent.frag"),
            transparent_extra,
            static_cast<uint32_t>(sizeof(TransparentRefractionPushConstants)));

        // Register every Transparent-domain derived shader (e.g. water) as a named pipeline
        // variant on both the forward transparent pass and its SSR-secondary-source capture
        // pass. The SAME vert entry point is registered on both -- see
        // TransparentCapturePass::add_variant()'s doc on why: SSR must reflect the same
        // displaced geometry the visible draw shows, not the undisplaced mesh.
        for (const auto& sd : config_.surface_shaders.all()) {
            if (sd.domain != coopa::gfx::pipeline::SurfaceShaderDomain::Transparent) continue;
            const std::string vert_spv = config_.shaders(sd.vert.empty() ? "pbr.vert" : sd.vert);
            transparent_pass_->add_variant(
                sd.name, vert_spv,
                config_.shaders(sd.frag.empty() ? "transparent.frag" : sd.frag));
            transparent_capture_pass_->add_variant(
                sd.name, vert_spv,
                config_.shaders(sd.capture_frag.empty() ? "transparent_capture.frag" : sd.capture_frag));
        }

        // SdfForwardPass shares transparent_pass_'s OWN render pass (see that accessor's doc) --
        // render passes only need to be attachment-compatible to back a second Pipeline, and
        // sharing lets record_transparent_() draw BLEND meshes and BLEND SdfRenderers inside the
        // SAME begin()/end() bracket, switching pipelines per item in one back-to-front sorted
        // list, so the two composite in correct depth order instead of one kind always landing
        // on top of the other. Same ExtraSets as transparent_pass_ (SSR's trace inputs), just
        // appended one set index later (4/5/6, not 3/4/5) since this pass's own SdfData set
        // occupies 3.
        coopa::gfx::engine::passes::ExtraSets sdf_forward_extra;
        sdf_forward_extra.layouts = {
            &ssr_pass_->trace_gbuffer_layout(), &ssr_pass_->hiz_layout(), &ssr_pass_->scene_color_layout()
        };
        sdf_forward_extra.bind = [this](coopa::gfx::command::CommandBuffer& cmd, uint32_t first_set) {
            cmd.bind_descriptor_set(ssr_pass_->trace_gbuffer_set(), first_set);
            cmd.bind_descriptor_set(ssr_pass_->hiz_set(), first_set + 1);
            cmd.bind_descriptor_set(ssr_pass_->scene_color_set(), first_set + 2);
        };
        sdf_forward_pass_ = std::make_unique<coopa::gfx::engine::passes::SdfForwardPass>(
            device, VK_FORMAT_R16G16B16A16_SFLOAT, transparent_pass_->render_pass(),
            *camera_layout_, *light_layout_, *shadow_layout_, sdf_data_.layout(),
            config_.shaders("sdf_quad.vert"),
            config_.shaders("sdf_forward.frag"),
            sdf_forward_extra);

        // Without SSR, post reads the deferred-lit+sky target directly; with it, post reads
        // ssr_pass_'s composite output (specular swap + SSGI bounce already applied). Chosen
        // once here from config_.ssr_enabled's STARTUP value, not re-selected per frame: unlike
        // ssr_pass_'s own execute()/wait_idle() gating (genuinely runtime -- see
        // render_features.h's policy doc), rebinding this descriptor would need either a
        // per-frame device_.wait_idle() (the same cost SSR itself already pays) or a
        // shader-side selector between two permanently-bound views, neither of which exists
        // yet. Harmless today -- nothing currently mutates config_.ssr_enabled after
        // construction -- but flip this toggle from a future live debug UI and post-process
        // will keep reading whichever source was current at startup. fog_pass_ shares this
        // exact same fixed source (see fog_pass_'s own construction just below).
        pre_fog_view_typed_ =
            config_.ssr_enabled ? ssr_pass_->output_view_typed() : offscreen_target_.color_view_typed();

        // Fog composite -- always constructed (mirrors SSR/transparency's always-on-but-
        // runtime-gated policy elsewhere in this pipeline); render() checks config_.fog_enabled
        // per frame to decide whether to execute it. Reads pre_fog_view_typed_ (fixed at
        // startup, same reasoning as pixel_stylize_pass_'s own binding above), so transparent
        // geometry -- drawn in place into that same underlying image -- is fogged too, matching
        // Unity and blendy's own FogPass integration.
        fog_pass_ = std::make_unique<coopa::gfx::engine::passes::FogPass>(
            device, fog_target_.render_pass_object(), fog_data_.buffer(),
            config_.shaders("fullscreen.vert"),
            config_.shaders("fog.frag"));
        fog_pass_->set_source_images(pre_fog_view_typed_, gbuffer_target_.g1_view_typed(),
                                     gbuffer_target_.g2_view_typed(), linear_sampler_);

        // The image BOTH pixel_stylize_pass_ and bloom_pass_ read: the final pre-tonemap HDR
        // frame. One local, not two independent expressions, so the two can never drift apart
        // (same single-source-of-truth reasoning as config_.indirect). Fixed at construction,
        // same startup-only-binding policy as pre_fog_view_typed_ above.
        //
        // This is a deliberate CORRECTNESS change from bloom's previous implementation, not
        // just a stability one: that version sourced offscreen_target_ (pre-SSR, pre-
        // transparent, pre-fog) purely as a side effect of reusing scene_color_mip_pass_'s
        // chain, so SSR reflections, BLEND geometry and fog never contributed to the glow.
        // They do now.
        coopa::gfx::TextureView post_source_view =
            config_.fog_enabled ? fog_target_.color_view_typed() : pre_fog_view_typed_;

        // Independent bloom pyramid (bright-pass threshold -> multi-tap downsample ->
        // tent-filter upsample+combine -- see gfxcoopa's bloom_pass.h). Always constructed
        // (mirrors this pipeline's always-on-but-runtime-gated policy elsewhere), but its
        // result is only BOUND into pixel_stylize_pass_ below when config_.bloom_enabled was
        // true at construction -- an unbound-but-never-executed target would leave that
        // binding pointing at an image still in VK_IMAGE_LAYOUT_UNDEFINED, the same class of
        // hazard ssr_pass_'s own secondary-source binding already hit and documented fixing
        // once. Unlike scene_color_mip_pass_, every descriptor here is bound once at
        // construction and never rebinds, so it needs no per-frame device_.wait_idle().
        bloom_pass_ = std::make_unique<coopa::gfx::engine::passes::BloomPass>(
            device, allocator, render_extent_.width, render_extent_.height,
            post_source_view, linear_sampler_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("bloom_prefilter.frag"),
            config_.shaders("bloom_downsample.frag"),
            config_.shaders("bloom_upsample.frag"));

        pixel_stylize_pass_ = std::make_unique<coopa::gfx::engine::passes::PixelStylizePass>(
            device, post_target_.render_pass_object(),
            config_.shaders("fullscreen.vert"),
            config_.shaders("pixel_stylize.frag"));
        // bloom_result is bound only when config_.bloom_enabled was true at construction. The
        // nullptr sampler falls back to a harmless self-bind of scene_color (see
        // PixelStylizePass::set_source_images), and bloom_intensity is forced to 0 on those
        // runs anyway (see the per-frame push-constant fill below).
        pixel_stylize_pass_->set_source_images(
            post_source_view,
            gbuffer_target_.depth_view_typed(), gbuffer_target_.g1_view_typed(),
            palette_lut_.view_typed(), linear_sampler_, nearest_sampler_,
            config_.bloom_enabled ? bloom_pass_->result_view_typed() : coopa::gfx::TextureView{},
            config_.bloom_enabled ? &linear_sampler_ : nullptr);

        // Diorama tilt-shift blur -- always constructed (mirrors bloom_pass_/fog_pass_'s
        // always-on-but-gated policy), sized to the DISPLAY (letterboxed) rect, not
        // render_extent_ -- see gfxcoopa's TiltShiftPass file doc for why it must run
        // after the upscale rather than before it. Reads post_target_ directly: it folds
        // the nearest-neighbour upscale into its own horizontal stage (bit-identical to
        // upscale_pass_'s own output wherever the blur strength is ~0), so when enabled
        // (see the source selection below) upscale_pass_ becomes a 1:1 letterbox blit of
        // this pass's already-full-resolution result instead of doing the upscale itself.
        tilt_shift_pass_ = std::make_unique<coopa::gfx::engine::passes::TiltShiftPass>(
            device, allocator, upscaled_extent_.w, upscaled_extent_.h,
            post_target_.color_image_object()->view_typed(), nearest_sampler_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("tilt_shift.frag"));

        upscale_pass_ = std::make_unique<passes::UpscalePass>(
            device, swapchain_pass,
            config_.shaders("fullscreen.vert"),
            config_.shaders("upscale.frag"));
        // Startup-fixed source selection, same caveat as post_source_view/
        // pre_fog_view_typed_ above: flipping config_.tilt_shift_enabled without a
        // pipeline rebuild would leave upscale_pass_ reading a stale source.
        upscale_pass_->set_source_image(
            config_.tilt_shift_enabled ? tilt_shift_pass_->result_view_typed()
                                       : post_target_.color_image_object()->view_typed(),
            nearest_sampler_);
    }

    PixelRenderPipeline(const PixelRenderPipeline&) = delete;
    PixelRenderPipeline& operator=(const PixelRenderPipeline&) = delete;

    /**
     * @brief Installs the JobEngine render()'s per-frame gathers may dispatch to.
     * @param jobs Non-owning pointer, or nullptr to force every gather fully serial
     *   (the default) -- see should_parallelize_()'s doc.
     */
    void set_job_engine(coopa::job::JobEngine* jobs) { jobs_ = jobs; }

    /**
     * @brief Sets the minimum element count before a gather dispatches jobs instead of
     *        running serially -- see should_parallelize_()'s doc. Default 256, matching
     *        coopa::anim::AnimationSystem's own house threshold.
     */
    void set_parallel_threshold(std::size_t n) { parallel_threshold_ = n; }

    /** @brief Low-resolution render width in pixels. */
    uint32_t render_width() const { return render_extent_.width; }
    /** @brief Low-resolution render height in pixels. */
    uint32_t render_height() const { return render_extent_.height; }
    /** @brief The final low-resolution LDR color image, for pixel-accurate screenshots. */
    coopa::gfx::memory::Image& low_res_color_image() const { return *post_target_.color_image_object(); }
    /**
     * @brief The final DISPLAY-resolution image actually shown in the window: tilt_shift_pass_'s
     *        result when config_.tilt_shift_enabled (the same startup-fixed selection
     *        upscale_pass_'s own source binding uses), else low_res_color_image() -- there is
     *        no separate full-resolution buffer to fall back to when tilt shift is off.
     */
    coopa::gfx::memory::Image& final_color_image() const {
        return config_.tilt_shift_enabled ? tilt_shift_pass_->result_image() : low_res_color_image();
    }

    /**
     * @brief Validates every loaded MeshRenderer/SdfRenderer material's `shader` field
     *        against config_.surface_shaders, throwing on the first unregistered name.
     *
     * Call once after a scene finishes loading (see Engine's ctor) -- an unresolvable
     * PBRMaterial::shader is a scene-authoring error, and this is what turns it into a
     * startup failure instead of a silent fall-back-to-stock at draw time. Materials are
     * plain YAML-parsed structs with no pipeline/registry context of their own (see
     * PBRMaterial::shader's doc), so this check can't happen during parsing itself --
     * it has to happen here, once both the scene and this pipeline's registry exist.
     *
     * @throws std::runtime_error via SurfaceShaderRegistry::require() if any material
     *         references an unregistered shader name.
     */
    void validate_material_shaders(coopa::scene::Scene& scene) const {
        for (auto* mr : scene.get_components<coopa::gfx::engine::components::MeshRenderer>()) {
            config_.surface_shaders.require(mr->material.shader);
        }
        for (auto* sr : scene.get_components<coopa::gfx::engine::components::SdfRenderer>()) {
            config_.surface_shaders.require(sr->material.shader);
        }
    }

    /**
     * @brief Renders one frame of the given scene.
     *
     * @param dt Wall-clock (or FIXED_DT-overridden) seconds since the previous frame -- see
     *           Engine::tick(). Accumulated into elapsed_time_ and pushed as gfx_time to
     *           every surface-shader backbone (see gfx/surface/gbuffer_vs.glsl), so a
     *           derived shader's displacement hook (e.g. foliage wind sway, water waves)
     *           can animate. Not read at all by the stock hooks, so a scene with no
     *           derived shaders is unaffected by this parameter's value.
     * @return True if the frame was presented, false if the window was minimized.
     */
    bool render(coopa::gfx::presentation::Renderer& renderer, coopa::scene::Scene& scene, float dt) {
        using coopa::gfx::engine::components::CameraComponent;
        using coopa::gfx::engine::components::CameraType;
        using coopa::gfx::engine::components::MeshRenderer;
        using coopa::gfx::engine::components::DirectionalLightComponent;
        using coopa::gfx::engine::components::PointLightComponent;

        frame_dt_ = dt;
        elapsed_time_ += dt;

        update_lights_(scene);

        // The main camera, not merely "the first one found" -- CameraComponent::main()
        // guarantees a stable choice across multi-camera scenes (see camera_component.h).
        auto* cam = CameraComponent::main();
        float aspect = static_cast<float>(render_extent_.width) / static_cast<float>(render_extent_.height);

        glm::mat4 view = cam ? cam->get_view_matrix() : glm::mat4(1.0f);
        glm::mat4 proj = cam ? cam->get_projection_matrix(aspect) : glm::mat4(1.0f);
        glm::vec3 cam_pos = cam ? cam->get_world_position() : glm::vec3(0.0f);

        float pixel_density = 0.0f;
        if (cam && config_.camera_pixel_snap) {
            if (cam->type == CameraType::Orthographic) {
                pixel_density = compute_pixel_density(true, cam->orthographic_size, render_extent_.height);
            } else if (!warned_perspective_snap_) {
                std::cerr << "[toyengine] camera_pixel_snap is enabled but the active camera is "
                             "perspective; snapping only applies to orthographic cameras and is "
                             "disabled for this camera.\n";
                warned_perspective_snap_ = true;
            }
        }
        // One frame-slot source of truth for every per-frame-in-flight upload this function
        // does (camera, instances, SDF data) -- collapsing what used to be independent
        // renderer.current_frame() calls onto this single local makes it structurally
        // impossible for the camera's slot to disagree with the SDF data's slot (see the
        // class file doc's paragraph on camera_ubos_/camera_sets_). current_frame_ only
        // advances at the end of Renderer::draw_frame, after record_fn runs, so frame_slot is
        // stable for the whole of render() including the pre_pass_fn lambda below.
        const uint32_t frame_slot = renderer.current_frame();
        // draw_frame() below will wait on this same slot's fence as its own first statement --
        // doing it here too, before any of the per-slot writes that follow, closes a narrower
        // race than that later wait alone would: without this, camera_ubos_[frame_slot]->update()
        // (and instance_stream_/sdf_data_'s own uploads just below) would race frame_slot's PRIOR
        // occupant, frame N-2, rather than N-1 -- provably safe today only because
        // need_ssr_trace_inputs's device_.wait_idle() further down already drains through N-2
        // whenever SSR/transparency is on (see that call site), but not when both are off. Cheap:
        // Renderer::wait_for_current_frame() waits on an already-signaled fence in that case.
        renderer.wait_for_current_frame();
        camera_frame_ = frame_slot;
        camera_ubos_[frame_slot]->update(view, proj, cam_pos, pixel_density);

        // Gather renderables once; the instance upload, the shadow AABB fit, and
        // the draw loops below all need the same list (and world matrices) in
        // the same order.
        auto renderers = scene.get_components<MeshRenderer>();
        std::vector<glm::mat4> world_matrices(renderers.size(), glm::mat4(1.0f));
        std::vector<uint8_t>   renderer_valid(renderers.size(), 0);
        instance_stream_.begin(frame_slot);
        std::vector<uint32_t> instance_idx(renderers.size(), UINT32_MAX);

        // world_matrix() (a pure read -- see Transform's thread-safety doc) is safe from any
        // number of concurrent readers, unlike get_world_matrix(), because TransformSystem's
        // resolve pass (installed by Engine's ctor) has already recomputed every dirty
        // transform earlier this frame. Each index writes only its own slot, so this is a
        // clean parallel_for candidate; the merge below stays serial to preserve
        // instance_stream_.add()'s required monotonic-index order.
        auto gather_mesh = [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (!renderers[i]->is_ready() || !renderers[i]->owner) continue;
                auto* tc = renderers[i]->owner->get_transform();
                if (!tc) continue;
                world_matrices[i] = tc->transform().world_matrix();
                renderer_valid[i] = 1;
            }
        };
        if (should_parallelize_(renderers.size())) {
            jobs_->parallel_for_blocking(renderers.size(), 0 /* auto grain */, gather_mesh);
        } else {
            gather_mesh(0, renderers.size());
        }

        for (size_t i = 0; i < renderers.size(); ++i) {
            if (renderer_valid[i]) instance_idx[i] = instance_stream_.add(world_matrices[i]);
        }
        instance_stream_.upload();

        // --- SDF gather ---
        // Mirrors the MeshRenderer gather above: one pass over every SdfRenderer in the scene,
        // producing the SdfDrawItem list every record_*() site below shares (same role
        // renderers/world_matrices/instance_idx play for meshes). Gated on sdf_enabled alone --
        // when it's off, sdf_draws stays empty and every downstream site (all of which check
        // sdf_draws.empty() or iterate it) is naturally a no-op, so this is the ONE place that
        // needs to know about the toggle.
        std::vector<SdfDrawItem> sdf_draws;
        sdf_data_.begin(frame_slot);
        if (config_.sdf_enabled) {
            using coopa::gfx::engine::components::SdfRenderer;
            using coopa::gfx::engine::data::SdfShapeGPU;
            using coopa::gfx::engine::data::SdfRendererGPU;

            glm::mat4 view_proj = proj * view;
            auto sdf_renderer_comps = scene.get_components<SdfRenderer>();

            // Per-SdfRenderer scratch result: everything the loop body below used to compute
            // and push straight into sdf_data_/sdf_draws, minus the two SSBO writes
            // (sdf_data_.add_shape()/add_renderer()) -- those must stay serial (SdfData hands
            // out contiguous indices), so they move to the merge pass below. This is the
            // heaviest per-item work in the frame (an 8-corner AABB, a frustum cull, then per
            // shape a glm::inverse + 3 glm::length + a cbrt), and every SdfRenderer here is
            // independent of every other, so it's the best parallel_for candidate of the three.
            struct SdfGatherResult {
                bool visible = false;
                SdfRenderer* comp = nullptr;
                SdfRendererGPU rec{};
                std::vector<SdfShapeGPU> shapes;
                glm::vec3 world_min{0.0f}, world_max{0.0f};
                bool is_blend = false, cast_shadows = true;
                PixelRect px_rect{};
            };
            std::vector<SdfGatherResult> gather_results(sdf_renderer_comps.size());

            auto gather_sdf = [&](size_t begin, size_t end) {
                for (size_t ri = begin; ri < end; ++ri) {
                    SdfRenderer* sr = sdf_renderer_comps[ri];
                    SdfGatherResult& out = gather_results[ri];
                    if (!sr->owner) continue;
                    auto* tc = sr->owner->get_transform();
                    if (!tc) continue;
                    // world_matrix() (pure read, safe for concurrent readers once
                    // TransformSystem has resolved this frame) -- see the MeshRenderer
                    // gather above for the same reasoning.
                    glm::mat4 world = tc->transform().world_matrix();

                    // World AABB from the renderer's local bounds box.
                    glm::vec3 bmin_local = sr->bounds_center - sr->bounds_extent;
                    glm::vec3 bmax_local = sr->bounds_center + sr->bounds_extent;
                    glm::vec3 wmin(std::numeric_limits<float>::max());
                    glm::vec3 wmax(std::numeric_limits<float>::lowest());
                    for (int c = 0; c < 8; ++c) {
                        glm::vec3 corner((c & 1) ? bmax_local.x : bmin_local.x,
                                         (c & 2) ? bmax_local.y : bmin_local.y,
                                         (c & 4) ? bmax_local.z : bmin_local.z);
                        glm::vec3 wc = glm::vec3(world * glm::vec4(corner, 1.0f));
                        wmin = glm::min(wmin, wc);
                        wmax = glm::max(wmax, wc);
                    }

                    // Screen-space (pre-upscale) rectangle -- the whole performance story: only
                    // pixels inside it ever raymarch (see record_gbuffer_()/record_transparent_()'s
                    // cmd.set_scissor() calls). An empty/off-screen rect means free frustum culling.
                    SdfClipRect clip_rect = compute_sdf_clip_rect(view_proj, wmin, wmax);
                    if (!clip_rect.visible) continue;

                    // Compute every shape's GPU record here; first_shape/shape_count (which
                    // require sdf_data_'s shared, monotonic index allocator) are filled in by
                    // the serial merge below instead of here.
                    auto shapes = sr->collect_shapes();
                    out.shapes.reserve(shapes.size());
                    for (auto* shape : shapes) {
                        if (!shape->owner) continue;
                        auto* shape_tc = shape->owner->get_transform();
                        if (!shape_tc) continue;
                        glm::mat4 shape_world = shape_tc->transform().world_matrix();

                        // Approximate uniform scale (geometric mean of the 3 basis lengths) -- true
                        // SDFs don't support non-uniform scale exactly; this rescales the local-space
                        // distance back to world units well enough for the common case. See
                        // SdfShapeGPU::type_op_blend's doc.
                        float sx = glm::length(glm::vec3(shape_world[0]));
                        float sy = glm::length(glm::vec3(shape_world[1]));
                        float sz = glm::length(glm::vec3(shape_world[2]));
                        float uniform_scale = std::cbrt(std::max(sx * sy * sz, 1e-6f));

                        SdfShapeGPU gpu;
                        gpu.inv_world    = glm::inverse(shape_world);
                        gpu.params_round = glm::vec4(shape->params, shape->rounding);
                        float blend = shape->blend > 0.0f ? shape->blend : sr->smoothing;
                        gpu.type_op_blend = glm::vec4(static_cast<float>(static_cast<int>(shape->type)),
                                                      static_cast<float>(static_cast<int>(shape->op)),
                                                      blend, uniform_scale);
                        out.shapes.push_back(gpu);
                    }
                    if (out.shapes.empty()) continue; // no shapes collected -- nothing to draw

                    out.rec.clip_rect    = glm::vec4(clip_rect.ndc_min, clip_rect.ndc_max);
                    out.rec.bounds_min   = glm::vec4(wmin, 0.0f);
                    out.rec.bounds_max   = glm::vec4(wmax, 0.0f);
                    out.rec.albedo_alpha = glm::vec4(sr->material.albedo, sr->material.alpha);
                    out.rec.mr_ao_cutoff = glm::vec4(sr->material.metallic, sr->material.roughness,
                                                 sr->material.ao, sr->material.gpu_alpha_cutoff());
                    out.rec.emissive     = sr->material.gpu_emissive();
                    uint32_t clamped_steps = static_cast<uint32_t>(
                        std::clamp(sr->max_steps, 1, static_cast<int>(config_.sdf_max_steps)));
                    out.rec.range = glm::uvec4(0, static_cast<uint32_t>(out.shapes.size()), clamped_steps, 0);
                    out.rec.march = glm::vec4(sr->surface_epsilon, sr->normal_epsilon, 0.0f, 0.0f);

                    out.comp         = sr;
                    out.is_blend     = sr->material.is_blended();
                    out.cast_shadows = sr->cast_shadows && config_.sdf_shadows_enabled;
                    out.world_min    = wmin;
                    out.world_max    = wmax;
                    out.px_rect      = sdf_clip_rect_to_pixels(clip_rect.ndc_min, clip_rect.ndc_max,
                                                                render_extent_.width, render_extent_.height);
                    out.visible = true;
                }
            };
            if (should_parallelize_(sdf_renderer_comps.size())) {
                jobs_->parallel_for_blocking(sdf_renderer_comps.size(), 0 /* auto grain */, gather_sdf);
            } else {
                gather_sdf(0, sdf_renderer_comps.size());
            }

            // Serial merge, in gather order: sdf_data_.add_shape()/add_renderer() hand out
            // contiguous SSBO indices and must not race, so this part stays a plain loop.
            for (auto& out : gather_results) {
                if (!out.visible) continue;

                uint32_t first_shape = 0;
                for (size_t s = 0; s < out.shapes.size(); ++s) {
                    uint32_t idx = sdf_data_.add_shape(out.shapes[s]);
                    if (s == 0) first_shape = idx;
                }
                out.rec.range.x = first_shape;

                SdfDrawItem item;
                item.comp         = out.comp;
                item.gpu_index    = sdf_data_.add_renderer(out.rec);
                item.is_blend     = out.is_blend;
                item.cast_shadows = out.cast_shadows;
                item.world_min    = out.world_min;
                item.world_max    = out.world_max;
                item.world_center = 0.5f * (out.world_min + out.world_max);
                item.px_rect      = out.px_rect;
                sdf_draws.push_back(item);
            }

            // Per-frame globals: camera ray reconstruction + the forward pass's lighting/
            // indirect/SSR tuning, sourced from the SAME config_ fields fill_forward_globals_()
            // reads for the mesh path's own ForwardGlobals UBO (see sdf_forward.frag's doc for
            // the exact field-order mapping).
            auto& g = sdf_data_.globals();
            g.inv_view_proj = glm::inverse(view_proj);
            g.camera_pos    = glm::vec4(cam_pos, 1.0f);
            g.lighting0 = glm::vec4(config_.light_bands, config_.spec_threshold,
                                    config_.soft_lighting ? 1.0f : 0.0f, config_.rim_strength);
            g.lighting1 = glm::vec4(config_.indirect.ambient_intensity, config_.indirect.sky_intensity,
                                    config_.ssr_enabled ? 1.0f : 0.0f, config_.indirect.ssgi_intensity);
            g.ssr0 = glm::vec4(config_.indirect.ssgi_distance, config_.ssr_max_distance,
                              config_.ssr_bias_texels, config_.ssr_thickness);
            g.ssr1 = glm::vec4(config_.ssr_thickness_scale, config_.ssr_roughness_cutoff, 0.0f, 0.0f);
            g.ssr_steps = glm::ivec4(config_.ssr_max_iterations,
                                     static_cast<int>(hiz_pass_->max_mip_level()),
                                     config_.ssr_start_mip, config_.ssr_min_mip0_steps);
            g.ssr_mip = glm::ivec4(static_cast<int>(scene_color_mip_pass_->max_mip_level()), 0, 0, 0);

            sdf_data_.upload();
        }

        auto* dir_light = scene.find_first_component<DirectionalLightComponent>();
        bool cast_dir_shadow = config_.shadows_enabled && dir_light && dir_light->cast_shadows;
        if (dir_light) {
            update_dir_shadow_matrix_(dir_light->direction, renderers, world_matrices, sdf_draws, cam, cast_dir_shadow);
        }

        // Only the scene's first shadow-casting point light gets a real cube map
        // (see the class doc for why). Marks light_data_.point_lights[0]'s
        // cast_shadows flag so pixel_lighting.frag knows to sample it.
        auto* shadow_point = find_first_shadow_casting_point_light_(scene);
        bool cast_point_shadow = config_.shadows_enabled && shadow_point != nullptr;
        if (cast_point_shadow) {
            auto& gpu0 = light_data_.data().point_lights[0];
            gpu0.attenuation.w = 1.0f;
        }
        light_data_.upload();

        // Fog UBO. Gated on fog_enabled -- when fog is off, record() skips fog_pass_'s draw
        // entirely (see that call site), so this upload would otherwise be wasted work.
        // Mirrors blendy's PbrRenderPipeline::update_scene_data_() fog fill (see
        // blendy/src/blendy/render/pbr_render_pipeline.h) -- gather via
        // scene.get_components<FogVolumeComponent>() instead of a cached traversal, matching
        // how this pipeline already gathers PointLightComponent/MeshRenderer above.
        if (config_.fog_enabled) {
            using coopa::gfx::engine::components::FogVolumeComponent;
            using coopa::gfx::engine::components::FogVolumeShape;

            auto& fog = fog_data_.data();
            // CAVEAT: fog_data_ is single-buffered (FogData owns exactly one UBO, unlike
            // camera_ubos_/sdf_data_ above), so this inv_view_proj is the same hazard class that
            // motivated per-slot camera_ubos_ -- a stale one-frame-old camera matrix under fast
            // motion. Currently unexercised: assets/config.yaml ships fog_enabled: false.
            // Documented rather than fixed here because FogPass owns its own descriptor set
            // (bound once at construction in gfxcoopa's fog_pass.h), so making it per-slot needs
            // an additive gfxcoopa API change for a feature no shipped scene turns on.
            fog.inv_view_proj = glm::inverse(proj * view);
            fog.camera_pos    = glm::vec4(cam_pos, 1.0f);
            fog.fog_color     = glm::vec4(config_.fog_color, 1.0f);
            if (dir_light) {
                fog.sun_direction = glm::vec4(glm::normalize(dir_light->direction), 0.0f);
                fog.sun_color     = glm::vec4(dir_light->color * dir_light->intensity, 1.0f);
            } else {
                fog.sun_direction = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
                fog.sun_color     = glm::vec4(0.0f);
            }
            fog.mode_density  = glm::vec4(static_cast<float>(config_.fog_mode), config_.fog_density,
                                          config_.fog_linear_start, config_.fog_linear_end);
            fog.height_params = glm::vec4(config_.fog_height_base, config_.fog_height_falloff,
                                          config_.fog_sky_blend, config_.fog_sun_amount);

            auto fog_volumes = scene.get_components<FogVolumeComponent>();
            uint32_t volume_count = static_cast<uint32_t>(
                std::min<size_t>(fog_volumes.size(), coopa::gfx::engine::data::MAX_FOG_VOLUMES));
            fog.misc_params = glm::vec4(config_.fog_sun_anisotropy, config_.fog_max_opacity,
                                        static_cast<float>(volume_count), config_.fog_max_distance);

            for (uint32_t i = 0; i < volume_count; ++i) {
                auto* fv = fog_volumes[i];
                auto& gpu = fog.volumes[i];
                glm::mat4 world = (fv->owner && fv->owner->get_transform())
                    ? fv->owner->get_transform()->get_world_matrix() : glm::mat4(1.0f);
                gpu.inv_world     = glm::inverse(world);
                gpu.extent_shape  = glm::vec4(fv->extent, fv->shape == FogVolumeShape::Sphere ? 1.0f : 0.0f);
                gpu.color_density = glm::vec4(fv->color, fv->density);
                gpu.falloff       = glm::vec4(fv->falloff, 0.0f, 0.0f, 0.0f);
            }

            fog_data_.upload();
        }

        LetterboxRect letterbox = compute_letterbox(
            swapchain_.extent().width, swapchain_.extent().height,
            render_extent_.width, render_extent_.height);

        // transparent.frag traces the SAME Hi-Z pyramid / prefiltered scene-colour chain
        // ssr.frag does (see record_transparent_()'s ExtraSets) -- so whenever BLEND geometry
        // might draw this frame, those images must be generated (and correctly laid out) even
        // if config_.ssr_enabled is false for the OPAQUE path. Conservative on
        // config_.transparency_enabled alone (not "does the scene actually have a BLEND
        // renderer this frame") to keep this a frame-independent, branch-once decision, the
        // same coarseness every other toggle here uses. ssr_reflect_transparent folded in too
        // (meaningless without the other two already true, but defensive): its own
        // transparent_hiz_pass_/transparent_scene_color_mip_pass_ rebind their descriptors
        // every execute() call exactly like hiz_pass_/scene_color_mip_pass_ do, needing the
        // same per-frame wait_idle() protection below. bloom_enabled is deliberately NOT
        // folded in here: bloom_pass_ is an independent pyramid with its own
        // construction-fixed descriptors (see its own doc), so it needs neither
        // scene_color_mip_pass_'s chain nor the wait_idle() this flag exists to pay for.
        bool need_ssr_trace_inputs = config_.ssr_enabled || config_.transparency_enabled
                                    || config_.ssr_reflect_transparent;

        if (need_ssr_trace_inputs) {
            // HiZPass::execute() and SceneColorMipPass::execute() (reused unmodified from
            // gfxcoopa -- see the class doc) each rebind their own mip-0 descriptor set via
            // update_descriptors() on every call, unconditionally. That is safe under blendy's
            // PbrRenderPipeline, which does a full device_.wait_idle() every frame, but this
            // pipeline otherwise overlaps MAX_FRAMES_IN_FLIGHT command buffers instead.
            // Recording frame N's rebind while frame N-1's submission is still executing on the
            // GPU is a vkUpdateDescriptorSets-on-a-pending-descriptor-set validation error (VUID
            // -vkUpdateDescriptorSets-None-03047), reliably hit within the first few frames of
            // sustained rendering with SSR (or transparency, which now shares these two passes'
            // output) on. Rather than fork two more sizeable gfxcoopa passes just to remove
            // their internal rebind, need_ssr_trace_inputs pays for a full wait here -- it is
            // already the heaviest toggle, and this exactly matches the synchronization model
            // these passes were designed under.
            device_.wait_idle();
        }

        bool frame_presented = renderer.begin_frame(
            [&](coopa::gfx::command::CommandBuffer& cmd) {
                upscale_pass_->draw(cmd, letterbox);
            },
            VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}},
            nullptr,
            [&](coopa::gfx::command::CommandBuffer& cmd) {
                record_directional_shadow_(cmd, renderers, instance_idx, sdf_draws, cast_dir_shadow);
                record_point_shadow_(cmd, renderers, instance_idx, sdf_draws, shadow_point, cast_point_shadow);
                record_gbuffer_(cmd, renderers, instance_idx, sdf_draws);

                if (need_ssr_trace_inputs) {
                    hiz_pass_->execute(cmd, gbuffer_target_.depth_image_handle(), gbuffer_target_.depth_view_typed());
                } else {
                    // GBufferTarget's render pass leaves the depth attachment in
                    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL (only G0/G1/G2 color end in
                    // SHADER_READ_ONLY_OPTIMAL -- see gbuffer_target.h), but
                    // pixel_stylize.frag samples it for the outline edge detector. When Hi-Z is
                    // generated this frame, HiZPass::execute() above performs this same
                    // transition as a side effect instead -- calling both would present a stale
                    // oldLayout on the second. Not transitioned back afterward: GBufferTarget's
                    // render pass declares DEPTH initialLayout = UNDEFINED, so next frame's
                    // begin() doesn't care what layout this is left in (see pipeline/render_pass.h).
                    transition_gbuffer_depth_to_shader_read_(cmd);
                }

                // ssr_reflect_transparent: capture transparent geometry's own depth/normal/
                // position/shaded-color and build its Hi-Z pyramid + scene-colour mip chain,
                // so ssr_pass_'s raymarch below can trace a SECOND source and let opaque
                // surfaces reflect transparent ones too (see record_transparent_capture_()'s
                // own doc). Placed here -- right after the opaque Hi-Z block, before SSAO/
                // lighting -- because this capture only needs shadows (already recorded
                // above) and the light UBO, nothing from the opaque G-buffer or lighting pass,
                // and it must finish before ssr_pass_->execute() later in this frame.
                if (config_.ssr_reflect_transparent) {
                    record_transparent_capture_(cmd, renderers, instance_idx, sdf_draws);
                    // TransparentCaptureTarget is out of scope for the Vulkan-sealing refactor
                    // (MRT, no sealed equivalent -- see gfxcoopa's plan), so its raw VkImageView
                    // accessors are wrapped here via detail::wrap() rather than gaining
                    // TextureView-returning siblings themselves.
                    transparent_hiz_pass_->execute(cmd, transparent_capture_target_.depth_image_handle(),
                                                   coopa::gfx::detail::wrap(transparent_capture_target_.depth_view()));
                    transparent_scene_color_mip_pass_->execute(
                        cmd, coopa::gfx::detail::wrap(transparent_capture_target_.shaded_color_view()));

                    // Rebind ssr_pass_'s secondary source (sets 4-6) to these NOW-populated,
                    // correctly-laid-out images -- deliberately NOT done once at construction
                    // (see that call site's own comment for why). Safe to call here, mid-
                    // recording, as a plain host-side vkUpdateDescriptorSets: this whole block
                    // only runs when need_ssr_trace_inputs is true, which already paid for a
                    // device_.wait_idle() before renderer.begin_frame() above -- the exact same
                    // protection hiz_pass_->execute()/scene_color_mip_pass_->execute()'s own
                    // internal per-frame descriptor rebinds already rely on.
                    ssr_pass_->set_secondary_source(
                        coopa::gfx::detail::wrap(transparent_capture_target_.normal_metallic_view()),
                        coopa::gfx::detail::wrap(transparent_capture_target_.position_roughness_view()),
                        transparent_hiz_pass_->full_hiz_view_typed(), transparent_hiz_pass_->sampler(),
                        transparent_scene_color_mip_pass_->full_view_typed(), transparent_scene_color_mip_pass_->sampler());
                }

                if (config_.ssao_enabled) {
                    coopa::gfx::engine::passes::SsaoPass::Params ssao_params{};
                    ssao_params.radius               = config_.ssao_radius;
                    ssao_params.bias                 = config_.ssao_bias;
                    ssao_params.power                = config_.ssao_power;
                    ssao_params.kernel_size          = config_.ssao_kernel_size;
                    ssao_params.noise_scale_x        = static_cast<float>(render_extent_.width)  / 4.0f;
                    ssao_params.noise_scale_y        = static_cast<float>(render_extent_.height) / 4.0f;
                    ssao_params.noise_rotation        = static_cast<int>(frame_index_ & 0x7u);
                    ssao_params.temporal_enabled     = config_.ssao_temporal_enabled;
                    ssao_params.temporal_blend       = config_.ssao_temporal_blend;
                    ssao_params.prev_view_proj       = prev_view_proj_;
                    ssao_params.prev_view_proj_valid = prev_view_proj_valid_;
                    ssao_pass_->execute(cmd, current_camera_set(), ssao_params);
                } else {
                    ssao_pass_->invalidate_history();
                }
                // Note: the SSAO image bound into pixel_lighting_pass_/ssr_pass_ is decided
                // once at construction (config_.ssao_enabled doesn't change at runtime), not
                // here -- see the constructor's comment on why a per-frame rebind would violate
                // this pipeline's frame-overlap model.

                // Lighting and skybox draw into the SAME open render pass instance:
                // pipeline::RenderPass always uses LOAD_OP_CLEAR on its color
                // attachment (gfxcoopa/pipeline/render_pass.h), so a target can never
                // be re-opened later to composite onto -- the skybox pass's discard
                // of already-lit pixels only works because it runs as a second draw
                // inside offscreen_target_'s single begin()/end(), not a separate pass.
                offscreen_target_.begin(cmd);

                PixelLightingPushConstants lighting_pc;
                lighting_pc.light_bands       = config_.light_bands;
                lighting_pc.spec_threshold    = config_.spec_threshold;
                lighting_pc.rim_strength      = config_.rim_strength;
                lighting_pc.ambient_intensity = config_.indirect.ambient_intensity;
                lighting_pc.sky_intensity     = config_.indirect.sky_intensity;
                lighting_pc.soft_lighting     = config_.soft_lighting ? 1.0f : 0.0f;
                // gfxcoopa's DeferredLightingPass::draw() pushes this internally now (the
                // templated overload), after its own bind_pipeline() -- no separate push needed.
                pixel_lighting_pass_->draw(cmd, current_camera_set(), *light_set_, *shadow_set_, lighting_pc,
                                          render_extent_.width, render_extent_.height);

                skybox_pass_->draw(cmd, current_camera_set(), view, proj, render_extent_.width, render_extent_.height);

                offscreen_target_.end(cmd);

                if (need_ssr_trace_inputs) {
                    // Prefiltered scene-colour mip chain: also feeds transparent.frag's
                    // gfx_ssr_trace() cone-LOD taps and SSGI bounce lookup, not just ssr.frag's
                    // own -- see need_ssr_trace_inputs' own doc.
                    scene_color_mip_pass_->execute(cmd, offscreen_target_.color_view_typed());
                }

                if (config_.ssr_enabled) {
                    coopa::gfx::engine::passes::SsrPass::Params ssr_params{};
                    ssr_params.proj              = proj;
                    ssr_params.max_iterations    = config_.ssr_max_iterations;
                    ssr_params.thickness_min     = config_.ssr_thickness;
                    ssr_params.thickness_scale   = config_.ssr_thickness_scale;
                    ssr_params.max_distance      = config_.ssr_max_distance;
                    ssr_params.bias_texels       = config_.ssr_bias_texels;
                    ssr_params.roughness_cutoff  = config_.ssr_roughness_cutoff;
                    ssr_params.max_hiz_mip       = static_cast<int>(hiz_pass_->max_mip_level());
                    ssr_params.start_mip         = config_.ssr_start_mip;
                    ssr_params.min_mip0_steps    = config_.ssr_min_mip0_steps;
                    ssr_params.max_color_mip     = static_cast<int>(scene_color_mip_pass_->max_mip_level());
                    ssr_params.temporal_enabled  = config_.ssr_temporal_enabled;
                    ssr_params.temporal_blend    = config_.ssr_temporal_blend;
                    ssr_params.temporal_gamma    = config_.ssr_temporal_gamma;
                    ssr_params.ssr_blur_radius   = config_.ssr_blur_radius;
                    ssr_params.jitter_strength   = config_.ssr_jitter;
                    // Longer period than SSAO's noise_rotation (& 0x7) -- matches ssr_ign2()'s
                    // own `frame & 0xFF` mask in gfx/ssr_common.glsl.
                    ssr_params.frame_index       = static_cast<int>(frame_index_ & 0xFFu);
                    // Fed from the SAME config_.indirect instance as lighting_pc above -- see
                    // IndirectParams' doc (render_features.h) for why this must stay one source.
                    ssr_params.sky_intensity     = config_.indirect.sky_intensity;
                    ssr_params.ssgi_intensity    = config_.indirect.ssgi_intensity;
                    ssr_params.ssgi_distance     = config_.indirect.ssgi_distance;
                    ssr_params.prev_view_proj       = prev_view_proj_;
                    ssr_params.prev_view_proj_valid = prev_view_proj_valid_;

                    // Secondary source -- see SsrPushConstants' own doc. max_hiz_mip_b/
                    // max_color_mip_b come from transparent_hiz_pass_/transparent_scene_
                    // color_mip_pass_'s OWN mip counts, not the primary pyramid's (they're
                    // independent instances, possibly over a differently-sized image chain).
                    ssr_params.has_secondary   = config_.ssr_reflect_transparent;
                    ssr_params.max_hiz_mip_b   = static_cast<int>(transparent_hiz_pass_->max_mip_level());
                    ssr_params.max_color_mip_b = static_cast<int>(transparent_scene_color_mip_pass_->max_mip_level());

                    ssr_pass_->execute(cmd, current_camera_set(), ssr_params);
                }

                if (config_.transparency_enabled && config_.refraction_enabled) {
                    // Builds refraction_scene_color_mip_pass_'s chain -- the dedicated,
                    // independent instance transparent.frag's u_scene_color reads instead of
                    // ssr_pass_'s own scene_color_set() whenever refraction is active (see that
                    // member's own doc, and the transparent_extra bind lambda above, for why a
                    // SEPARATE instance is required rather than a second execute() on
                    // scene_color_mip_pass_ itself: that pass rebinds its own internal per-mip
                    // descriptor sets on every execute(), and doing so twice in one frame's
                    // not-yet-submitted command buffer corrupts it). MESH-only: BLEND
                    // SdfRenderers keep reading the original scene_color_mip_pass_ chain
                    // (pre-SSR) via ssr_pass_->scene_color_set(), unchanged (see the refraction
                    // plan for why SDF glass is excluded from refraction entirely).
                    coopa::gfx::TextureView refraction_source =
                        (config_.refraction_include_reflections && config_.ssr_enabled)
                            ? ssr_pass_->output_view_typed() : offscreen_target_.color_view_typed();
                    refraction_scene_color_mip_pass_->execute(cmd, refraction_source);
                }

                if (config_.transparency_enabled) {
                    // Per-frame globals for the forward MESH pass's lighting/indirect/SSR/
                    // refraction tuning -- sourced from the SAME config_ fields sdf_data_'s own
                    // globals() fill above uses, in particular config_.indirect (shared with
                    // SsrPass::Params so opaque and transparent indirect terms can never
                    // disagree -- see IndirectParams' own doc). Uploaded to frame_slot's UBO
                    // slot, then bound once per mesh-kind transition in record_transparent_() --
                    // see transparent.frag's set 6.
                    forward_globals_.begin(frame_slot);
                    fill_forward_globals_(forward_globals_.globals());
                    forward_globals_.upload();

                    // Depth is always SHADER_READ_ONLY_OPTIMAL by this point -- both branches
                    // above (HiZPass::execute() when need_ssr_trace_inputs, transition_gbuffer_
                    // depth_to_shader_read_() otherwise) leave it there; see that if/else's own
                    // comment. transparency_enabled implies need_ssr_trace_inputs, so
                    // HiZPass::execute() is always the branch taken here.
                    VkImageView hdr_source_view = config_.ssr_enabled
                        ? ssr_pass_->output_view() : offscreen_target_.color_view();
                    bool transparent_ran = record_transparent_(cmd, renderers, world_matrices, instance_idx,
                                        sdf_draws, hdr_source_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                        cam_pos);
                    // TransparentPass::begin()/end() bracket its own render pass, whose depth
                    // attachment declares DEPTH_STENCIL_READ_ONLY_OPTIMAL as BOTH initial and
                    // final layout (transparent_pass.h's create_render_pass_()) -- so the depth
                    // image comes out of record_transparent_() in that layout, not the
                    // SHADER_READ_ONLY_OPTIMAL pixel_stylize_pass_'s pre-bound descriptor expects
                    // for outline edge detection (its source images are bound once at
                    // construction, not rebound per frame -- see that call site's own comment).
                    // record_transparent_() early-returns without touching depth at all when
                    // there are no BLEND-material renderers this frame (transparency_enabled
                    // being true doesn't guarantee the scene has any), in which case depth is
                    // still sitting in SHADER_READ_ONLY_OPTIMAL from the branch above and this
                    // transition must be skipped -- issuing it anyway asserts a false oldLayout
                    // and trips synchronization validation.
                    if (transparent_ran) {
                        transition_gbuffer_depth_to_shader_read_(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
                    }
                }

                // Fog composite. After the transparent pass (so BLEND geometry is fogged too --
                // it was drawn in place into the same underlying image pre_fog_view_typed_
                // names, above) and before pixel_stylize_pass_ (so fog sits in linear HDR,
                // ahead of tonemap/outline/dither/palette). fog_pass_ always reads from the
                // fixed pre_fog_view_typed_ chosen at construction (see that call site's
                // comment); pixel_stylize_pass_'s own source was likewise fixed at construction
                // to fog_target_ whenever config_.fog_enabled was true then, so this draw is
                // gated the same way -- flipping the config flag without a pipeline rebuild
                // would leave post-process reading a stale target, exactly like the ssr_enabled
                // caveat those same comments already document.
                if (config_.fog_enabled) {
                    fog_target_.begin(cmd);
                    fog_pass_->draw(cmd, render_extent_.width, render_extent_.height);
                    fog_target_.end(cmd);
                }

                // Bloom pyramid. After the fog branch (so fog and everything before it
                // bloom too) and before post_target_, whose pixel_stylize_pass_ draw below
                // composites bloom_pass_'s finished result. Gated on the same startup-fixed
                // flag its descriptor binding was decided from at construction.
                if (config_.bloom_enabled) {
                    coopa::gfx::engine::passes::BloomPass::Params bloom_params{};
                    bloom_params.threshold = config_.bloom_threshold;
                    bloom_params.soft_knee = config_.bloom_soft_knee;
                    bloom_params.clamp_max = config_.bloom_clamp;
                    bloom_params.radius    = config_.bloom_radius;
                    bloom_params.scatter   = config_.bloom_scatter;
                    bloom_pass_->execute(cmd, bloom_params);
                }

                post_target_.begin(cmd);
                coopa::gfx::engine::passes::PixelStylizePass::PushConstants post_pc;
                post_pc.outline_color    = config_.outline_color;
                post_pc.inv_render_size  = glm::vec2(1.0f / render_extent_.width, 1.0f / render_extent_.height);
                post_pc.outline_thickness = config_.outline_enabled ? config_.outline_thickness : 0.0f;
                post_pc.depth_threshold  = config_.depth_threshold;
                post_pc.normal_threshold = config_.normal_threshold;
                post_pc.dither_strength  = config_.dither_enabled ? config_.dither_strength : 0.0f;
                post_pc.palette_count    = config_.palette_enabled ? static_cast<float>(palette_lut_.count()) : 0.0f;
                post_pc.camera_near           = cam ? cam->clip_start : 0.1f;
                post_pc.camera_far            = cam ? cam->clip_end : 1000.0f;
                post_pc.camera_is_perspective = (!cam || cam->type == CameraType::Perspective) ? 1.0f : 0.0f;
                // This pipeline has no separate tonemap pass, unlike blendy -- exposure > 0
                // keeps pixel_stylize.frag's tonemap step live (see PushConstants::exposure's doc).
                post_pc.exposure         = config_.exposure;
                post_pc.bloom_intensity  = config_.bloom_enabled ? config_.bloom_intensity : 0.0f;
                pixel_stylize_pass_->draw(cmd, post_pc, render_extent_.width, render_extent_.height);
                post_target_.end(cmd);

                // Diorama tilt-shift blur, after every other post effect and at DISPLAY
                // resolution -- see gfxcoopa's TiltShiftPass file doc for why it belongs
                // here rather than inside post_target_. Gated on the same startup-fixed
                // flag upscale_pass_'s source binding was decided from at construction.
                if (config_.tilt_shift_enabled) {
                    coopa::gfx::engine::passes::TiltShiftPass::Params ts_params{};
                    ts_params.focus_center  = config_.tilt_shift_focus_center;
                    ts_params.focus_width   = config_.tilt_shift_focus_width;
                    ts_params.ramp_width    = config_.tilt_shift_ramp_width;
                    ts_params.max_radius    = config_.tilt_shift_max_radius;
                    ts_params.blur_top      = config_.tilt_shift_blur_top;
                    ts_params.blur_bottom   = config_.tilt_shift_blur_bottom;
                    ts_params.angle_degrees = config_.tilt_shift_angle;
                    tilt_shift_pass_->execute(cmd, ts_params);
                }
            }
        );

        // Needed by both SSAO's and SSR's temporal resolve passes -- kept unconditional (not
        // gated on either toggle) since ssao_pass_ always exists and either toggle can be
        // re-enabled without a resize/reconstruct in between.
        prev_view_proj_       = proj * view;
        prev_view_proj_valid_ = true;
        ++frame_index_;

        return frame_presented;
    }

private:

    /**
     * @brief True when `n` items justify job dispatch over a plain serial loop.
     *
     * Keeps the guard identical at every gather site (MeshRenderer gather, SDF gather,
     * directional shadow AABB fit). jobs_ is nullptr unless the owning Engine calls
     * set_job_engine(), so a pipeline used standalone (e.g. in a test) stays fully serial
     * with no behavior change. parallel_threshold_ defaults to 256, matching
     * coopa::anim::AnimationSystem's own house value; toyengine's Engine overrides it from
     * AppConfig::jobs.parallel_threshold (see JobsConfig's doc for why that default is low).
     */
    bool should_parallelize_(std::size_t n) const { return jobs_ && n >= parallel_threshold_; }

    /** @brief Camera descriptor set for the slot render() most recently selected -- bound once
     *  at construction, never rewritten. Mirrors sdf_data_.current_set(). */
    const coopa::gfx::pipeline::DescriptorSet& current_camera_set() const {
        return *camera_sets_[camera_frame_];
    }

    /**
     * @brief Populates LightUBO color/intensity/count fields (not the shadow
     *        matrix or cast_shadows flags -- see update_dir_shadow_matrix_ and
     *        render()'s point-light handling, both of which run after this
     *        and share the single light_data_.upload() at the end of render()).
     */
    void update_lights_(coopa::scene::Scene& scene) {
        using coopa::gfx::engine::components::DirectionalLightComponent;
        using coopa::gfx::engine::components::PointLightComponent;

        auto& ubo = light_data_.data();

        auto* dir = scene.find_first_component<DirectionalLightComponent>();
        if (dir) {
            ubo.dir_direction = glm::vec4(dir->direction, dir->intensity);
            ubo.dir_color     = glm::vec4(dir->color, 0.0f);
            ubo.dir_ambient   = glm::vec4(dir->ambient, 0.0f);
            ubo.light_counts.x = 1;
        } else {
            ubo.light_counts.x = 0;
        }

        auto points = scene.get_components<PointLightComponent>();
        uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(points.size()), 16);
        ubo.light_counts.y = count;
        for (uint32_t i = 0; i < count; ++i) {
            auto* pl = points[i];
            auto& gpu = ubo.point_lights[i];
            gpu.position_range  = glm::vec4(pl->get_world_position(), pl->range);
            gpu.color_intensity = glm::vec4(pl->color, pl->intensity);
            gpu.attenuation     = glm::vec4(pl->attenuation_constant, pl->attenuation_linear,
                                            pl->attenuation_quadratic, 0.0f); // cast_shadows set below, light 0 only
        }
    }

    /**
     * @brief Fits an orthographic light-space box to the AABB of every ready
     *        shadow caster, with texel-snapped centering to eliminate sub-texel
     *        shadow crawl -- ported from blendy's PbrRenderPipeline::update_scene_data_
     *        (see blendy/src/blendy/render/pbr_render_pipeline.h, "Directional
     *        shadow-map projection" section). Writes light_data_.data()'s
     *        dir_light_space_matrix and dir_shadow_params (upload() happens once
     *        in render() after this and the point-light shadow flag are both set).
     */
    void update_dir_shadow_matrix_(const glm::vec3& direction,
                                   const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                                   const std::vector<glm::mat4>& world_matrices,
                                   const std::vector<SdfDrawItem>& sdf_draws,
                                   const coopa::gfx::engine::components::CameraComponent* cam,
                                   bool cast_dir_shadow) {
        glm::vec3 light_dir = glm::normalize(direction);
        glm::vec3 up = (std::abs(light_dir.z) < 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::mat4 light_rot = glm::lookAt(glm::vec3(0.0f), light_dir, up);

        glm::vec3 aabb_min(std::numeric_limits<float>::max());
        glm::vec3 aabb_max(std::numeric_limits<float>::lowest());
        bool any_caster = false;

        // Pure min/max reduction, no shared mutable state -- float min/max is associative, so
        // splitting the range across workers and folding each worker's own partial result back
        // in afterwards (in a fixed, worker-index order every run) is bit-identical to the
        // plain serial fold. One accumulator slot per worker (indexed by JobContext::worker_index,
        // which is the thread ACTUALLY running a given chunk -- safe even under work-stealing,
        // since a worker only ever executes one job body at a time).
        struct AabbAccum {
            glm::vec3 mn{std::numeric_limits<float>::max()};
            glm::vec3 mx{std::numeric_limits<float>::lowest()};
            bool any = false;
        };
        size_t worker_slots = jobs_ ? std::max<size_t>(1, jobs_->worker_count()) : 1;

        std::vector<AabbAccum> mesh_accum(worker_slots);
        auto fit_mesh = [&](size_t begin, size_t end, const coopa::job::JobContext& ctx) {
            size_t widx = (ctx.worker_index < worker_slots) ? ctx.worker_index : 0;
            AabbAccum& acc = mesh_accum[widx];
            for (size_t i = begin; i < end; ++i) {
                if (!renderers[i]->is_ready()) continue;
                const auto* mesh = renderers[i]->get_mesh().get();
                if (!mesh) continue;
                acc.any = true;

                const glm::vec3& bmin = mesh->bounds_min();
                const glm::vec3& bmax = mesh->bounds_max();
                for (int c = 0; c < 8; ++c) {
                    glm::vec3 corner((c & 1) ? bmax.x : bmin.x, (c & 2) ? bmax.y : bmin.y, (c & 4) ? bmax.z : bmin.z);
                    glm::vec3 ls = glm::vec3(light_rot * (world_matrices[i] * glm::vec4(corner, 1.0f)));
                    acc.mn = glm::min(acc.mn, ls);
                    acc.mx = glm::max(acc.mx, ls);
                }
            }
        };
        if (should_parallelize_(renderers.size())) {
            jobs_->parallel_for_blocking(renderers.size(), 0 /* auto grain */, fit_mesh);
        } else {
            fit_mesh(0, renderers.size(), coopa::job::JobContext{});
        }
        for (const auto& acc : mesh_accum) {
            if (!acc.any) continue;
            any_caster = true;
            aabb_min = glm::min(aabb_min, acc.mn);
            aabb_max = glm::max(aabb_max, acc.mx);
        }

        // Fold in every shadow-casting SdfRenderer's world AABB too, same BLEND-at-full-opacity
        // rule record_directional_shadow_()/record_point_shadow_() apply to their own draws --
        // an SDF that won't actually cast a shadow this frame shouldn't influence the fitted box.
        // Same reduction shape as the mesh loop above.
        std::vector<AabbAccum> sdf_accum(worker_slots);
        auto fit_sdf = [&](size_t begin, size_t end, const coopa::job::JobContext& ctx) {
            size_t widx = (ctx.worker_index < worker_slots) ? ctx.worker_index : 0;
            AabbAccum& acc = sdf_accum[widx];
            for (size_t i = begin; i < end; ++i) {
                const auto& d = sdf_draws[i];
                if (!d.cast_shadows) continue;
                if (d.is_blend && d.comp->material.alpha < 1.0f) continue;
                acc.any = true;
                for (int c = 0; c < 8; ++c) {
                    glm::vec3 corner((c & 1) ? d.world_max.x : d.world_min.x,
                                     (c & 2) ? d.world_max.y : d.world_min.y,
                                     (c & 4) ? d.world_max.z : d.world_min.z);
                    glm::vec3 ls = glm::vec3(light_rot * glm::vec4(corner, 1.0f));
                    acc.mn = glm::min(acc.mn, ls);
                    acc.mx = glm::max(acc.mx, ls);
                }
            }
        };
        if (should_parallelize_(sdf_draws.size())) {
            jobs_->parallel_for_blocking(sdf_draws.size(), 0 /* auto grain */, fit_sdf);
        } else {
            fit_sdf(0, sdf_draws.size(), coopa::job::JobContext{});
        }
        for (const auto& acc : sdf_accum) {
            if (!acc.any) continue;
            any_caster = true;
            aabb_min = glm::min(aabb_min, acc.mn);
            aabb_max = glm::max(aabb_max, acc.mx);
        }

        glm::mat4 light_proj;
        if (any_caster) {
            const float pad = 1.0f;
            float half_w = 0.5f * (aabb_max.x - aabb_min.x) + pad;
            float half_h = 0.5f * (aabb_max.y - aabb_min.y) + pad;
            float extent = std::max(half_w, half_h);
            const float extent_step = 0.5f;
            extent = std::ceil(extent / extent_step) * extent_step;
            if (config_.shadow_max_extent > 0.0f) extent = std::min(extent, config_.shadow_max_extent);

            glm::vec2 center(0.5f * (aabb_min.x + aabb_max.x), 0.5f * (aabb_min.y + aabb_max.y));
            float texel = (2.0f * extent) / static_cast<float>(config_.shadow_map_resolution);
            center.x = std::floor(center.x / texel) * texel;
            center.y = std::floor(center.y / texel) * texel;

            float near_plane = -aabb_max.z - pad;
            float far_plane  = -aabb_min.z + pad;
            if (far_plane - near_plane < 0.01f) far_plane = near_plane + 0.01f;

            light_proj = glm::orthoRH_ZO(center.x - extent, center.x + extent,
                                        center.y - extent, center.y + extent,
                                        near_plane, far_plane);
        } else {
            glm::vec3 center = cam ? cam->get_world_position() : glm::vec3(0.0f);
            glm::vec3 center_ls = glm::vec3(light_rot * glm::vec4(center, 1.0f));
            float ortho_extent = 15.0f;
            light_proj = glm::orthoRH_ZO(center_ls.x - ortho_extent, center_ls.x + ortho_extent,
                                        center_ls.y - ortho_extent, center_ls.y + ortho_extent,
                                        0.1f, 60.0f);
        }
        light_proj[1][1] *= -1.0f; // Vulkan Y-flip

        auto& ubo = light_data_.data();
        ubo.dir_light_space_matrix = light_proj * light_rot;
        // .y (formerly the PCF penumbra radius in texels) is unused now that this engine only
        // does a single hard depth compare -- zero-filled rather than restructuring the vec4,
        // since this UBO layout is shared with gfxcoopa's LightData::dir_shadow_params.
        ubo.dir_shadow_params      = glm::vec4(config_.shadow_bias, 0.0f, cast_dir_shadow ? 1.0f : 0.0f, 0.05f);
    }

    /** @brief The first PointLightComponent with cast_shadows set, or nullptr. */
    coopa::gfx::engine::components::PointLightComponent* find_first_shadow_casting_point_light_(coopa::scene::Scene& scene) {
        for (auto* pl : scene.get_components<coopa::gfx::engine::components::PointLightComponent>()) {
            if (pl->cast_shadows) return pl;
        }
        return nullptr;
    }

    void record_directional_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                                    const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                                    const std::vector<uint32_t>& instance_idx,
                                    const std::vector<SdfDrawItem>& sdf_draws,
                                    bool cast_dir_shadow) {
        shadow_target_.begin_directional_pass(cmd);
        if (cast_dir_shadow) {
            shadow_pipeline_->bind_directional(cmd);
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

            coopa::gfx::engine::passes::DirectionalShadowPushConstants pc{};
            pc.light_space_matrix = light_data_.data().dir_light_space_matrix;
            pc.gfx_time = glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), 0.0f);

            // Same last-shader transition guard as record_gbuffer_() -- a caster's shadow
            // must displace identically to its G-buffer draw (see gfx/surface/shadow_vs.glsl's
            // doc), which means binding the SAME named variant here, not just the stock pass.
            std::string last_shader;
            bool have_bound = true; // stock, bound just above

            for (size_t i = 0; i < renderers.size(); ++i) {
                if (instance_idx[i] == UINT32_MAX) continue;
                // A BLEND material only casts a shadow at full opacity -- this pass has no
                // per-fragment discard (single hard depth compare, no PCF to average a partial
                // alpha into a partial shadow -- see gfx/shadow_dither.glsl's doc), so there is
                // no way to draw a *partial* shadow for a translucent object; it's binary,
                // caster or not.
                if (renderers[i]->material.is_blended() && renderers[i]->material.alpha < 1.0f) continue;

                if (!have_bound || renderers[i]->material.shader != last_shader) {
                    shadow_pipeline_->bind_directional(cmd, renderers[i]->material.shader);
                    last_shader = renderers[i]->material.shader;
                    have_bound  = true;
                }

                // CUTOUT (AlphaMode::Mask): the mask texture punches through the shadow too,
                // via the same set/cutoff shadow_depth.frag tests against.
                pc.alpha_cutoff = renderers[i]->material.gpu_alpha_cutoff();
                pc.gfx_params   = renderers[i]->material.shader_params;
                cmd.bind_descriptor_set(material_cache_->set_for(renderers[i]->material), 0);
                shadow_pipeline_->push_directional(cmd, pc);
                renderers[i]->get_mesh()->bind(cmd);
                renderers[i]->get_mesh()->draw(cmd, 1, instance_idx[i]);
            }

            if (!sdf_draws.empty()) {
                sdf_shadow_pass_->bind_directional(cmd);
                cmd.bind_descriptor_set(sdf_data_.current_set(), 0);

                coopa::gfx::engine::passes::SdfDirectionalShadowPushConstants sdf_pc{};
                sdf_pc.light_space_matrix = light_data_.data().dir_light_space_matrix;
                sdf_pc.shadow_max_steps   = config_.sdf_shadow_max_steps;
                for (const auto& d : sdf_draws) {
                    if (!d.cast_shadows) continue;
                    // Same binary BLEND-at-full-opacity rule as the mesh loop above.
                    if (d.is_blend && d.comp->material.alpha < 1.0f) continue;
                    sdf_pc.renderer_index = d.gpu_index;
                    sdf_shadow_pass_->push_directional(cmd, sdf_pc);
                    cmd.draw(6);
                }
            }
        }
        shadow_target_.end_directional_pass(cmd);
        shadow_target_.transition_dir_to_shader_read(cmd);
    }

    void record_point_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                              const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                              const std::vector<uint32_t>& instance_idx,
                              const std::vector<SdfDrawItem>& sdf_draws,
                              coopa::gfx::engine::components::PointLightComponent* shadow_point,
                              bool cast_point_shadow) {
        if (!cast_point_shadow) {
            shadow_target_.transition_cube_to_shader_read(cmd);
            return;
        }

        glm::vec3 light_pos = shadow_point->get_world_position();
        float range = shadow_point->range;

        for (uint32_t face = 0; face < 6; ++face) {
            shadow_target_.begin_cube_face_pass(cmd, face);
            shadow_pipeline_->bind_cube(cmd);
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

            coopa::gfx::engine::passes::CubeShadowPushConstants pc{};
            pc.light_space_matrix = coopa::gfx::engine::targets::ShadowMapTarget::get_cube_face_matrix(face, light_pos, range);
            pc.light_pos_range    = glm::vec4(light_pos, range);
            pc.gfx_time = glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), 0.0f);

            // See record_directional_shadow_'s identical transition guard.
            std::string last_shader;
            bool have_bound = true; // stock, bound just above

            for (size_t i = 0; i < renderers.size(); ++i) {
                if (instance_idx[i] == UINT32_MAX) continue;
                // See record_directional_shadow_'s identical check -- a BLEND material only
                // casts a shadow at full opacity, since this pass has no partial-alpha discard.
                if (renderers[i]->material.is_blended() && renderers[i]->material.alpha < 1.0f) continue;

                if (!have_bound || renderers[i]->material.shader != last_shader) {
                    shadow_pipeline_->bind_cube(cmd, renderers[i]->material.shader);
                    last_shader = renderers[i]->material.shader;
                    have_bound  = true;
                }

                // See record_directional_shadow_'s identical CUTOUT handling.
                pc.alpha_cutoff = renderers[i]->material.gpu_alpha_cutoff();
                pc.gfx_params   = renderers[i]->material.shader_params;
                cmd.bind_descriptor_set(material_cache_->set_for(renderers[i]->material), 0);
                shadow_pipeline_->push_cube(cmd, pc);
                renderers[i]->get_mesh()->bind(cmd);
                renderers[i]->get_mesh()->draw(cmd, 1, instance_idx[i]);
            }

            if (!sdf_draws.empty()) {
                sdf_shadow_pass_->bind_cube(cmd);
                cmd.bind_descriptor_set(sdf_data_.current_set(), 0);

                coopa::gfx::engine::passes::SdfCubeShadowPushConstants sdf_pc{};
                sdf_pc.light_space_matrix = pc.light_space_matrix;
                sdf_pc.light_pos_range    = pc.light_pos_range;
                sdf_pc.shadow_max_steps   = config_.sdf_shadow_max_steps;
                for (const auto& d : sdf_draws) {
                    if (!d.cast_shadows) continue;
                    if (d.is_blend && d.comp->material.alpha < 1.0f) continue;
                    sdf_pc.renderer_index = d.gpu_index;
                    sdf_shadow_pass_->push_cube(cmd, sdf_pc);
                    cmd.draw(6);
                }
            }
            shadow_target_.end_cube_face_pass(cmd);
        }
    }

    /// Transitions the G-buffer depth image to SHADER_READ_ONLY_OPTIMAL, for pixel_stylize_pass_'s
    /// pre-bound outline-detection descriptor (see that construction-time comment for why it
    /// can't just be rebound instead). `from_layout` is the depth image's actual current
    /// layout -- DEPTH_STENCIL_ATTACHMENT_OPTIMAL right after record_gbuffer_() wrote it (the
    /// default, matching this function's original single call site), or
    /// DEPTH_STENCIL_READ_ONLY_OPTIMAL after record_transparent_()'s own render pass read it
    /// (transparent_pass.h's create_render_pass_() declares that as both its depth attachment's
    /// initial AND final layout).
    void transition_gbuffer_depth_to_shader_read_(
        coopa::gfx::command::CommandBuffer& cmd,
        VkImageLayout from_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout                       = from_layout;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = gbuffer_target_.depth_image_handle();
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        // Covers both possible sources unconditionally rather than picking one based on
        // from_layout -- see the ALL_COMMANDS_BIT comment below.
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;

        // ALL_COMMANDS_BIT rather than a hand-picked fragment-test stage: this barrier sits
        // downstream of a multi-pass chain (G-buffer write -> HiZPass's own internal transition
        // -> optionally record_transparent_()'s own render pass), and a narrower stage mask that
        // looked individually correct (EARLY_FRAGMENT_TESTS_BIT for the read-only case, mirroring
        // blendy's identical helper before this fix) still produced a WRITE_AFTER_WRITE hazard
        // under VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT once transparency ran
        // -- TransparentPass's own render pass declares no explicit VK_SUBPASS_EXTERNAL
        // dependency for the depth attachment, so exactly which stage its depth-test reads land
        // at isn't something this call site can determine reliably. This barrier runs once per
        // frame on one small image, so the conservative cost is negligible.
        vkCmdPipelineBarrier(cmd.handle(),
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    void record_gbuffer_(coopa::gfx::command::CommandBuffer& cmd,
                         const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                         const std::vector<uint32_t>& instance_idx,
                         const std::vector<SdfDrawItem>& sdf_draws) {
        gbuffer_target_.begin(cmd);
        // Stock pipeline bound first so a scene with no derived shaders (the overwhelming
        // common case) pays for exactly one bind, as before this pass gained variants.
        gbuffer_pipeline_->bind(cmd);
        cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), current_camera_set(), 0);
        cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

        // Tracks which named variant (or "" for stock) is currently bound, so a run of
        // same-shader renderers in `renderers` only rebinds the pipeline at a transition --
        // same idea as record_transparent_()'s last_kind guard. Renderers aren't sorted by
        // shader first (unlike the back-to-front transparent list, opaque draw order is
        // otherwise unconstrained, so sorting would be a real option), so this only pays off
        // when a scene's derived-shader objects happen to be contiguous; interleaved shaders
        // still render correctly, just with more binds than the theoretical minimum.
        std::string last_shader;
        bool have_bound = true; // stock, bound just above

        for (size_t i = 0; i < renderers.size(); ++i) {
            if (instance_idx[i] == UINT32_MAX) continue;
            auto* mr = renderers[i];
            // BLEND materials are drawn by transparent_pass_ instead, after SSR compositing --
            // never into the opaque G-buffer (see record_transparent_).
            if (mr->material.is_blended()) continue;

            if (!have_bound || mr->material.shader != last_shader) {
                gbuffer_pipeline_->bind(cmd, mr->material.shader);
                last_shader = mr->material.shader;
                have_bound  = true;
            }

            coopa::gfx::engine::passes::GBufferPipeline::PushConstants pc;
            pc.albedo       = glm::vec4(mr->material.albedo, mr->material.alpha);
            pc.metallic     = mr->material.metallic;
            pc.roughness    = mr->material.roughness;
            pc.ao           = mr->material.ao;
            pc.alpha_cutoff = mr->material.gpu_alpha_cutoff();
            pc.emissive     = mr->material.gpu_emissive();
            pc.gfx_time     = glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), 0.0f);
            pc.gfx_params   = mr->material.shader_params;
            gbuffer_pipeline_->push(cmd, pc);
            // Set 1: alpha-mask sampler (white 1x1 fallback unless this is a CUTOUT material
            // with a loaded texture_alpha_mask) -- see MaterialTextureCache.
            cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), material_cache_->set_for(mr->material), 1);

            mr->get_mesh()->bind(cmd);
            mr->get_mesh()->draw(cmd, 1, instance_idx[i]);
        }

        // Opaque/Mask SdfRenderers -- BLEND ones go through record_transparent_() instead, same
        // split as meshes above. Each draw is scissored to its own screen-space rectangle (see
        // the SDF gather block in render()); the scissor is restored to the full target
        // afterward since nothing else in this bracket expects it narrowed.
        bool any_opaque_sdf = false;
        for (const auto& d : sdf_draws) {
            if (d.is_blend || d.px_rect.w == 0 || d.px_rect.h == 0) continue;
            if (!any_opaque_sdf) {
                sdf_gbuffer_pass_->bind(cmd);
                cmd.bind_descriptor_set(current_camera_set(), 0);
                cmd.bind_descriptor_set(sdf_data_.current_set(), 1);
                any_opaque_sdf = true;
            }
            cmd.set_scissor(d.px_rect.x, d.px_rect.y, d.px_rect.w, d.px_rect.h);
            sdf_gbuffer_pass_->push(cmd, d.gpu_index);
            cmd.draw(6);
        }
        if (any_opaque_sdf) {
            cmd.set_scissor(0, 0, render_extent_.width, render_extent_.height);
        }

        gbuffer_target_.end(cmd);
    }

    /// Draws every BLEND-material renderer's depth/normal/position/shaded-color into
    /// transparent_capture_target_ -- feeds ssr_pass_'s secondary trace source (see
    /// TransparentCaptureTarget/TransparentCapturePass's own docs), NOT a visible draw of its
    /// own. No back-to-front sort needed, unlike record_transparent_()'s alpha-blended draw
    /// below: this is a normal depth-tested opaque-style pass, front-most transparent surface
    /// wins per pixel regardless of draw order.
    ///
    /// Always begins/ends the render pass even when there's nothing to draw -- every
    /// attachment in transparent_capture_target_'s render pass declares
    /// initialLayout = UNDEFINED, so an "empty" frame transitions exactly as safely as a
    /// populated one, and transparent_hiz_pass_->execute() right after this call
    /// unconditionally expects the depth image in DEPTH_STENCIL_ATTACHMENT_OPTIMAL -- only
    /// this render pass's own finalLayout guarantees that every frame, unlike
    /// record_transparent_()'s early-return (which is safe there only because that pass never
    /// owns the depth image it tests against).
    void record_transparent_capture_(coopa::gfx::command::CommandBuffer& cmd,
                                     const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                                     const std::vector<uint32_t>& instance_idx,
                                     const std::vector<SdfDrawItem>& sdf_draws) {
        transparent_capture_target_.begin(cmd);
        transparent_capture_pass_->bind(cmd);
        cmd.bind_descriptor_set(transparent_capture_pass_->layout(), current_camera_set(), 0);
        cmd.bind_descriptor_set(transparent_capture_pass_->layout(), *light_set_,  1);
        cmd.bind_descriptor_set(transparent_capture_pass_->layout(), *shadow_set_, 2);
        cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

        // Frame-level, not per-object -- same config_ sourcing as record_transparent_()'s own
        // lighting_pc, minus the ssr_enabled/ssgi/GfxSsrParams fields that struct carries
        // (this capture never traces its own reflection -- see transparent_capture.frag's doc).
        TransparentCaptureLightingPushConstants lighting_pc;
        lighting_pc.light_bands       = config_.light_bands;
        lighting_pc.spec_threshold    = config_.spec_threshold;
        lighting_pc.soft_lighting     = config_.soft_lighting ? 1.0f : 0.0f;
        lighting_pc.rim_strength      = config_.rim_strength;
        lighting_pc.ambient_intensity = config_.indirect.ambient_intensity;
        lighting_pc.sky_intensity     = config_.indirect.sky_intensity;
        // VERTEX|FRAGMENT, not FRAGMENT alone: TransparentCapturePass's push-constant range now
        // covers both stages (see its PushConstants' gfx_time/gfx_params doc), and Vulkan
        // requires a push call's stageFlags to match the declared range for every byte it
        // touches, including this trailing per-frame lighting block.
        cmd.push_constants(transparent_capture_pass_->layout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           sizeof(coopa::gfx::engine::passes::TransparentCapturePass::PushConstants),
                           sizeof(TransparentCaptureLightingPushConstants), &lighting_pc);

        // Same last-shader transition guard as record_gbuffer_() -- and the SAME variant
        // name TransparentPass binds for this material, since SSR must reflect the same
        // displaced geometry the visible draw shows (see TransparentCapturePass::
        // add_variant()'s doc).
        std::string last_shader;
        bool have_bound = true; // stock, bound just above

        for (size_t i = 0; i < renderers.size(); ++i) {
            if (instance_idx[i] == UINT32_MAX) continue;
            if (!renderers[i]->material.is_blended()) continue;

            auto* mr = renderers[i];
            if (!have_bound || mr->material.shader != last_shader) {
                transparent_capture_pass_->bind(cmd, mr->material.shader);
                last_shader = mr->material.shader;
                have_bound  = true;
            }

            coopa::gfx::engine::passes::TransparentCapturePass::PushConstants pc;
            pc.albedo     = glm::vec4(mr->material.albedo, mr->material.alpha);
            pc.metallic   = mr->material.metallic;
            pc.roughness  = mr->material.roughness;
            pc.ao         = mr->material.ao;
            pc.gfx_time   = glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), 0.0f);
            pc.gfx_params = mr->material.shader_params;
            transparent_capture_pass_->push(cmd, pc);

            mr->get_mesh()->bind(cmd);
            mr->get_mesh()->draw(cmd, 1, instance_idx[i]);
        }

        // BLEND SdfRenderers -- feeds the exact same secondary reflection source, via
        // SdfCapturePass (see that class's doc). Scissored per-object like every other
        // main-camera SDF draw.
        bool any_blend_sdf = false;
        for (const auto& d : sdf_draws) {
            if (!d.is_blend || d.px_rect.w == 0 || d.px_rect.h == 0) continue;
            if (!any_blend_sdf) {
                sdf_capture_pass_->bind(cmd);
                cmd.bind_descriptor_set(current_camera_set(), 0);
                cmd.bind_descriptor_set(*light_set_,  1);
                cmd.bind_descriptor_set(*shadow_set_, 2);
                cmd.bind_descriptor_set(sdf_data_.current_set(), 3);
                any_blend_sdf = true;
            }
            cmd.set_scissor(d.px_rect.x, d.px_rect.y, d.px_rect.w, d.px_rect.h);
            sdf_capture_pass_->push(cmd, d.gpu_index);
            cmd.draw(6);
        }
        if (any_blend_sdf) {
            cmd.set_scissor(0, 0, render_extent_.width, render_extent_.height);
        }

        transparent_capture_target_.end(cmd);
    }

    /// Fills the forward MESH pass's per-frame globals (lighting/indirect/SSR/refraction)
    /// from config_ -- the exact same fields sdf_data_'s own globals() fill (in render()'s
    /// SDF gather block) sources, kept in sync by hand since the two are independent UBOs
    /// (see ForwardGlobals's own doc for why the duplication is accepted, not removed).
    void fill_forward_globals_(ForwardGlobals& g) const {
        g.lighting0 = glm::vec4(config_.light_bands, config_.spec_threshold,
                                config_.soft_lighting ? 1.0f : 0.0f, config_.rim_strength);
        g.lighting1 = glm::vec4(config_.indirect.ambient_intensity, config_.indirect.sky_intensity,
                                config_.ssr_enabled ? 1.0f : 0.0f, config_.indirect.ssgi_intensity);
        g.ssr0 = glm::vec4(config_.indirect.ssgi_distance, config_.ssr_max_distance,
                          config_.ssr_bias_texels, config_.ssr_thickness);
        g.ssr1 = glm::vec4(config_.ssr_thickness_scale, config_.ssr_roughness_cutoff, 0.0f, 0.0f);
        g.ssr_steps = glm::ivec4(config_.ssr_max_iterations,
                                 static_cast<int>(hiz_pass_->max_mip_level()),
                                 config_.ssr_start_mip, config_.ssr_min_mip0_steps);
        g.ssr_mip = glm::ivec4(static_cast<int>(scene_color_mip_pass_->max_mip_level()), 0, 0, 0);
        g.refract0 = glm::vec4(config_.refraction_enabled ? 1.0f : 0.0f, config_.refraction_strength,
                               config_.refraction_max_offset, config_.refraction_chromatic);
        g.refract1 = glm::vec4(config_.refraction_blur, config_.refraction_density,
                               config_.refraction_fresnel ? 1.0f : 0.0f, 0.0f);
    }

    /// Draws every BLEND-material renderer AND every BLEND SdfRenderer, back-to-front by
    /// squared distance from the camera, into hdr_target_view (the deferred-lit target or,
    /// when SSR is on, its composite output -- same choice pixel_stylize_pass_ reads from).
    /// Must run after SSR compositing: ssr_composite.frag derives reflections from the
    /// G-buffer, which describes opaque geometry only -- compositing it on top of
    /// already-blended glass would add the OCCLUDED surface's reflection to the glass (same
    /// reasoning as blendy's PbrRenderPipeline).
    ///
    /// Meshes and SDFs are merged into ONE sorted list, switching between transparent_pass_'s
    /// and sdf_forward_pass_'s pipelines as the list's item kind changes -- both draw into the
    /// SAME render pass instance (see SdfForwardPass's file doc for why sharing
    /// transparent_pass_->render_pass() is what makes this possible), so a glass mesh and a
    /// glass SDF blob composite in correct depth order instead of one kind always landing on
    /// top of the other.
    ///
    /// @return true if the pass actually ran (at least one BLEND item this frame) and
    ///         therefore left the G-buffer depth image in DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    ///         false if it early-returned with depth untouched -- see the call site's comment.
    bool record_transparent_(coopa::gfx::command::CommandBuffer& cmd,
                             const std::vector<coopa::gfx::engine::components::MeshRenderer*>& renderers,
                             const std::vector<glm::mat4>& world_matrices,
                             const std::vector<uint32_t>& instance_idx,
                             const std::vector<SdfDrawItem>& sdf_draws,
                             VkImageView hdr_target_view,
                             VkImageLayout depth_layout,
                             const glm::vec3& camera_pos) {
        struct Item {
            bool      is_sdf;
            size_t    index; // into `renderers`/`world_matrices` if !is_sdf, else into `sdf_draws`
            glm::vec3 pos;
        };
        std::vector<Item> order;
        for (size_t i = 0; i < renderers.size(); ++i) {
            if (instance_idx[i] == UINT32_MAX) continue;
            if (!renderers[i]->material.is_blended()) continue;
            order.push_back({false, i, glm::vec3(world_matrices[i][3])});
        }
        for (size_t i = 0; i < sdf_draws.size(); ++i) {
            if (!sdf_draws[i].is_blend) continue;
            if (sdf_draws[i].px_rect.w == 0 || sdf_draws[i].px_rect.h == 0) continue;
            order.push_back({true, i, sdf_draws[i].world_center});
        }
        if (order.empty()) return false;

        std::stable_sort(order.begin(), order.end(), [&](const Item& a, const Item& b) {
            glm::vec3 da = a.pos - camera_pos;
            glm::vec3 db = b.pos - camera_pos;
            return glm::dot(da, da) > glm::dot(db, db); // back-to-front (farthest first)
        });

        transparent_pass_->set_targets(hdr_target_view, gbuffer_target_.depth_view(),
                                       render_extent_.width, render_extent_.height);
        transparent_pass_->begin(cmd, gbuffer_target_.depth_image_handle(), depth_layout);

        // Frame-level lighting/indirect/SSR/refraction tuning now lives in forward_globals_'s
        // UBO (set 6, bound below via bind_extra()) instead of a push constant -- filled and
        // uploaded once per frame in render() (fill_forward_globals_()), not here, since it
        // no longer needs to be re-issued at every mesh-kind transition the way a push
        // constant did (a descriptor set, unlike push-constant contents, survives a bind to a
        // pipeline with an incompatible layout).

        // -1 = neither yet bound, 0 = mesh, 1 = sdf -- tracks which pipeline+sets are current so
        // a run of same-kind items in `order` only rebinds once, at the transition.
        int last_kind = -1;
        // Independent of last_kind: which named shader variant transparent_pass_ currently has
        // bound. A mesh-kind run in `order` isn't sorted by shader (back-to-front depth is the
        // only order that matters here -- see this function's own doc), so two consecutive mesh
        // items can legitimately need different pipelines even though last_kind doesn't change;
        // switching pipelines mid-run is safe without re-binding sets 0-2/extra, since every
        // variant shares the same descriptor set layouts (see TransparentPass::add_variant()).
        // Starts as "never bound" via last_kind's own -1 sentinel (checked alongside it below),
        // so there's no separate bool to fall out of sync with last_kind.
        std::string last_mesh_shader;

        for (const auto& item : order) {
            if (!item.is_sdf) {
                auto* mr = renderers[item.index];

                // The pipeline MUST be (re)bound before the 2-argument bind_descriptor_set()
                // calls below -- that overload derives its pipeline layout from whatever
                // CommandBuffer last had bound (see command_buffer.h's bound_pipeline_ cache),
                // which after an SDF run (or nothing, at the very start of the frame) is NOT
                // transparent_pass_'s layout. Binding descriptor sets first and the pipeline
                // second, even briefly, resolves set 0 against the wrong layout and is a
                // real validation error (descriptor type mismatch), not just a style issue.
                if (last_kind != 0 || mr->material.shader != last_mesh_shader) {
                    transparent_pass_->bind(cmd, mr->material.shader);
                    last_mesh_shader = mr->material.shader;
                }

                if (last_kind != 0) {
                    // An earlier SDF item in this back-to-front list narrows the scissor to
                    // its own px_rect (see the sdf branch below) and nothing widens it again
                    // until the loop ends -- reset here or this mesh draw inherits that SDF's
                    // bounding box and gets clipped to it.
                    cmd.set_scissor(0, 0, render_extent_.width, render_extent_.height);
                    cmd.bind_descriptor_set(current_camera_set(), 0);
                    cmd.bind_descriptor_set(*light_set_,  1);
                    cmd.bind_descriptor_set(*shadow_set_, 2);
                    // Sets 3/4/5 -- ssr_pass_'s own trace-input sets; set 6 -- forward_globals_'s
                    // UBO (see the ExtraSets bind lambda in the ctor). All bound via the
                    // ExtraSets callback configured at construction.
                    transparent_pass_->bind_extra(cmd);
                    cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);
                    last_kind = 0;
                }

                coopa::gfx::engine::passes::TransparentPass::PushConstants pc;
                pc.albedo     = glm::vec4(mr->material.albedo, mr->material.alpha);
                pc.metallic   = mr->material.metallic;
                pc.roughness  = mr->material.roughness;
                pc.ao         = mr->material.ao;
                pc.gfx_time   = glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), 0.0f);
                pc.gfx_params = mr->material.shader_params;
                transparent_pass_->push(cmd, pc);

                // Pushed into transparent.frag's PushConstants' [32, 64) region (see the ctor's
                // extra_pc_bytes comment) -- per-object, unlike forward_globals_'s per-frame UBO.
                // Each field falls back to its PixelRenderConfig engine-wide default when the
                // material left it at PBRMaterial's negative sentinel (see that struct's own
                // doc for why -1 rather than baking the default straight into PBRMaterial).
                TransparentRefractionPushConstants refract_pc;
                float refract_ior       = mr->material.ior >= 0.0f ? mr->material.ior : config_.refraction_ior;
                float refract_thickness = mr->material.refraction_thickness >= 0.0f
                                              ? mr->material.refraction_thickness : config_.refraction_thickness;
                glm::vec3 refract_tint  = mr->material.refraction_tint.r >= 0.0f
                                              ? mr->material.refraction_tint : config_.refraction_tint;
                refract_pc.tint_thickness = glm::vec4(refract_tint, refract_thickness);
                refract_pc.ior_flags      = glm::vec4(refract_ior, mr->material.has_refraction() ? 1.0f : 0.0f, 0.0f, 0.0f);
                // VERTEX|FRAGMENT, not FRAGMENT alone: TransparentPass's push-constant range now
                // covers both stages (see its PushConstants' gfx_time/gfx_params doc), and
                // Vulkan requires a push call's stageFlags to match the declared range for
                // every byte it touches, including this trailing per-object refraction block.
                cmd.push_constants(transparent_pass_->layout(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   sizeof(coopa::gfx::engine::passes::TransparentPass::PushConstants),
                                   sizeof(TransparentRefractionPushConstants), &refract_pc);

                mr->get_mesh()->bind(cmd);
                mr->get_mesh()->draw(cmd, 1, instance_idx[item.index]);
            } else {
                if (last_kind != 1) {
                    sdf_forward_pass_->bind(cmd);
                    cmd.bind_descriptor_set(current_camera_set(), 0);
                    cmd.bind_descriptor_set(*light_set_,  1);
                    cmd.bind_descriptor_set(*shadow_set_, 2);
                    cmd.bind_descriptor_set(sdf_data_.current_set(), 3);
                    // Sets 4/5/6 -- see sdf_forward_pass_'s own ExtraSets ctor argument.
                    sdf_forward_pass_->bind_extra(cmd);
                    last_kind = 1;
                }

                const auto& d = sdf_draws[item.index];
                cmd.set_scissor(d.px_rect.x, d.px_rect.y, d.px_rect.w, d.px_rect.h);
                sdf_forward_pass_->push(cmd, d.gpu_index);
                cmd.draw(6);
            }
        }

        // Restore the full scissor -- an SDF item above may have narrowed it, and
        // pixel_stylize_pass_'s later outline/dither pass expects the whole render area.
        cmd.set_scissor(0, 0, render_extent_.width, render_extent_.height);

        transparent_pass_->end(cmd);
        return true;
    }

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::gfx::core::Swapchain&   swapchain_;
    PixelRenderConfig              config_;
    RenderExtent                   render_extent_;

    // Fed to every surface-shader backbone's gfx_time field (see render()'s own doc) --
    // elapsed_time_ accumulates render()'s dt parameter every frame, never reset, so a
    // derived shader's displacement hook gets a monotonically increasing clock regardless
    // of frame rate. frame_dt_ is gfx_time's y component; z reuses the existing frame_index_
    // member below (already incremented once per frame for SSAO/SSR noise rotation) rather
    // than adding a second counter.
    float elapsed_time_ = 0.0f;
    float frame_dt_     = 0.0f;
    // DISPLAY (letterboxed) size tilt_shift_pass_ is sized to -- computed once from the
    // STARTUP swapchain extent, same no-resize-support caveat as render_extent_ itself
    // (see this class's file doc / the comment at hiz_pass_'s construction site). A window
    // resize after construction would leave this pass's fixed-size targets mismatched with
    // the swapchain's new letterbox rect, exactly like every other fixed-at-construction
    // target in this pipeline.
    LetterboxRect                  upscaled_extent_;

    coopa::gfx::engine::targets::GBufferTarget   gbuffer_target_;
    coopa::gfx::engine::targets::OffscreenTarget offscreen_target_; // lit + sky, pre-post, HDR
    coopa::gfx::engine::targets::OffscreenTarget post_target_;      // final low-res LDR, post PixelStylizePass
    // Forward capture of transparent geometry -- a second reflection SOURCE for opaque
    // reflectors' SSR (see record_transparent_capture_()), not a visible target. Always
    // constructed (mirrors gbuffer_target_'s own always-on policy); render() checks
    // config_.ssr_reflect_transparent per frame to decide whether to draw into/read from it.
    coopa::gfx::engine::targets::TransparentCaptureTarget transparent_capture_target_;
    coopa::gfx::engine::targets::OffscreenTarget fog_target_; // fog composite, pre-post, HDR
    coopa::gfx::engine::util::Sampler            nearest_sampler_;
    coopa::gfx::engine::util::Sampler            linear_sampler_;
    coopa::gfx::engine::util::Sampler            shadow_sampler_;

    // Per-frame-in-flight: see the class file doc and the construction-site comment above
    // camera_pool_'s build. camera_frame_ tracks the slot render() most recently selected;
    // current_camera_set() (below, in the private section) is what every draw/pass call site
    // actually binds.
    static constexpr uint32_t kCameraFrames = coopa::gfx::presentation::MAX_FRAMES_IN_FLIGHT;
    std::vector<std::unique_ptr<coopa::gfx::engine::data::CameraUBO>> camera_ubos_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> camera_layout_; // one shared layout
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      camera_pool_;
    std::vector<std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> camera_sets_;
    uint32_t camera_frame_ = 0;

    coopa::gfx::engine::data::LightData light_data_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> light_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      light_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>       light_set_;

    coopa::gfx::engine::data::FogData fog_data_;
    std::unique_ptr<coopa::gfx::engine::passes::FogPass> fog_pass_;
    // Fixed source view fog_pass_ reads from -- chosen once from config_.ssr_enabled's startup
    // value, same policy as pixel_stylize_pass_'s own binding (see that construction-time
    // comment). Kept as a member (not a local) so pixel_stylize_pass_'s own construction, later
    // in the ctor body, can fall back to it when fog is disabled.
    coopa::gfx::TextureView pre_fog_view_typed_;

    coopa::gfx::engine::targets::ShadowMapTarget shadow_target_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   shadow_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        shadow_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         shadow_set_;
    // Material set (alpha-mask sampler) shared by gbuffer_pipeline_ and shadow_pipeline_ -- see
    // MaterialTextureCache's own doc. Constructed before both in the ctor.
    std::unique_ptr<toy::render::MaterialTextureCache>           material_cache_;
    std::unique_ptr<coopa::gfx::engine::passes::ShadowPipeline>  shadow_pipeline_;

    coopa::gfx::engine::data::PaletteLut palette_lut_;

    std::unique_ptr<coopa::gfx::engine::passes::GBufferPipeline> gbuffer_pipeline_;
    std::unique_ptr<passes::GBufferVisualizePass>                gbuffer_visualize_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SsaoPass>        ssao_pass_;       // always constructed
    std::unique_ptr<coopa::gfx::engine::passes::DeferredLightingPass> pixel_lighting_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SkyboxPass>      skybox_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::PixelStylizePass> pixel_stylize_pass_;
    // Independent bloom pyramid -- see its construction-site comment. Unlike
    // scene_color_mip_pass_/hiz_pass_, it binds every descriptor once at construction and
    // never rebinds, so it does NOT require the per-frame device_.wait_idle()
    // need_ssr_trace_inputs pays for.
    std::unique_ptr<coopa::gfx::engine::passes::BloomPass> bloom_pass_;
    // Diorama tilt-shift blur -- see its construction-site comment and gfxcoopa's
    // TiltShiftPass file doc. Runs at DISPLAY resolution, after upscale_pass_ would
    // otherwise be the last step; upscale_pass_'s own source binding is chosen once at
    // construction from config_.tilt_shift_enabled's startup value, same policy as
    // bloom_pass_/fog_pass_'s bindings above.
    std::unique_ptr<coopa::gfx::engine::passes::TiltShiftPass> tilt_shift_pass_;
    std::unique_ptr<passes::UpscalePass>                         upscale_pass_;

    // Mesh-only forward-pass globals UBO (see forward_globals.h's file doc for why this
    // isn't a push constant) -- constructed before transparent_pass_ since that pass's
    // ExtraSets ctor argument binds its layout/set at construction.
    ForwardGlobalsData forward_globals_;

    // Always constructed (mirrors ssr_pass_'s policy -- see render_features.h); render()
    // checks config_.transparency_enabled per frame and skips the whole pass when off.
    std::unique_ptr<coopa::gfx::engine::passes::TransparentPass> transparent_pass_;

    // --- SSR/SSGI: always constructed; render() checks config_.ssr_enabled per frame ---
    std::unique_ptr<coopa::gfx::engine::passes::HiZPass>           hiz_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SceneColorMipPass> scene_color_mip_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SsrPass>            ssr_pass_;

    // --- ssr_reflect_transparent: opaque surfaces also reflect transparent geometry ---
    // Always constructed (same always-on-but-gated policy as the SSR/SSGI trio above); render()
    // checks config_.ssr_reflect_transparent per frame. transparent_hiz_pass_/
    // transparent_scene_color_mip_pass_ are SECOND, independent instances of the exact same
    // classes hiz_pass_/scene_color_mip_pass_ already use -- both are fully generic against any
    // depth/color image (no GBufferTarget reference held), confirmed reusable for exactly this.
    std::unique_ptr<coopa::gfx::engine::passes::TransparentCapturePass>  transparent_capture_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::HiZPass>                 transparent_hiz_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SceneColorMipPass>       transparent_scene_color_mip_pass_;

    // --- Refraction: independent post-SSR scene-colour chain for the MESH forward pass's
    // u_scene_color (set 5 in transparent_extra above), so a refracting glass object's
    // background sample -- and its own gfx_ssr_trace()/SSGI lookup -- see SSR reflections,
    // not the pre-SSR image scene_color_mip_pass_ still holds at that point. A THIRD,
    // independent SceneColorMipPass instance, not a second execute() on scene_color_mip_pass_
    // itself: that pass rebinds its own internal per-mip descriptor sets on every execute()
    // call, and doing that twice in one frame's not-yet-submitted command buffer invalidates
    // it (Vulkan poisons a command buffer the moment a descriptor set it already bound gets
    // updated again before submission) -- this was caught by validation layers during
    // testing (VUID-vkCmdBindPipeline-commandBuffer-recording, "descriptor set was destroyed
    // or updated"). Own dedicated descriptor set for the same reason: reusing
    // ssr_pass_->scene_color_set() here would require rebinding IT per frame too, which
    // that set's own doc says never happens ("bound once for the run, never per-frame").
    std::unique_ptr<coopa::gfx::engine::passes::SceneColorMipPass> refraction_scene_color_mip_pass_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>     refraction_scene_color_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>          refraction_scene_color_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>           refraction_scene_color_set_;

    // Previous frame's proj * view, for SSAO's and SSR's temporal resolve passes. Not
    // camera-jittered (this engine has no TAA), just last frame's camera -- see
    // SsrPass::Params::prev_view_proj / SsaoPass::Params::prev_view_proj.
    glm::mat4 prev_view_proj_       = glm::mat4(1.0f);
    bool      prev_view_proj_valid_ = false;
    // Monotonic per-rendered-frame counter, incremented once at the end of render(). Shared
    // by SSAO's noise-tile rotation and SSR's stochastic ray jitter (see each pass's own
    // Params::*frame_index* doc) -- both are per-pixel noise sources that need decorrelating
    // frame to frame, and there is no reason for the two to disagree about which frame it is.
    uint32_t  frame_index_          = 0;

    InstanceStream instance_stream_;
    bool           warned_perspective_snap_ = false;

    // --- SDF raymarching system ---
    // sdf_data_ must be constructed before the four passes below (they bind its layout at
    // construction) -- declaration order here matches initializer-list order in the ctor.
    coopa::gfx::engine::data::SdfData sdf_data_;
    std::unique_ptr<coopa::gfx::engine::passes::SdfGBufferPass> sdf_gbuffer_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SdfForwardPass> sdf_forward_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SdfShadowPass>  sdf_shadow_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SdfCapturePass> sdf_capture_pass_;

    // --- Job dispatch for the per-frame gathers -- see should_parallelize_()'s doc ---
    coopa::job::JobEngine* jobs_ = nullptr;      // non-owning; nullptr = always serial
    std::size_t            parallel_threshold_ = 256;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_RENDER_PIPELINE_H
