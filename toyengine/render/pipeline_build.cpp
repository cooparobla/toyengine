/**
 * @file pipeline_build.cpp
 * @brief Construction of the frame graph: every pass, target and descriptor set,
 *        plus the rebuilds a swapchain or display-rect change forces.
 *
 * One of toy_render_pipeline.cpp's sibling translation units -- same class, split by
 * concern so a 5,000-line file does not have to be navigated as one. The declarations
 * (and the rules that govern them) are in toy_render_pipeline.h.
 */

#include <toyengine/render/toy_render_pipeline.h>
#include <glm/gtx/quaternion.hpp>

#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/sdf_renderer.h>
#include <gfxcoopa/engine/components/sdf_shape.h>
#include <gfxcoopa/engine/data/light_data.h>
#include <gfxcoopa/engine/passes/hiz_pass.h>
#include <gfxcoopa/engine/passes/ssr_pass.h>
#include <gfxcoopa/engine/passes/transparent_pass.h>
#include <gfxcoopa/engine/targets/gbuffer_target.h>
#include <gfxcoopa/engine/targets/offscreen_target.h>
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/presentation/renderer.h>
#include <cstdio>
#include <cstdlib>
#include <toyengine/core/runtime_paths.h>
#include <toyengine/render/frame_profile.h>
#include <toyengine/render/particle_types.h>
#include <toyengine/render/sky_state.h>
#include <toyengine/render/toy_render_config.h>
#include <toyengine/render/toy_render_math.h>
#include <toyengine/scene/camera_controller.h>
#include <uicoopa/layout/canvas.h>
#include <uicoopa/render/ui_pass.h>
#include <volk/volk.h>

namespace toy {
namespace render {

void ToyRenderPipeline::build_frame_descriptors_() {
    camera_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
        coopa::gfx::pipeline::DescriptorLayoutBuilder()
            .uniform_buffer(0, device_.supports_tessellation()
                                   ? coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment |
                                     coopa::gfx::ShaderStage::TessControl | coopa::gfx::ShaderStage::TessEval
                                   : coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment)
            .build(device_));

    // One CameraUBO and one descriptor set per frame-in-flight slot (file doc, rule 2); the
    // layout stays shared, since every set allocated from it is interchangeable. unique_ptr is
    // load-bearing rather than stylistic: CameraUBO's deleted copy ctor suppresses its move
    // ctor too, so std::vector<CameraUBO> will not compile.
    camera_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
        coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*camera_layout_, kCameraFrames).build(device_));

    camera_ubos_.reserve(kCameraFrames);
    camera_sets_.reserve(kCameraFrames);
    for (uint32_t i = 0; i < kCameraFrames; ++i) {
        camera_ubos_.push_back(std::make_unique<coopa::gfx::engine::data::CameraUBO>(device_, allocator_));
        camera_sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *camera_pool_, *camera_layout_));
        camera_sets_[i]->bind_buffer(0, camera_ubos_[i]->buffer());
    }

    light_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
        coopa::gfx::pipeline::DescriptorLayoutBuilder()
            .uniform_buffer(0, coopa::gfx::ShaderStage::Fragment)
            .build(device_));
    // One LightData + one descriptor set per frame-in-flight slot -- see light_datas_'s own
    // doc for why. Same shape as camera_ubos_/camera_sets_ above: bind_buffer() still only
    // happens once per slot here at construction, not per frame.
    light_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
        coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*light_layout_, kCameraFrames).build(device_));
    light_datas_.reserve(kCameraFrames);
    light_sets_.reserve(kCameraFrames);
    for (uint32_t i = 0; i < kCameraFrames; ++i) {
        focus_probes_.push_back(std::make_unique<coopa::gfx::memory::Buffer>(
            device_, allocator_, kFocusProbeSamples * 16u,
            coopa::gfx::BufferUsage::TransferDst, coopa::gfx::MemoryResidency::GpuToCpu));
    }
    for (uint32_t i = 0; i < kCameraFrames; ++i) {
        light_datas_.push_back(std::make_unique<coopa::gfx::engine::data::LightData>(device_, allocator_));
        light_sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *light_pool_, *light_layout_));
        light_sets_[i]->bind_buffer(0, light_datas_[i]->buffer());
    }

    shadow_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
        coopa::gfx::pipeline::DescriptorLayoutBuilder()
            .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(3, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(4, coopa::gfx::ShaderStage::Fragment)
            .build(device_));
    shadow_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
        coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*shadow_layout_, 1).build(device_));
    shadow_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *shadow_pool_, *shadow_layout_);
    // The point/spot shadow atlas (see LocalShadowAtlas) -- built here because its view is
    // bound from frame 0 whether or not any light casts.
    local_shadow_atlas_ = std::make_unique<LocalShadowAtlas>(
        device_, allocator_, config_.local_shadow_atlas_resolution, config_.shadow_cache_enabled);
    shadow_set_->bind_image(0, shadow_target_.dir_shadow_view(), shadow_sampler_.handle());
    // Binding 1: every point and spot shadow (gfx/local_shadow.glsl's local_shadow_atlas).
    // Binding 2 is not declared by any shader; it is still bound (to the same image) because
    // the set layout -- shared by every pass that binds set 2 -- declares it.
    shadow_set_->bind_image(1, local_shadow_atlas_->view(), shadow_sampler_.handle());
    shadow_set_->bind_image(2, local_shadow_atlas_->view(), shadow_sampler_.handle());
    // Binding 3: the directional map AGAIN, through a plain nearest sampler --
    // PCSS's blocker search reads stored depths, which the compare sampler at
    // binding 0 cannot return (see gfx_shadow_dir_pcss).
    shadow_set_->bind_image(3, shadow_target_.dir_shadow_view(), nearest_sampler_.handle());
    // Binding 4: the cloud layer's shadow map (cloud_shadow.glsl) -- built here because its
    // view is bound from frame 0 whether or not the clouds cast.
    cloud_shadow_pass_ = std::make_unique<passes::CloudShadowPass>(
        device_, allocator_, kCameraFrames, config_.shaders("fullscreen.vert"),
        config_.shaders("cloud_shadow_volumetric.frag"), config_.shaders("cloud_shadow_flat.frag"));
    shadow_set_->bind_image(4, cloud_shadow_pass_->view(), cloud_shadow_pass_->sampler());
}

void ToyRenderPipeline::build_geometry_passes_() {
    // Built before shadow_pipeline_/gbuffer_pipeline_ below -- both need material_cache_'s
    // layout handle to append the CUTOUT alpha-mask sampler as their material set. See
    // MaterialTextureCache's own doc for why lazily allocating sets from it later (once a
    // scene's masked materials finish loading) is safe under overlapped command buffers.
    material_cache_ = std::make_unique<coopa::gfx::engine::util::MaterialTextureCache>(device_, allocator_, cmd_pool_);
    // The surface world set (tessellation view, snow, precipitation occlusion, trench field):
    // set 2 of every G-buffer pipeline, set 1 of every shadow pipeline -- see surface_world.h.
    surface_world_ = std::make_unique<SurfaceWorldData>(device_, allocator_);
    // Set 3 of every G-buffer pipeline: heightmap terrains' splats and layer arrays. Stock
    // shaders never declare it, so only `heightmap_terrain` draws bind it.
    terrain_gpu_ = std::make_unique<TerrainGpuRegistry>(device_, allocator_, cmd_pool_);

    shadow_pipeline_ = std::make_unique<coopa::gfx::engine::passes::ShadowPipeline>(
        device_, shadow_target_.dir_render_pass(), shadow_target_.cube_render_pass(),
        config_.shaders("shadow_depth.vert"),
        config_.shaders("shadow_depth.frag"),
        config_.shaders("shadow_cube.vert"),
        config_.shaders("shadow_cube.frag"),
        &material_cache_->layout_object(),
        std::vector<const coopa::gfx::pipeline::DescriptorSetLayout*>{&surface_world_->layout()});

    gbuffer_pipeline_ = std::make_unique<coopa::gfx::engine::passes::GBufferPipeline>(
        device_, gbuffer_target_.render_pass(), camera_layout_->handle(), material_cache_->layout(),
        config_.shaders("gbuffer.vert"),
        config_.shaders("gbuffer.frag"),
        std::vector<VkDescriptorSetLayout>{surface_world_->layout().handle(), terrain_gpu_->layout().handle()});

    // Tessellation (MeshRenderer::tessellation): the stock tessellated pipelines, and the
    // shared pass-through vertex / default control stages every variant's twin uses. A
    // device without tessellation builds none, and tessellated renderers draw as authored.
    if (device_.supports_tessellation()) {
        gbuffer_pipeline_->enable_tessellation(config_.shaders("surface_tess.vert"),
                                               config_.shaders("gbuffer.tesc"), config_.shaders("gbuffer.tese"));
        shadow_pipeline_->enable_tessellation(config_.shaders("surface_tess.vert"),
                                              config_.shaders("shadow_depth.tesc"), config_.shaders("shadow_depth.tese"),
                                              config_.shaders("shadow_cube.tesc"), config_.shaders("shadow_cube.tese"));
    }
    auto opt = [&](const std::string& n) { return n.empty() ? std::string() : config_.shaders(n); };

    // Register every Opaque-domain derived shader (e.g. foliage) as a named pipeline
    // variant on both the G-buffer and shadow passes -- see SurfaceShaderDesc's doc on
    // why an entry point left empty falls back to the STOCK logical name rather than
    // being skipped: a shader that only overrides, say, the fragment stage still needs
    // a real shadow entry point, or its shadow silently stops moving with it.
    for (const auto& sd : config_.surface_shaders.all()) {
        if (sd.domain != coopa::gfx::pipeline::SurfaceShaderDomain::Opaque) continue;
        // A shader with its own shadow vertex stage may displace over time (foliage wind):
        // its shadow can never be cached as static (see gather_meshes_()'s static_ok).
        if (!sd.shadow_vert.empty()) animated_shadow_shaders_.insert(sd.name);
        // Tessellated twins: a shader with the stock vertex stage uses the stock evaluation
        // stages; one with its own vertex stage needs its own `tese` (its hook compiled
        // against the evaluation backbone) or draws untessellated -- see SurfaceShaderDesc.
        const std::string tese = !sd.tese.empty() ? sd.tese : (sd.vert.empty() ? "gbuffer.tese" : "");
        const std::string dir_tese = !sd.shadow_tese.empty() ? sd.shadow_tese : (sd.shadow_vert.empty() ? "shadow_depth.tese" : "");
        const std::string cube_tese = !sd.shadow_cube_tese.empty() ? sd.shadow_cube_tese
                                                                   : (sd.shadow_cube_vert.empty() ? "shadow_cube.tese" : "");
        gbuffer_pipeline_->add_variant(
            sd.name,
            config_.shaders(sd.vert.empty() ? "gbuffer.vert" : sd.vert),
            config_.shaders(sd.frag.empty() ? "gbuffer.frag" : sd.frag),
            sd.cull, opt(sd.tesc), opt(tese));
        shadow_pipeline_->add_variant(
            sd.name,
            config_.shaders(sd.shadow_vert.empty() ? "shadow_depth.vert" : sd.shadow_vert),
            config_.shaders(sd.shadow_frag.empty() ? "shadow_depth.frag" : sd.shadow_frag),
            config_.shaders(sd.shadow_cube_vert.empty() ? "shadow_cube.vert" : sd.shadow_cube_vert),
            config_.shaders(sd.shadow_cube_frag.empty() ? "shadow_cube.frag" : sd.shadow_cube_frag),
            sd.cull, opt(dir_tese), opt(cube_tese), opt(sd.shadow_tesc), opt(sd.shadow_cube_tesc));
    }
}

void ToyRenderPipeline::build_sdf_passes_() {
    // --- SDF raymarching system ---
    // Two of the three SDF passes need nothing this pipeline hasn't already built by this
    // point (camera_layout_/shadow_target_/sdf_data_); the
    // third (sdf_forward_pass_) needs transparent_pass_'s render pass and ssr_pass_'s trace
    // sets, both constructed later, so it's built further down alongside transparent_pass_
    // itself (see that call site).
    sdf_gbuffer_pass_ = std::make_unique<coopa::gfx::engine::passes::SdfGBufferPass>(
        device_, gbuffer_target_.render_pass(), *camera_layout_, sdf_data_.layout(),
        config_.shaders("sdf_quad.vert"),
        config_.shaders("sdf_gbuffer.frag"));

    sdf_shadow_pass_ = std::make_unique<coopa::gfx::engine::passes::SdfShadowPass>(
        device_, shadow_target_.dir_render_pass(), shadow_target_.cube_render_pass(),
        sdf_data_.layout(),
        config_.shaders("sdf_shadow.vert"),
        config_.shaders("sdf_shadow.frag"),
        config_.shaders("sdf_shadow_cube.vert"),
        config_.shaders("sdf_shadow_cube.frag"));

}

void ToyRenderPipeline::build_lighting_passes_() {
    // SsaoPass is always constructed: toy_lighting.frag (and the SSR composite, when
    // that's built below) always has a g_ssao binding to fill, toggle or not -- cheap
    // either way, since SsaoPass's ctor already builds a permanent 1x1 neutral texture.
    ssao_pass_ = std::make_unique<coopa::gfx::engine::passes::SsaoPass>(
        device_, allocator_, cmd_pool_, *camera_layout_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("ssao.frag"),
        config_.shaders("ssao_resolve.frag"),
        config_.shaders("ssao_blur.frag"),
        config_.ssao_half_res,
        config_.shaders("ssao_upsample.frag"));
    ssao_pass_->recreate(render_extent_.width, render_extent_.height);
    // update_descriptors() happens in build_ssr_chain_(): the raw pass marches the Hi-Z
    // pyramid, which is not constructed yet at this point in the ctor sequence.

    // Shared per-pixel accumulation count for every stochastic screen-space effect -- SSR,
    // the traced SSGI bounce and the contact shadows all run their running average against
    // it (see TemporalHistoryPass). Always constructed: contact shadows are a runtime
    // toggle, so the buffer has to exist whether or not SSR was built.
    temporal_history_pass_ = std::make_unique<coopa::gfx::engine::passes::TemporalHistoryPass>(
        device_, allocator_, cmd_pool_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("temporal_history.frag"));
    temporal_history_pass_->recreate(render_extent_.width, render_extent_.height);
    temporal_history_pass_->set_depth_image(gbuffer_target_.depth_view_typed());
    temporal_history_pass_->set_velocity_image(gbuffer_target_.g4_view_typed());

    // Contact shadows, as their own pass ahead of lighting. Always constructed:
    // contact_shadows_enabled is a RUNTIME toggle, so there is no construction-time answer to
    // "will this buffer ever be read". While it is off, record_scene_() skips the march and
    // resolve and only clears the output once (ContactShadowPass::clear_output), which keeps
    // the image validly laid out and reading 0 -- and lighting ignores it anyway, since
    // contact_params.x is 0.
    contact_shadow_pass_ = std::make_unique<passes::ContactShadowPass>(
        device_, allocator_, *camera_layout_, *light_layout_,
        render_extent_.width, render_extent_.height,
        config_.shaders("fullscreen.vert"),
        config_.shaders("contact_shadow.frag"),
        // The SSR chain's temporal resolve, reused verbatim -- see ContactShadowPass's doc.
        config_.shaders("contact_shadow_resolve.frag"));
    contact_shadow_pass_->update_descriptors(
        gbuffer_target_.g0_view_typed(), gbuffer_target_.g1_view_typed(),
        gbuffer_target_.g2_view_typed(), gbuffer_target_.depth_view_typed(),
        temporal_history_pass_->count_view_typed(0), temporal_history_pass_->count_view_typed(1),
        temporal_history_pass_->sampler());

    // toy_lighting_pass_'s one app-supplied extra set (set 4): the resolved occlusion the
    // directional term max()-combines in. An ExtraSets rather than a sixth binding on
    // gfxcoopa's own G-buffer set, so DeferredLightingPass's layout -- shared with every
    // other consumer of that library -- is untouched.
    // The physical sky's tables and cloud layer (render sky_model / clouds). Always
    // constructed and bound below, so both switch live; nothing records while they are off.
    // GPU skinning (render.skinning). Startup-fixed; the CPU path needs nothing here.
    if (config_.skinning == "gpu" && device_.supports_compute()) {
        skinning_pass_ = std::make_unique<passes::SkinningPass>(device_, config_.shaders("skin.comp"));
    }
    sky_atmosphere_pass_ = std::make_unique<passes::SkyAtmospherePass>(
        device_, allocator_, config_.shaders("fullscreen.vert"), config_.shaders("sky_transmittance.frag"),
        config_.shaders("sky_multiscatter.frag"), config_.shaders("sky_view.frag"));
    sky_cloud_pass_ = std::make_unique<passes::SkyCloudPass>(
        device_, allocator_, render_extent_.width, render_extent_.height, kCameraFrames, *camera_layout_, *light_layout_,
        config_.shaders("fullscreen.vert"), config_.shaders("sky_clouds.frag"), config_.shaders("sky_cloud_resolve.frag"),
        config_.shaders("sky_cloud_copy.frag"), config_.shaders("sky_cloud_weather.comp"),
        config_.shaders("sky_cloud_shape.comp"), config_.shaders("sky_cloud_detail.comp"),
        config_.shaders("sky_cloud_mip2d.comp"), config_.shaders("sky_cloud_mip3d.comp"));
    sky_cloud_pass_->set_inputs(sky_atmosphere_pass_->transmittance_view(), gbuffer_target_.g1_view_typed(),
                                gbuffer_target_.g2_view_typed());

    // Bindings 1-3 are the physical sky's inputs (toy_lighting.frag's sky branch).
    contact_extra_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
        coopa::gfx::pipeline::DescriptorLayoutBuilder()
            .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(3, coopa::gfx::ShaderStage::Fragment)
            .build(device_));
    contact_extra_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
        coopa::gfx::pipeline::DescriptorPoolBuilder()
            .add_sets(*contact_extra_layout_, 1).build(device_));
    contact_extra_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(
        device_, *contact_extra_pool_, *contact_extra_layout_);
    contact_extra_set_->bind_image(0, contact_shadow_pass_->output_view_typed(),
                                   contact_shadow_pass_->sampler());
    contact_extra_set_->bind_image(1, sky_atmosphere_pass_->transmittance_view(), sky_atmosphere_pass_->sampler());
    contact_extra_set_->bind_image(2, sky_atmosphere_pass_->sky_view_view(), sky_atmosphere_pass_->sampler());
    contact_extra_set_->bind_image(3, sky_cloud_pass_->output_view(), sky_cloud_pass_->sampler());

    // ssao_enabled is a load-time config value (no live reload), so which image to bind is
    // decided once here rather than every frame -- see the update_descriptors comment above
    // for why a per-frame rebind would be unsafe anyway.
    const VkImageView ssao_view = ssao_source_view_();

    // No ExtraSets (toyengine has neither GI nor reflection probes to plumb through), so
    // this collapses to the {camera=0, light=1, shadow=2, gbuffer=3} layout --
    // gbuffer_set_index_ is derived from the extras, not hardcoded, so this stays correct
    // if extras are ever added (see gfxcoopa's deferred_lighting_pass.h).
    coopa::gfx::engine::passes::ExtraSets lighting_extra;
    lighting_extra.layouts = {contact_extra_layout_.get()};
    lighting_extra.bind = [this](coopa::gfx::command::CommandBuffer& cmd, uint32_t first_set) {
        cmd.bind_descriptor_set(*contact_extra_set_, first_set);
    };

    toy_lighting_pass_ = std::make_unique<coopa::gfx::engine::passes::DeferredLightingPass>(
        device_, offscreen_target_.render_pass_object(), *camera_layout_, *light_layout_,
        *shadow_layout_, linear_sampler_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("toy_lighting.frag"),
        lighting_extra,
        std::vector<coopa::gfx::pipeline::PushConstantRange>{
            {coopa::gfx::ShaderStage::Fragment, 0, sizeof(ToyLightingPushConstants)}});
    toy_lighting_pass_->set_gbuffer_images(
        gbuffer_target_.g0_view_typed(), gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed(),
        gbuffer_target_.g3_view_typed(), linear_sampler_);
    toy_lighting_pass_->set_ssao_image(ssao_view, ssao_pass_->sampler().handle());
}

void ToyRenderPipeline::build_ssr_passes_() {
    // Built unconditionally -- roughly 2MB of screen-sized targets at this internal
    // resolution -- so ssr_enabled stays a runtime flag that render() re-reads every frame to
    // decide whether to execute and composite them.
    //
    // Hi-Z depth pyramid plus prefiltered scene-colour mip chain, both consumed by ssr_pass_'s
    // raymarch and cone trace. HiZPass::execute() also performs the gbuffer-depth
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL transition that
    // transition_gbuffer_depth_to_shader_read_() does when SSR is off; render() must call
    // exactly one of the two.
    hiz_pass_ = std::make_unique<coopa::gfx::engine::passes::HiZPass>(
        device_, allocator_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("hiz_downsample.frag"));
    hiz_pass_->recreate(render_extent_.width, render_extent_.height);

    // The SSAO march's own prefiltered pyramid. Chain length is exactly what the march's
    // pixel-radius clamp can reach: ssao.frag selects mip floor(log2(d_px)) - 2 with
    // d_px <= ssao_max_radius_px, so deeper levels would never be sampled. Level 0 is
    // not rendered (external_level0): it was a texelFetch copy of the depth buffer, so
    // ssao.frag reads the depth buffer there and this chain holds levels 1..N.
    const uint32_t ao_pyramid_levels = 1u + static_cast<uint32_t>(std::max(
        0.0, std::floor(std::log2(std::max(config_.ssao_max_radius_px, 1.0f))) - 2.0));
    ao_depth_pyramid_pass_ = std::make_unique<coopa::gfx::engine::passes::HiZPass>(
        device_, allocator_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("ao_depth_downsample.frag"),
        ao_pyramid_levels, /*external_level0=*/true);
    ao_depth_pyramid_pass_->recreate(render_extent_.width, render_extent_.height);

    // Bound once: bind_image() semantics, and every view here is startup-fixed
    // (render_extent_ never changes). Deferred from the SSAO construction site above
    // because the raw pass's set includes the AO depth pyramid built just now.
    // A one-level chain (ssao_max_radius_px < 8) renders nothing and owns no image; the
    // march then never leaves level 0, so the depth view stands in for the pyramid.
    ssao_pass_->update_descriptors(gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed(),
                                   ao_depth_pyramid_pass_->max_mip_level() > 0
                                       ? ao_depth_pyramid_pass_->full_hiz_view_typed()
                                       : gbuffer_target_.depth_view_typed(),
                                   gbuffer_target_.depth_view_typed(),
                                   ao_depth_pyramid_pass_->sampler(),
                                   linear_sampler_,
                                   // Per-object motion vectors: the resolve follows moving
                                   // objects instead of reprojecting camera-only.
                                   gbuffer_target_.g4_view_typed());

    scene_color_mip_pass_ = std::make_unique<coopa::gfx::engine::passes::SceneColorMipPass>(
        device_, allocator_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("scene_color_downsample.frag"));
    scene_color_mip_pass_->recreate(render_extent_.width, render_extent_.height);

    // No ExtraSets (toyengine has neither GI nor reflection probes to plumb through).
    // half_res follows config ssr_half_res: at a full-HD internal resolution the trace,
    // resolve and blur of both chains are worth tracing at a quarter of the pixels, and
    // the composite's depth/normal-aware upsample keeps silhouettes sharp.
    ssr_pass_ = std::make_unique<coopa::gfx::engine::passes::SsrPass>(
        device_, allocator_, *camera_layout_,
        render_extent_.width, render_extent_.height,
        config_.shaders("fullscreen.vert"),
        config_.shaders("ssr.frag"),
        config_.shaders("fullscreen.vert"),
        config_.shaders("ssr_composite.frag"),
        config_.shaders("fullscreen.vert"),
        config_.shaders("ssr_resolve.frag"),
        /*half_res=*/config_.ssr_half_res,
        coopa::gfx::engine::passes::ExtraSets{},
        // Spatial denoise for the SSR buffer (bilateral blur, same technique as
        // SsaoPass's own ssao_blur.frag) -- addresses hit/miss noise at reflection
        // boundaries that temporal accumulation alone doesn't fully resolve, especially
        // under continuous camera motion (this scene's auto-rotating orbit camera).
        config_.shaders("fullscreen.vert"),
        config_.shaders("ssr_blur.frag"),
        // Traced-SSGI stage (see ssgi.frag / the ctor's ssgi_frag_spv doc):
        // startup-fixed on config_.ssgi_traced, since it constructs a pipeline and
        // binds the composite's u_ssgi_map descriptor.
        config_.ssgi_traced ? config_.shaders("ssgi.frag") : std::string{},
        // SSGI traces at the SSR trace resolution divided by this (startup-fixed: it sizes
        // the SSGI chain's targets).
        static_cast<uint32_t>(std::max(config_.ssgi_resolution_scale, 1)));
    ssr_pass_->update_descriptors(
        gbuffer_target_, hiz_pass_->full_hiz_view_typed(), hiz_pass_->sampler(),
        scene_color_mip_pass_->full_view_typed(), scene_color_mip_pass_->sampler(),
        offscreen_target_.color_view_typed(), linear_sampler_,
        gbuffer_target_.g4_view_typed());
    ssr_pass_->set_ssao_image(ssao_source_view_(), ssao_pass_->sampler().handle());
    // The shared accumulation count both resolve chains average against, in place of the
    // fixed-rate blend that can never converge on a re-jittered trace. Bound once: that pass
    // keeps one stable target image, and rebinding a descriptor per frame is unsafe under this
    // pipeline's frame-overlap model (see the ssao_source_view_() comment).
    ssr_pass_->set_temporal_count_image(temporal_history_pass_->count_view_typed(0),
                                        temporal_history_pass_->count_view_typed(1),
                                        temporal_history_pass_->sampler());
}

void ToyRenderPipeline::build_transparency_passes_() {
    // Refraction's own post-SSR scene-colour chain (see refraction_scene_color_mip_pass_'s
    // own doc for why this must be a separate instance/set, not a second execute() on
    // scene_color_mip_pass_ itself). Always constructed, matching this pipeline's usual
    // policy elsewhere -- render() gates execute() per frame on refraction_this_frame_
    // (refraction + transparency enabled AND a BLEND mesh in view); its image is only ever
    // sampled by the transparent_extra bind lambda just below, which only a BLEND mesh
    // draw reaches, on a frame where that same gate built the chain first.
    refraction_scene_color_mip_pass_ = std::make_unique<coopa::gfx::engine::passes::SceneColorMipPass>(
        device_, allocator_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("scene_color_downsample.frag"));
    refraction_scene_color_mip_pass_->recreate(render_extent_.width, render_extent_.height);

    refraction_scene_color_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
        coopa::gfx::pipeline::DescriptorLayoutBuilder()
            .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
            .build(device_));
    refraction_scene_color_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
        coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*refraction_scene_color_layout_, 1).build(device_));
    refraction_scene_color_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(
        device_, *refraction_scene_color_pool_, *refraction_scene_color_layout_);
    refraction_scene_color_set_->bind_image(0, refraction_scene_color_mip_pass_->full_view(),
                                            refraction_scene_color_mip_pass_->sampler().handle());

    // Sets 3/4/5 are ssr_pass_'s own trace-input sets (G-buffer, Hi-Z, scene-colour mips), so
    // transparent.frag can call gfx_ssr_trace() against the SAME descriptors ssr.frag traces
    // -- giving BLEND geometry real screen-space reflections and an SSGI bounce instead of the
    // flat analytic sky fallback. Set 6 is forward_globals_'s per-frame UBO
    // (forward_globals.h), mesh-only: SDF glass keeps reading its own SdfGlobals.
    //
    // u_scene_color (set 5) comes from refraction's dedicated chain when refraction is active,
    // otherwise from ssr_pass_'s own -- decided ONCE here, since refraction_enabled is
    // startup-fixed (file doc, rule 1). With refraction off the binding is byte-identical to
    // what it would be without this feature.
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

    // transparent.vert is the forward-transparent vertex backbone
    // (gfx/surface/transparent_vs.glsl). extra_pc_bytes is TransparentRefractionPushConstants;
    // the frame-level lighting/SSR data lives in forward_globals_'s UBO (set 6 above).
    transparent_pass_ = std::make_unique<coopa::gfx::engine::passes::TransparentPass>(
        device_, VK_FORMAT_R16G16B16A16_SFLOAT, *camera_layout_, *light_layout_,
        *shadow_layout_,
        config_.shaders("transparent.vert"),
        config_.shaders("transparent.frag"),
        transparent_extra,
        static_cast<uint32_t>(sizeof(TransparentRefractionPushConstants)),
        &material_cache_->layout_object());

    // Register every Transparent-domain derived shader (e.g. water) as a named pipeline
    // variant on the forward transparent pass. (SSR reflects these through the previous
    // frame's final colour, so no separate capture variant is needed.)
    if (device_.supports_tessellation()) {
        transparent_pass_->enable_tessellation(config_.shaders("surface_tess.vert"),
                                               config_.shaders("transparent.tesc"), config_.shaders("transparent.tese"));
    }
    for (const auto& sd : config_.surface_shaders.all()) {
        if (sd.domain != coopa::gfx::pipeline::SurfaceShaderDomain::Transparent) continue;
        const std::string vert_spv = config_.shaders(sd.vert.empty() ? "transparent.vert" : sd.vert);
        // Tessellated twin: see the opaque registration's rule (build_geometry_passes_).
        const std::string tese = !sd.tese.empty() ? sd.tese : (sd.vert.empty() ? "transparent.tese" : "");
        transparent_pass_->add_variant(
            sd.name, vert_spv,
            config_.shaders(sd.frag.empty() ? "transparent.frag" : sd.frag), sd.cull,
            sd.tesc.empty() ? std::string() : config_.shaders(sd.tesc),
            tese.empty() ? std::string() : config_.shaders(tese));
    }

    // Global fog over the opaque scene -- drawn in place inside transparent_pass_'s render
    // pass (LOAD on the HDR image), before translucency; see record_fog_(). Always built,
    // gated per frame on fog_enabled.
    fog_pass_ = std::make_unique<coopa::gfx::engine::passes::FogPass>(
        device_, transparent_pass_->render_pass(), *light_layout_,
        config_.shaders("fullscreen.vert"), config_.shaders("fog.frag"));
    fog_pass_->set_source_images(gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed());

    // The cloud layer over geometry (render clouds): drawn after the translucent pass, inside
    // its render pass. Its flat-look buffers and SkyCloudPass's frame buffers feed the cloud
    // shadow map too.
    cloud_overlay_pass_ = std::make_unique<passes::CloudOverlayPass>(
        device_, allocator_, transparent_pass_->render_pass(), kCameraFrames, *camera_layout_, *light_layout_,
        config_.shaders("fullscreen.vert"), config_.shaders("flat_clouds.frag"), config_.shaders("cloud_composite.frag"));
    cloud_overlay_pass_->set_inputs(gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed(),
                                    sky_cloud_pass_->weather_view(), sky_cloud_pass_->shape_view(),
                                    sky_cloud_pass_->noise_sampler(), sky_cloud_pass_->output_view());
    {
        std::vector<const coopa::gfx::memory::Buffer*> cloud_frames, flat_frames;
        for (uint32_t i = 0; i < kCameraFrames; ++i) {
            cloud_frames.push_back(&sky_cloud_pass_->frame_buffer(i));
            flat_frames.push_back(&cloud_overlay_pass_->flat_buffer(i));
        }
        cloud_shadow_pass_->set_inputs(sky_cloud_pass_->weather_view(), sky_cloud_pass_->shape_view(),
                                       sky_cloud_pass_->detail_view(), sky_cloud_pass_->noise_sampler(),
                                       cloud_frames, flat_frames);
    }

    // SdfForwardPass shares transparent_pass_'s render pass -- render passes only need to be
    // attachment-compatible to back a second pipeline, and sharing lets record_transparent_()
    // draw BLEND meshes and BLEND SDFs inside ONE begin()/end(), switching pipelines per item
    // down a single back-to-front list, so the two composite in correct depth order. Same
    // ExtraSets as transparent_pass_, one set index later (4/5/6), since this pass's own
    // SdfData set occupies 3.
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
        device_, VK_FORMAT_R16G16B16A16_SFLOAT, transparent_pass_->render_pass(),
        *camera_layout_, *light_layout_, *shadow_layout_, sdf_data_.layout(),
        config_.shaders("sdf_quad.vert"),
        config_.shaders("sdf_forward.frag"),
        sdf_forward_extra);

    // Particle quads share the same render pass for the same reason SDFs do: one bracket,
    // one back-to-front list. Set 2 is the Hi-Z pyramid (mip 0 = opaque depth, for soft
    // particles), set 3 the material set (the sprite texture in its albedo slot), set 4 the
    // shadow maps (lit particles receive shadows).
    particle_pass_ = std::make_unique<passes::ParticlePass>(
        device_, allocator_, transparent_pass_->render_pass(),
        *camera_layout_, *light_layout_, ssr_pass_->hiz_layout(), material_cache_->layout_object(),
        *shadow_layout_, config_.shaders("particle.vert"), config_.shaders("particle.frag"));
    // `simulation: gpu` particle systems: compute emit / simulate / sort, drawn by the pass
    // above with draw_indirect() (see passes/gpu_particle_pass.h).
    if (device_.supports_compute()) {
        try {
            gpu_particle_pass_ = std::make_unique<passes::GpuParticlePass>(
                device_, allocator_, [this](const char* name) { return config_.shaders(name); });
            particle_pass_->set_gpu(gpu_particle_pass_.get());
        } catch (const std::exception& e) {
            std::cerr << "[toy::render] GPU particles unavailable: " << e.what() << "\n";
            gpu_particle_pass_.reset();
        }
    }
}

void ToyRenderPipeline::build_post_chain_(coopa::gfx::pipeline::RenderPass& swapchain_pass) {
    // Without SSR, post reads the deferred-lit+sky target; with it, ssr_pass_'s composite
    // output. Chosen once from ssr_enabled's STARTUP value -- rebinding it per frame would
    // need either a wait or a shader-side selector between two permanently-bound views (file
    // doc, rule 1). Fog and translucency are composited into this image in place.
    pre_volumetrics_view_typed_ =
        config_.ssr_enabled ? ssr_pass_->output_view_typed() : offscreen_target_.color_view_typed();

    // Underwater: first in the chain, right after the transparent pass drew into the source
    // (so the water's underside is fogged by its in-water distance like everything else).
    // The global fog has already run by then, over the air part of each ray only (see
    // gfx_fog_eval), so it never washes out the underwater look. Everything downstream
    // reads this output instead.
    if (config_.underwater_enabled) {
        underwater_pass_ = std::make_unique<passes::UnderwaterPass>(
            device_, underwater_target_.render_pass_object(),
            config_.shaders("fullscreen.vert"), config_.shaders("underwater.frag"),
            config_.shaders("underwater_planet.frag"));
        underwater_pass_->set_source_images(pre_volumetrics_view_typed_, gbuffer_target_.g1_view_typed(),
                                            gbuffer_target_.g2_view_typed(), linear_sampler_);
        pre_volumetrics_view_typed_ = underwater_target_.color_view_typed();
    }

    coopa::gfx::TextureView pre_volumetrics_view = pre_volumetrics_view_typed_;

    // Local volumes -- render() checks config_.volumetrics_enabled per frame. Where fog
    // integrates an analytic everywhere-medium in one sample, this raymarches a sparse
    // noise field advected along a wind vector, which is what makes it read as moving
    // air rather than haze (see gfx/volumetrics.glsl's header for the three ideas involved).
    //
    // volumetrics_mode selects how: "froxel" (FroxelVolumetricsPass -- grid inject, per-column
    // integrate, per-pixel apply into volumetrics_target_) or "raymarch" (VolumetricsPass --
    // the march into volumetrics_march_target_ at reduced resolution, then a depth-aware
    // upsample + composite into volumetrics_target_). Only the selected one is built.
    if (froxel_volumetrics_()) {
        coopa::gfx::engine::passes::FroxelVolumetricsPass::Desc fd;
        fd.render_width       = render_extent_.width;
        fd.render_height      = render_extent_.height;
        fd.tile               = std::max<uint32_t>(1u, config_.volumetrics_froxel_tile);
        fd.slices             = std::max<uint32_t>(1u, config_.volumetrics_froxel_slices);
        fd.vert_spv           = config_.shaders("fullscreen.vert");
        fd.inject_frag_spv    = config_.shaders("volumetrics_froxel_inject.frag");
        fd.integrate_partial_frag_spv = config_.shaders("volumetrics_froxel_integrate_partial.frag");
        fd.integrate_frag_spv = config_.shaders("volumetrics_froxel_integrate.frag");
        fd.apply_frag_spv     = config_.shaders("volumetrics_froxel_apply.frag");
        froxel_volumetrics_pass_ = std::make_unique<coopa::gfx::engine::passes::FroxelVolumetricsPass>(
            device_, allocator_, fd, volumetrics_target_.render_pass_object(),
            volumetrics_data_.buffer());
        froxel_volumetrics_pass_->set_source_images(pre_volumetrics_view, gbuffer_target_.g1_view_typed(),
                                                    gbuffer_target_.g2_view_typed(), linear_sampler_);
        froxel_volumetrics_pass_->set_shadow_images(shadow_target_.dir_shadow_view_typed(),
                                                    local_shadow_atlas_->view_typed(), shadow_sampler_);
        froxel_volumetrics_pass_->set_cloud_shadow_image(cloud_shadow_pass_->view(), cloud_shadow_pass_->sampler());
    } else {
    volumetrics_pass_ = std::make_unique<coopa::gfx::engine::passes::VolumetricsPass>(
        device_, volumetrics_march_target_.render_pass_object(),
        volumetrics_target_.render_pass_object(), volumetrics_data_.buffer(),
        config_.shaders("fullscreen.vert"),
        config_.shaders("volumetrics_march.frag"),
        config_.shaders("volumetrics_composite.frag"));
    volumetrics_pass_->set_source_images(pre_volumetrics_view, gbuffer_target_.g1_view_typed(),
                                  gbuffer_target_.g2_view_typed(), linear_sampler_);
    volumetrics_pass_->set_march_result(volumetrics_march_target_.color_view_typed());
    // Shadow maps for the march's in-scatter terms (light shafts) -- same images and
    // compare sampler shadow_set_ binds for the lighting pass, bound once here under
    // the same construction-time rule. Whether they are actually sampled is the
    // RUNTIME volumetrics_shadows_enabled flag, routed through the volumetrics UBO's
    // shadow_params (see update_volumetrics_data_).
    volumetrics_pass_->set_shadow_images(shadow_target_.dir_shadow_view_typed(),
                                         local_shadow_atlas_->view_typed(), shadow_sampler_);
    volumetrics_pass_->set_cloud_shadow_image(cloud_shadow_pass_->view(), cloud_shadow_pass_->sampler());
    }

    // What DOF reads: the final pre-tonemap HDR frame, before any lens effect. Sourcing the
    // full chain here (rather than the pre-SSR image) is what puts SSR reflections, BLEND
    // geometry and fog into the defocus, and transitively into bloom.
    coopa::gfx::TextureView pre_dof_view =
        config_.volumetrics_enabled ? volumetrics_target_.color_view_typed() : pre_volumetrics_view;

    // The image behind pre_dof_view, mirroring the selections above step for step: the final
    // pre-lens HDR frame, which record_post_chain_() copies into the SSR scene-colour chain's
    // mip 0 so the NEXT frame's reflections see transparents, fog and other reflections
    // (Unreal's PrevSceneColor). Startup-fixed like the views it mirrors.
    {
        coopa::gfx::engine::targets::OffscreenTarget* hist =
            config_.ssr_enabled ? &ssr_pass_->composite_target() : &offscreen_target_;
        if (config_.underwater_enabled) hist = &underwater_target_;
        if (config_.volumetrics_enabled) hist = &volumetrics_target_;
        scene_color_history_src_ = hist->color_image_object()->handle();

        // Motion blur works IN PLACE on that same image -- the one DoF, bloom, exposure and
        // stylize are about to bind below -- so motion_blur stays a runtime toggle: off records
        // nothing and every downstream binding is the image the scene chain left. Always built
        // (both tile targets and the colour copy it gathers from), never rebound.
        motion_blur_pass_ = std::make_unique<passes::MotionBlurPass>(
            device_, allocator_, render_extent_.width, render_extent_.height, *hist,
            gbuffer_target_.g4_view_typed(), config_.shaders);
    }

    // Physically-based depth of field (thin-lens CoC -> half-res bokeh gather -> full-res
    // composite). Sized to render_extent_, not the display rect: depth only exists at
    // render_extent_, and DOF is a property of the image being formed rather than a filter
    // over the finished frame the way tilt shift is.
    dof_pass_ = std::make_unique<coopa::gfx::engine::passes::DofPass>(
        device_, allocator_, render_extent_.width, render_extent_.height,
        pre_dof_view, gbuffer_target_.depth_view_typed(),
        linear_sampler_, nearest_sampler_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("dof_coc.frag"),
        config_.shaders("dof_bokeh.frag"),
        config_.shaders("dof_composite.frag"));

    // What BOTH stylize_pass_ and bloom_pass_ read. One local, not two expressions, so
    // the two can never drift apart.
    coopa::gfx::TextureView post_source_view =
        config_.dof_enabled ? dof_pass_->result_view_typed() : pre_dof_view;

    // Independent bloom pyramid (bright-pass threshold -> multi-tap downsample -> tent-filter
    // upsample). Its result is BOUND below only when bloom_enabled was set at construction: an
    // unbound-but-never-executed target would leave that binding on an image still in
    // VK_IMAGE_LAYOUT_UNDEFINED. Every descriptor here is bound once and never rebinds, so
    // unlike scene_color_mip_pass_ this needs no per-frame wait.
    bloom_pass_ = std::make_unique<coopa::gfx::engine::passes::BloomPass>(
        device_, allocator_, render_extent_.width, render_extent_.height,
        post_source_view, linear_sampler_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("bloom_prefilter.frag"),
        config_.shaders("bloom_downsample.frag"),
        config_.shaders("bloom_upsample.frag"));

    // Auto-exposure meters the SAME image the stylize pass tonemaps (post_source_view),
    // so the adaptation is measured against exactly what the viewer ends up seeing --
    // metering the pre-DOF/pre-bloom image instead would chase a brightness the final
    // frame never has. Constructed only when enabled: the pass owns two targets, and
    // an unexecuted target would leave binding 5 on an image in UNDEFINED layout (the
    // same rule bloom_pass_'s binding documents just above).
    if (config_.auto_exposure_enabled) {
        exposure_pass_ = std::make_unique<coopa::gfx::engine::passes::ExposurePass>(
            device_, allocator_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("exposure.frag"));
        exposure_pass_->set_source_image(post_source_view);
    }

    stylize_pass_ = std::make_unique<coopa::gfx::engine::passes::StylizePass>(
        device_, post_target_.render_pass_object(),
        config_.shaders("fullscreen.vert"),
        config_.shaders("stylize.frag"));
    // bloom_result is bound only when config_.bloom_enabled was true at construction. The
    // nullptr sampler falls back to a harmless self-bind of scene_color (see
    // StylizePass::set_source_images), and bloom_intensity is forced to 0 on those
    // runs anyway (see the per-frame push-constant fill below).
    stylize_pass_->set_source_images(
        post_source_view,
        gbuffer_target_.depth_view_typed(), gbuffer_target_.g1_view_typed(),
        palette_lut_.view_typed(), linear_sampler_, nearest_sampler_,
        config_.bloom_enabled ? bloom_pass_->result_view_typed() : coopa::gfx::TextureView{},
        config_.bloom_enabled ? &linear_sampler_ : nullptr,
        // Same conditional-binding rule as bloom above, for the same reason.
        exposure_pass_ ? exposure_pass_->output_view_typed() : coopa::gfx::TextureView{},
        exposure_pass_ ? &exposure_pass_->sampler() : nullptr,
        // The grading LUT is always a valid texture (a 1x1 dummy when no path is
        // configured), so unlike the two above it binds unconditionally -- grading_size
        // 0 is what disables the lookup.
        grading_lut_.view_typed(), &grading_lut_.sampler_object(),
        // The per-material outline mask: the G-buffer's emissive attachment, whose alpha
        // carries each material's toon code. Bound unconditionally (it is always a real,
        // rendered attachment) -- outline_mode is what decides whether it is read.
        gbuffer_target_.g3_view_typed(), &nearest_sampler_);

    // Debug view: replaces post_target_'s draw with a raw intermediate-buffer readout --
    // see ToyRenderConfig::debug_view / the DebugView enum for the full option list, and
    // record_post_chain_() for how the choice among this / stylize_pass_ / DOF-CoC /
    // volumetrics-density is made every frame. Always constructed (same always-built,
    // runtime-gated policy as volumetrics_pass_ above), so debug_view stays a
    // runtime field.
    //
    // Shares gfxcoopa's DeferredLightingPass with toy_lighting_pass_ above -- same 3
    // leading sets (camera/light/shadow) and the same owned 5-binding G-buffer+SSAO set,
    // via debug_view.frag, which declares an identical descriptor contract to
    // toy_lighting.frag's so "direct"/"indirect"/"shadows"/"contact_shadows"/"ssao" are
    // computed by the exact same functions the shipped lighting term uses. The one extra
    // set (SSR reflection / traced-SSGI / G-buffer depth) is what toy_lighting_pass_
    // doesn't need and this shader does, for the "ssr"/"ssr_confidence"/"ssgi"/"depth"
    // channels.
    debug_view_extra_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
        coopa::gfx::pipeline::DescriptorLayoutBuilder()
            .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(3, coopa::gfx::ShaderStage::Fragment)
            .combined_sampler(4, coopa::gfx::ShaderStage::Fragment)   // G4 velocity
            .build(device_));
    debug_view_extra_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
        coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*debug_view_extra_layout_, 1).build(device_));
    debug_view_extra_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(
        device_, *debug_view_extra_pool_, *debug_view_extra_layout_);
    // ssr_enabled is startup-fixed (file doc, rule 1): decided once here, exactly like
    // ssao_view above -- ssr_pass_ is always CONSTRUCTED (see its own file doc) but its
    // targets stay in VK_IMAGE_LAYOUT_UNDEFINED until execute() has run at least once,
    // which never happens on a frame where config_.ssr_enabled is false. Binding its real
    // views in that case would be a hard Vulkan validation error even on a channel that
    // never samples them at runtime -- so both fall back to the same permanent 1x1
    // neutral texture SsrPass itself uses for its own never-enabled fallbacks.
    debug_view_extra_set_->bind_image(0,
        config_.ssr_enabled ? ssr_pass_->reflection_view_typed() : ssr_pass_->zero_view_typed(),
        linear_sampler_);
    debug_view_extra_set_->bind_image(1,
        config_.ssr_enabled ? ssr_pass_->ssgi_view_typed() : ssr_pass_->zero_view_typed(),
        linear_sampler_);
    debug_view_extra_set_->bind_image(2, gbuffer_target_.depth_view_typed(), nearest_sampler_);
    debug_view_extra_set_->bind_image(3, contact_shadow_pass_->output_view_typed(),
                                      contact_shadow_pass_->sampler());
    debug_view_extra_set_->bind_image(4, gbuffer_target_.g4_view_typed(), nearest_sampler_);

    coopa::gfx::engine::passes::ExtraSets debug_view_extra;
    debug_view_extra.layouts = {debug_view_extra_layout_.get()};
    debug_view_extra.bind = [this](coopa::gfx::command::CommandBuffer& cmd, uint32_t first_set) {
        cmd.bind_descriptor_set(*debug_view_extra_set_, first_set);
    };

    debug_view_pass_ = std::make_unique<coopa::gfx::engine::passes::DeferredLightingPass>(
        device_, post_target_.render_pass_object(), *camera_layout_, *light_layout_,
        *shadow_layout_, linear_sampler_,
        config_.shaders("fullscreen.vert"),
        config_.shaders("debug_view.frag"),
        debug_view_extra,
        std::vector<coopa::gfx::pipeline::PushConstantRange>{
            {coopa::gfx::ShaderStage::Fragment, 0, sizeof(DebugViewPushConstants)}});
    debug_view_pass_->set_gbuffer_images(
        gbuffer_target_.g0_view_typed(), gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed(),
        gbuffer_target_.g3_view_typed(), linear_sampler_);
    debug_view_pass_->set_ssao_image(ssao_source_view_(), ssao_pass_->sampler().handle());

    // Anti-aliasing. Conditionally constructed, unlike the passes above: aa_mode == "off"
    // must allocate nothing and leave every downstream binding as it was, so the mode is
    // checked here rather than per frame. All three passes are built together and share
    // aa_target_, so switching AMONG fxaa/smaa/taa at runtime is safe; going to or from
    // "off" is not. Each binds once, so no per-frame wait is needed.
    if (config_.aa_mode != "off") {
        // Colour-only: FXAA, SMAA's final stage and TAA's present all draw a fullscreen
        // triangle into it, and nothing reads a depth of its own.
        aa_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
            device_, allocator_, render_extent_.width, render_extent_.height,
            coopa::gfx::Format::RGBA8_Unorm, coopa::gfx::engine::targets::kColorOnly);

        fxaa_pass_ = std::make_unique<coopa::gfx::engine::passes::FxaaPass>(
            device_, aa_target_->render_pass_object(),
            config_.shaders("fullscreen.vert"),
            config_.shaders("fxaa.frag"));
        fxaa_pass_->set_source_image(post_target_.color_image_object()->view_typed(), linear_sampler_);

        smaa_pass_ = std::make_unique<coopa::gfx::engine::passes::SmaaPass>(
            device_, allocator_, aa_target_->render_pass_object(), cmd_pool_,
            render_extent_.width, render_extent_.height, linear_sampler_, config_.shaders);
        smaa_pass_->set_source_image(post_target_.color_image_object()->view_typed(), linear_sampler_);

        taa_pass_ = std::make_unique<coopa::gfx::engine::passes::TaaPass>(
            device_, allocator_, aa_target_->render_pass_object(),
            linear_sampler_, nearest_sampler_,
            render_extent_.width, render_extent_.height,
            config_.shaders("taa.vert"), config_.shaders("taa.frag"),
            config_.shaders("taa_present.frag"));
        // The depth buffer feeds the resolve's camera reprojection (the sky, and the
        // fallback); like DoF's and the stylize pass's bindings above, it is the G-buffer
        // depth at render_extent_. The velocity attachment gives every surface its own
        // per-object motion vector, so moving objects stop ghosting.
        // The TAA reactive mask: `reactive` particles (rain, snow, sparks) draw their coverage
        // into it right before the resolve (record_reactive_mask_()), and the resolve trusts
        // the current frame there -- a fast particle has no motion vector, so history would
        // otherwise smear it into streaks across the screen.
        reactive_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
            device_, allocator_, render_extent_.width, render_extent_.height,
            coopa::gfx::Format::R8_Unorm, coopa::gfx::engine::targets::kColorOnly);
        if (particle_pass_) particle_pass_->build_reactive(reactive_target_->render_pass());
        taa_pass_->set_source_images(post_target_.color_image_object()->view_typed(),
                                     gbuffer_target_.depth_view_typed(),
                                     gbuffer_target_.g4_view_typed(),
                                     reactive_target_->color_view_typed());
    }

    // Single source of truth for everything downstream of post_target_/aa_target_ -- same
    // pattern as post_source_view/pre_volumetrics_view_typed_ above. aa_target_ is null whenever
    // aa_mode == "off", so this collapses to post_target_ exactly as before AA existed.
    coopa::gfx::TextureView display_source_view =
        aa_target_ ? aa_target_->color_view_typed()
                   : post_target_.color_image_object()->view_typed();
    // Cached for rebuild_overlay_chain_(), which reruns on every swapchain resize and has
    // to make the identical choice -- the resize path already had one copy of this
    // selection to keep in sync (see render()'s tilt-shift rebuild) and a second
    // hand-written copy is exactly how those drift apart.
    display_source_view_ = display_source_view;

    // Diorama tilt-shift blur, sized to the DISPLAY (letterboxed) rect rather than
    // render_extent_: it is a lens effect over the final image, so it must run after the
    // upscale. It folds that nearest upscale into its own horizontal stage, which is why
    // ui_composite_pass_ samples its result 1:1 when it is enabled.
    tilt_shift_pass_ = std::make_unique<coopa::gfx::engine::passes::TiltShiftPass>(
        device_, allocator_, upscaled_extent_.w, upscaled_extent_.h,
        display_source_view, display_sampler_(),
        config_.shaders("fullscreen.vert"),
        config_.shaders("tilt_shift.frag"));

    upscale_pass_ = std::make_unique<passes::UpscalePass>(
        device_, swapchain_pass,
        config_.shaders("fullscreen.vert"),
        config_.shaders("upscale.frag"));
    // upscale_pass_ is a 1:1 blit of overlay_target_ (already swapchain-sized, letterbox bars
    // included) into the swapchain; the nearest upscale happens one stage earlier, inside
    // ui_composite_pass_, so the world UI can be composited after the display-space effects
    // while still landing on the low-res pixel grid. rebuild_overlay_chain_() binds the
    // source, since overlay_target_ follows the window.

    // Built against post_target_'s render pass, NOT the swapchain's: pipeline::RenderPass
    // hardcodes LOAD_OP_CLEAR, so this draws as a guest inside post_target_'s already-open
    // bracket rather than reopening it. That also makes the overlay visible to
    // low_res_color_image()/final_color_image(), which is how every screenshot and headless
    // test reads a frame back -- nothing reads the swapchain image itself. The cost is that it
    // lands pre-upscale and picks up AA and tilt shift.
    //
    // debug_view is checked per frame, not here. Its one descriptor (the G-buffer depth that
    // occluded lines -- the editor grid -- compare against) is bound once below: gbuffer_target_
    // is startup-sized and never recreated.
    debug_line_pass_ = std::make_unique<passes::DebugLinePass>(
        device_, allocator_, post_target_.render_pass_object(),
        config_.shaders("debug_line.vert"),
        config_.shaders("debug_line.frag"));
    debug_line_pass_->set_scene_depth(gbuffer_target_.depth_view_typed(), nearest_sampler_,
                                      render_extent_.width, render_extent_.height);
    // Same guest arrangement, for BLEND meshes in the editor's viewport shading modes.
    transparent_preview_pass_ = std::make_unique<passes::TransparentPreviewPass>(
        device_, post_target_.render_pass_object(), *camera_layout_, material_cache_->layout_object(),
        config_.shaders("transparent.vert"), config_.shaders("transparent_preview.frag"));
    transparent_preview_pass_->set_scene_depth(gbuffer_target_.depth_view_typed(), nearest_sampler_,
                                               render_extent_.width, render_extent_.height);

    // See-through silhouettes. Built only when asked, unlike the two guests above: it owns a
    // full-resolution mask target, and the whole point of the feature's design is that a scene
    // that does not want it pays nothing -- no image, no clear, no draw. The COMPOSITE is
    // another post_target_ guest, drawn after stylize so the authored colour is not pushed
    // through ACES, the grading LUT and the palette quantizer; the MASK has its own target and
    // is recorded earlier, at the top of the post chain.
    if (config_.xray_enabled) {
        xray_pass_ = std::make_unique<passes::XrayPass>(
            device_, allocator_, post_target_.render_pass_object(), *camera_layout_,
            render_extent_.width, render_extent_.height,
            config_.shaders("transparent.vert"),
            config_.shaders("xray_mask.frag"),
            config_.shaders("fullscreen.vert"),
            config_.shaders("xray_composite.frag"));
        xray_pass_->set_scene_depth(gbuffer_target_.depth_view_typed(), nearest_sampler_);
    }
}

void ToyRenderPipeline::build_world_ui_descriptor_() {
    // --- World-space UI: the scene-depth descriptor ---
    // The world UI draws into ui_world_target_, a transparent layer ui_composite_pass_ puts
    // back over the frame after AA and tilt shift have run -- not into post_target_, which
    // would feed it through both.
    //
    // Only the descriptor set is built here; the PASS is built by rebuild_world_ui_pass_(),
    // because it is bound to ui_world_target_'s render pass and that target follows the window.
    //
    // A world canvas can ask to be OCCLUDED by scene geometry, and cannot use a hardware depth
    // test to do it: post_target_'s own D32 attachment is cleared at begin() and only ever
    // written by a fullscreen triangle, so it holds nothing about the scene. The real scene
    // depth is the G-buffer's, which is sampled-capable and already in SHADER_READ_ONLY_OPTIMAL
    // by this point, so it is handed over as a sampled texture at set 1 and compared per
    // fragment (see ui_world_occlude.glsl).
    if (config_.world_ui_enabled) {
        world_ui_depth_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .build(device_));
        world_ui_depth_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder()
                .add_sets(*world_ui_depth_layout_, 1).build(device_));
        world_ui_depth_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(
            device_, *world_ui_depth_pool_, *world_ui_depth_layout_);
        // Bound once (file doc, rule 1). Safe because gbuffer_target_ is a by-value member sized
        // to the startup-fixed render_extent_ and never recreated. nearest_sampler_, not linear_:
        // D32_Sfloat is not guaranteed to support linear filtering.
        world_ui_depth_set_->bind_image(0, gbuffer_target_.depth_view_typed(), nearest_sampler_);
    }

    // Builds ui_world_target_ + world_ui_pass_ + overlay_target_ + ui_composite_pass_ +
    // screen_ui_pass_ and points upscale_pass_ at the result. Last, because it reads
    // tilt_shift_pass_, upscaled_extent_ and world_ui_depth_layout_, and binds
    // upscale_pass_'s source -- all of which must already exist.
}

void ToyRenderPipeline::rebuild_overlay_chain_(uint32_t w, uint32_t h) {
    rebuild_display_layer_();

    // Retired, not destroyed: this frame's DrawLists still hold screen_ui_pass_'s white view.
    retired_ui_.screen_pass    = std::move(screen_ui_pass_);
    retired_ui_.overlay_target = std::move(overlay_target_);
    overlay_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
        device_, allocator_, w, h, coopa::gfx::Format::RGBA8_Unorm);
    overlay_extent_ = VkExtent2D{w, h};

    ui_composite_pass_ = std::make_unique<passes::UiCompositePass>(
        device_, overlay_target_->render_pass_object(),
        config_.shaders("fullscreen.vert"),
        config_.shaders("ui_composite.frag"));
    bind_composite_inputs_();

    if (config_.screen_ui_enabled) {
        // No ExtraSets, unlike world_ui_pass_: a screen-space overlay has nothing to be
        // occluded by, so there is no scene-depth compare and no set 1.
        screen_ui_pass_ = std::make_unique<coopa::ui::UiPass>(
            device_, allocator_, cmd_pool_, overlay_target_->render_pass_object(),
            config_.shaders("ui.vert"),
            config_.shaders("ui_quad.frag"),
            config_.shaders("ui_text.frag"));
    }

    upscale_pass_->set_source_image(overlay_target_->color_view_typed(), nearest_sampler_);
}

void ToyRenderPipeline::rebuild_display_layer_() {
    // --- The world-UI layer, at the DISPLAY rect ---
    // The pass is retired together with the target it was built against (TexturedQuad2DPass
    // stores that target's RenderPass&), so the reference never dangles -- and retired
    // rather than destroyed because this frame's world canvases emitted against its white
    // view. (device_.wait_idle() is the caller's job and has already run.)
    retired_ui_.world_pass   = std::move(world_ui_pass_);
    retired_ui_.world_target = std::move(ui_world_target_);
    ui_world_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
        device_, allocator_, upscaled_extent_.w, upscaled_extent_.h,
        coopa::gfx::Format::RGBA8_Unorm);
    ui_world_layer_clear_ = false;   // a fresh image is UNDEFINED until its first write
    rebuild_world_ui_pass_();
}

void ToyRenderPipeline::bind_composite_inputs_() {
    // Startup-fixed source selection, the same caveat post_source_view/pre_volumetrics_view_typed_
    // carry: flipping config_.tilt_shift_enabled without a pipeline rebuild would leave
    // this reading a target tilt_shift_pass_ never wrote this frame.
    // With tilt shift on, its result is ALREADY at upscaled_extent_ and this fetch is 1:1,
    // so NEAREST is an exact texel read. With it off, THIS pass is the scene-to-display
    // resample, and the filter has to follow the direction of that scale -- see
    // ToyRenderConfig::upscale_filter.
    ui_composite_pass_->set_base_image(
        config_.tilt_shift_enabled ? tilt_shift_pass_->result_view_typed()
                                   : display_source_view_,
        config_.tilt_shift_enabled ? nearest_sampler_ : display_sampler_());
    // NEAREST is an EXACT texel fetch here, not a filter choice: the layer is built at
    // upscaled_extent_ and the composite draws over a viewport of exactly that size, so
    // fullscreen.vert's in_uv.x = (x + 0.5)/w gives floor(uv * w) == x. Bilinear would be
    // equally exact at 1:1 but would silently turn into a blur the moment the two sizes
    // drift by a pixel, which is exactly the failure this binding should not hide.
    ui_composite_pass_->set_ui_image(ui_world_target_->color_view_typed(), nearest_sampler_);
}

void ToyRenderPipeline::rebuild_world_ui_pass_() {
    if (!config_.world_ui_enabled) return;

    coopa::gfx::engine::passes::ExtraSets world_ui_extra;
    world_ui_extra.layouts = {world_ui_depth_layout_.get()};
    // Capturing `this` is safe only because ToyRenderPipeline is non-copyable and
    // non-movable (see the deleted copy ctor, and the unique_ptr members that make a move
    // meaningless), so the callback can never outlive its target. first_set comes from the
    // pass, never hardcoded -- see ExtraSets' own doc.
    world_ui_extra.bind = [this](coopa::gfx::command::CommandBuffer& cmd, uint32_t first_set) {
        cmd.bind_descriptor_set(*world_ui_depth_set_, first_set);
    };

    world_ui_pass_ = std::make_unique<coopa::ui::UiWorldPass>(
        device_, allocator_, cmd_pool_, ui_world_target_->render_pass_object(),
        config_.shaders("ui_world.vert"),
        config_.shaders("ui_world_quad.frag"),
        config_.shaders("ui_world_text.frag"),
        std::move(world_ui_extra));
}

LetterboxRect ToyRenderPipeline::handle_resize_() {
    // --- Resize: everything sized to the window, rebuilt before anything else touches it ---
    //
    // This runs FIRST in render(), and the position is load-bearing rather than tidy.
    // rebuild_overlay_chain_() replaces world_ui_pass_ and screen_ui_pass_, and a UI pass
    // carries per-frame state that only the canvas gather below installs: its registered
    // textures, its text-atlas marks, and the streaming-buffer capacity begin_frame() sizes.
    // Rebuild after the gather and this frame draws every texture as the 1x1 white fallback,
    // every glyph atlas through the RGBA "quad" variant, and uploads geometry into a buffer
    // that was never sized for it -- Buffer::upload is an unchecked memcpy. (This was a live
    // bug for screen_ui_pass_ while the rebuild sat ~400 lines further down.)
    //
    // The two triggers are genuinely different quantities and BOTH are needed:
    //   * the swapchain extent sizes overlay_target_;
    //   * the letterbox rect sizes ui_world_target_ and tilt_shift_pass_.
    // Under upscale_mode == "integer" the rect snaps to render_extent_ * floor(scale), so the
    // window can change size -- and the swapchain with it -- while the rect does not. Keying
    // only on the rect would leave overlay_target_ at the old window size; keying only on the
    // swapchain would be correct here but would rebuild TiltShiftPass for nothing, hence the
    // inner guard.
    //
    // upscaled_extent_ is assigned before the rebuilds because rebuild_overlay_chain_() reads
    // it to size the world-UI layer. upscale_pass_ needs no rebuild: it is built against
    // swapchain_pass, which Renderer::recreate_framebuffers() keeps compatible across a resize.
    const LetterboxRect letterbox = display_rect_for(swapchain_.extent().width, swapchain_.extent().height);

    // Last resize's retired UI passes: the frame that drew with them was submitted, and is
    // done once the device idles.
    if (!retired_ui_.empty()) {
        device_.wait_idle();
        retired_ui_ = RetiredUi{};
    }

    const VkExtent2D swapchain_extent = swapchain_.extent();
    const bool swapchain_changed = swapchain_extent.width  != overlay_extent_.width ||
                                   swapchain_extent.height != overlay_extent_.height;
    const bool letterbox_changed = letterbox.w != upscaled_extent_.w ||
                                   letterbox.h != upscaled_extent_.h;
    // The nonzero guard covers a minimized window, which reports 0x0 -- not a valid image
    // size, and compute_display_rect() on it yields a degenerate 1x1 rect.
    if (swapchain_extent.width != 0 && swapchain_extent.height != 0 &&
        (swapchain_changed || letterbox_changed)) {
        device_.wait_idle(); // old targets' images/descriptors may still be in flight
        upscaled_extent_ = letterbox;
        if (letterbox_changed && config_.tilt_shift_enabled) {
            // display_source_view_ is the ctor's cached selection (post_target_, or
            // aa_target_ once AA is on). Re-deriving the source by hand here instead is
            // how a resize would silently revert to the un-AA'd image.
            tilt_shift_pass_ = std::make_unique<coopa::gfx::engine::passes::TiltShiftPass>(
                device_, allocator_, upscaled_extent_.w, upscaled_extent_.h,
                display_source_view_, display_sampler_(),
                config_.shaders("fullscreen.vert"), config_.shaders("tilt_shift.frag"));
        }
        // Last, and it binds the composite's base image itself -- which is why the tilt-shift
        // rebuild above has to come first. A display-rect change alone (an editor viewport
        // panel resized) leaves the swapchain-sized half -- overlay target, screen-UI pass --
        // alone: rebuilding the screen-UI pass would drop the editor UI's registered textures
        // on every splitter drag.
        if (swapchain_changed) {
            rebuild_overlay_chain_(swapchain_extent.width, swapchain_extent.height);
        } else {
            rebuild_display_layer_();
            bind_composite_inputs_();
        }
    }
    return letterbox;
}

bool ToyRenderPipeline::trace_inputs_need_rebind_() const {
    const coopa::gfx::TextureView depth = gbuffer_target_.depth_view_typed();
    if (hiz_pass_ && hiz_pass_->needs_descriptor_update(depth)) return true;
    if (config_.ssao_enabled && ao_depth_pyramid_pass_ &&
        ao_depth_pyramid_pass_->needs_descriptor_update(depth)) return true;
    if (scene_color_mip_pass_ &&
        scene_color_mip_pass_->needs_descriptor_update(offscreen_target_.color_view_typed())) return true;
    // Gated on the per-frame flag, not the config pair: needs_descriptor_update() stays true
    // until the chain's first execute(), which never comes on a scene without BLEND meshes --
    // the config gate alone would wait_idle() every frame there.
    if (refraction_this_frame_ && refraction_scene_color_mip_pass_) {
        const coopa::gfx::TextureView refraction_source =
            (config_.refraction_include_reflections && config_.ssr_enabled)
                ? ssr_pass_->output_view_typed() : offscreen_target_.color_view_typed();
        if (refraction_scene_color_mip_pass_->needs_descriptor_update(refraction_source)) return true;
    }
    return false;
}
} // namespace render
} // namespace toy
