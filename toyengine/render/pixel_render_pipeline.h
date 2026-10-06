/**
 * @file pixel_render_pipeline.h
 * @brief Orchestrates the low-resolution deferred pixel-art frame graph.
 *
 * Owns every target, UBO, descriptor set and pass, and records the whole frame into a single
 * command buffer through Renderer::begin_frame()'s pre_pass_fn seam.
 *
 * ## Two rules govern almost every design choice in this file
 *
 * **1. Descriptors are bound once, at construction.** DescriptorSet::bind_image() issues
 * vkUpdateDescriptorSets immediately, and this pipeline overlaps MAX_FRAMES_IN_FLIGHT command
 * buffers without a per-frame wait -- so rebinding from inside the frame loop would update a
 * set a still-pending submission references (VUID-vkUpdateDescriptorSets-None-03047). Two
 * consequences follow: anything read per frame lives in a push constant or a UBO, not a new
 * binding; and a toggle that selects *which image* a pass reads is **startup-fixed**, because
 * that selection was baked into a descriptor when the pipeline was built. Each such toggle
 * says so in PixelRenderConfig, and apply_live_config() refuses to change them.
 *
 * The one exception: HiZPass and SceneColorMipPass (gfxcoopa) bind their own descriptors inside
 * execute(), and ssr_pass_'s secondary source can only be bound once the transparent chain has
 * run. All of these bind lazily and only once -- the passes skip the write when the source view
 * is unchanged -- so only a frame that would actually write one (the first, or the first with
 * ssr_reflect_transparent on) pays a device_.wait_idle(); see trace_inputs_need_rebind_().
 *
 * **2. Per-frame-in-flight data needs per-slot buffers.** gfxcoopa's CameraUBO, LightData,
 * SdfData and this engine's InstanceStream/ForwardGlobalsData each own one buffer, so a single
 * shared instance updated in place would let frame N's upload race frame N-1's still-executing
 * command buffer. Every one of them is allocated per slot, and render() takes the slot index
 * once (FrameContext::frame_slot) so no two uploads can disagree about which frame it is.
 *
 * ## Feature toggles
 *
 * Direct lighting has two looks, selected by `soft_lighting`: the default banded-cel formula
 * (diffuse quantized by `light_bands`, specular hard-masked by `spec_threshold`) or a smooth
 * Cook-Torrance falloff. Outline, palette, dither, camera pixel-snap, SSAO, SSR/SSGI,
 * transparency, refraction, fog, volumetrics, SDF, bloom, DOF, tilt shift and AA layer on top
 * independently. Optional passes are constructed unconditionally and gated per frame at their
 * record site, so a toggle that only changes push-constant contents can flip at runtime.
 *
 * Only one point light casts a shadow: ShadowMapTarget holds exactly one cube map. Every other
 * point light still lights the scene, without occlusion. Spot lights work the same way, via
 * their own single dedicated 2D shadow map: exactly one shadow-casting spot light gets a real
 * shadow (see find_first_shadow_casting_spot_light_()), every other spot still lights the
 * scene unshadowed.
 */

#ifndef TOYENGINE_RENDER_PIXEL_RENDER_PIPELINE_H
#define TOYENGINE_RENDER_PIXEL_RENDER_PIPELINE_H

#include <volk/volk.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>

#include <glm/gtc/packing.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
#include <gfxcoopa/engine/passes/temporal_history_pass.h>
#include "toyengine/render/passes/contact_shadow_pass.h"
#include <gfxcoopa/engine/util/sampler.h>
#include <gfxcoopa/engine/data/camera_ubo.h>
#include <gfxcoopa/engine/data/light_data.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/engine/components/point_light.h>
#include <gfxcoopa/engine/components/spot_light.h>
#include <gfxcoopa/engine/passes/transparent_pass.h>
#include <gfxcoopa/engine/targets/transparent_capture_target.h>
#include <gfxcoopa/engine/passes/transparent_capture_pass.h>
#include <gfxcoopa/engine/passes/fog_pass.h>
#include <gfxcoopa/engine/data/fog_data.h>
#include <gfxcoopa/engine/passes/volumetrics_pass.h>
#include <gfxcoopa/engine/passes/froxel_volumetrics_pass.h>
#include <gfxcoopa/engine/data/volumetrics_data.h>
#include <gfxcoopa/engine/components/volume.h>
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
#include <uicoopa/layout/canvas.h>
#include <uicoopa/ui_yaml.h>
#include <uicoopa/render/ui_world_pass.h>
#include <uicoopa/render/ui_pass.h>

#include <toyengine/render/pixel_math.h>
#include <toyengine/render/pixel_render_types.h>
#include <toyengine/render/passes/ui_composite_pass.h>
#include <toyengine/render/instance_stream.h>
#include <toyengine/render/visibility.h>
#include <toyengine/render/frame_profile.h>
#include <toyengine/render/gpu_profiler.h>
#include <toyengine/render/forward_globals.h>
#include <gfxcoopa/engine/util/material_texture_cache.h>
#include <gfxcoopa/engine/data/palette_lut.h>
#include <gfxcoopa/engine/data/grading_lut.h>
#include <gfxcoopa/engine/passes/exposure_pass.h>
#include <gfxcoopa/engine/passes/deferred_lighting_pass.h>
#include <gfxcoopa/engine/passes/ssr_pass.h>
#include <gfxcoopa/engine/passes/pixel_stylize_pass.h>
#include <gfxcoopa/engine/passes/dof_pass.h>
#include <gfxcoopa/engine/passes/bloom_pass.h>
#include <gfxcoopa/engine/passes/tilt_shift_pass.h>
#include <gfxcoopa/engine/passes/fxaa_pass.h>
#include <gfxcoopa/engine/passes/smaa_pass.h>
#include <gfxcoopa/engine/passes/taa_pass.h>
#include <toyengine/render/passes/upscale_pass.h>
#include <toyengine/render/passes/debug_line_pass.h>
#include <toyengine/render/passes/transparent_preview_pass.h>
#include <toyengine/render/passes/underwater_pass.h>
#include <toyengine/render/passes/particle_pass.h>
#include <toyengine/render/particle_types.h>
#include <toyengine/scene/camera_controller.h>

namespace toy {
namespace render {

/**
 * @class PixelRenderPipeline
 * @brief Renders a scene through the low-resolution deferred pixel-art frame graph and
 *        upscales it into the swapchain.
 *
 * Not copyable or movable: passes hold references to sibling members, and the ExtraSets
 * callbacks capture `this`.
 */
class PixelRenderPipeline {
public:
    PixelRenderPipeline(coopa::gfx::core::Device& device,
                        coopa::gfx::memory::Allocator& allocator,
                        coopa::gfx::core::Swapchain& swapchain,
                        coopa::gfx::pipeline::RenderPass& swapchain_pass,
                        coopa::gfx::command::CommandPool& cmd_pool,
                        PixelRenderConfig config)
        : device_(device), allocator_(allocator), swapchain_(swapchain), cmd_pool_(cmd_pool),
          config_(std::move(config)),
          render_extent_(compute_render_extent(config_, swapchain.extent().width, swapchain.extent().height)),
          upscaled_extent_(compute_display_rect(config_, swapchain.extent().width, swapchain.extent().height,
                                                render_extent_.width, render_extent_.height)),
          gbuffer_target_(device, allocator, render_extent_.width, render_extent_.height),
          // HDR always -- the sky-based indirect lighting (see pixel_lighting.frag) can exceed
          // 1.0 regardless of whether SSR/SSAO are toggled, and pixel_stylize.frag's tonemap
          // step (gated on PushConstants::exposure) always applies before dither/palette to
          // bring it back down.
          // Colour-only: the lighting draw is one depth-less fullscreen triangle, and nothing
          // ever reads this target's depth (record_transparent_() attaches gbuffer_target_'s
          // depth to its own render pass; stylize/debug sample gbuffer depth too). A D32
          // attachment here would be cleared and stored for nothing every frame.
          offscreen_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat,
                            coopa::gfx::engine::targets::kColorOnly),
          post_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA8_Unorm),
          transparent_capture_target_(device, allocator, render_extent_.width, render_extent_.height),
          // Fog composite target -- HDR, same reasoning as offscreen_target_ above: fog belongs
          // in linear HDR (Unity applies it there too), ahead of pixel_stylize_pass_'s tonemap
          // step. A separate target is mandatory, not a style choice: pipeline::RenderPass
          // hardcodes LOAD_OP_CLEAR, so FogPass can't reopen and composite in place onto the
          // image it reads from.
          fog_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat,
                              coopa::gfx::engine::targets::kColorOnly),
          // Underwater composite target -- the same HDR format and LOAD_OP_CLEAR-forced
          // separation as fog_target_ (see UnderwaterPass's file doc).
          underwater_target_(device, allocator, render_extent_.width, render_extent_.height,
                             coopa::gfx::Format::RGBA16_Sfloat, coopa::gfx::engine::targets::kColorOnly),
          // Wind composite target -- same HDR format and the same LOAD_OP_CLEAR-forced
          // separation as fog_target_ above. Wind reads fog's output and writes its own.
          volumetrics_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat,
                              coopa::gfx::engine::targets::kColorOnly),
          // The march half of VolumetricsPass, at 1/volumetrics_resolution_scale per axis:
          // rgb = in-scatter, a = transmittance, upsampled by the composite into
          // volumetrics_target_ above.
          volumetrics_march_target_(device, allocator,
                                    volumetrics_march_dim_(render_extent_.width),
                                    volumetrics_march_dim_(render_extent_.height),
                                    coopa::gfx::Format::RGBA16_Sfloat,
                                    coopa::gfx::engine::targets::kColorOnly),
          nearest_sampler_(coopa::gfx::engine::util::Sampler::nearest(device)),
          linear_sampler_(coopa::gfx::engine::util::Sampler::linear(device)),
          // Hardware compareEnable, not a plain linear sampler -- see gfx/shadow_sampling.glsl's
          // *Shadow-family doc (pixel_lighting.frag/transparent.frag's dir_shadow_map and
          // point_shadow_map are sampler2DShadow/samplerCubeShadow to match).
          shadow_sampler_(coopa::gfx::engine::util::Sampler::shadow(device)),
          fog_data_(device, allocator),
          volumetrics_data_(device, allocator),
          shadow_target_(device, allocator, config_.shadow_map_resolution, config_.cube_shadow_resolution,
                        config_.spot_shadow_resolution, config_.shadow_cascades),
          palette_lut_(coopa::gfx::engine::data::PaletteLut::load(device, allocator, cmd_pool, config_.palette_path)),
          grading_lut_(coopa::gfx::engine::data::GradingLut::load(device, allocator, cmd_pool, config_.grading_lut_path)),
          instance_stream_(device, allocator),
          forward_globals_(device, allocator),
          sdf_data_(device, allocator, config_.sdf_max_renderers, config_.sdf_max_shapes)
    {
        // Construction order is load-bearing in four places:
        //  * material_cache_ before the shadow/G-buffer pipelines -- both append its layout
        //    as their material set.
        //  * the camera/light/shadow layouts before every pass that binds them.
        //  * ssr_pass_ before the transparency passes, which borrow its trace-input layouts
        //    and descriptor sets.
        //  * everything before rebuild_overlay_chain_(), which reads tilt_shift_pass_ and
        //    world_ui_depth_layout_ and binds upscale_pass_'s source image.
        build_frame_descriptors_();
        build_geometry_passes_();
        build_sdf_passes_();
        build_lighting_passes_();
        build_ssr_passes_();
        build_transparency_passes_();
        build_post_chain_(swapchain_pass);
        build_world_ui_descriptor_();
        rebuild_overlay_chain_(swapchain.extent().width, swapchain.extent().height);
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

    /**
     * @brief This frame's physics debug-draw lines -- fill it (typically from
     *        PhysicsWorld::debug_draw(), see debug_line_pass.h's file doc) between
     *        Scene::update()/late_update() and render(); drawn when config_.debug_view ==
     *        "lines", cleared by the caller each frame (this pipeline never clears it itself,
     *        matching how it never owns the scene it reads).
     */
    std::vector<DebugLine>& debug_lines() { return debug_lines_; }

    /**
     * @brief Confines the scene image to a window-pixel rect (an editor viewport panel); the
     *        image is letterboxed/fit inside it exactly as it would be inside the window.
     *        nullopt (the default) uses the whole window.
     *
     * Applied at the next render(). Only the display-rect-sized resources (world-UI layer,
     * tilt shift) are rebuilt when the rect's SIZE changes, and nothing when only its
     * position does -- the screen-UI pass and overlay target follow the swapchain alone.
     */
    void set_display_region(std::optional<LetterboxRect> region) {
        if (region && (region->w == 0 || region->h == 0)) region.reset();
        display_region_ = region;
    }
    const std::optional<LetterboxRect>& display_region() const { return display_region_; }

    /** @brief The window-pixel rect the scene image lands in for a swapchain of sw x sh. */
    LetterboxRect display_rect_for(uint32_t sw, uint32_t sh) const {
        if (display_region_) {
            LetterboxRect box = compute_display_rect(config_, display_region_->w, display_region_->h,
                                                     render_extent_.width, render_extent_.height);
            box.x += display_region_->x;
            box.y += display_region_->y;
            return box;
        }
        return compute_display_rect(config_, sw, sh, render_extent_.width, render_extent_.height);
    }

    /**
     * @brief A second scene whose screen-space canvases draw over the main scene's (the
     *        editor UI). Null removes it. Needs screen_ui_enabled.
     */
    void set_overlay_scene(coopa::scene::Scene* scene) { overlay_scene_ = scene; }

    /**
     * @brief world_ui_pass_'s 1x1 white texture view, for seeding a world canvas's
     *        DrawList::set_default_texture() -- or a default-constructed (null) view when
     *        config_.world_ui_enabled is off.
     *
     * A DrawList emits solid-colour quads (every untextured Image, every panel) against its
     * default texture, so a canvas whose default was never seeded submits draws bound to a
     * VK_NULL_HANDLE image view. That is a validation error and, in a release build, undefined
     * behaviour -- not a blank quad. The host seeds it before Scene::late_update() emits, since
     * the DrawList captures the view at emit time; see Engine::drive_ui_canvases_().
     */
    coopa::gfx::TextureView world_ui_white_view() const {
        return world_ui_pass_ ? world_ui_pass_->white_view() : coopa::gfx::TextureView{};
    }

    /** @brief Low-resolution render width in pixels. */
    uint32_t render_width() const { return render_extent_.width; }
    /** @brief Low-resolution render height in pixels. */
    uint32_t render_height() const { return render_extent_.height; }
    /**
     * @brief The final low-resolution LDR color image, for pixel-accurate screenshots:
     *        aa_target_'s result once config_.aa_mode != "off", else post_target_ directly.
     *
     * Scene and debug lines only -- NEITHER UI layer is in here. Both are composited one
     * stage later, at window resolution, which is the whole point of the overlay chain (see
     * overlay_target_'s member doc); use final_color_image() for a capture that includes UI.
     */
    coopa::gfx::memory::Image& low_res_color_image() const {
        return aa_target_ ? *aa_target_->color_image_object() : *post_target_.color_image_object();
    }
    /**
     * @brief The full-window image actually presented: the post-processed scene with both UI
     *        layers composited over it, letterbox bars included.
     */
    coopa::gfx::memory::Image& final_color_image() const {
        return *overlay_target_->color_image_object();
    }
    /**
     * @brief The screen-space UI pass's default white texture, for seeding a screen canvas's
     *        DrawList::set_default_texture(). Empty when config_.screen_ui_enabled is off.
     *        Same contract and the same null-view hazard as world_ui_white_view() above.
     */
    coopa::gfx::TextureView screen_ui_white_view() const {
        return screen_ui_pass_ ? screen_ui_pass_->white_view() : coopa::gfx::TextureView{};
    }

    /**
     * @brief Hands over this frame's water state -- ripple rings for the water shader and the
     *        underwater parameters for UnderwaterPass. Engine calls it every frame before
     *        render() from toy::water::WaterSystem; keeping it a plain struct means render/ never
     *        depends on toyengine/water/.
     */
    void set_water_state(WaterFrameState state) { water_state_ = std::move(state); }

    /**
     * @brief gfx_time for every surface-shader push: x = renderer clock, y = frame dt,
     *        z = frame index, w = the water clock (WaterFrameState::time) -- the wave phase the
     *        water shader must share with the CPU's buoyancy queries. The renderer clock runs
     *        from pipeline creation and the water clock from scene start, so they are never the
     *        same; w falls back to x when no water system reported a time.
     */
    glm::vec4 surface_gfx_time_() const {
        const float water_time = water_state_.time >= 0.0f ? water_state_.time : elapsed_time_;
        return glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), water_time);
    }
    const WaterFrameState& water_state() const { return water_state_; }

    /**
     * @brief Hands over this frame's particle draw batches (toy::particles, via Engine). Quads
     *        draw inside the forward transparent pass, sorted with BLEND meshes and SDFs; mesh
     *        batches join the opaque G-buffer and shadow batching as instanced draws. The batch
     *        pointers must stay valid until render() returns.
     */
    void set_particle_state(ParticleFrameState state) { particle_state_ = std::move(state); }
    const ParticleFrameState& particle_state() const { return particle_state_; }

    /** @brief True when UnderwaterPass exists and is applying the underwater look this frame. */
    bool underwater_active() const { return underwater_pass_ != nullptr && water_state_.underwater; }

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
            for (const auto& sm : mr->slot_materials) config_.surface_shaders.require(sm.shader);
        }
        for (auto* sr : scene.get_components<coopa::gfx::engine::components::SdfRenderer>()) {
            config_.surface_shaders.require(sr->material.shader);
        }
    }

    /**
     * @brief Read-only access to the live render config.
     */
    const PixelRenderConfig& render_config() const { return config_; }

    /**
     * @brief Turns on profiling mode: CPU phase timers record into `profile`, and GPU
     *        timestamp queries time each feature's passes (see GpuProfiler). Null turns it
     *        off. Without it the frame loop records no queries and pays nothing.
     */
    void set_profiler(FrameProfile* profile) {
        device_.wait_idle();
        gpu_profiler_.reset();
        profile_ = profile;
        if (profile_) gpu_profiler_ = std::make_unique<GpuProfiler>(device_, *profile_);
        if (froxel_volumetrics_pass_) {
            using FStage = coopa::gfx::engine::passes::FroxelVolumetricsPass::Stage;
            if (gpu_profiler_) {
                froxel_volumetrics_pass_->set_stage_hook([this](coopa::gfx::command::CommandBuffer& cmd, FStage st) {
                    gpu_mark_(cmd, st == FStage::Inject ? GpuScope::VolumetricsInject : GpuScope::VolumetricsIntegrate);
                });
            } else {
                froxel_volumetrics_pass_->set_stage_hook({});
            }
        }
        if (ssr_pass_) {
            using Stage = coopa::gfx::engine::passes::SsrPass::Stage;
            if (gpu_profiler_) {
                ssr_pass_->set_stage_hook([this](coopa::gfx::command::CommandBuffer& cmd, Stage stage) {
                    static constexpr GpuScope kScopes[] = {
                        GpuScope::SsrTrace, GpuScope::SsrResolve, GpuScope::SsrBlur, GpuScope::SsgiTrace,
                        GpuScope::SsgiResolve, GpuScope::SsgiBlur, GpuScope::SsrComposite};
                    gpu_mark_(cmd, kScopes[static_cast<size_t>(stage)]);
                });
            } else {
                ssr_pass_->set_stage_hook({});
            }
        }
    }

    /**
     * @brief Per-frame mesh draw averages since startup -- MeshRenderers, how many the camera
     *        sees, and draw calls / instances / triangles for the camera passes and for all
     *        shadow views together. Empty before the second frame. Engine::run() prints it
     *        on exit, beside the frame time.
     */
    std::string mesh_draw_stats_summary() const {
        if (stats_frames_ == 0) return {};
        const double f = static_cast<double>(stats_frames_);
        char buf[320];
        std::snprintf(buf, sizeof(buf),
                      "meshes %.0f (camera-visible %.0f) | camera: %.0f draws, %.0f instances, %.0fk tris"
                      " | shadows: %.0f draws, %.0f instances, %.0fk tris",
                      stats_total_.renderers / f, stats_total_.camera_visible / f,
                      stats_total_.camera_draws / f, stats_total_.camera_instances / f,
                      stats_total_.camera_triangles / f / 1000.0,
                      stats_total_.shadow_draws / f, stats_total_.shadow_instances / f,
                      stats_total_.shadow_triangles / f / 1000.0);
        return buf;
    }

    /**
     * @brief MUTABLE access to the live render config, for changing parameters at runtime.
     *
     * Most parameters are re-read from config_ every frame -- the fog and volumetrics UBO
     * fills and the bloom/DOF/stylize/tilt-shift push constants all source their values inside
     * render() -- so writing through this reference takes effect on the very next frame:
     *
     * @code
     * pipeline.render_config_mut().fog_density = 0.12f;   // visible next frame
     * @endcode
     *
     * The exception is the STARTUP-FIXED set (see the file doc's rule 1, and each toggle's own
     * doc in PixelRenderConfig): writing those changes the struct but not what renders, and can
     * leave descriptors pointing at targets nothing writes. Use apply_live_config() when setting
     * many fields at once -- it refuses those and names them.
     *
     * Mutate between frames, never mid-record.
     */
    PixelRenderConfig& render_config_mut() { return config_; }

    /**
     * @brief Applies a whole config to the live pipeline, skipping startup-fixed fields.
     *
     * The bulk counterpart to render_config_mut(): use it when replacing many values at once and
     * you cannot be sure none are startup-fixed. Those are restored from the live values and
     * NAMED in a warning rather than silently dropped -- an edit that quietly does nothing is the
     * failure mode that makes runtime tweaking feel broken.
     *
     * Call between frames, never mid-record.
     *
     * @param next The config to apply.
     */
    void apply_live_config(const PixelRenderConfig& next) {
        PixelRenderConfig merged = next;
        std::vector<const char*> ignored;

        // Restore a startup-fixed field from the live config, remembering it if the
        // file tried to change it.
#define TOY_KEEP_STARTUP_FIXED(field)                             \
        do {                                                      \
            if (merged.field != config_.field) {                  \
                ignored.push_back(#field);                        \
            }                                                     \
            merged.field = config_.field;                         \
        } while (0)

        // Toggles that fixed a descriptor binding or a source view at construction.
        TOY_KEEP_STARTUP_FIXED(fog_enabled);
        TOY_KEEP_STARTUP_FIXED(volumetrics_enabled);
        TOY_KEEP_STARTUP_FIXED(bloom_enabled);
        TOY_KEEP_STARTUP_FIXED(dof_enabled);
        TOY_KEEP_STARTUP_FIXED(tilt_shift_enabled);
        TOY_KEEP_STARTUP_FIXED(ssr_enabled);
        TOY_KEEP_STARTUP_FIXED(ssgi_traced);
        TOY_KEEP_STARTUP_FIXED(ssao_enabled);
        TOY_KEEP_STARTUP_FIXED(transparency_enabled);
        TOY_KEEP_STARTUP_FIXED(refraction_enabled);
        TOY_KEEP_STARTUP_FIXED(aa_mode);
        TOY_KEEP_STARTUP_FIXED(world_ui_enabled);
        TOY_KEEP_STARTUP_FIXED(screen_ui_enabled);
        // Sizes baked into targets, SSBOs and the palette LUT at construction.
        TOY_KEEP_STARTUP_FIXED(resolution_mode);
        TOY_KEEP_STARTUP_FIXED(render_width);
        TOY_KEEP_STARTUP_FIXED(render_height);
        TOY_KEEP_STARTUP_FIXED(fill_aspect);
        TOY_KEEP_STARTUP_FIXED(scale_divisor);
        TOY_KEEP_STARTUP_FIXED(ssr_half_res);
        TOY_KEEP_STARTUP_FIXED(ssao_half_res);
        TOY_KEEP_STARTUP_FIXED(volumetrics_resolution_scale);
        TOY_KEEP_STARTUP_FIXED(volumetrics_mode);
        TOY_KEEP_STARTUP_FIXED(volumetrics_froxel_tile);
        TOY_KEEP_STARTUP_FIXED(volumetrics_froxel_slices);
        TOY_KEEP_STARTUP_FIXED(shadow_map_resolution);
        TOY_KEEP_STARTUP_FIXED(shadow_cascades);
        TOY_KEEP_STARTUP_FIXED(cube_shadow_resolution);
        TOY_KEEP_STARTUP_FIXED(spot_shadow_resolution);
        TOY_KEEP_STARTUP_FIXED(sdf_max_renderers);
        TOY_KEEP_STARTUP_FIXED(sdf_max_shapes);
        TOY_KEEP_STARTUP_FIXED(palette_path);
        TOY_KEEP_STARTUP_FIXED(grading_lut_path);
        TOY_KEEP_STARTUP_FIXED(auto_exposure_enabled);
        // sdf_enabled and shadows_enabled are deliberately absent: both are read fresh
        // every frame (the SDF gather, and the two cast_*_shadow flags), so they apply
        // at runtime like any other tunable.

#undef TOY_KEEP_STARTUP_FIXED

        config_ = merged;

        if (!ignored.empty()) {
            std::cerr << "[toy::render] Config reloaded, but these are startup-fixed and were "
                         "IGNORED (restart to apply):";
            for (const char* name : ignored) {
                std::cerr << ' ' << name;
            }
            std::cerr << '\n';
        }
    }

    /**
     * @brief Renders one frame of the given scene.
     *
     * @param dt Seconds since the previous frame (or the FIXED_DT override). Accumulated into
     *           elapsed_time_ and pushed as gfx_time to every surface-shader backbone, so a
     *           derived shader's displacement hook (foliage sway, water waves) can animate. The
     *           stock hooks ignore it, so a scene with no derived shaders is unaffected.
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

        const LetterboxRect letterbox = handle_resize_();

        // The main camera, not merely "the first one found" -- CameraComponent::main()
        // guarantees a stable choice across multi-camera scenes (see camera_component.h).
        auto* cam = CameraComponent::main();
        float aspect = static_cast<float>(render_extent_.width) / static_cast<float>(render_extent_.height);

        glm::mat4 view = cam ? cam->get_view_matrix() : glm::mat4(1.0f);
        glm::mat4 proj = cam ? cam->get_projection_matrix(aspect) : glm::mat4(1.0f);
        glm::vec3 cam_pos = cam ? cam->get_world_position() : glm::vec3(0.0f);

        // Resolved here (unjittered view, before the TAA jitter below) rather than
        // inline in the DOF block further down: the object-focus branch advances
        // smoothed_dof_focus_, a dt-driven mutable member, and that block sits inside
        // the [&] command-recording lambda this function opens later -- advancing
        // state during command recording is a hazard the rest of this pipeline avoids.
        DofFocus dof_focus = resolve_dof_focus_(cam, view, scene, dt);

        // TAA sub-pixel jitter: an 8-frame Halton(2,3) sequence added to whichever projection
        // terms shift NDC by a depth-independent constant (see apply_taa_jitter_).
        //
        // unjittered_proj is kept for the consumers that must NOT see the jitter -- fog, the
        // volumetrics UBO and the world-UI layer. The rule behind all three: the jitter exists
        // only to be resolved, so anything outside the temporal resolve takes the unjittered
        // matrix. Fog and volumetrics reproject WORLD-space samples, which would swim
        // independently of the pixel grid; the UI layer is composited after taa_pass_ has already
        // resolved the scene, so nothing would ever average its jitter back out.
        //
        // Every other consumer -- the camera UBO, the SDF/mesh screen-rect fits,
        // debug_line_pass_ (inside post_target_, so it IS resolved) and prev_view_proj_ --
        // intentionally sees the jittered matrix, so SSAO's and SSR's temporal resolves reproject
        // against the camera TAA is actually showing.
        const glm::mat4 unjittered_proj = proj;
        apply_taa_jitter_(proj);

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
        // One frame-slot source of truth for every per-frame-in-flight upload (camera, lights,
        // instances, SDF data), so none of them can disagree about which slot they are writing.
        // current_frame() only advances at the end of Renderer::draw_frame, so this stays stable
        // for all of render(), the recording lambda included.
        const uint32_t frame_slot = renderer.current_frame();
        // draw_frame() waits on this same slot's fence later, but waiting HERE -- before the
        // per-slot writes below -- is what makes those writes race frame N-1 rather than N-2.
        // Cheap: the fence is already signaled in the common case.
        {
            CpuTimer wait_timer(profile_, CpuScope::WaitFence);
            renderer.wait_for_current_frame();
        }
        // This slot's previous frame has finished on the GPU: read its timestamps back now,
        // before begin_frame() below reuses the slot (profiling mode only).
        if (gpu_profiler_) gpu_profiler_->collect(frame_slot);
        // Same reasoning for the shadow focus probe: this slot's copy from its previous frame
        // has landed, so reading it here never stalls.
        read_focus_probe_(frame_slot, view, dt);
        focus_probe_wanted_ = cam && config_.shadow_fit == "focus" &&
                              config_.shadow_focus_distance <= 0.0f && !dof_focus.from_scene;
        const auto gather_start = std::chrono::steady_clock::now();
        camera_frame_ = frame_slot;
        light_frame_ = frame_slot;
        camera_ubos_[frame_slot]->update(view, proj, cam_pos, pixel_density);

        // One hierarchy walk for every component type this frame's gathers read -- see
        // FrameScene. Everything below reads frame_scene_ instead of walking the scene again.
        snapshot_scene_(scene);

        // Must follow the light_frame_ assignment above: update_lights_() writes through
        // current_light_data(), which reads light_frame_. Writing first would put this frame's
        // light colours in the other slot while the shadow-matrix write below landed correctly.
        update_lights_(scene);

        // debug_lines_ was filled by the caller before render() ran (see debug_lines()'s
        // doc); upload it unconditionally -- cheap when empty, and it keeps this slot's buffer
        // valid if debug_view is switched to "lines" between frames.
        debug_line_pass_->upload(frame_slot, debug_lines_);

        gather_ui_canvases_(scene, frame_slot, view);
        const std::vector<SdfDrawItem> sdf_draws = gather_sdf_(scene, view, proj, cam_pos, frame_slot);

        auto* dir_light = frame_scene_.dir_light;
        bool cast_dir_shadow = config_.shadows_enabled && dir_light && dir_light->cast_shadows;
        if (dir_light) {
            // The shadow focus: what the scene says the camera is looking at when it says so,
            // else what the screen-centre probe found (0 until its first readback lands).
            update_dir_shadow_matrix_(dir_light->direction, cam, cast_dir_shadow,
                                      dof_focus.from_scene ? dof_focus.distance : probed_focus_distance_);
        }

        // Only the scene's first shadow-casting point light gets a real cube map
        // (see the class doc for why). Marks current_light_data().point_lights[0]'s
        // cast_shadows flag so pixel_lighting.frag knows to sample it.
        auto* shadow_point = find_first_shadow_casting_point_light_(scene);
        bool cast_point_shadow = config_.shadows_enabled && shadow_point != nullptr;
        if (cast_point_shadow) {
            auto& gpu0 = current_light_data().point_lights[0];
            gpu0.attenuation.w = 1.0f;
        }

        // Same one-shadow-casting-light rule as the point light above, but via an explicit
        // index (light_counts.w) rather than a hardcoded slot 0 -- see
        // find_first_shadow_casting_spot_light_()'s doc for why. update_spot_shadow_matrix_()
        // must run even when there is no caster, same as update_dir_shadow_matrix_() is only
        // skipped (not the shadow_params write) when there's no directional light: it still
        // needs to clear spot_shadow_params.z to 0 so calc_spot_shadow() bails cleanly.
        auto spot_caster = find_first_shadow_casting_spot_light_(scene);
        bool cast_spot_shadow = config_.shadows_enabled && spot_caster.light != nullptr;
        if (cast_spot_shadow) {
            current_light_data().spot_lights[spot_caster.index].params.z = 1.0f;
            current_light_data().light_counts.w = spot_caster.index;
        } else {
            current_light_data().light_counts.w = 0xFFFFFFFFu;
        }
        update_spot_shadow_matrix_(spot_caster.light, cast_spot_shadow);

        // After both shadow fits: every view's frustum is now known, so the gather can cull
        // and batch each view's draw list (see gather_meshes_()).
        const MeshGather meshes = gather_meshes_(frame_slot, view, unjittered_proj, cast_dir_shadow,
                                                 shadow_point, cast_point_shadow, cast_spot_shadow);

        // Particle quads: every batch's instances into this slot's buffer in one go. They only
        // draw inside the forward transparent pass, so nothing uploads when that is off -- said
        // once, rather than an effect silently missing.
        if (!config_.transparency_enabled && !particle_state_.quads.empty() && !warned_particles_need_transparency_) {
            std::cerr << "[toy::render] This scene has particle effects, but transparency_enabled is off: "
                         "particle quads draw in the forward transparent pass and are skipped "
                         "(mesh-mode particles still draw).\n";
            warned_particles_need_transparency_ = true;
        }
        if (config_.transparency_enabled) {
            particle_pass_->upload(frame_slot, particle_state_.quads);
            particle_depth_ = glm::vec4(cam ? cam->clip_start : 0.1f, cam ? cam->clip_end : 1000.0f,
                                        (!cam || cam->type != CameraType::Orthographic) ? 1.0f : 0.0f, 0.0f);
        }

        // SSR's second trace source (ssr_reflect_transparent) only exists when something
        // transparent is actually in view. Otherwise skip the capture pass and its pyramids AND
        // the per-pixel second trace -- it could only ever miss.
        secondary_this_frame_ = false;
        if (config_.ssr_reflect_transparent) {
            secondary_this_frame_ = !meshes.capture.empty();
            for (const auto& d : sdf_draws) {
                if (d.is_blend && d.px_rect.w > 0 && d.px_rect.h > 0) secondary_this_frame_ = true;
            }
        }

        // Refraction's scene-colour chain is only ever sampled by transparent.frag, i.e. by a
        // camera-visible BLEND mesh (BLEND SDFs read ssr_pass_'s chain instead). On a frame
        // with none in view -- every frame of a scene without transparency -- the seven-mip
        // build would be pure bandwidth, so it is skipped the same way the SSR capture is.
        refraction_this_frame_ = config_.transparency_enabled && config_.refraction_enabled
                              && meshes.has_blend_mesh;

        light_datas_[light_frame_]->upload();

        if (config_.fog_enabled) {
            update_fog_data_(unjittered_proj, view, cam_pos, dir_light);
        }
        update_underwater_params_(unjittered_proj, view, cam_pos, dir_light);

        if (config_.volumetrics_enabled) {
            update_volumetrics_data_(scene, unjittered_proj, view, cam_pos, dir_light);
        }

        // transparent.frag traces the SAME Hi-Z pyramid and scene-colour mip chain ssr.frag
        // does, so those images must exist and be correctly laid out whenever BLEND geometry
        // might draw -- even with ssr_enabled false for the opaque path. Keyed on
        // transparency_enabled rather than "does the scene have a BLEND renderer this frame", to
        // keep it a branch-once decision. bloom_pass_ is deliberately NOT folded in: it owns an
        // independent pyramid with construction-fixed descriptors, so it needs neither this chain
        // nor the wait below.
        bool need_ssr_trace_inputs = config_.ssr_enabled || config_.transparency_enabled
                                    || config_.ssr_reflect_transparent
                                    // ssao.frag marches its own prefiltered depth pyramid
                                    // (ao_depth_pyramid_pass_), whose per-frame descriptor
                                    // rebind needs the same wait as HiZPass's.
                                    || config_.ssao_enabled;

        if (need_ssr_trace_inputs && trace_inputs_need_rebind_()) {
            // HiZPass and SceneColorMipPass bind their descriptor sets inside execute(), and only
            // when the source view differs from what the sets already hold -- in practice once,
            // on the first frame. A write to a set a pending command buffer references is invalid
            // (file doc, rule 1), so that one frame waits for the GPU; every later frame overlaps.
            device_.wait_idle();
        }

        FrameContext ctx;
        ctx.cam                   = cam;
        ctx.view                  = view;
        ctx.proj                  = proj;
        ctx.unjittered_proj       = unjittered_proj;
        ctx.cam_pos               = cam_pos;
        ctx.frame_slot            = frame_slot;
        ctx.letterbox             = letterbox;
        ctx.dof_focus_distance    = dof_focus.distance;
        ctx.dof_focus_range       = dof_focus.range;
        ctx.need_ssr_trace_inputs = need_ssr_trace_inputs;
        ctx.cast_dir_shadow       = cast_dir_shadow;
        ctx.cast_point_shadow     = cast_point_shadow;
        ctx.shadow_point          = shadow_point;
        ctx.cast_spot_shadow      = cast_spot_shadow;

        if (profile_) {
            profile_->add_cpu(CpuScope::Gather, std::chrono::duration<double, std::milli>(
                                                    std::chrono::steady_clock::now() - gather_start).count());
        }
        const auto submit_start = std::chrono::steady_clock::now();
        double record_ms = 0.0;
        bool frame_presented = renderer.begin_frame(
            [&](coopa::gfx::command::CommandBuffer& cmd) {
                // 1:1 over the full extent, NOT the letterbox rect: overlay_target_ is already
                // swapchain-sized and already contains the bars, because the upscale and the
                // letterboxing both happened earlier, in ui_composite_pass_. All this adds is
                // upscale.frag's srgb_decode(), cancelling the SRGB swapchain's implicit encode on write
                // -- overlay_target_ is UNORM and holds already-encoded bytes.
                upscale_pass_->draw(cmd, LetterboxRect{0, 0,
                                                       overlay_extent_.width,
                                                       overlay_extent_.height});
            },
            VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}},
            nullptr,
            [&](coopa::gfx::command::CommandBuffer& cmd) {
                const auto record_start = std::chrono::steady_clock::now();
                if (gpu_profiler_) gpu_profiler_->begin_frame(cmd, frame_slot, profile_->current_frame());
                record_scene_(cmd, ctx, meshes, sdf_draws);
                frame_meshes_ = &meshes;
                record_post_chain_(cmd, ctx);
                frame_meshes_ = nullptr;
                record_overlay_(cmd, ctx);
                record_ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - record_start).count();
            },
            [&](coopa::gfx::command::CommandBuffer& cmd) {
                gpu_mark_(cmd, GpuScope::Present);   // after the swapchain copy pass
            }
        );
        if (profile_) {
            const double total_ms = std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - submit_start).count();
            profile_->add_cpu(CpuScope::Record, record_ms);
            profile_->add_cpu(CpuScope::SubmitPresent, std::max(0.0, total_ms - record_ms));
        }

        // Needed by both SSAO's and SSR's temporal resolve passes -- kept unconditional (not
        // gated on either toggle) since ssao_pass_ always exists and either toggle can be
        // re-enabled without a resize/reconstruct in between.
        prev_view_proj_                = proj * view;
        prev_unjittered_view_proj_     = unjittered_proj * view;
        prev_view_proj_valid_          = true;
        ++frame_index_;

        return frame_presented;
    }

private:

    /**
     * @struct MeshBatch
     * @brief One instanced draw: `instance_count` renderers that share a mesh, an LOD level
     *        and every material input the pass reads, whose transforms sit contiguously in
     *        this frame's InstanceStream from `first_instance`. `item` is any one of them
     *        (index into MeshGather::renderers) -- the source of the shared mesh/material.
     */
    struct MeshBatch {
        uint32_t item           = 0;
        uint32_t lod            = 0;
        uint32_t first_instance = 0;
        uint32_t instance_count = 0;
    };

    /**
     * @struct MeshGather
     * @brief This frame's MeshRenderers, their world matrices/bounds and LOD, and the
     *        frustum-culled, batched draw list of every view that draws meshes.
     *
     * Built once per frame by gather_meshes_(); every recording site draws its own list, so
     * a view only ever pays for what it can see. Per-renderer arrays share one index space.
     */
    struct MeshGather {
        /// One entry per DRAW ITEM: a renderer's material part (submesh, see data::MeshPart).
        /// A renderer whose mesh has N material slots appears N times, with part 0..N-1;
        /// every per-item array below shares this index space.
        std::vector<coopa::gfx::engine::components::MeshRenderer*> renderers;
        std::vector<uint32_t>    part;    ///< The item's material slot.
        /** @brief The material the item draws with (its renderer's material for its slot). */
        const coopa::gfx::engine::components::PBRMaterial& material(size_t i) const {
            return renderers[i]->material_for(part[i]);
        }
        std::vector<glm::mat4>   world_matrices;
        std::vector<WorldBounds> bounds;
        std::vector<uint8_t>     valid;   ///< Ready, has a transform, not LOD-culled.
        /// Per item: a particle mesh batch's world matrices (ParticleMeshBatch), null for an
        /// ordinary renderer. Such an item contributes `multi_count[i]` instances from these
        /// matrices to whichever batch it joins, instead of world_matrices[i]; its bounds are
        /// the whole particle batch's.
        std::vector<const glm::mat4*> multi;
        std::vector<uint32_t>         multi_count;
        std::vector<uint32_t>    lod;     ///< Chosen against the camera; shadow views reuse it.
        /// Single-instance index for each camera-visible BLEND renderer -- the forward pass
        /// draws those one at a time, back to front, so they are never batched. Otherwise
        /// InstanceStream::kInvalidIndex.
        std::vector<uint32_t>    instance_idx;
        /// Any camera-visible BLEND renderer this frame (some instance_idx is valid) -- the
        /// only thing that samples refraction's scene-colour chain, so render() skips building
        /// it when this is false.
        bool                     has_blend_mesh = false;

        std::vector<MeshBatch>                gbuffer;   ///< Camera; opaque + mask.
        std::vector<MeshBatch>                capture;   ///< Camera; blend (SSR capture).
        std::array<std::vector<MeshBatch>, 4> cascade;   ///< Directional shadow, per cascade.
        std::array<std::vector<MeshBatch>, 6> cube;      ///< Point shadow, per cube face.
        std::vector<MeshBatch>                spot;      ///< Spot shadow.
    };

    /**
     * @struct MeshDrawStats
     * @brief Draw calls / instances / triangles recorded for meshes in one frame, by pass
     *        family. Accumulated across frames for Engine::run()'s exit summary.
     */
    struct MeshDrawStats {
        uint64_t camera_draws = 0, camera_instances = 0, camera_triangles = 0;
        uint64_t shadow_draws = 0, shadow_instances = 0, shadow_triangles = 0;
        uint64_t renderers = 0, camera_visible = 0;
    };

    /**
     * @struct FrameContext
     * @brief Everything render() resolves before recording, shared by the three record_*
     *        stages so none of them re-derives it.
     */
    struct FrameContext {
        const coopa::gfx::engine::components::CameraComponent* cam = nullptr;
        glm::mat4 view{1.0f};
        glm::mat4 proj{1.0f};            /**< TAA-jittered when aa_mode == "taa". */
        glm::mat4 unjittered_proj{1.0f}; /**< For consumers outside TAA's temporal resolve. */
        glm::vec3 cam_pos{0.0f};
        uint32_t  frame_slot = 0;
        LetterboxRect letterbox{};
        float dof_focus_distance = 0.0f;
        /** Half-width in metres of the forced-sharp band around the focal plane; see
         *  DofPass::Params::focus_range and resolve_dof_focus_(). 0 = pure thin lens. */
        float dof_focus_range = 0.0f;
        /** True when the Hi-Z pyramid and scene-colour mip chain must be built this frame --
         *  transparent.frag traces the same ones ssr.frag does, so this is wider than
         *  ssr_enabled alone. Its passes bind descriptors lazily, which costs a wait_idle() on
         *  the frames trace_inputs_need_rebind_() reports. */
        bool need_ssr_trace_inputs = false;
        bool cast_dir_shadow   = false;
        bool cast_point_shadow = false;
        coopa::gfx::engine::components::PointLightComponent* shadow_point = nullptr;
        /** Unlike shadow_point, record_spot_shadow_() needs no SpotLightComponent* -- its
         *  light-space matrix is already resolved into current_light_data() by
         *  update_spot_shadow_matrix_() before record_scene_() runs (a spot map, like the
         *  directional map, needs no per-face light_pos_range the way the cube map's
         *  per-face record_point_shadow_() loop does). */
        bool cast_spot_shadow  = false;
    };

    /**
     * @brief Builds the per-frame-in-flight camera and light UBOs plus their descriptor sets, and the
     * shared shadow-map sampler set. Every bind happens here, once -- see the class doc.
     */
    void build_frame_descriptors_() {
        camera_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .uniform_buffer(0, coopa::gfx::ShaderStage::Vertex | coopa::gfx::ShaderStage::Fragment)
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
                device_, allocator_, kFocusProbeSamples * 8u,
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
                .build(device_));
        shadow_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*shadow_layout_, 1).build(device_));
        shadow_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *shadow_pool_, *shadow_layout_);
        shadow_set_->bind_image(0, shadow_target_.dir_shadow_view(), shadow_sampler_.handle());
        shadow_set_->bind_image(1, shadow_target_.cube_shadow_view(), shadow_sampler_.handle());
        shadow_set_->bind_image(2, shadow_target_.spot_shadow_view(), shadow_sampler_.handle());
        // Binding 3: the directional map AGAIN, through a plain nearest sampler --
        // PCSS's blocker search reads stored depths, which the compare sampler at
        // binding 0 cannot return (see gfx_shadow_dir_pcss).
        shadow_set_->bind_image(3, shadow_target_.dir_shadow_view(), nearest_sampler_.handle());
    }
    /**
     * @brief Builds the material texture cache and the shadow/G-buffer pipelines, plus one named
     * pipeline variant per Opaque-domain surface shader.
     */
    void build_geometry_passes_() {
        // Built before shadow_pipeline_/gbuffer_pipeline_ below -- both need material_cache_'s
        // layout handle to append the CUTOUT alpha-mask sampler as their material set. See
        // MaterialTextureCache's own doc for why lazily allocating sets from it later (once a
        // scene's masked materials finish loading) is safe under overlapped command buffers.
        material_cache_ = std::make_unique<coopa::gfx::engine::util::MaterialTextureCache>(device_, allocator_, cmd_pool_);

        shadow_pipeline_ = std::make_unique<coopa::gfx::engine::passes::ShadowPipeline>(
            device_, shadow_target_.dir_render_pass(), shadow_target_.cube_render_pass(),
            config_.shaders("shadow_depth.vert"),
            config_.shaders("shadow_depth.frag"),
            config_.shaders("shadow_cube.vert"),
            config_.shaders("shadow_cube.frag"),
            &material_cache_->layout_object());

        gbuffer_pipeline_ = std::make_unique<coopa::gfx::engine::passes::GBufferPipeline>(
            device_, gbuffer_target_.render_pass(), camera_layout_->handle(), material_cache_->layout(),
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
                config_.shaders(sd.shadow_cube_frag.empty() ? "shadow_cube.frag" : sd.shadow_cube_frag),
                sd.cull);
        }
    }
    /**
     * @brief Builds the three SDF passes that need nothing beyond what the initializer list and the
     * blocks above already provide. sdf_forward_pass_ is built with the transparency passes,
     * since it shares transparent_pass_'s render pass and ssr_pass_'s trace sets.
     */
    void build_sdf_passes_() {
        // --- SDF raymarching system ---
        // Three of the four SDF passes need nothing this pipeline hasn't already built by this
        // point (camera_layout_/shadow_target_/transparent_capture_target_/sdf_data_); the
        // fourth (sdf_forward_pass_) needs transparent_pass_'s render pass and ssr_pass_'s trace
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

        sdf_capture_pass_ = std::make_unique<coopa::gfx::engine::passes::SdfCapturePass>(
            device_, transparent_capture_target_.render_pass(),
            *camera_layout_, *light_layout_, *shadow_layout_, sdf_data_.layout(),
            config_.shaders("sdf_quad.vert"),
            config_.shaders("sdf_capture.frag"));
    }
    /**
     * @brief Builds the SSAO, SSAO debug view and deferred lighting passes. (The sky is drawn
     *        by the lighting pass itself at background pixels -- see pixel_lighting.frag.)
     */
    void build_lighting_passes_() {
        // SsaoPass is always constructed: pixel_lighting.frag (and the SSR composite, when
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
            config_.shaders("ssr_resolve.frag"));
        contact_shadow_pass_->update_descriptors(
            gbuffer_target_.g0_view_typed(), gbuffer_target_.g1_view_typed(),
            gbuffer_target_.g2_view_typed(), gbuffer_target_.depth_view_typed(),
            temporal_history_pass_->count_view_typed(0), temporal_history_pass_->count_view_typed(1),
            temporal_history_pass_->sampler());

        // pixel_lighting_pass_'s one app-supplied extra set (set 4): the resolved occlusion the
        // directional term max()-combines in. An ExtraSets rather than a sixth binding on
        // gfxcoopa's own G-buffer set, so DeferredLightingPass's layout -- shared with every
        // other consumer of that library -- is untouched.
        contact_extra_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .build(device_));
        contact_extra_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder()
                .add_sets(*contact_extra_layout_, 1).build(device_));
        contact_extra_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(
            device_, *contact_extra_pool_, *contact_extra_layout_);
        contact_extra_set_->bind_image(0, contact_shadow_pass_->output_view_typed(),
                                       contact_shadow_pass_->sampler());

        // ssao_enabled is a load-time config value (no live reload), so which image to bind is
        // decided once here rather than every frame -- see the update_descriptors comment above
        // for why a per-frame rebind would be unsafe anyway.
        const VkImageView ssao_view = ssao_source_view_();

        // No ExtraSets (toyengine has neither GI nor reflection probes to plumb through), so
        // this collapses to the same {camera=0, light=1, shadow=2, gbuffer=3} layout the old
        // fork hardcoded -- gbuffer_set_index_ is derived, not hardcoded, so this is correct
        // whether or not extras are ever added later (see the fix in deferred_lighting_pass.h).
        coopa::gfx::engine::passes::ExtraSets lighting_extra;
        lighting_extra.layouts = {contact_extra_layout_.get()};
        lighting_extra.bind = [this](coopa::gfx::command::CommandBuffer& cmd, uint32_t first_set) {
            cmd.bind_descriptor_set(*contact_extra_set_, first_set);
        };

        pixel_lighting_pass_ = std::make_unique<coopa::gfx::engine::passes::DeferredLightingPass>(
            device_, offscreen_target_.render_pass_object(), *camera_layout_, *light_layout_,
            *shadow_layout_, linear_sampler_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("pixel_lighting.frag"),
            lighting_extra,
            std::vector<coopa::gfx::pipeline::PushConstantRange>{
                {coopa::gfx::ShaderStage::Fragment, 0, sizeof(PixelLightingPushConstants)}});
        pixel_lighting_pass_->set_gbuffer_images(
            gbuffer_target_.g0_view_typed(), gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed(),
            gbuffer_target_.g3_view_typed(), linear_sampler_);
        pixel_lighting_pass_->set_ssao_image(ssao_view, ssao_pass_->sampler().handle());
    }
    /**
     * @brief Builds the Hi-Z pyramid, the prefiltered scene-colour mip chain and the SSR pass that
     * consumes both. Must follow build_lighting_passes_(), whose ssao_pass_ this binds.
     */
    void build_ssr_passes_() {
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
                                       linear_sampler_);

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
            config_.ssgi_traced ? config_.shaders("ssgi.frag") : std::string{});
        ssr_pass_->update_descriptors(
            gbuffer_target_, hiz_pass_->full_hiz_view_typed(), hiz_pass_->sampler(),
            scene_color_mip_pass_->full_view_typed(), scene_color_mip_pass_->sampler(),
            offscreen_target_.color_view_typed(), linear_sampler_);
        ssr_pass_->set_ssao_image(ssao_source_view_(), ssao_pass_->sampler().handle());
        // The shared accumulation count both resolve chains average against, in place of the
        // fixed-rate blend that can never converge on a re-jittered trace. Bound once: that pass
        // keeps one stable target image, and rebinding a descriptor per frame is unsafe under this
        // pipeline's frame-overlap model (see the ssao_source_view_() comment).
        ssr_pass_->set_temporal_count_image(temporal_history_pass_->count_view_typed(0),
                                            temporal_history_pass_->count_view_typed(1),
                                            temporal_history_pass_->sampler());
    }
    /**
     * @brief Builds the transparent-capture chain, refraction's own scene-colour chain, and the forward
     * transparent and SDF forward passes. Must follow build_ssr_passes_(), whose trace-input
     * layouts and sets these borrow.
     */
    void build_transparency_passes_() {
        // --- ssr_reflect_transparent: opaque surfaces also reflect transparent geometry ---
        // Always constructed (same always-on-but-runtime-gated policy as hiz_pass_/
        // scene_color_mip_pass_/ssr_pass_ above); render() re-reads
        // config_.ssr_reflect_transparent every frame.
        transparent_capture_pass_ = std::make_unique<coopa::gfx::engine::passes::TransparentCapturePass>(
            device_, transparent_capture_target_.render_pass(),
            camera_layout_->handle(), light_layout_->handle(), shadow_layout_->handle(),
            config_.shaders("pbr.vert"),
            config_.shaders("transparent_capture.frag"),
            static_cast<uint32_t>(sizeof(TransparentCaptureLightingPushConstants)),
            material_cache_->layout());

        // Second, independent HiZPass/SceneColorMipPass instances over transparent_capture_
        // target_'s own depth/shaded-color images -- both classes are fully generic
        // (execute() takes a plain depth/colour image+view, no GBufferTarget reference held),
        // so this reuse needs no engine changes beyond what hiz_pass_/scene_color_mip_pass_
        // already prove works.
        transparent_hiz_pass_ = std::make_unique<coopa::gfx::engine::passes::HiZPass>(
            device_, allocator_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("hiz_downsample.frag"));
        transparent_hiz_pass_->recreate(render_extent_.width, render_extent_.height);

        transparent_scene_color_mip_pass_ = std::make_unique<coopa::gfx::engine::passes::SceneColorMipPass>(
            device_, allocator_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("scene_color_downsample.frag"));
        transparent_scene_color_mip_pass_->recreate(render_extent_.width, render_extent_.height);

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

        // set_secondary_source() is deliberately NOT called here. transparent_hiz_pass_ and
        // transparent_scene_color_mip_pass_'s images are freshly recreate()'d
        // (VK_IMAGE_LAYOUT_UNDEFINED) and stay that way until their first execute(), which only
        // happens on a frame where ssr_reflect_transparent is set. Binding them now would aim
        // ssr_pass_'s always-valid layout at descriptors that are not validly laid out on any
        // frame before that. record_scene_() binds them instead, after that same frame's
        // execute(), leaving sets 4-6 on SsrPass's own neutral fallback until then.

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

        // transparent.frag reuses pbr.vert (byte-identical to gbuffer.vert -- see gfx/), the
        // same convention blendy uses for its own TransparentPass. extra_pc_bytes is now just
        // TransparentRefractionPushConstants -- the frame-level lighting/SSR block that used
        // to occupy this region moved to forward_globals_'s UBO (set 6 above); see that
        // struct's own doc for why.
        transparent_pass_ = std::make_unique<coopa::gfx::engine::passes::TransparentPass>(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT, *camera_layout_, *light_layout_,
            *shadow_layout_,
            config_.shaders("pbr.vert"),
            config_.shaders("transparent.frag"),
            transparent_extra,
            static_cast<uint32_t>(sizeof(TransparentRefractionPushConstants)),
            &material_cache_->layout_object());

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
                config_.shaders(sd.frag.empty() ? "transparent.frag" : sd.frag), sd.cull);
            transparent_capture_pass_->add_variant(
                sd.name, vert_spv,
                config_.shaders(sd.capture_frag.empty() ? "transparent_capture.frag" : sd.capture_frag));
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
        // particles), set 3 the material set (the sprite texture in its albedo slot).
        particle_pass_ = std::make_unique<passes::ParticlePass>(
            device_, allocator_, transparent_pass_->render_pass(),
            *camera_layout_, *light_layout_, ssr_pass_->hiz_layout(), material_cache_->layout_object(),
            config_.shaders("particle.vert"), config_.shaders("particle.frag"));
    }
    /**
     * @brief Builds the post-process chain in frame-graph order -- fog, volumetrics, DOF, bloom,
     * stylize, AA, tilt shift, upscale, debug lines -- resolving each stage's source view from
     * the previous stage's startup-fixed toggle.
     *
     * @param swapchain_pass The swapchain's render pass; upscale_pass_ is built against it.
     */
    void build_post_chain_(coopa::gfx::pipeline::RenderPass& swapchain_pass) {
        // Without SSR, post reads the deferred-lit+sky target; with it, ssr_pass_'s composite
        // output. Chosen once from ssr_enabled's STARTUP value -- rebinding it per frame would
        // need either a wait or a shader-side selector between two permanently-bound views (file
        // doc, rule 1). fog_pass_ reads this same fixed source.
        pre_fog_view_typed_ =
            config_.ssr_enabled ? ssr_pass_->output_view_typed() : offscreen_target_.color_view_typed();

        // Underwater: first in the chain, right after the transparent pass drew into the source
        // (so the water's underside is fogged by its in-water distance like everything else),
        // and ahead of global fog. Everything downstream then reads its output instead.
        if (config_.underwater_enabled) {
            underwater_pass_ = std::make_unique<passes::UnderwaterPass>(
                device_, underwater_target_.render_pass_object(),
                config_.shaders("fullscreen.vert"), config_.shaders("underwater.frag"));
            underwater_pass_->set_source_images(pre_fog_view_typed_, gbuffer_target_.g1_view_typed(),
                                                gbuffer_target_.g2_view_typed(), linear_sampler_);
            pre_fog_view_typed_ = underwater_target_.color_view_typed();
        }

        // Fog composite -- always constructed, gated per frame on fog_enabled. Reads
        // pre_fog_view_typed_, so transparent geometry (drawn in place into that same image) is
        // fogged too.
        fog_pass_ = std::make_unique<coopa::gfx::engine::passes::FogPass>(
            device_, fog_target_.render_pass_object(), fog_data_.buffer(),
            config_.shaders("fullscreen.vert"),
            config_.shaders("fog.frag"));
        fog_pass_->set_source_images(pre_fog_view_typed_, gbuffer_target_.g1_view_typed(),
                                     gbuffer_target_.g2_view_typed(), linear_sampler_);

        // What wind reads: fog's output if fog ran this build, otherwise straight through
        // to the pre-fog source. A separate link rather than folding wind into the
        // expression below, so wind works with fog DISABLED -- the two effects are
        // independent toggles. Startup-fixed, exactly like pre_fog_view_typed_ above.
        // MERGED fog+volumetrics: when both are on, volumetrics_pass_ applies the global
        // fog term itself (gfx_fog_apply, the same call fog.frag makes) over the pre-fog
        // image, and fog_pass_ never draws -- saving one full-resolution HDR pass and its
        // target's worth of bandwidth every frame. Startup-fixed like the views it feeds,
        // since both toggles already are; record_post_chain_() and
        // update_volumetrics_data_() both re-derive it from the same two flags.
        coopa::gfx::TextureView pre_volumetrics_view =
            (config_.fog_enabled && !fog_merged_into_volumetrics_())
                ? fog_target_.color_view_typed() : pre_fog_view_typed_;

        // Volumetric wind -- always constructed (same always-on-but-runtime-gated policy as
        // fog_pass_ above); render() checks config_.volumetrics_enabled per frame. Where fog
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
                volumetrics_data_.buffer(), fog_data_.buffer());
            froxel_volumetrics_pass_->set_source_images(pre_volumetrics_view, gbuffer_target_.g1_view_typed(),
                                                        gbuffer_target_.g2_view_typed(), linear_sampler_);
            froxel_volumetrics_pass_->set_shadow_images(shadow_target_.dir_shadow_view_typed(),
                                                        shadow_target_.spot_shadow_view_typed(), shadow_sampler_);
        } else {
        volumetrics_pass_ = std::make_unique<coopa::gfx::engine::passes::VolumetricsPass>(
            device_, volumetrics_march_target_.render_pass_object(),
            volumetrics_target_.render_pass_object(), volumetrics_data_.buffer(),
            fog_data_.buffer(),
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
                                             shadow_target_.spot_shadow_view_typed(), shadow_sampler_);
        }

        // What DOF reads: the final pre-tonemap HDR frame, before any lens effect. Sourcing the
        // full chain here (rather than the pre-SSR image) is what puts SSR reflections, BLEND
        // geometry and fog into the defocus, and transitively into bloom.
        coopa::gfx::TextureView pre_dof_view =
            config_.volumetrics_enabled ? volumetrics_target_.color_view_typed() : pre_volumetrics_view;

        // Physically-based depth of field (thin-lens CoC -> half-res bokeh gather -> full-res
        // composite). Sized to render_extent_, not the display rect: depth only exists at
        // render_extent_, and DOF is a property of the image being formed rather than a filter
        // over the finished pixel-art frame the way tilt shift is.
        dof_pass_ = std::make_unique<coopa::gfx::engine::passes::DofPass>(
            device_, allocator_, render_extent_.width, render_extent_.height,
            pre_dof_view, gbuffer_target_.depth_view_typed(),
            linear_sampler_, nearest_sampler_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("dof_coc.frag"),
            config_.shaders("dof_bokeh.frag"),
            config_.shaders("dof_composite.frag"));

        // What BOTH pixel_stylize_pass_ and bloom_pass_ read. One local, not two expressions, so
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

        pixel_stylize_pass_ = std::make_unique<coopa::gfx::engine::passes::PixelStylizePass>(
            device_, post_target_.render_pass_object(),
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
            config_.bloom_enabled ? &linear_sampler_ : nullptr,
            // Same conditional-binding rule as bloom above, for the same reason.
            exposure_pass_ ? exposure_pass_->output_view_typed() : coopa::gfx::TextureView{},
            exposure_pass_ ? &exposure_pass_->sampler() : nullptr,
            // The grading LUT is always a valid texture (a 1x1 dummy when no path is
            // configured), so unlike the two above it binds unconditionally -- grading_size
            // 0 is what disables the lookup.
            grading_lut_.view_typed(), &grading_lut_.sampler_object());

        // Debug view: replaces post_target_'s draw with a raw intermediate-buffer readout --
        // see PixelRenderConfig::debug_view / the DebugView enum for the full option list, and
        // record_post_chain_() for how the choice among this / pixel_stylize_pass_ / DOF-CoC /
        // volumetrics-density is made every frame. Always constructed (same always-built,
        // runtime-gated policy as fog_pass_/volumetrics_pass_ above), so debug_view stays a
        // runtime field.
        //
        // Shares gfxcoopa's DeferredLightingPass with pixel_lighting_pass_ above -- same 3
        // leading sets (camera/light/shadow) and the same owned 5-binding G-buffer+SSAO set,
        // via debug_view.frag, which declares an identical descriptor contract to
        // pixel_lighting.frag's so "direct"/"indirect"/"shadows"/"contact_shadows"/"ssao" are
        // computed by the exact same functions the shipped lighting term uses. The one extra
        // set (SSR reflection / traced-SSGI / G-buffer depth) is what pixel_lighting_pass_
        // doesn't need and this shader does, for the "ssr"/"ssr_confidence"/"ssgi"/"depth"
        // channels.
        debug_view_extra_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(2, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(3, coopa::gfx::ShaderStage::Fragment)
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
            // The depth buffer feeds the resolve's camera reprojection; like DoF's and the
            // stylize pass's bindings above, it is the G-buffer depth at render_extent_.
            taa_pass_->set_source_images(post_target_.color_image_object()->view_typed(),
                                         gbuffer_target_.depth_view_typed());
        }

        // Single source of truth for everything downstream of post_target_/aa_target_ -- same
        // pattern as post_source_view/pre_fog_view_typed_ above. aa_target_ is null whenever
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
            display_source_view, nearest_sampler_,
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
            config_.shaders("pbr.vert"), config_.shaders("transparent_preview.frag"));
        transparent_preview_pass_->set_scene_depth(gbuffer_target_.depth_view_typed(), nearest_sampler_,
                                                   render_extent_.width, render_extent_.height);
    }
    /**
     * @brief Builds the scene-depth descriptor set the world-space UI pass compares against. The PASS
     * itself is built by rebuild_world_ui_pass_(), since it follows the window size.
     */
    void build_world_ui_descriptor_() {
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
    /**
     * @brief The occlusion image both lighting and SSR sample: SsaoPass's blurred output when
     *        ssao_enabled, else its permanent 1x1 neutral texture.
     *
     * ssao_enabled is startup-fixed, so every binding site calls this once at construction;
     * rebinding per frame would be unsafe under this pipeline's frame overlap (class doc).
     */
    VkImageView ssao_source_view_() const {
        return config_.ssao_enabled ? ssao_pass_->output_view() : ssao_pass_->neutral_view();
    }

    /**
     * @brief Records the scene: shadow maps, G-buffer, Hi-Z, the transparent capture, SSAO,
     *        deferred lighting and sky, SSR, and the forward transparent pass.
     */
    void record_scene_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx,
                       const MeshGather& meshes, const std::vector<SdfDrawItem>& sdf_draws) {
        record_directional_shadow_(cmd, meshes, sdf_draws, ctx.cast_dir_shadow);
        gpu_mark_(cmd, GpuScope::ShadowDirectional);
        record_point_shadow_(cmd, meshes, sdf_draws, ctx.shadow_point, ctx.cast_point_shadow);
        gpu_mark_(cmd, GpuScope::ShadowPoint);
        record_spot_shadow_(cmd, meshes, sdf_draws, ctx.cast_spot_shadow);
        gpu_mark_(cmd, GpuScope::ShadowSpot);
        record_gbuffer_(cmd, meshes, sdf_draws);
        gpu_mark_(cmd, GpuScope::GBuffer);
        if (focus_probe_wanted_) record_focus_probe_(cmd);

        if (ctx.need_ssr_trace_inputs) {
            hiz_pass_->execute(cmd, gbuffer_target_.depth_image_handle(), gbuffer_target_.depth_view_typed());
            // The SSAO march reads its own prefiltered pyramid, not the min() chain above.
            // transition_depth = false: hiz_pass_ just performed the depth transition, and
            // ssao_enabled implies need_ssr_trace_inputs, so it always has by this point.
            if (config_.ssao_enabled) {
                ao_depth_pyramid_pass_->execute(cmd, gbuffer_target_.depth_image_handle(),
                                                gbuffer_target_.depth_view_typed(),
                                                /*transition_depth=*/false);
            }
            gpu_mark_(cmd, GpuScope::HiZ);
        } else {
            // GBufferTarget's render pass leaves depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL (only the
            // colour attachments end in SHADER_READ_ONLY_OPTIMAL), but pixel_stylize.frag samples
            // depth for the outline edge detector. When Hi-Z runs, HiZPass::execute() performs this
            // same transition instead -- doing both would present a stale oldLayout on the second. Not
            // transitioned back: GBufferTarget declares depth initialLayout = UNDEFINED, so next
            // frame's begin() does not care what it is left in.
            transition_gbuffer_depth_to_shader_read_(cmd);
        }

        // Capture transparent geometry's own depth/normal/position/shaded colour and build its
        // Hi-Z pyramid and scene-colour mip chain, so ssr_pass_ below can trace a SECOND source
        // and let opaque surfaces reflect transparent ones. Placed right after the opaque Hi-Z
        // block: it needs only the shadows recorded above and the light UBO, and it must finish
        // before ssr_pass_->execute().
        if (secondary_this_frame_) {
            record_transparent_capture_(cmd, meshes, sdf_draws);
            // TransparentCaptureTarget is out of scope for the Vulkan-sealing refactor
            // (MRT, no sealed equivalent -- see gfxcoopa's plan), so its raw VkImageView
            // accessors are wrapped here via detail::wrap() rather than gaining
            // TextureView-returning siblings themselves.
            transparent_hiz_pass_->execute(cmd, transparent_capture_target_.depth_image_handle(),
                                           coopa::gfx::detail::wrap(transparent_capture_target_.depth_view()));
            transparent_scene_color_mip_pass_->execute(
                cmd, coopa::gfx::detail::wrap(transparent_capture_target_.shaded_color_view()));

            // Bind ssr_pass_'s secondary source (sets 4-6) to these now-populated, correctly
            // laid-out images -- deliberately not done at construction, see that call site. Once
            // only: the views never change, and the first such frame is one
            // trace_inputs_need_rebind_() made wait for the GPU before begin_frame().
            if (!ssr_secondary_bound_) {
                ssr_pass_->set_secondary_source(
                    coopa::gfx::detail::wrap(transparent_capture_target_.normal_metallic_view()),
                    coopa::gfx::detail::wrap(transparent_capture_target_.position_roughness_view()),
                    transparent_hiz_pass_->full_hiz_view_typed(), transparent_hiz_pass_->sampler(),
                    transparent_scene_color_mip_pass_->full_view_typed(), transparent_scene_color_mip_pass_->sampler());
                ssr_secondary_bound_ = true;
            }
            gpu_mark_(cmd, GpuScope::TransparentCapture);
        }


        // The AO sample jitter advances EVERY frame the accumulator is running, still or
        // moving -- the resolved image is then always the same many-frame average, so
        // stopping the camera cannot reveal a different-looking AO (the artifact every
        // "moving vs still" state split produced). Only once the camera has been still for
        // kTemporalFreezeAfter frames do the stochastic terms freeze -- the AO jitter
        // (together with the resolve's verbatim history hold) and SSR/SSGI's ray jitter --
        // which is what makes a resting image byte-static (the static_camera_converges /
        // image_settles_after_camera_stops contracts). Motion is tested on the UNJITTERED
        // view-projection: TAA's sub-pixel jitter changes proj every frame by design, and a
        // pure equality check would report motion forever on the last bits of a smoothed
        // camera easing to rest, hence the epsilon.
        {
            // Stillness is a DEADBAND ON VISIBLE MOTION, measured against the pose the
            // camera last anchored at -- not a per-frame matrix-epsilon test. A hand
            // resting on the mouse emits sub-pixel micro deltas; under a per-frame test
            // each one unfroze the stochastic passes for the whole smoothing tail, blended
            // a handful of freshly jittered AO/SSR draws into the held averages, and
            // re-froze on a visibly different image -- a full-frame AO "pop" between
            // every two micro inputs. The anchor makes oscillation around a point read as
            // still (frozen, byte-static) while bounding staleness: sustained real drift
            // accumulates deviation from the anchor and exits the deadband.
            const glm::mat3 rot_cur(ctx.view);
            const glm::mat3 rot_anchor(ssao_freeze_anchor_view_);
            const float tr_anchor = glm::clamp(
                (rot_anchor[0][0]*rot_cur[0][0] + rot_anchor[0][1]*rot_cur[0][1] + rot_anchor[0][2]*rot_cur[0][2]
               + rot_anchor[1][0]*rot_cur[1][0] + rot_anchor[1][1]*rot_cur[1][1] + rot_anchor[1][2]*rot_cur[1][2]
               + rot_anchor[2][0]*rot_cur[2][0] + rot_anchor[2][1]*rot_cur[2][1] + rot_anchor[2][2]*rot_cur[2][2]
               - 1.0f) * 0.5f, -1.0f, 1.0f);
            const float anchor_angle_px =
                std::acos(tr_anchor) * ctx.proj[1][1] * 0.5f * static_cast<float>(render_extent_.height);
            const float anchor_trans = glm::length(ctx.cam_pos - ssao_freeze_anchor_pos_);
            const bool moved = anchor_angle_px > kFreezeDeadbandPx || anchor_trans > kFreezeDeadbandWu;
            if (moved) {
                ssao_freeze_anchor_view_ = ctx.view;
                ssao_freeze_anchor_pos_  = ctx.cam_pos;
            }
            // Approximate image-space camera speed for the AO blur's velocity widening: the
            // rotation angle between this frame's view and the last, converted to pixels at
            // the screen centre. Translation-only motion under-reports here, which is fine --
            // rotation is what sweeps AO detail across the grid fastest.
            {
                const glm::mat3 r_prev(ssao_prev_view_);
                const glm::mat3 r_cur(ctx.view);
                const float tr    = glm::clamp((r_prev[0][0]*r_cur[0][0] + r_prev[0][1]*r_cur[0][1] + r_prev[0][2]*r_cur[0][2]
                                              + r_prev[1][0]*r_cur[1][0] + r_prev[1][1]*r_cur[1][1] + r_prev[1][2]*r_cur[1][2]
                                              + r_prev[2][0]*r_cur[2][0] + r_prev[2][1]*r_cur[2][1] + r_prev[2][2]*r_cur[2][2]
                                              - 1.0f) * 0.5f, -1.0f, 1.0f);
                const float angle = std::acos(tr);
                ssao_motion_px_   = angle * ctx.proj[1][1] * 0.5f * static_cast<float>(render_extent_.height);
                ssao_prev_view_   = ctx.view;
            }
            camera_frames_still_  = moved ? 0u : camera_frames_still_ + 1u;
            temporal_frozen_      = camera_frames_still_ > kTemporalFreezeAfter;
            if (!temporal_frozen_) {
                ++ssao_rotation_index_;
            }
        }

        // Current clip space -> previous frame's clip space, shared by every temporally
        // accumulated pass below (the shared count buffer, SSAO, SSR/SSGI) and recomposed here
        // rather than per consumer so they can never disagree about where a pixel was.
        //
        // Composed in double, truncated to float only at the end -- same reasoning as
        // taa_params.reproject further down: the world-scale magnitudes inside the two
        // view-projections cancel in the double product, leaving a matrix whose float truncation
        // reprojects at sub-pixel accuracy. A float composition (or routing the reprojection
        // through the RGBA16F G-buffer position, which this matrix replaces) is off by whole
        // pixels at this scene's world-coordinate scale, which made the accumulated AO slide and
        // boil against the geometry in motion, and did the same to the reflections.
        const glm::mat4 temporal_reproject = glm::mat4(
            glm::dmat4(prev_view_proj_) *
            glm::inverse(glm::dmat4(ctx.proj) * glm::dmat4(ctx.view)));

        // Shared accumulation count, written before any consumer reads it. The cap is the
        // DEEPEST any consumer asks for; each one clamps the count to its own depth in-shader,
        // so one buffer serves accumulation schedules of different lengths.
        {
            coopa::gfx::engine::passes::TemporalHistoryPass::Params th_params{};
            th_params.reproject       = temporal_reproject;
            th_params.reproject_valid = prev_view_proj_valid_;
            th_params.max_accum       = std::max({config_.ssr_temporal_frames,
                                                  config_.ssgi_temporal_frames,
                                                  config_.contact_shadow_temporal_frames});
            // Same three-line camera idiom every depth-linearizing consumer in this file uses.
            th_params.near_z          = ctx.cam ? ctx.cam->clip_start : 0.1f;
            th_params.far_z           = ctx.cam ? ctx.cam->clip_end : 1000.0f;
            th_params.perspective     =
                (!ctx.cam || ctx.cam->type == coopa::gfx::engine::components::CameraType::Perspective);
            temporal_history_pass_->execute(cmd, th_params);
            gpu_mark_(cmd, GpuScope::TemporalHistory);
        }

        // Contact shadows, into their own buffer, before the lighting pass that reads it.
        {
            // The march's four knobs are RUNTIME, and an accumulated average necessarily lags a
            // change to any of them -- at a depth of 16 a new value is still a third blended out
            // eight frames later, which reads as the setting "not taking effect". Dropping the
            // history on a change makes the next frame show the new march outright, which is
            // what a live tweak has to do; the accumulation then rebuilds from there.
            const ContactShadowKnobs knobs{config_.contact_shadow_length,
                                           config_.contact_shadow_strength,
                                           config_.contact_shadow_thickness,
                                           config_.contact_shadow_steps,
                                           config_.contact_shadows_enabled};
            if (!(knobs == contact_shadow_knobs_)) {
                contact_shadow_pass_->invalidate_history();
                contact_shadow_knobs_ = knobs;
            }

            passes::ContactShadowPass::Params cs_params{};
            cs_params.reproject       = temporal_reproject;
            cs_params.reproject_valid = prev_view_proj_valid_;
            cs_params.temporal_frames = config_.contact_shadow_temporal_enabled
                ? config_.contact_shadow_temporal_frames : 0;
            cs_params.temporal_gamma  = config_.ssr_temporal_gamma;
            cs_params.frozen          = temporal_frozen_;
            cs_params.count_parity    = temporal_history_pass_->current_parity();
            if (config_.contact_shadows_enabled) {
                contact_shadow_pass_->execute(cmd, current_camera_set(), current_light_set(), cs_params);
                contact_output_cleared_ = false;
                gpu_mark_(cmd, GpuScope::ContactShadows);
            } else if (!contact_output_cleared_) {
                // Off: two full-resolution passes and a history copy would only ever produce 0.
                // One clear leaves the buffer valid and neutral until the toggle flips back.
                contact_shadow_pass_->clear_output(cmd);
                contact_output_cleared_ = true;
                gpu_mark_(cmd, GpuScope::ContactShadows);
            }
        }

        if (config_.ssao_enabled) {
            coopa::gfx::engine::passes::SsaoPass::Params ssao_params{};
            ssao_params.radius               = config_.ssao_radius;
            ssao_params.bias                 = config_.ssao_bias;
            ssao_params.power                = config_.ssao_power;
            ssao_params.slices               = config_.ssao_slices;
            ssao_params.steps                = config_.ssao_steps;
            ssao_params.max_radius_px        = config_.ssao_max_radius_px;
            ssao_params.blur_plane_sigma     = config_.ssao_blur_plane_sigma;
            ssao_params.blur_light           = config_.ssao_blur_light;
            ssao_params.max_mip              = static_cast<int>(ao_depth_pyramid_pass_->max_mip_level());
            // ssao_rotation_index_, not frame_index_: the AO slice rotation must hold still
            // whenever the camera does, or ssao_resolve.frag's accumulator has a fresh estimate
            // to chase every frame and never converges -- the image would keep visibly settling
            // for dozens of frames after the camera stopped. See that member's own doc.
            //
            // 0xFF, NOT 0x7. The mask is the PERIOD of the rotation, and at 8 it would be exactly
            // apply_taa_jitter_()'s Halton period, phase-locking the two so every 8th frame
            // reproduced both identically -- which defeats the decorrelation ssao.frag's comment
            // says this rotation exists to provide. 256 matches what SSR (ssr_params.frame_index
            // below) and the shadow PCF rotation (dir_shadow_extra.w) use, so no per-frame term
            // shares a period with the jitter.
            ssao_params.noise_rotation        = static_cast<int>(ssao_rotation_index_ & 0xFFu);
            ssao_params.temporal_enabled     = config_.ssao_temporal_enabled;
            ssao_params.temporal_frames      = config_.ssao_temporal_frames;
            ssao_params.reproject        = temporal_reproject;
            ssao_params.reproject_valid  = prev_view_proj_valid_;
            // The eye ssao_resolve.frag measures its stored distance channel against.
            ssao_params.camera_pos           = ctx.cam_pos;
            ssao_params.frozen               = temporal_frozen_;
            ssao_params.motion_px            = ssao_motion_px_;
            ssao_pass_->execute(cmd, current_camera_set(), ssao_params);
            gpu_mark_(cmd, GpuScope::Ssao);
        } else {
            ssao_pass_->invalidate_history();
        }
        // Note: the SSAO image bound into pixel_lighting_pass_/ssr_pass_ is decided
        // once at construction (config_.ssao_enabled doesn't change at runtime), not
        // here -- see the constructor's comment on why a per-frame rebind would violate
        // this pipeline's frame-overlap model.

        // One fullscreen draw writes every pixel of offscreen_target_: lit surfaces, and the
        // procedural sky at background pixels (pixel_lighting.frag -- formerly a second
        // SkyboxPass draw that re-read every pixel's normal just to discard the lit ones).
        offscreen_target_.begin(cmd);

        PixelLightingPushConstants lighting_pc;
        lighting_pc.light_bands       = config_.light_bands;
        lighting_pc.spec_threshold    = config_.spec_threshold;
        lighting_pc.rim_strength      = config_.rim_strength;
        lighting_pc.ambient_intensity = config_.indirect.ambient_intensity;
        lighting_pc.sky_intensity     = config_.indirect.sky_intensity;
        lighting_pc.soft_lighting     = config_.soft_lighting ? 1.0f : 0.0f;
        lighting_pc.ssao_direct_strength = config_.ssao_direct_lighting_strength;
        // The sky is drawn by this same pass at background pixels (see pixel_lighting.frag),
        // from the matrix SkyboxPass used to compute: identical sky, one fullscreen draw.
        lighting_pc.sky_inv_view_proj    = glm::inverse(ctx.proj * ctx.view);
        // gfxcoopa's DeferredLightingPass::draw() pushes this internally now (the
        // templated overload), after its own bind_pipeline() -- no separate push needed.
        pixel_lighting_pass_->draw(cmd, current_camera_set(), current_light_set(), *shadow_set_, lighting_pc,
                                  render_extent_.width, render_extent_.height);


        offscreen_target_.end(cmd);
        gpu_mark_(cmd, GpuScope::LightingSky);

        if (ctx.need_ssr_trace_inputs) {
            // Prefiltered scene-colour mip chain: also feeds transparent.frag's
            // gfx_ssr_trace() cone-LOD taps and SSGI bounce lookup, not just ssr.frag's
            // own -- see need_ssr_trace_inputs' own doc.
            scene_color_mip_pass_->execute(cmd, offscreen_target_.color_view_typed());
            gpu_mark_(cmd, GpuScope::SceneColorMips);
        }

        // Runs on debug_view frames too: with ssr_enabled, the whole post chain reads
        // ssr_pass_'s composite output (see pre_fog_view_typed_ in build_post_chain_), so
        // skipping the composite would leave that image never written this frame. The
        // "ssr"/"ssr_confidence"/"ssgi" channels also read this pass's own resolved buffers
        // directly (see debug_view_pass_'s extra set), so it must run whenever a channel
        // might want them, not just when lighting does.
        if (config_.ssr_enabled) {
            coopa::gfx::engine::passes::SsrPass::Params ssr_params{};
            ssr_params.proj              = ctx.proj;
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
            ssr_params.temporal_frames   = config_.ssr_temporal_frames;
            ssr_params.ssgi_temporal_frames = config_.ssgi_temporal_frames;
            // Only reached where the shared count buffer is absent, which never happens in this
            // pipeline (set_temporal_count_image() is called at construction) -- carried so the
            // pass's own fallback path stays configurable rather than hardcoded.
            ssr_params.temporal_blend    = config_.ssr_temporal_blend;
            ssr_params.temporal_gamma    = config_.ssr_temporal_gamma;
            // Holds the accumulated reflection verbatim once the camera has been still long
            // enough, the same way the AO resolve holds its own -- that verbatim hold, not a
            // frozen ray jitter, is what makes a resting image byte-static.
            ssr_params.temporal_frozen   = temporal_frozen_;
            ssr_params.reproject         = temporal_reproject;
            ssr_params.reproject_valid   = prev_view_proj_valid_;
            ssr_params.ssr_blur_radius   = config_.ssr_blur_radius;
            ssr_params.ssr_blur_light    = config_.ssr_blur_light;
            ssr_params.count_parity      = temporal_history_pass_->current_parity();
            ssr_params.ssr_blur_zero_skip = config_.ssr_blur_zero_skip;
            ssr_params.jitter_strength   = config_.ssr_jitter;
            // Matches ssr_ign2()'s own `frame & 0xFF` mask in gfx/ssr_common.glsl -- and the
            // same 256 period SSAO's noise_rotation and the shadow PCF rotation use, so no
            // per-frame term in this pipeline shares a period with the TAA jitter.
            // frame_index_, advancing every frame including at rest: the resolve is a
            // converging running average, so it needs a FRESH ray every frame to average -- a
            // held jitter would pin it to one draw's grain. The resting image is made static by
            // that resolve's verbatim hold (temporal_frozen above), not by freezing the input.
            ssr_params.frame_index       = static_cast<int>(frame_index_ & 0xFFu);
            // Fed from the SAME config_.indirect instance as lighting_pc above -- see
            // IndirectParams' doc (render_features.h) for why this must stay one source.
            ssr_params.sky_intensity     = config_.indirect.sky_intensity;
            ssr_params.ssgi_intensity    = config_.indirect.ssgi_intensity;
            ssr_params.ssgi_distance     = config_.indirect.ssgi_distance;
            ssr_params.ssgi_max_distance   = config_.ssgi_max_distance;
            ssr_params.ssgi_blur_radius    = config_.ssgi_blur_radius;
            ssr_params.ssgi_blur_light     = config_.ssgi_blur_light;
            ssr_params.ssgi_max_iterations = config_.ssgi_max_iterations;
            ssr_params.sky_zenith        = config_.indirect.sky_zenith;
            ssr_params.sky_horizon       = config_.indirect.sky_horizon;
            ssr_params.sky_ground        = config_.indirect.sky_ground;

            // Secondary source -- see SsrPushConstants' own doc. max_hiz_mip_b/
            // max_color_mip_b come from transparent_hiz_pass_/transparent_scene_
            // color_mip_pass_'s OWN mip counts, not the primary pyramid's (they're
            // independent instances, possibly over a differently-sized image chain).
            ssr_params.has_secondary   = secondary_this_frame_;
            ssr_params.skip_threshold  = config_.ssr_skip_negligible ? config_.ssr_skip_threshold : 0.0f;
            ssr_params.max_hiz_mip_b   = static_cast<int>(transparent_hiz_pass_->max_mip_level());
            ssr_params.max_color_mip_b = static_cast<int>(transparent_scene_color_mip_pass_->max_mip_level());

            ssr_pass_->execute(cmd, current_camera_set(), ssr_params);
            // GPU scopes for SsrPass come from its stage hook (see set_profiler()).
        }

        if (refraction_this_frame_) {
            // Builds refraction's own scene-colour chain -- the dedicated instance transparent.frag's
            // u_scene_color reads whenever refraction is active and a BLEND mesh is in view (see
            // refraction_this_frame_). It must be a SEPARATE instance, not a
            // second execute() on scene_color_mip_pass_: that pass rebinds its internal per-mip
            // descriptor sets on every execute(), and doing so twice in one not-yet-submitted command
            // buffer invalidates it. MESH-only -- BLEND SDFs keep reading the original chain.
            coopa::gfx::TextureView refraction_source =
                (config_.refraction_include_reflections && config_.ssr_enabled)
                    ? ssr_pass_->output_view_typed() : offscreen_target_.color_view_typed();
            refraction_scene_color_mip_pass_->execute(cmd, refraction_source);
            gpu_mark_(cmd, GpuScope::RefractionMips);
        }

        if (config_.transparency_enabled) {
            // Per-frame globals for the forward MESH pass, from the same config_ fields the SDF path's
            // own globals() fill uses -- in particular config_.indirect, shared with SsrPass::Params so
            // the opaque and transparent indirect terms can never disagree. Uploaded to this frame's
            // slot, then bound once per mesh-kind transition in record_transparent_() as set 6.
            forward_globals_.begin(ctx.frame_slot);
            fill_forward_globals_(forward_globals_.globals());
            forward_globals_.upload();

            // Depth is always SHADER_READ_ONLY_OPTIMAL by this point -- both branches
            // above (HiZPass::execute() when need_ssr_trace_inputs, transition_gbuffer_
            // depth_to_shader_read_() otherwise) leave it there; see that if/else's own
            // comment. transparency_enabled implies need_ssr_trace_inputs, so
            // HiZPass::execute() is always the branch taken here.
            VkImageView hdr_source_view = config_.ssr_enabled
                ? ssr_pass_->output_view() : offscreen_target_.color_view();
            bool transparent_ran = record_transparent_(cmd, meshes, sdf_draws, hdr_source_view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ctx.cam_pos);
            // TransparentPass's render pass declares DEPTH_STENCIL_READ_ONLY_OPTIMAL as both the
            // initial and final layout of its depth attachment, so depth comes out of
            // record_transparent_() in that layout rather than the SHADER_READ_ONLY_OPTIMAL
            // pixel_stylize_pass_'s pre-bound descriptor expects. record_transparent_() early-returns
            // without touching depth when the scene has no BLEND renderers this frame, in which case
            // depth is still where the branch above left it and this transition must be skipped --
            // issuing it anyway asserts a false oldLayout and trips synchronization validation.
            if (transparent_ran) {
                transition_gbuffer_depth_to_shader_read_(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            }
            gpu_mark_(cmd, GpuScope::Transparent);
        }

    }

    /**
     * @brief Records the post-process chain: fog, volumetrics, DOF, bloom, the stylize pass
     *        (with the debug-line overlay as its guest), the world-UI layer, AA and tilt shift.
     *
     * Every stage is gated on a startup-fixed toggle, because each one's source image was
     * chosen from that toggle when the pipeline was built.
     */
    void record_post_chain_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
        using coopa::gfx::engine::components::CameraType;

        // Parsed once per frame -- debug_view is runtime (file doc, rule 1's exception list),
        // so every branch below that reads it must see the SAME frame's value.
        const DebugView active_view = parse_debug_view(config_.debug_view);

        // Fog composite. After the transparent pass, so BLEND geometry is fogged too (it was drawn
        // in place into the same image), and before pixel_stylize_pass_, so fog sits in linear HDR
        // ahead of tonemap/outline/dither/palette. Gated on the startup-fixed flag both this pass's
        // source and pixel_stylize_pass_'s were chosen from.
        // Skipped entirely on the merged path, where volumetrics_pass_ applies the same
        // global fog term itself -- see fog_merged_into_volumetrics_().
        // Underwater -- every frame once built (see UnderwaterPass's file doc); a copy unless the
        // camera is below a water surface.
        if (underwater_pass_) {
            underwater_target_.begin(cmd);
            underwater_pass_->draw(cmd, render_extent_.width, render_extent_.height, underwater_params_);
            underwater_target_.end(cmd);
            gpu_mark_(cmd, GpuScope::Underwater);
        }

        if (config_.fog_enabled && !fog_merged_into_volumetrics_()) {
            fog_target_.begin(cmd);
            fog_pass_->draw(cmd, render_extent_.width, render_extent_.height);
            fog_target_.end(cmd);
            gpu_mark_(cmd, GpuScope::Fog);
        }

        // Volumetric wind. After fog, so wisps layer over fogged geometry, and before DOF and
        // bloom, so they defocus with everything else and sun-lit ones glow. Gated on the same
        // startup-fixed flag its source view was chosen from.
        if (config_.volumetrics_enabled && froxel_volumetrics_pass_) {
            froxel_volumetrics_pass_->record_grid(cmd, static_cast<uint32_t>(frame_index_ & 1u));
            volumetrics_target_.begin(cmd);
            froxel_volumetrics_pass_->draw_apply(cmd, render_extent_.width, render_extent_.height);
            volumetrics_target_.end(cmd);
            gpu_mark_(cmd, GpuScope::Volumetrics);
        } else if (config_.volumetrics_enabled) {
            volumetrics_march_target_.begin(cmd);
            volumetrics_pass_->draw_march(cmd, volumetrics_march_target_.width(),
                                          volumetrics_march_target_.height());
            volumetrics_march_target_.end(cmd);

            volumetrics_target_.begin(cmd);
            volumetrics_pass_->draw_composite(cmd, render_extent_.width, render_extent_.height);
            volumetrics_target_.end(cmd);
            gpu_mark_(cmd, GpuScope::Volumetrics);
        }

        // Depth of field. After fog (so fogged geometry defocuses too) and before
        // bloom (so defocused HDR highlights bloom into real bokeh, rather than DOF
        // blurring an already-glowing image). Gated on the same startup-fixed flag
        // post_source_view was chosen from at construction.
        if (config_.dof_enabled) {
            coopa::gfx::engine::passes::DofPass::Params dof_params{};

            // Per-camera override > config.yaml > CameraComponent fallback. The <= 0
            // sentinel on both the config field and the camera field is what keeps
            // scenes/configs that set neither on the built-in defaults.
            dof_params.focal_length_mm = config_.dof_focal_length > 0.0f
                ? config_.dof_focal_length : (ctx.cam ? ctx.cam->lens : 50.0f);
            dof_params.sensor_width_mm = config_.dof_sensor_width > 0.0f
                ? config_.dof_sensor_width : (ctx.cam ? ctx.cam->sensor_width : 36.0f);
            dof_params.aperture = (ctx.cam && ctx.cam->aperture > 0.0f)
                ? ctx.cam->aperture : config_.dof_aperture;

            // Resolved once per frame, outside this recording lambda -- see
            // resolve_dof_focus_()'s own doc for why (dt-driven smoothing state).
            dof_params.focus_distance = glm::max(ctx.dof_focus_distance, 0.01f);
            dof_params.focus_range    = ctx.dof_focus_range;
            dof_params.blur_scale     = config_.dof_blur_scale;

            dof_params.max_radius        = config_.dof_max_radius;
            dof_params.sample_count      = config_.dof_sample_count;
            dof_params.blade_count       = config_.dof_blade_count;
            dof_params.blade_rotation_deg = config_.dof_blade_rotation;
            // Same three-line camera idiom pixel_stylize_pass_'s push constants use
            // below, for the same linearization formula (see gfx/depth.glsl).
            dof_params.camera_near           = ctx.cam ? ctx.cam->clip_start : 0.1f;
            dof_params.camera_far            = ctx.cam ? ctx.cam->clip_end : 1000.0f;
            dof_params.camera_is_perspective = (!ctx.cam || ctx.cam->type == CameraType::Perspective);
            dof_params.debug_view = (active_view == DebugView::Dof);

            dof_pass_->execute(cmd, dof_params);
            gpu_mark_(cmd, GpuScope::Dof);
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
            gpu_mark_(cmd, GpuScope::Bloom);
        }

        // Auto-exposure metering. Alongside the bloom pyramid rather than inside
        // post_target_'s bracket: it writes its own 1x1 target, and it must read the
        // same post_source_view image the stylize draw below tonemaps. The value it
        // produces is consumed NEXT frame (this frame's stylize draw was already
        // recorded against whatever the last execute() left in binding 5) -- one frame
        // of lag on a multi-hundred-millisecond adaptation ramp is not observable.
        if (exposure_pass_) {
            coopa::gfx::engine::passes::ExposurePass::PushConstants exposure_pc{};
            exposure_pc.dt           = frame_dt_;
            exposure_pc.speed_up     = config_.auto_exposure_speed_up;
            exposure_pc.speed_down   = config_.auto_exposure_speed_down;
            exposure_pc.compensation = config_.auto_exposure_compensation;
            exposure_pc.min_exposure = config_.auto_exposure_min;
            exposure_pc.max_exposure = config_.auto_exposure_max;
            exposure_pass_->execute(cmd, exposure_pc);
            gpu_mark_(cmd, GpuScope::Exposure);
        }

        post_target_.begin(cmd);
        if (debug_view_is_channel(active_view)) {
            // Replaces the stylize draw entirely -- debug_view_pass_ reads the G-buffer/
            // lighting/SSAO/SSR sources directly (see build_post_chain_'s own doc), so none
            // of post_source_view's tonemap/bloom/outline/dither/palette/grading chain
            // touches it: what lands on screen is exactly what the renderer computed.
            DebugViewPushConstants dbg_pc;
            dbg_pc.channel               = static_cast<int32_t>(active_view);
            dbg_pc.light_bands           = config_.light_bands;
            dbg_pc.spec_threshold        = config_.spec_threshold;
            dbg_pc.ambient_intensity     = config_.indirect.ambient_intensity;
            dbg_pc.sky_intensity         = config_.indirect.sky_intensity;
            dbg_pc.soft_lighting         = config_.soft_lighting ? 1.0f : 0.0f;
            dbg_pc.ssao_direct_strength  = config_.ssao_direct_lighting_strength;
            dbg_pc.camera_near           = ctx.cam ? ctx.cam->clip_start : 0.1f;
            dbg_pc.camera_far            = ctx.cam ? ctx.cam->clip_end : 1000.0f;
            dbg_pc.camera_is_perspective = (!ctx.cam || ctx.cam->type == CameraType::Perspective) ? 1.0f : 0.0f;
            dbg_pc.editor_ao             = (config_.editor_ssao && config_.ssao_enabled) ? 1.0f : 0.0f;
            dbg_pc.editor_xray_alpha     = std::clamp(config_.editor_xray_alpha, 0.0f, 1.0f);
            debug_view_pass_->draw(cmd, current_camera_set(), current_light_set(), *shadow_set_, dbg_pc,
                                   render_extent_.width, render_extent_.height);
            if (debug_view_is_editor_shading(active_view) && frame_meshes_) {
                record_transparent_preview_(cmd, *frame_meshes_, active_view, ctx.cam_pos);
            }
        } else {
            // Dof/Volumetrics debug views already replaced post_source_view's content with a
            // raw intermediate buffer (dof_pass_/volumetrics_pass_'s own debug branch, driven
            // by active_view above) -- raw_passthrough reduces every OTHER stylize step to its
            // documented no-op value so the tonemap/bloom/outline/dither/palette/grading chain
            // never rescales that buffer on its way to the screen. Off/Lines take none of this.
            const bool raw_passthrough = (active_view == DebugView::Dof || active_view == DebugView::Volumetrics);

            coopa::gfx::engine::passes::PixelStylizePass::PushConstants post_pc;
            post_pc.outline_color    = config_.outline_color;
            post_pc.inv_render_size  = glm::vec2(1.0f / render_extent_.width, 1.0f / render_extent_.height);
            post_pc.outline_thickness = (config_.outline_enabled && !raw_passthrough) ? config_.outline_thickness : 0.0f;
            post_pc.depth_threshold  = config_.depth_threshold;
            post_pc.normal_threshold = config_.normal_threshold;
            post_pc.dither_strength  = (config_.dither_enabled && !raw_passthrough) ? config_.dither_strength : 0.0f;
            post_pc.palette_count    = (config_.palette_enabled && !raw_passthrough)
                ? static_cast<float>(palette_lut_.count()) : 0.0f;
            post_pc.camera_near           = ctx.cam ? ctx.cam->clip_start : 0.1f;
            post_pc.camera_far            = ctx.cam ? ctx.cam->clip_end : 1000.0f;
            post_pc.camera_is_perspective = (!ctx.cam || ctx.cam->type == CameraType::Perspective) ? 1.0f : 0.0f;
            // This pipeline has no separate tonemap pass, unlike blendy -- exposure > 0
            // keeps pixel_stylize.frag's tonemap step live (see PushConstants::exposure's doc).
            post_pc.exposure         = raw_passthrough ? 0.0f : config_.exposure;
            post_pc.bloom_intensity  = (config_.bloom_enabled && !raw_passthrough) ? config_.bloom_intensity : 0.0f;
            post_pc.auto_exposure    = (exposure_pass_ && !raw_passthrough) ? 1.0f : 0.0f;
            post_pc.grading_size     = (config_.grading_enabled && !raw_passthrough)
                ? static_cast<float>(grading_lut_.size()) : 0.0f;
            pixel_stylize_pass_->draw(cmd, post_pc, render_extent_.width, render_extent_.height);
        }
        // Also whenever a host filled lines itself (the editor's grid, gizmos and wireframe): Engine
        // clears debug_lines_ every frame unless debug_view is "lines", so a game frame with no host
        // lines draws exactly as before.
        if (active_view == DebugView::Lines || !debug_lines_.empty()) {
            debug_line_pass_->draw(cmd, ctx.proj * ctx.view,
                LetterboxRect{0, 0, render_extent_.width, render_extent_.height});
        }
        post_target_.end(cmd);
        gpu_mark_(cmd, GpuScope::Stylize);

        // World-space UI, into its OWN layer rather than post_target_: everything from here to the
        // composite (AA, tilt shift) is a filter over the finished frame, and the UI has to sit
        // above all of it.
        //
        // Cleared to fully TRANSPARENT black -- this is a coverage layer, not an image, and
        // ui_composite.frag reads that alpha as "how much UI is here".
        //
        // unjittered_proj, NOT proj: this layer is composited after taa_pass_ resolves and is never
        // one of its inputs, so a jitter applied here would never be averaged out -- the scene would
        // resolve stable while the UI wobbled through the 8-frame cycle. The tradeoff is that
        // ui_world_occlude.glsl's depth compare samples a G-buffer that IS still jittered, so an
        // occluded edge is displaced by up to half a render pixel; that is confined to the
        // silhouette of occluding geometry, where a whole-canvas wobble was not. The UI's own depth
        // is unaffected either way -- the jitter is a constant NDC x/y shift (see
        // apply_taa_jitter_), so gl_FragCoord.z is identical with or without it.
        //
        // Drawn at the DISPLAY rect, not render_extent_, so the composite samples it 1:1; at render
        // resolution it needed a non-integer NEAREST upscale and came out visibly stepped. The
        // residual cost is that the depth texture it compares against is lower-resolution than the
        // layer, so an occluded edge stair-steps on the render grid while the canvas's own edges
        // stay crisp.
        //
        // ui_composite_pass_ samples the target every frame, so it must always hold a valid,
        // SHADER_READ_ONLY_OPTIMAL image -- but with no canvases that image is just transparent
        // black, and once it has been cleared to that it stays correct. So an empty frame skips
        // the display-resolution clear whenever the previous write was already empty.
        const bool world_ui_empty = world_canvases_.empty() || !world_ui_pass_;
        if (ui_world_target_ && !(world_ui_empty && ui_world_layer_clear_)) {
            ui_world_layer_clear_ = world_ui_empty;
            ui_world_target_->begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
            if (world_ui_pass_) {
                for (coopa::ui::CanvasComponent* canvas : world_canvases_) {
                    world_ui_pass_->draw(cmd, ctx.frame_slot,
                                         upscaled_extent_.w, upscaled_extent_.h,
                                         ctx.unjittered_proj * ctx.view, *canvas);
                }
            }
            ui_world_target_->end(cmd);
            gpu_mark_(cmd, GpuScope::WorldUi);
        }

        // Anti-aliasing, after pixel_stylize_pass_ and before tilt shift. A no-op when aa_mode is
        // "off", which is also when aa_target_ is null. Debug lines DO get AA'd, since
        // debug_line_pass_ draws as a guest inside post_target_ upstream of this -- desirable,
        // wireframe edges being the jaggiest thing on screen.
        if (aa_target_) {
            if (config_.aa_mode == "fxaa") {
                coopa::gfx::engine::passes::FxaaPass::PushConstants fxaa_pc{};
                fxaa_pc.screen_width        = static_cast<float>(render_extent_.width);
                fxaa_pc.screen_height       = static_cast<float>(render_extent_.height);
                fxaa_pc.subpixel_quality    = config_.fxaa_subpixel;
                fxaa_pc.edge_threshold      = config_.fxaa_edge_threshold;
                fxaa_pc.edge_threshold_min  = config_.fxaa_edge_threshold_min;
                aa_target_->begin(cmd);
                fxaa_pass_->draw(cmd, fxaa_pc, render_extent_.width, render_extent_.height);
                aa_target_->end(cmd);
            } else if (config_.aa_mode == "smaa") {
                // SmaaPass owns all three of its own begin/end brackets (edge -> blend
                // -> neighborhood-into-output_target) -- unlike fxaa_pass_ above, the
                // caller doesn't bracket this one itself.
                smaa_pass_->draw(cmd, *aa_target_, /*exposure (unused by the shader body,
                                 see SmaaPass::NeighborhoodPush's doc)*/ 1.0f,
                                 config_.smaa_threshold, config_.smaa_max_search_steps,
                                 render_extent_.width, render_extent_.height);
            } else if (config_.aa_mode == "taa") {
                taa_pass_->prepare_history(cmd);
                coopa::gfx::engine::passes::TaaPass::Params taa_params{};
                // Maps current jittered clip space to LAST frame's unjittered clip space:
                // the current jitter is removed separately in the shader (jitter_ndc), and
                // the previous frame's never enters -- both unjittered, so the velocity it
                // yields is exactly zero for a still camera. SSAO/SSR deliberately keep
                // using the JITTERED prev_view_proj_ instead (see their param fill above).
                //
                // The inverse runs in DOUBLE precision: a view-projection with a world
                // translation of hundreds of units is ill-conditioned enough that an fp32
                // inverse leaves ~1e-4 UV of noise in the reprojected position -- a quarter
                // pixel at 1080p, which resamples the converged history at a wandering
                // sub-pixel offset every frame and reads as permanent shimmer at rest. The
                // composed matrix is near-identity, so the cast back to fp32 is harmless.
                taa_params.reproject       = glm::mat4(
                    glm::dmat4(prev_unjittered_view_proj_) *
                    glm::inverse(glm::dmat4(ctx.proj * ctx.view)));
                taa_params.reproject_valid = prev_view_proj_valid_;
                taa_params.jitter_ndc      = taa_jitter_ndc_;
                // Any debug view other than "off"/"lines" (channel views AND the DOF-CoC/
                // volumetrics-density in-chain views) is a raw per-frame readout -- history
                // blend forced to 0 so a jittered term's flicker shows up honestly instead of
                // getting smoothed away by TAA's own accumulation (the very thing a debug view
                // exists to let you see). "lines" overlays wireframes on the NORMAL image, so
                // it keeps normal TAA, as do the editor's viewport shading modes.
                const bool raw_taa = (active_view != DebugView::Off && active_view != DebugView::Lines &&
                                      !debug_view_is_editor_shading(active_view));
                taa_params.feedback_still  = raw_taa ? 0.0f : config_.taa_blending_weight;
                taa_params.feedback_motion = raw_taa ? 0.0f : config_.taa_feedback_motion;
                taa_params.velocity_scale  = config_.taa_weight_scale;
                taa_params.sharpness       = config_.taa_sharpness;
                taa_params.variance_gamma  = config_.taa_variance_gamma;
                // TaaPass owns its brackets (resolve into its accumulation ping-pong, then
                // the passthrough into aa_target_) -- like smaa_pass_, no caller bracket.
                taa_pass_->draw(cmd, *aa_target_, taa_params,
                                render_extent_.width, render_extent_.height);
            }
            gpu_mark_(cmd, GpuScope::Aa);
        }

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
            gpu_mark_(cmd, GpuScope::TiltShift);
        }

    }

    /**
     * @brief Records the window-sized overlay: the letterboxed composite of scene + world UI,
     *        then the screen-space UI as a guest in the same bracket at full window resolution.
     */
    void record_overlay_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
        // --- Overlay: both UI layers, above every post effect ---
        //
        // overlay_target_ is the full swapchain extent, not the letterbox rect, so the bars are part
        // of it and a screen-space HUD can draw over them. The clear is what fills them --
        // ui_composite_pass_ only draws inside `letterbox`.
        //
        // The composite also performs the nearest-neighbour upscale: with tilt shift off it samples
        // post_target_/aa_target_ at destination-resolution UVs over this rect, and with it on, tilt
        // shift already produced an upscaled_extent_-sized image it samples 1:1. Either way the
        // world-UI layer rides the same UVs and the same NEAREST sampler.
        //
        // Screen UI draws second, as a guest in the same bracket, at FULL window resolution -- it is
        // an overlay, not part of the pixel-art image, so it is not quantised to the render grid. It
        // sets its own viewport and per-batch scissor.
        overlay_target_->begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 1.0f}});
        ui_composite_pass_->draw(cmd, ctx.letterbox);
        if (screen_ui_pass_) {
            for (coopa::ui::CanvasComponent* canvas : screen_canvases_) {
                // A canvas a host placed inside part of the window (Engine::set_scene_ui_placement())
                // draws at its own origin and size, clipped to them; every other canvas covers the
                // target exactly as before.
                const glm::uvec2 vs = canvas->viewport_size();
                const bool placed = vs.x > 0 && vs.y > 0;
                const float zoom = placed ? canvas->display_zoom() : 1.0f;
                screen_ui_pass_->draw(cmd, ctx.frame_slot,
                                      overlay_extent_.width, overlay_extent_.height,
                                      placed ? canvas->screen_origin() : glm::vec2(0.0f),
                                      placed ? static_cast<uint32_t>(std::lround(vs.x * zoom)) : overlay_extent_.width,
                                      placed ? static_cast<uint32_t>(std::lround(vs.y * zoom)) : overlay_extent_.height,
                                      canvas->scale_factor() * zoom, canvas->draw_list());
            }
        }
        overlay_target_->end(cmd);
        gpu_mark_(cmd, GpuScope::Overlay);
    }

    /// The per-renderer inputs a batch must agree on, precomputed once per frame so the
    /// per-view sorts compare plain data. Two renderers batch together in a view only when
    /// their key matches exactly (the pass then draws both with the first one's material).
    struct MeshBatchKey {
        size_t      shader_hash = 0;      ///< std::hash of material.shader -- the pipeline variant
        uint32_t    flags       = 0;      ///< bit 0 cull_backfaces, bits 1-2 alpha_mode
        const void* set         = nullptr; ///< MaterialTextureCache set: identifies the textures
        const void* mesh        = nullptr;
        uint32_t    part        = 0;       ///< material slot of the mesh drawn
        /// Every scalar the camera passes push (GBuffer / capture constants). Compared bytewise.
        struct Camera {
            glm::vec4 albedo_alpha;
            glm::vec4 mra_cutoff;   // metallic, roughness, ao, alpha cutoff
            glm::vec4 emissive;
            glm::vec4 params;       // shader_params
        } camera{};
        /// The subset the shadow passes push -- so casters that differ only in colour still
        /// share one instanced shadow draw.
        struct Shadow {
            glm::vec4 params;
            float     alpha_cutoff;
            float     pad[3];
        } shadow{};
    };

    /**
     * @brief Resolves every MeshRenderer's transform, bounds and LOD, then frustum-culls and
     *        batches it into each view's draw list, uploading all transforms in batch order.
     *
     * Must run after update_dir_shadow_matrix_()/update_spot_shadow_matrix_(): the cascade
     * and spot frusta come from the matrices those write.
     *
     * Per view: frustum test on the world AABB; for shadow views also the small-caster test
     * (config shadow_min_caster_texels); then a sort on MeshBatchKey so identical
     * mesh + LOD + material runs become one instanced draw, with their transforms appended
     * contiguously. A transform is therefore uploaded once per view that sees it.
     */
    MeshGather gather_meshes_(uint32_t frame_slot, const glm::mat4& view, const glm::mat4& unjittered_proj,
                              bool cast_dir_shadow,
                              coopa::gfx::engine::components::PointLightComponent* shadow_point,
                              bool cast_point_shadow, bool cast_spot_shadow) {
        using coopa::gfx::engine::components::MeshRenderer;
        MeshGather out;
        // One draw item per (renderer, material part).
        for (MeshRenderer* mr : frame_scene_.mesh_renderers) {
            mr->resolve_slot_names();   // name-keyed `materials:` once the mesh has loaded
            const uint32_t parts = mr->is_ready() ? mr->get_mesh()->part_count() : 1u;
            for (uint32_t p = 0; p < parts; ++p) {
                out.renderers.push_back(mr);
                out.part.push_back(p);
            }
        }
        // Mesh-mode particle systems: one item per (batch, part), drawn from the batch's own
        // instance matrices through the same batching as every other opaque mesh.
        const size_t first_particle_item = out.renderers.size();
        std::vector<const ParticleMeshBatch*> particle_items;
        for (const ParticleMeshBatch& pb : particle_state_.meshes) {
            MeshRenderer* mr = pb.proxy;
            if (!mr || pb.count == 0 || !pb.matrices || !mr->is_ready()) continue;
            mr->resolve_slot_names();
            for (uint32_t p = 0; p < mr->get_mesh()->part_count(); ++p) {
                out.renderers.push_back(mr);
                out.part.push_back(p);
                particle_items.push_back(&pb);
            }
        }
        const size_t n = out.renderers.size();
        out.multi.assign(n, nullptr);
        out.multi_count.assign(n, 0u);
        for (size_t k = 0; k < particle_items.size(); ++k) {
            out.multi[first_particle_item + k]       = particle_items[k]->matrices;
            out.multi_count[first_particle_item + k] = particle_items[k]->count;
        }
        out.world_matrices.assign(n, glm::mat4(1.0f));
        out.bounds.assign(n, WorldBounds{});
        out.valid.assign(n, 0);
        out.lod.assign(n, 0);
        out.instance_idx.assign(n, InstanceStream::kInvalidIndex);
        instance_stream_.begin(frame_slot);

        // world_matrix() is a pure read, safe for any number of concurrent readers (unlike
        // get_world_matrix()), because TransformSystem already resolved every dirty transform
        // earlier this frame. Each index writes only its own slot.
        auto gather_mesh = [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                MeshRenderer* mr = out.renderers[i];
                if (out.multi[i]) {
                    // A particle batch: world-space matrices already, bounds from the system.
                    const ParticleMeshBatch& pb = *particle_items[i - first_particle_item];
                    out.bounds[i].center = 0.5f * (pb.bounds_min + pb.bounds_max);
                    out.bounds[i].extent = 0.5f * (pb.bounds_max - pb.bounds_min);
                    out.valid[i] = 1;
                    continue;
                }
                if (!mr->is_ready() || !mr->owner) continue;
                auto* tc = mr->owner->get_transform();
                if (!tc) continue;
                out.world_matrices[i] = tc->transform().world_matrix();
                const auto& mesh = mr->get_mesh();
                out.bounds[i] = world_aabb(out.world_matrices[i], mesh->bounds_min(), mesh->bounds_max());
                out.valid[i] = 1;
            }
        };
        if (should_parallelize_(n)) {
            jobs_->parallel_for_blocking(n, 0 /* auto grain */, gather_mesh);
        } else {
            gather_mesh(0, n);
        }

        // --- LOD (camera), and keys. Serial: MaterialTextureCache::set_for() may allocate. ---
        std::vector<MeshBatchKey> keys(n);
        std::unordered_map<const MeshRenderer*, int> next_lod_state;
        next_lod_state.reserve(lod_state_.size());
        std::vector<LodThreshold> thresholds;
        std::hash<std::string> hash_str;
        for (size_t i = 0; i < n; ++i) {
            if (!out.valid[i]) continue;
            MeshRenderer* mr = out.renderers[i];
            const auto& mesh = *mr->get_mesh();

            const bool has_lods = mesh.lods().size() > 1 || mesh.cull_screen_size() > 0.0f;
            if (auto done = next_lod_state.find(mr); has_lods && mr->lods_enabled && done != next_lod_state.end()) {
                // Another part of the same renderer chose already: every part shares its LOD.
                if (done->second < 0) { out.valid[i] = 0; continue; }
                out.lod[i] = static_cast<uint32_t>(done->second);
            } else if (has_lods && mr->lods_enabled) {
                thresholds.clear();
                for (const auto& l : mesh.lods()) thresholds.push_back({l.screen_size});
                const float size = screen_height_fraction(out.bounds[i].center, out.bounds[i].radius(),
                                                          view, unjittered_proj)
                                 * config_.mesh_lod_bias * mr->lod_bias;
                auto it = lod_state_.find(mr);
                const int prev = (it != lod_state_.end()) ? it->second : -2;
                const int level = select_lod(size, thresholds, mesh.cull_screen_size(), prev, 0.1f);
                next_lod_state[mr] = level;
                if (level < 0) { out.valid[i] = 0; continue; }   // below cull size: no view draws it
                out.lod[i] = static_cast<uint32_t>(level);
            }

            const auto& m = out.material(i);
            MeshBatchKey& k = keys[i];
            k.shader_hash = hash_str(m.shader);
            k.flags       = (m.cull_backfaces ? 1u : 0u) | (static_cast<uint32_t>(m.alpha_mode) << 1);
            k.set         = &material_cache_->set_for(m);
            k.mesh        = &mesh;
            k.part        = out.part[i];
            k.camera.albedo_alpha = glm::vec4(m.albedo, m.alpha);
            k.camera.mra_cutoff   = glm::vec4(m.metallic, m.roughness, m.ao, m.gpu_alpha_cutoff());
            k.camera.emissive     = m.gpu_emissive();
            k.camera.params       = m.shader_params;
            k.shadow.params       = m.shader_params;
            k.shadow.alpha_cutoff = m.gpu_alpha_cutoff();
        }
        lod_state_.swap(next_lod_state);

        // --- Per-view culling + batching ---
        std::vector<uint32_t> visible;
        visible.reserve(n);
        auto build = [&](const glm::mat4& view_proj, bool shadow, float shadow_res,
                         const std::function<bool(size_t)>& accept, std::vector<MeshBatch>& list) {
            list.clear();
            visible.clear();
            const Frustum f = Frustum::from_matrix(view_proj);
            const float min_texels = shadow ? config_.shadow_min_caster_texels : 0.0f;
            for (size_t i = 0; i < n; ++i) {
                if (!out.valid[i] || !accept(i)) continue;
                if (!f.intersects(out.bounds[i])) continue;
                if (min_texels > 0.0f &&
                    projected_texels(view_proj, out.bounds[i].center, out.bounds[i].radius(), shadow_res) < min_texels) {
                    continue;
                }
                visible.push_back(static_cast<uint32_t>(i));
            }
            auto key_less = [&](uint32_t a, uint32_t b) {
                const MeshBatchKey& ka = keys[a];
                const MeshBatchKey& kb = keys[b];
                if (ka.shader_hash != kb.shader_hash) return ka.shader_hash < kb.shader_hash;
                if (ka.flags != kb.flags)             return ka.flags < kb.flags;
                if (ka.set != kb.set)                 return std::less<const void*>()(ka.set, kb.set);
                if (ka.mesh != kb.mesh)               return std::less<const void*>()(ka.mesh, kb.mesh);
                if (ka.part != kb.part)               return ka.part < kb.part;
                if (out.lod[a] != out.lod[b])         return out.lod[a] < out.lod[b];
                const int c = shadow ? std::memcmp(&ka.shadow, &kb.shadow, sizeof(ka.shadow))
                                     : std::memcmp(&ka.camera, &kb.camera, sizeof(ka.camera));
                if (c != 0) return c < 0;
                return a < b;   // stable within a batch: scene order
            };
            auto same_batch = [&](uint32_t a, uint32_t b) {
                const MeshBatchKey& ka = keys[a];
                const MeshBatchKey& kb = keys[b];
                return ka.shader_hash == kb.shader_hash && ka.flags == kb.flags && ka.set == kb.set &&
                       ka.mesh == kb.mesh && ka.part == kb.part && out.lod[a] == out.lod[b] &&
                       out.material(a).shader == out.material(b).shader &&
                       (shadow ? std::memcmp(&ka.shadow, &kb.shadow, sizeof(ka.shadow)) == 0
                               : std::memcmp(&ka.camera, &kb.camera, sizeof(ka.camera)) == 0);
            };
            std::sort(visible.begin(), visible.end(), key_less);
            for (size_t r = 0; r < visible.size();) {
                size_t e = r + 1;
                while (e < visible.size() && same_batch(visible[r], visible[e])) ++e;
                MeshBatch batch;
                batch.item           = visible[r];
                batch.lod            = out.lod[visible[r]];
                batch.first_instance = instance_stream_.size();
                uint32_t added = 0;
                for (size_t j = r; j < e; ++j) {
                    const uint32_t it = visible[j];
                    if (out.multi[it]) {
                        for (uint32_t m = 0; m < out.multi_count[it]; ++m) instance_stream_.add(out.multi[it][m]);
                        added += out.multi_count[it];
                    } else {
                        instance_stream_.add(out.world_matrices[it]);
                        ++added;
                    }
                }
                batch.instance_count = added;
                list.push_back(batch);
                r = e;
            }
        };

        // A BLEND material only casts a shadow at full opacity -- the shadow passes have no
        // per-fragment discard (single hard depth compare, no PCF to average a partial alpha
        // into a partial shadow -- see gfx/shadow_dither.glsl's doc), so a translucent object
        // is binary: caster or not.
        auto casts = [&](size_t i) {
            const auto& m = out.material(i);
            return !(m.is_blended() && m.alpha < 1.0f);
        };
        auto opaque = [&](size_t i) { return !out.material(i).is_blended(); };
        auto blended = [&](size_t i) { return out.material(i).is_blended(); };

        const glm::mat4 camera_vp = unjittered_proj * view;
        build(camera_vp, false, 0.0f, opaque, out.gbuffer);
        if (config_.ssr_reflect_transparent) build(camera_vp, false, 0.0f, blended, out.capture);

        // The forward BLEND pass draws one renderer at a time in back-to-front order, so its
        // transforms are added singly.
        {
            const Frustum f = Frustum::from_matrix(camera_vp);
            for (size_t i = 0; i < n; ++i) {
                if (out.valid[i] && blended(i) && f.intersects(out.bounds[i])) {
                    out.instance_idx[i] = instance_stream_.add(out.world_matrices[i]);
                    out.has_blend_mesh  = true;
                }
            }
        }

        if (cast_dir_shadow) {
            const uint32_t cascades = std::min<uint32_t>(shadow_target_.dir_cascade_count(), 4u);
            const float tile_res = static_cast<float>(shadow_target_.dir_tile_resolution());
            for (uint32_t c = 0; c < cascades; ++c) {
                build(current_light_data().dir_cascade_matrix[c], true, tile_res, casts, out.cascade[c]);
            }
        }
        if (cast_point_shadow && shadow_point) {
            const glm::vec3 light_pos = shadow_point->get_world_position();
            const float     range     = shadow_point->range;
            const float     res       = static_cast<float>(config_.cube_shadow_resolution);
            for (uint32_t face = 0; face < 6; ++face) {
                build(coopa::gfx::engine::targets::ShadowMapTarget::get_cube_face_matrix(face, light_pos, range),
                      true, res, casts, out.cube[face]);
            }
        }
        if (cast_spot_shadow) {
            build(current_light_data().spot_light_space_matrix, true,
                  static_cast<float>(config_.spot_shadow_resolution), casts, out.spot);
        }

        instance_stream_.upload();

        // Fold LAST frame's counts (complete now: its recording has finished) into the totals.
        if (frame_stats_.renderers > 0) {
            stats_total_.camera_draws     += frame_stats_.camera_draws;
            stats_total_.camera_instances += frame_stats_.camera_instances;
            stats_total_.camera_triangles += frame_stats_.camera_triangles;
            stats_total_.shadow_draws     += frame_stats_.shadow_draws;
            stats_total_.shadow_instances += frame_stats_.shadow_instances;
            stats_total_.shadow_triangles += frame_stats_.shadow_triangles;
            stats_total_.renderers        += frame_stats_.renderers;
            stats_total_.camera_visible   += frame_stats_.camera_visible;
            ++stats_frames_;
        }
        frame_stats_ = MeshDrawStats{};
        frame_stats_.renderers = n;
        for (const MeshBatch& b : out.gbuffer) frame_stats_.camera_visible += b.instance_count;
        return out;
    }

    /**
     * @brief Gathers this frame's UI canvases into world_canvases_/screen_canvases_ and
     *        resolves their textures into each pass's descriptor sets.
     *
     * **Must run before Renderer::begin_frame().** register_textures() reaches
     * DescriptorSet::bind_image(), which issues vkUpdateDescriptorSets immediately -- unsafe
     * once a render pass is open or while a previous frame may still be reading the set.
     *
     * @param view Camera view matrix, for the world canvases' depth sort.
     */
    void gather_ui_canvases_(coopa::scene::Scene& scene, uint32_t frame_slot, const glm::mat4& view) {
        // collect_canvases() returns EVERY canvas in the scene, so the world-space filter here is
        // load-bearing: without it a screen-space canvas's canvas-pixel geometry would be fed
        // through a 3D projection. It is called ONCE and the two gathers partition its result.
        //
        // Each canvas's DrawList was emitted by CanvasComponent::late_update() during
        // Scene::late_update(), which the caller runs before render().
        std::vector<coopa::ui::CanvasComponent*> all_canvases = coopa::ui::collect_canvases(scene);
        // The overlay scene's (the editor UI's) screen canvases come last, so they draw over the
        // game's own. Its world-space canvases, if any, are ignored: it has no camera of its own.
        if (overlay_scene_) {
            for (coopa::ui::CanvasComponent* c : coopa::ui::collect_canvases(*overlay_scene_)) {
                if (!c->is_world_space()) all_canvases.push_back(c);
            }
        }

        world_canvases_.clear();
        if (world_ui_pass_) {
            size_t ui_verts = 0;
            size_t ui_indices = 0;
            for (coopa::ui::CanvasComponent* canvas : all_canvases) {
                if (!canvas->is_world_space()) continue;
                world_canvases_.push_back(canvas);
                world_ui_pass_->register_textures(canvas->draw_list());
                ui_verts   += canvas->draw_list().vertices().size();
                ui_indices += canvas->draw_list().indices().size();
            }
            // Painter's order, farthest first. World UI composites with the depth test off, so two
            // canvases that overlap on screen resolve purely by draw order -- without this the one
            // later in the scene file wins regardless of which is actually in front. sort_order stays
            // the primary key, so an explicit authoring choice still wins; depth only breaks ties, which
            // is the common case of everything left at 0. stable_sort keeps scene order as the final
            // tiebreak. N is the handful of canvases a scene has, so this is free.
            std::stable_sort(world_canvases_.begin(), world_canvases_.end(),
                [&view](const coopa::ui::CanvasComponent* a, const coopa::ui::CanvasComponent* b) {
                    if (a->sort_order != b->sort_order) return a->sort_order < b->sort_order;
                    // view maps world -> camera, which looks down -Z, so a SMALLER (more
                    // negative) z is farther away and must be drawn first.
                    float za = (view * a->model()[3]).z;
                    float zb = (view * b->model()[3]).z;
                    return za < zb;
                });

            // Size the shared geometry buffers for the WHOLE frame up front and reset the
            // append cursor: every canvas streams into one buffer pair per frame slot, and
            // growing it mid-frame would reallocate out from under geometry already uploaded.
            world_ui_pass_->begin_frame(frame_slot, ui_verts, ui_indices);
            // Glyph atlases are R8 coverage, not RGBA. Without this every world-space Text
            // batch would bind the "quad" variant and draw coverage values as colour.
            if (!world_canvases_.empty()) {
                coopa::ui::UIResourceCache::instance().mark_text_atlases(*world_ui_pass_);
            }
        }

        // Screen-space UI: the same gather and the complementary half of the filter above. Two
        // things are deliberately not shared. There is no depth sort -- collect_canvases() already
        // ordered by sort_order, which for an overlay is all "in front" means, and a screen canvas
        // has no view-space position anyway. And mark_text_atlases() must be repeated, because the
        // marked-view set is a member of each pass; an unmarked glyph atlas draws through the
        // "quad" variant as raw coverage.
        screen_canvases_.clear();
        if (screen_ui_pass_) {
            size_t ui_verts = 0;
            size_t ui_indices = 0;
            for (coopa::ui::CanvasComponent* canvas : all_canvases) {
                if (canvas->is_world_space()) continue;
                screen_canvases_.push_back(canvas);
                screen_ui_pass_->register_textures(canvas->draw_list());
                ui_verts   += canvas->draw_list().vertices().size();
                ui_indices += canvas->draw_list().indices().size();
            }
            screen_ui_pass_->begin_frame(frame_slot, ui_verts, ui_indices);
            if (!screen_canvases_.empty()) {
                coopa::ui::UIResourceCache::instance().mark_text_atlases(*screen_ui_pass_);
            }
        }
    }

    /**
     * @brief Gathers every visible SdfRenderer into the draw list each recording site shares,
     *        uploading this frame's shape/renderer SSBO records and globals.
     *
     * The single gate for the whole SDF system: when sdf_enabled is off the list comes back
     * empty and every downstream site is naturally a no-op.
     */
    std::vector<SdfDrawItem> gather_sdf_(coopa::scene::Scene& scene, const glm::mat4& view,
                                         const glm::mat4& proj, const glm::vec3& cam_pos,
                                         uint32_t frame_slot) {
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
            const auto& sdf_renderer_comps = frame_scene_.sdf_renderers;

            // Per-SdfRenderer scratch result. Holds everything computable independently, but NOT the
            // two SSBO writes -- SdfData hands out contiguous indices, so those stay serial in the merge
            // below. This is the heaviest per-item work in the frame (an 8-corner AABB, a frustum cull,
            // then per shape a glm::inverse, three glm::length and a cbrt) and every renderer is
            // independent of every other, so it is the best parallel candidate of the three gathers.
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
                    // Off camera, but possibly casting a shadow INTO view: keep it with an empty
                    // pixel rect -- every camera pass skips those -- and let each shadow view
                    // frustum-test and scissor it itself (see draw_sdf_shadow_casters_()).
                    const bool casts = sr->cast_shadows && config_.sdf_shadows_enabled &&
                                       config_.shadows_enabled;
                    if (!clip_rect.visible) {
                        if (!casts) continue;
                        clip_rect = SdfClipRect{glm::vec2(0.0f), glm::vec2(0.0f), false};
                    }

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
                    out.px_rect      = clip_rect.visible
                                     ? sdf_clip_rect_to_pixels(clip_rect.ndc_min, clip_rect.ndc_max,
                                                               render_extent_.width, render_extent_.height)
                                     : PixelRect{};
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
        return sdf_draws;
    }

    /**
     * @brief Rebuilds everything sized to the window when the swapchain extent or the
     *        letterbox rect changes, and returns this frame's letterbox rect.
     *
     * **Must run first in render().** rebuild_overlay_chain_() replaces world_ui_pass_ and
     * screen_ui_pass_, dropping the registered textures, text-atlas marks and streaming-buffer
     * capacity that only the canvas gather installs. Rebuilding after that gather draws every
     * texture as the 1x1 white fallback, every glyph atlas through the RGBA "quad" variant, and
     * uploads geometry into a buffer never sized for it (Buffer::upload is an unchecked memcpy).
     *
     * Both triggers are needed and measure different things: the swapchain extent sizes
     * overlay_target_, while the letterbox rect sizes ui_world_target_ and tilt_shift_pass_.
     * Under upscale_mode == "integer" the rect snaps to render_extent_ * floor(scale), so the
     * window (and swapchain) can change size while the rect does not.
     *
     * @return This frame's letterbox rect, whether or not anything was rebuilt.
     */
    LetterboxRect handle_resize_() {
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
                    display_source_view_, nearest_sampler_,
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

    /**
     * @brief Adds this frame's TAA sub-pixel jitter to `proj`, when aa_mode == "taa".
     *
     * An 8-frame Halton(2,3) sequence added to whichever projection terms displace NDC by a
     * depth-independent constant -- proj[2][0]/[2][1] for perspective, proj[3][0]/[3][1] for
     * orthographic. That constant is also stored in taa_jitter_ndc_, so the TAA resolve can
     * subtract this frame's jitter out of its velocity. Callers keep an unjittered copy for
     * the consumers that must not see the jitter -- see render()'s own comment on that rule.
     *
     * The sequence length is deliberately SHORT, and lengthening it is a trap worth naming. A
     * periodic jitter makes the whole render periodic, and TaaPass's exponential history blend is
     * a low-pass filter: it attenuates a HIGH-frequency cycle more than a low-frequency one. A
     * 16-entry table therefore leaves MORE residual flicker than this 8-entry one, not less
     * (measured). The periodicity itself is what cannot be filtered away -- see config.yaml's
     * `aa_mode` for why this engine does not ship TAA as its default.
     */
    void apply_taa_jitter_(glm::mat4& proj) {
        if (config_.aa_mode == "taa") {
            static const float halton_offset[8][2] = {
                {1.0f / 2.0f, 1.0f / 3.0f}, {1.0f / 4.0f, 2.0f / 3.0f},
                {3.0f / 4.0f, 1.0f / 9.0f}, {1.0f / 8.0f, 4.0f / 9.0f},
                {5.0f / 8.0f, 7.0f / 9.0f}, {3.0f / 8.0f, 2.0f / 9.0f},
                {7.0f / 8.0f, 5.0f / 9.0f}, {1.0f / 16.0f, 8.0f / 9.0f},
            };
            const float jx = (2.0f * halton_offset[taa_jitter_index_][0] - 1.0f) / static_cast<float>(render_extent_.width);
            const float jy = (2.0f * halton_offset[taa_jitter_index_][1] - 1.0f) / static_cast<float>(render_extent_.height);
            if (proj[2][3] != 0.0f) {
                // Perspective (perspectiveRH_ZO, proj[2][3] == -1): a [2][x] term contributes
                // j * z_view to clip x/y while w_clip = -z_view, so the NDC displacement is
                // the depth-independent constant -j.
                proj[2][0] += jx;
                proj[2][1] += jy;
                taa_jitter_ndc_ = glm::vec2(-jx, -jy);
            } else {
                // Orthographic (w_clip == 1): a [2][x] term would scale with view depth, so
                // the constant NDC shift lives in the translation column instead, where the
                // displacement is +j.
                proj[3][0] += jx;
                proj[3][1] += jy;
                taa_jitter_ndc_ = glm::vec2(jx, jy);
            }
            taa_jitter_index_ = (taa_jitter_index_ + 1) & 0x7u;
        } else {
            taa_jitter_ndc_ = glm::vec2(0.0f);
        }
    }

    /**
     * @brief Fills the global fog UBO from config_. Only called when fog_enabled.
     *
     * Fog is GLOBAL and config-sourced only: there is no fog component and no local fog
     * volume, because anything bounded is a VolumeComponent on the volumetrics pass instead.
     *
     * Takes the UNJITTERED projection -- fog reprojects world-space samples, so TAA jitter
     * here would make it swim independently of the visible pixel grid.
     */
    /** @brief Fills UnderwaterPass's push constants from water_state_ (see set_water_state()). */
    void update_underwater_params_(const glm::mat4& unjittered_proj, const glm::mat4& view, const glm::vec3& cam_pos,
                                   const coopa::gfx::engine::components::DirectionalLightComponent* dir_light) {
        auto& p = underwater_params_;
        const WaterFrameState& w = water_state_;
        // Visibility is where the in-scatter reaches ~95%: exp(-3) -> density = 3 / visibility.
        const float density = 3.0f / std::max(w.visibility, 0.1f);
        float light = 1.0f;
        if (dir_light) light = glm::clamp(dir_light->intensity, 0.0f, 4.0f);
        p.inv_view_proj = glm::inverse(unjittered_proj * view);
        p.camera_time   = glm::vec4(cam_pos, elapsed_time_);
        p.water         = glm::vec4(w.surface_level, density, w.caustics, w.underwater ? 1.0f : 0.0f);
        p.fog_color     = glm::vec4(w.fog_color, 1.0f);
        p.absorption    = glm::vec4(w.absorption, light);
    }

    void update_fog_data_(const glm::mat4& unjittered_proj, const glm::mat4& view,
                          const glm::vec3& cam_pos,
                          const coopa::gfx::engine::components::DirectionalLightComponent* dir_light) {
        // Fog UBO. Gated on fog_enabled -- when fog is off, record() skips fog_pass_'s draw
        // entirely (see that call site), so this upload would otherwise be wasted work.
        //
        // Fog is GLOBAL ONLY and sourced entirely from config_: there is no scene component
        // and no local volume array. Local volumes of every kind -- including static fog
        // pockets -- are raymarched by volumetrics_pass_ instead (see VolumeComponent).

        auto& fog = fog_data_.data();
        // CAVEAT: fog_data_ is single-buffered, so this inv_view_proj carries the same hazard that
        // made camera_ubos_ per-slot (file doc, rule 2) -- a one-frame-stale camera matrix under
        // fast motion. Unexercised today (assets/config.yaml ships fog_enabled: false), and
        // documented rather than fixed because FogPass owns its own descriptor set bound once at
        // construction, so making it per-slot needs an additive gfxcoopa API change.
        fog.inv_view_proj = glm::inverse(unjittered_proj * view);
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
        // Same config_.indirect instance the lighting pass and SSR composite read --
        // see IndirectParams' doc (render_features.h).
        fog.sky_zenith  = glm::vec4(config_.indirect.sky_zenith, 0.0f);
        fog.sky_horizon = glm::vec4(config_.indirect.sky_horizon, 0.0f);
        fog.sky_ground  = glm::vec4(config_.indirect.sky_ground, 0.0f);

        fog.misc_params = glm::vec4(config_.fog_sun_anisotropy, config_.fog_max_opacity,
                                    0.0f, config_.fog_max_distance);

        fog_data_.upload();

    }

    /**
     * @brief Fills the volumetrics UBO from config_ and the scene's VolumeComponents. Only
     *        called when volumetrics_enabled.
     *
     * There is no global term here: fog is the global atmosphere, and everything in this
     * buffer is a bounded, scene-placed volume carrying its own complete field description.
     * Takes the UNJITTERED projection for the same reason fog does.
     */
    void update_volumetrics_data_(coopa::scene::Scene& scene, const glm::mat4& unjittered_proj,
                                  const glm::mat4& view, const glm::vec3& cam_pos,
                                  const coopa::gfx::engine::components::DirectionalLightComponent* dir_light) {
        // Volumetrics UBO. Gated on volumetrics_enabled -- when off, record() skips the
        // draw entirely, so this upload would otherwise be wasted work.
        //
        // There is NO global term here: fog above is the global atmosphere, and
        // everything in this buffer is a bounded, scene-placed VolumeComponent that
        // carries its own complete field description. Inherits fog_data_'s
        // single-buffered caveat (see VolumetricsData's doc).

        using coopa::gfx::engine::components::VolumeComponent;
        using coopa::gfx::engine::components::VolumeKind;
        using coopa::gfx::engine::components::VolumeShape;

        auto& vol = volumetrics_data_.data();

        // unjittered_proj, not proj -- same reason fog uses it: these fields are
        // sampled in WORLD space, so TAA jitter here would make them swim against
        // the pixel grid on top of their own intended motion.
        vol.inv_view_proj = glm::inverse(unjittered_proj * view);
        vol.camera_pos    = glm::vec4(cam_pos,
            parse_debug_view(config_.debug_view) == DebugView::Volumetrics ? 1.0f : 0.0f);
        if (dir_light) {
            vol.sun_direction = glm::vec4(glm::normalize(dir_light->direction), 0.0f);
            vol.sun_color     = glm::vec4(dir_light->color * dir_light->intensity, 1.0f);
        } else {
            vol.sun_direction = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
            vol.sun_color     = glm::vec4(0.0f);
        }
        vol.march_params = glm::vec4(static_cast<float>(glm::max(config_.volumetrics_step_count, 1)),
                                     config_.volumetrics_max_distance,
                                     config_.volumetrics_max_opacity,
                                     config_.volumetrics_sun_anisotropy);
        vol.time_params  = glm::vec4(elapsed_time_, frame_dt_,
                                     static_cast<float>(frame_index_), 0.0f);

        const auto& volumes = frame_scene_.volumes;
        uint32_t volume_count = static_cast<uint32_t>(
            std::min<size_t>(volumes.size(), coopa::gfx::engine::data::MAX_VOLUMES));
        vol.counts = glm::vec4(static_cast<float>(volume_count), 0.0f, 0.0f, 0.0f);

        for (uint32_t i = 0; i < volume_count; ++i) {
            auto* vc  = volumes[i];
            auto& gpu = vol.volumes[i];
            glm::mat4 world = (vc->owner && vc->owner->get_transform())
                ? vc->owner->get_transform()->get_world_matrix() : glm::mat4(1.0f);
            gpu.inv_world    = glm::inverse(world);
            gpu.extent_shape = glm::vec4(vc->extent,
                                         vc->shape == VolumeShape::Sphere ? 1.0f : 0.0f);

            // Normalized here, not in the shader: gfx_volume_field decomposes the
            // sample point against this vector, which is only a clean along/perp
            // split at unit length. A zero vector (an easy authoring typo) would
            // collapse that decomposition, so fall back to +X.
            const float dir_len = glm::length(vc->direction);
            const glm::vec3 dir = (dir_len > 1e-5f) ? vc->direction / dir_len
                                                    : glm::vec3(1.0f, 0.0f, 0.0f);
            gpu.direction_speed = glm::vec4(dir, vc->speed);
            gpu.field_params    = glm::vec4(vc->noise_scale, vc->streak,
                                            vc->coverage, vc->detail_gain);
            gpu.shape_params    = glm::vec4(vc->density, vc->height_base, vc->height_falloff,
                                            static_cast<float>(glm::clamp(vc->octaves, 1, 4)));
            gpu.flow_params     = glm::vec4(vc->flow_warp, vc->flow_scale,
                                            vc->sharpness, vc->gate_scale);
            gpu.color_occlusion = glm::vec4(vc->color, vc->occlusion);
            gpu.mode_params     = glm::vec4(static_cast<float>(static_cast<int>(vc->kind)),
                                            vc->sun_amount, vc->falloff, 0.0f);
        }

        // Shadowing + local-light in-scatter (light shafts). Sourced from this frame's
        // already-populated LightUBO slot rather than re-gathered from the scene, so the
        // march's lights and shadow matrices byte-match the surface lighting's -- render()
        // calls this after update_lights_()/update_dir_shadow_matrix_()/
        // update_spot_shadow_matrix_() have all written the current slot.
        const auto& lubo = current_light_data();
        vol.dir_light_space_matrix  = lubo.dir_light_space_matrix;
        vol.spot_light_space_matrix = lubo.spot_light_space_matrix;
        // The cascade set too, not just cascade 0's matrix: a sun shaft marches the whole
        // volumetrics_max_distance, so a march point past the near cascade must find its own
        // tile of the atlas or it would come back unshadowed. volumetrics_march.frag runs the same
        // containment select calc_dir_shadow() does -- see vol_shadow_vis_cascaded().
        for (uint32_t c = 0; c < coopa::gfx::engine::data::MAX_DIR_CASCADES; ++c) {
            vol.dir_cascade_matrix[c] = lubo.dir_cascade_matrix[c];
        }
        vol.dir_cascade_info = lubo.dir_cascade_info;
        const bool dir_shadowed = config_.volumetrics_shadows_enabled &&
                                  lubo.dir_shadow_params.z > 0.5f;
        vol.shadow_params = glm::vec4(dir_shadowed ? 1.0f : 0.0f,
                                      glm::clamp(lubo.dir_shadow_extra.x, 0.0f, 1.0f),
                                      config_.shadow_bias, config_.shadow_bias);

        // Nearest-to-camera selection: MAX_SCATTER_LIGHTS slots, each costing one
        // falloff evaluation per march step per pixel, go to the lights closest to
        // the camera -- the ones whose in-scatter is most likely to be visible.
        struct ScatterCandidate { float dist2; uint32_t index; bool spot; };
        ScatterCandidate cands[coopa::gfx::engine::data::MAX_POINT_LIGHTS +
                               coopa::gfx::engine::data::MAX_SPOT_LIGHTS];
        uint32_t cand_count = 0;
        const uint32_t n_points = std::min(lubo.light_counts.y,
                                           coopa::gfx::engine::data::MAX_POINT_LIGHTS);
        for (uint32_t i = 0; i < n_points; ++i) {
            const auto& pl = lubo.point_lights[i];
            if (pl.color_intensity.w <= 0.0f || pl.position_range.w <= 0.0f) continue;
            const glm::vec3 d = glm::vec3(pl.position_range) - cam_pos;
            cands[cand_count++] = {glm::dot(d, d), i, false};
        }
        const uint32_t n_spots = std::min(lubo.light_counts.z,
                                          coopa::gfx::engine::data::MAX_SPOT_LIGHTS);
        for (uint32_t i = 0; i < n_spots; ++i) {
            const auto& sl = lubo.spot_lights[i];
            if (sl.color_intensity.w <= 0.0f || sl.position_range.w <= 0.0f) continue;
            const glm::vec3 d = glm::vec3(sl.position_range) - cam_pos;
            cands[cand_count++] = {glm::dot(d, d), i, true};
        }

        // volumetrics_max_scatter_lights (the volumetrics_quality dial) caps this below the
        // buffer's own MAX_SCATTER_LIGHTS -- every light here is evaluated at every march
        // step of every pixel, so it multiplies against volumetrics_step_count.
        const uint32_t scatter_budget = std::min<uint32_t>(
            static_cast<uint32_t>(std::max(config_.volumetrics_max_scatter_lights, 0)),
            coopa::gfx::engine::data::MAX_SCATTER_LIGHTS);
        const uint32_t scatter_count = std::min(cand_count, scatter_budget);
        std::partial_sort(cands, cands + scatter_count, cands + cand_count,
                          [](const ScatterCandidate& a, const ScatterCandidate& b) {
                              return a.dist2 < b.dist2;
                          });
        const bool spot_shadowed = config_.volumetrics_shadows_enabled &&
                                   lubo.spot_shadow_params.z > 0.5f;
        for (uint32_t k = 0; k < scatter_count; ++k) {
            auto& dst = vol.scatter_lights[k];
            if (cands[k].spot) {
                const auto& sl = lubo.spot_lights[cands[k].index];
                dst.position_range  = sl.position_range;
                dst.color_intensity = sl.color_intensity;
                dst.direction_cone  = sl.direction_cone;
                dst.params = glm::vec4(sl.params.x, sl.params.y, 1.0f,
                                       (spot_shadowed && cands[k].index == lubo.light_counts.w)
                                           ? 1.0f : 0.0f);
            } else {
                const auto& pl = lubo.point_lights[cands[k].index];
                dst.position_range  = pl.position_range;
                dst.color_intensity = pl.color_intensity;
                dst.direction_cone  = glm::vec4(0.0f);
                dst.params = glm::vec4(pl.attenuation.x, 0.0f, 0.0f, 0.0f);
            }
        }
        vol.counts.y = static_cast<float>(scatter_count);
        vol.counts.z = glm::max(config_.volumetrics_light_scatter, 0.0f);
        // Merged path: the march applies the global fog term itself and fog_pass_ never
        // draws (see fog_merged_into_volumetrics_()). update_fog_data_() has already run
        // this frame -- render() calls it whenever fog_enabled, which the merge requires --
        // so the fog UBO this flag sends the shader to read is current.
        vol.counts.w = fog_merged_into_volumetrics_() ? 1.0f : 0.0f;

        // Froxel mode's grid description and temporal history inputs (ignored by the march).
        if (froxel_volumetrics_pass_) {
            vol.froxel_grid = glm::vec4(static_cast<float>(froxel_volumetrics_pass_->grid_width()),
                                        static_cast<float>(froxel_volumetrics_pass_->grid_height()),
                                        static_cast<float>(froxel_volumetrics_pass_->slices()),
                                        static_cast<float>(coopa::gfx::engine::passes::FroxelVolumetricsPass::kAtlasColumns));
            const float far = glm::max(config_.volumetrics_max_distance, 0.2f);
            // History is last frame's grid only if last frame ran this pass with the same grid:
            // record_grid() ping-pongs on frame parity, so a skipped frame (volumetrics toggled
            // off and back on) would otherwise read a grid two or more frames stale.
            const bool history_ok = froxel_history_valid_ && prev_view_proj_valid_ &&
                                    froxel_last_frame_ + 1 == frame_index_;
            vol.froxel_params   = glm::vec4(0.1f, far, glm::clamp(config_.volumetrics_froxel_history, 0.0f, 0.99f),
                                            history_ok ? 1.0f : 0.0f);
            vol.prev_view_proj  = prev_unjittered_view_proj_;
            vol.prev_camera_pos = glm::vec4(prev_vol_cam_pos_, 0.0f);
            vol.froxel_params2  = glm::vec4(
                static_cast<float>(glm::clamp(config_.volumetrics_froxel_miss_samples, 1u, 16u)),
                config_.aa_mode == "taa" ? glm::max(config_.volumetrics_froxel_lookup_jitter, 0.0f) : 0.0f,
                0.0f, 0.0f);
            prev_vol_cam_pos_     = cam_pos;
            froxel_history_valid_ = true;
            froxel_last_frame_    = frame_index_;
        }

        volumetrics_data_.upload();

    }

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

    /** @brief Light descriptor set for the slot render() most recently selected -- see
     *  light_datas_'s own doc for why this is per-slot. Mirrors current_camera_set(). */
    const coopa::gfx::pipeline::DescriptorSet& current_light_set() const {
        return *light_sets_[light_frame_];
    }

    /** @brief LightUBO for the slot render() most recently selected -- write light parameters
     *  through this, not any other slot's, so they land in the buffer current_light_set() binds.
     *  Mirrors current_camera_set()/current_light_set(); see light_datas_'s own doc. */
    coopa::gfx::engine::data::LightUBO& current_light_data() {
        return light_datas_[light_frame_]->data();
    }

    /// @brief resolve_dof_focus_()'s pair of outputs: where the focal plane sits and how
    /// wide a band around it is forced sharp. Both feed DofPass::Params of the same names.
    struct DofFocus {
        float distance = 0.0f; ///< Metres from the eye to the focal plane.
        float range    = 0.0f; ///< Half-width in metres of the forced-sharp band; 0 = pure thin lens.
        /// True when `distance` came from the scene -- a camera's focus_distance or
        /// focus_object, or the orbit target -- rather than config_.dof_focus_distance's manual
        /// fallback. The "focus" shadow fit trusts only these; otherwise it probes what the
        /// camera is looking at (see read_focus_probe_()).
        bool  from_scene = false;
    };

    /**
     * @brief Resolves this frame's DOF focal plane and forced-sharp band, before recording.
     *
     * Precedence (unchanged):
     *   1. CameraComponent::focus_distance (per-camera; > 0 overrides everything below)
     *   2. config_.dof_focus_distance (manual, global fallback)
     *   3a. CameraComponent::focus_object, if non-empty, self-activates object focus for THIS
     *       camera regardless of config_.dof_focus_mode
     *   3b. otherwise dof_focus_mode == "orbit_target" or "object" activates that global mode
     *
     * Object focus resolves `path` through Scene::find_object_by_path() and takes the view-space
     * DEPTH of the target -- exactly the quantity DofPass::dof_signed_coc() consumes, unlike
     * orbit_target's RADIAL distance to the pivot. A depth <= 0 (object behind the eye, or
     * unresolved) falls through to steps 1-2 rather than being trusted.
     *
     * The target point is the object's world BOUNDS CENTRE where bounds are available
     * (MeshRenderer via Mesh::bounds_min()/bounds_max(), SdfRenderer via its own
     * bounds_center/bounds_extent), not its Transform origin. A mesh origin is not generally
     * its centre -- pixel_demo's pillar spans [0,1] in plan, so its origin is a CORNER, 0.2 m in
     * front of the centre -- and focusing there spends half the depth of field on empty space
     * in front of the subject.
     *
     * The same bounds give `range` (see view_depth_half_extent()): with
     * config_.dof_focus_cover_object the band is fitted to the subject's own depth extent, so
     * the WHOLE subject stays sharp no matter how close the camera gets. That is not a tuning
     * nicety -- the physical sharp band goes as F^2 (see gfx/dof_common.glsl's dof_signed_coc()),
     * so an object framed sharp at 11 m is ~90% defocused at 3 m at any aperture. config_.
     * dof_focus_range is added on top in every mode, including manual focus.
     *
     * Both outputs are smoothed in object mode, at the same dof_focus_smoothing rate: the
     * depth extent of a box swings with orbit angle, and an unsmoothed range pops the sharp
     * region's edge around as the camera moves. orbit_target is left unsmoothed because it is
     * already smoothed twice over by CameraController's follow_smoothing/movement_smoothing.
     *
     * Called once per frame from render(), OUTSIDE the recording lambda -- smoothed_dof_focus_
     * and smoothed_dof_range_ are dt-driven mutable state, and advancing them during command
     * recording is a hazard the rest of this pipeline avoids.
     *
     * @param cam   Active camera, or nullptr (config-only values).
     * @param view  This frame's UNJITTERED view matrix.
     * @param scene Scene to resolve the focus object's path against.
     * @param dt    Frame delta, for the object-focus smoothing step.
     * @return Focus distance and forced-sharp half-width in metres. The distance is unclamped;
     *         the DOF block applies its own 0.01 floor.
     */
    DofFocus resolve_dof_focus_(const coopa::gfx::engine::components::CameraComponent* cam,
                                const glm::mat4& view, coopa::scene::Scene& scene, float dt) {
        DofFocus out;
        out.distance = (cam && cam->focus_distance > 0.0f)
            ? cam->focus_distance : config_.dof_focus_distance;
        out.from_scene = cam && cam->focus_distance > 0.0f;

        std::string_view path = (cam && !cam->focus_object.empty())
            ? std::string_view(cam->focus_object)
            : (config_.dof_focus_mode == "object" ? std::string_view(config_.dof_focus_object)
                                                   : std::string_view());

        bool object_mode = false;
        if (!path.empty()) {
            object_mode = true;
            if (auto* obj = scene.find_object_by_path(path)) {
                if (auto* tc = obj->get_transform()) {
                    const glm::mat4 model = tc->get_world_matrix();

                    // Local-space bounds of whatever renderable this object carries. Left
                    // unset for an object with neither (an empty used purely as a focus
                    // marker), which keeps today's transform-origin behaviour and a zero
                    // range rather than inventing an extent for a point.
                    glm::vec3 lo(0.0f), hi(0.0f);
                    bool has_bounds = false;
                    if (auto* mr = obj->get_component<coopa::gfx::engine::components::MeshRenderer>()) {
                        // is_ready() gates the dereference: an async mesh load may still be in
                        // flight on the first frames after a scene loads (see MeshRenderer's doc).
                        if (mr->is_ready()) {
                            lo = mr->get_mesh()->bounds_min();
                            hi = mr->get_mesh()->bounds_max();
                            has_bounds = true;
                        }
                    } else if (auto* sr = obj->get_component<coopa::gfx::engine::components::SdfRenderer>()) {
                        lo = sr->bounds_center - sr->bounds_extent;
                        hi = sr->bounds_center + sr->bounds_extent;
                        has_bounds = true;
                    }

                    const glm::vec3 target = has_bounds ? world_bounds_center(model, lo, hi)
                                                        : glm::vec3(model[3]);
                    float depth = view_space_depth(view, target);
                    if (depth > 0.0f) {
                        out.distance = depth;
                        out.from_scene = true;
                        if (has_bounds && config_.dof_focus_cover_object) {
                            out.range = view_depth_half_extent(view, model, lo, hi);
                        }
                    }
                }
            } else if (!warned_missing_dof_object_) {
                std::cerr << "[toyengine] PixelRenderPipeline: dof focus_object \"" << path
                          << "\" not found; falling back to dof_focus_distance.\n";
                warned_missing_dof_object_ = true;
            }
        } else if (config_.dof_focus_mode == "orbit_target" && cam && cam->owner) {
            // orbit_distance() returns 0 outside Orbit mode (see its own doc) -- that 0
            // would collapse the focal plane onto the camera itself, so it falls through
            // to the manual focus above rather than being trusted blindly.
            if (auto* controller = cam->owner->get_component<toy::scene::CameraController>()) {
                float orbit_dist = controller->orbit_distance();
                if (orbit_dist > 0.0f) {
                    out.distance = orbit_dist;
                    out.from_scene = true;
                }
            }
        }

        if (object_mode) {
            if (smoothed_dof_focus_ <= 0.0f) smoothed_dof_focus_ = out.distance; // seed, don't rack from 0
            if (smoothed_dof_range_ < 0.0f)  smoothed_dof_range_  = out.range;   // 0 is a legal range, so < 0 is the sentinel
            smoothed_dof_focus_ = exp_smooth_toward(smoothed_dof_focus_, out.distance,
                                                    config_.dof_focus_smoothing, dt);
            smoothed_dof_range_ = exp_smooth_toward(smoothed_dof_range_, out.range,
                                                    config_.dof_focus_smoothing, dt);
            out.distance = smoothed_dof_focus_;
            out.range    = smoothed_dof_range_;
        }

        // Added last, and outside the object-mode branch, so it is a flat widening of the
        // sharp band in EVERY mode -- including a manual-focus camera, which has no bounds to
        // fit to and for which this is the only way to widen the band at all.
        out.range += glm::max(config_.dof_focus_range, 0.0f);
        return out;
    }

    /**
     * @brief (Re)builds everything sized to the window: the world-UI layer and its pass, the
     *        overlay target, the composite and screen-UI passes, and upscale_pass_'s source.
     *
     * Called once at construction and again whenever the swapchain extent OR the letterbox rect
     * changes. They must move together: TexturedQuad2DPass (behind both UI passes) stores the
     * `RenderPass&` it was built against, and ui_composite_pass_/upscale_pass_ hold descriptors
     * pointing at the images being replaced -- none of which survive their target's destruction.
     *
     * **Reads upscaled_extent_**, so the caller must assign this frame's letterbox rect first,
     * and **must run before render()'s canvas gather** -- see handle_resize_() for why.
     *
     * The caller must have waited for the device to idle and must pass a nonzero extent.
     *
     * @param w Swapchain width in pixels.
     * @param h Swapchain height in pixels.
     */
    void rebuild_overlay_chain_(uint32_t w, uint32_t h) {
        rebuild_display_layer_();

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

    /**
     * @brief (Re)builds the display-rect-sized world-UI layer and its pass. Reads
     *        upscaled_extent_; the caller has waited for the device to idle.
     */
    void rebuild_display_layer_() {
        // --- The world-UI layer, at the DISPLAY rect ---
        // Reset the pass BEFORE replacing the target it was built against: TexturedQuad2DPass
        // stores the RenderPass& it was constructed from, and this closes the window in which
        // that reference dangles. (device_.wait_idle() is the caller's job and has already run.)
        world_ui_pass_.reset();
        ui_world_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
            device_, allocator_, upscaled_extent_.w, upscaled_extent_.h,
            coopa::gfx::Format::RGBA8_Unorm);
        ui_world_layer_clear_ = false;   // a fresh image is UNDEFINED until its first write
        rebuild_world_ui_pass_();
    }

    /** @brief Points ui_composite_pass_ at the current scene image and world-UI layer. */
    void bind_composite_inputs_() {
        // Startup-fixed source selection, the same caveat post_source_view/pre_fog_view_typed_
        // carry: flipping config_.tilt_shift_enabled without a pipeline rebuild would leave
        // this reading a target tilt_shift_pass_ never wrote this frame.
        ui_composite_pass_->set_base_image(
            config_.tilt_shift_enabled ? tilt_shift_pass_->result_view_typed()
                                       : display_source_view_,
            nearest_sampler_);
        // NEAREST is an EXACT texel fetch here, not a filter choice: the layer is built at
        // upscaled_extent_ and the composite draws over a viewport of exactly that size, so
        // fullscreen.vert's in_uv.x = (x + 0.5)/w gives floor(uv * w) == x. Bilinear would be
        // equally exact at 1:1 but would silently turn into a blur the moment the two sizes
        // drift by a pixel, which is exactly the failure this binding should not hide.
        ui_composite_pass_->set_ui_image(ui_world_target_->color_view_typed(), nearest_sampler_);
    }

    /**
     * @brief (Re)builds world_ui_pass_ against the current ui_world_target_.
     *
     * Split out of rebuild_overlay_chain_() for readability; it has no other caller and must not
     * gain one that skips the target rebuild. A no-op when world_ui_enabled is false, which is
     * also when world_ui_depth_layout_ was never created.
     *
     * The ExtraSets is rebuilt each time, which is safe because TexturedQuad2DPass stores its
     * descriptor by value, and because the set the callback binds is a stable member bound once
     * against the startup-fixed gbuffer_target_ -- it deliberately survives every rebuild.
     */
    void rebuild_world_ui_pass_() {
        if (!config_.world_ui_enabled) return;

        coopa::gfx::engine::passes::ExtraSets world_ui_extra;
        world_ui_extra.layouts = {world_ui_depth_layout_.get()};
        // Capturing `this` is safe only because PixelRenderPipeline is non-copyable and
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

    /**
     * @brief Populates this frame's slot's LightUBO colour/intensity/count fields.
     *
     * Not the shadow matrix or the cast_shadows flags -- update_dir_shadow_matrix_() and
     * render()'s point-light handling write those into the same slot afterwards, and all three
     * share the single upload() at the end of render(). Must run after light_frame_ is set to
     * this frame's slot.
     */
    void update_lights_(coopa::scene::Scene& scene) {
        using coopa::gfx::engine::components::DirectionalLightComponent;
        using coopa::gfx::engine::components::PointLightComponent;
        using coopa::gfx::engine::components::SpotLightComponent;

        auto& ubo = current_light_data();

        // Configurable sky/ambient colour (see IndirectParams, config_.indirect) -- not tied
        // to the directional light's presence, so set unconditionally every frame, same as
        // the lighting pass's own ambient_intensity/sky_intensity push-constant fields.
        ubo.sky_zenith  = glm::vec4(config_.indirect.sky_zenith, 0.0f);
        ubo.sky_horizon = glm::vec4(config_.indirect.sky_horizon, 0.0f);
        ubo.sky_ground  = glm::vec4(config_.indirect.sky_ground, 0.0f);

        auto* dir = frame_scene_.dir_light;
        ubo.light_counts.x = dir ? 1 : 0;
        if (dir) {
            ubo.dir_direction = glm::vec4(dir->direction, dir->intensity);
            ubo.dir_color     = glm::vec4(dir->color, 0.0f);
        }
        // Soft-shadow tuning shared by every calc_dir_shadow()/calc_point_shadow() call site.
        // Written UNCONDITIONALLY, not only when a directional light exists: .y is the POINT-light
        // PCF radius and .w the shared rotation offset, so a point-light-only scene still needs
        // them filled or its shadows silently fall back to the hard path. .x (directional shadow
        // intensity) is meaningless without a directional light and keeps its 1.0 default there.
        //
        // .y converts point_shadow_softness from cube-map TEXELS into a tangent-space offset on a
        // unit sample direction, the unit calc_point_shadow wants. Clamped to 8 texels so a
        // too-large config value degrades to "slightly over-soft" rather than washing every point
        // shadow out to uniform grey.
        float point_pcf_radius = config_.soft_shadows
            ? std::min(config_.point_shadow_softness, 8.0f) *
              (2.0f / static_cast<float>(std::max(config_.cube_shadow_resolution, 1u)))
            : 0.0f;
        ubo.dir_shadow_extra = glm::vec4(
            dir ? glm::clamp(dir->shadow_intensity, 0.0f, 1.0f) : 1.0f,
            point_pcf_radius,
            static_cast<float>(std::clamp<uint32_t>(config_.shadow_pcf_samples, 1u, 32u)),
            static_cast<float>(frame_index_ & 0xFFu));

        // Screen-space contact shadows (pixel_lighting.frag's directional block).
        // Written unconditionally, same policy as dir_shadow_extra above; .x = 0
        // disables the march outright.
        ubo.contact_params = glm::vec4(
            config_.contact_shadows_enabled
                ? glm::clamp(config_.contact_shadow_strength, 0.0f, 1.0f) : 0.0f,
            glm::max(config_.contact_shadow_length, 0.0f),
            glm::max(config_.contact_shadow_thickness, 0.01f),
            static_cast<float>(std::clamp(config_.contact_shadow_steps, 1, 24)));
        // Soft contact-shadow penumbra: .x is the sun's angular-size tangent, shared with
        // the shadow map's PCSS growth dial (shadow_pcss_light_size), gated by soft_shadows.
        // 0 selects the hard single-ray march. Written unconditionally, same policy as the
        // rows above.
        ubo.contact_soft_params = glm::vec4(
            config_.soft_shadows ? std::max(config_.shadow_pcss_light_size, 0.0f) : 0.0f,
            0.0f, 0.0f, 0.0f);

        const auto& points = frame_scene_.point_lights;
        uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(points.size()), coopa::gfx::engine::data::MAX_POINT_LIGHTS);
        ubo.light_counts.y = count;
        for (uint32_t i = 0; i < count; ++i) {
            auto* pl = points[i];
            auto& gpu = ubo.point_lights[i];
            gpu.position_range  = glm::vec4(pl->get_world_position(), pl->range);
            gpu.color_intensity = glm::vec4(pl->color, pl->intensity);
            gpu.attenuation     = glm::vec4(pl->attenuation_constant, pl->attenuation_linear,
                                            pl->attenuation_quadratic, 0.0f); // cast_shadows set below, light 0 only
        }

        // spot_shadow_params (including .y, the PCF penumbra scale) is written entirely by
        // render()'s update_spot_shadow_matrix_() afterward -- unlike dir_shadow_extra.y
        // above, the spot radius needs the shadow-casting spot's cone angle for its
        // world-to-texel conversion, and only that function has the caster in hand.

        const auto& spots = frame_scene_.spot_lights;
        uint32_t spot_count = std::min<uint32_t>(static_cast<uint32_t>(spots.size()), coopa::gfx::engine::data::MAX_SPOT_LIGHTS);
        ubo.light_counts.z = spot_count;
        for (uint32_t i = 0; i < spot_count; ++i) {
            auto* sl = spots[i];
            auto& gpu = ubo.spot_lights[i];
            gpu.position_range = glm::vec4(sl->get_world_position(), sl->range);
            gpu.direction_cone = glm::vec4(sl->get_world_direction(),
                                           std::cos(glm::radians(sl->clamped_outer_angle())));
            gpu.color_intensity = glm::vec4(sl->color, sl->intensity);
            gpu.params = glm::vec4(sl->attenuation_constant,
                                   std::cos(glm::radians(sl->clamped_inner_angle())),
                                   0.0f, 0.0f); // cast_shadows set below, shadow-owning slot only
        }
    }

    /**
     * @brief Writes this frame's directional cascade matrices and shadow parameters into the
     *        current slot's LightUBO.
     *
     * One ortho fit per cascade, each to its own slice of the camera's depth range
     * (pixel_math.h's compute_cascade_splits() + compute_dir_shadow_fit_slice()); this adds
     * the three things that need pipeline state: the world-to-texel conversions each cascade
     * needs for its PCF radius, normal bias and PCSS penumbra, the atlas-tile geometry the
     * shader selects against, and the write through current_light_data().
     *
     * Every conversion is PER CASCADE because every cascade has its own world-per-texel
     * scale, while shadow_softness/shadow_normal_bias are world-space constants -- a single
     * shared radius would make the near cascade's penumbra four times too wide in texels.
     *
     * Must run after render() has set light_frame_ to this frame's slot, same requirement as
     * update_lights_(); the single upload() happens in render() once this and the point-light
     * flag are both set.
     */
    void update_dir_shadow_matrix_(const glm::vec3& direction,
                                   const coopa::gfx::engine::components::CameraComponent* cam,
                                   bool cast_dir_shadow, float focus_distance) {
        using coopa::gfx::engine::components::CameraType;

        ShadowFitCamera fit_cam;
        if (cam) {
            // Read through CameraComponent rather than the owning object's Transform --
            // view = inverse(world), and this is the sealed surface the rest of the file
            // already goes through.
            fit_cam.view              = cam->get_view_matrix();
            fit_cam.is_perspective    = cam->type == CameraType::Perspective;
            fit_cam.fov_degrees       = cam->fov;
            fit_cam.orthographic_size = cam->orthographic_size;
            fit_cam.near_clip         = cam->clip_start;
            fit_cam.far_clip          = cam->clip_end;
            fit_cam.aspect            = static_cast<float>(render_extent_.width) /
                                        static_cast<float>(render_extent_.height);
        }
        const ShadowFitCamera* fit_cam_ptr = cam ? &fit_cam : nullptr;

        const uint32_t cascades  = shadow_target_.dir_cascade_count();
        const uint32_t tile_res  = shadow_target_.dir_tile_resolution();
        const float    near_clip = cam ? cam->clip_start : 0.1f;
        const CascadeSplits splits = compute_cascade_splits(
            near_clip, config_.shadow_distance, cascades, config_.shadow_cascade_split_lambda);

        // shadow_fit "focus": nested spheres around the point the camera is looking at,
        // instead of slices of its frustum (see PixelRenderConfig::shadow_fit). The point is
        // shadow_focus_distance along the view direction, else the DOF focal distance this
        // frame resolved, else the finest cascade's own radius as a last resort.
        const bool focus_fit = cam && config_.shadow_fit == "focus";
        glm::vec3 focus_point(0.0f);
        std::vector<float> focus_radii;
        if (focus_fit) {
            const glm::mat4 cam_to_world = glm::inverse(fit_cam.view);
            float d = config_.shadow_focus_distance > 0.0f ? config_.shadow_focus_distance : focus_distance;
            if (!(d > 0.0f)) d = config_.shadow_focus_radius;
            focus_point = glm::vec3(cam_to_world[3]) - glm::vec3(cam_to_world[2]) * d;
            focus_radii = compute_focus_cascade_radii(config_.shadow_focus_radius,
                                                      config_.shadow_distance, cascades);
        }
        const float pcf_max = std::max(config_.shadow_pcf_max_texels, 1.0f);

        const bool pcss_on_config = config_.shadow_pcss_enabled;
        const float pcss_search = glm::clamp(config_.shadow_pcss_search_texels, 1.0f, 16.0f);

        auto& ubo = current_light_data();

        bool any_soft = false;
        for (uint32_t c = 0; c < coopa::gfx::engine::data::MAX_DIR_CASCADES; ++c) {
            // Slots past the live count repeat the last cascade rather than holding an
            // identity matrix: the shader's containment loop stops at the count, but a stale
            // identity here would project every world position into the same degenerate tile
            // if that bound were ever read wrong.
            const uint32_t i = std::min(c, cascades - 1u);
            const float slice_near = (i == 0) ? near_clip : splits.distance[i - 1];
            const DirShadowFit fit = focus_fit
                ? compute_dir_shadow_fit_sphere(direction, focus_point, focus_radii[i],
                                                config_.shadow_distance, tile_res)
                : compute_dir_shadow_fit_slice(direction, fit_cam_ptr, slice_near,
                                               splits.distance[i], config_.shadow_distance, tile_res);

            // The PCF radius in ATLAS texels (a tile texel and an atlas texel are the same
            // physical texel, so no per-tile correction), converted from the world-space
            // config_.shadow_softness against THIS cascade's texel size so the penumbra stays
            // visually constant in world units. Clamped to shadow_pcf_max_texels (12 by
            // default): the radius is unbounded above (a fine cascade asks for tens) while the
            // Vogel disk is tuned for single-digit radii. 0 (soft_shadows off) selects the
            // single hard compare.
            const float pcf_texels = config_.soft_shadows
                ? std::min(config_.shadow_softness / std::max(fit.texel_world, 1e-6f), pcf_max)
                : 0.0f;

            ubo.dir_cascade_matrix[c]      = fit.light_space_matrix;
            ubo.dir_cascade_pcf_texels[c]  = pcf_texels;
            // The normal-offset bias, in world units, derived from THIS cascade's texel size --
            // the same conversion the PCF radius just did, and for the same reason: a
            // world-space constant means a different number of texels in every cascade. See
            // compute_shadow_normal_bias().
            // With the receiver-plane bias on, the taps already follow the receiver, so the
            // offset no longer has to clear the PCF disk -- only shadow_normal_bias texels.
            const bool plane_bias = config_.shadow_receiver_plane_bias && !pcss_on_config;
            ubo.dir_cascade_normal_bias[c] = compute_shadow_normal_bias(
                plane_bias ? 0.0f : pcf_texels, config_.shadow_normal_bias, fit.texel_world);
            // PCSS contact hardening (gfx_shadow_dir_pcss): the whole penumbra conversion
            // folded into one factor. A stored-vs-receiver gap of `g` in [0,1] light-space
            // depth spans g * depth_range_world metres, and a sun of angular size
            // shadow_pcss_light_size grows the penumbra by that many metres * size --
            // divided by texel_world to land in the texels gfx_shadow_dir_pcf_vogel wants.
            ubo.dir_cascade_pcss_scale[c]  = fit.depth_range_world *
                config_.shadow_pcss_light_size / std::max(fit.texel_world, 1e-6f);

            if (c < cascades && pcf_texels > 0.0f) any_soft = true;

            if (c == 0) {
                // Cascade 0 mirrored into the pre-cascade fields, which gfxcoopa's own
                // shaders (pbr.frag/deferred_lighting.frag/transparent.frag) still read
                // through the shorter LightUBO prefix -- see light_data.h's cascade doc.
                ubo.dir_light_space_matrix = fit.light_space_matrix;
                ubo.dir_shadow_params = glm::vec4(
                    config_.shadow_bias, pcf_texels, cast_dir_shadow ? 1.0f : 0.0f,
                    ubo.dir_cascade_normal_bias[0]);
                ubo.pcss_params = glm::vec4(
                    0.0f, ubo.dir_cascade_pcss_scale[0], pcss_search,
                    static_cast<float>(std::clamp<uint32_t>(config_.shadow_pcss_taps, 1u, 16u)));
            }
        }
        // PCSS requires the soft path (radius > 0) in at least one live cascade: the constant
        // radius above becomes PCSS's maximum, so shadow_softness keeps its role as the
        // artist's width dial.
        ubo.pcss_params.x = (pcss_on_config && any_soft) ? 1.0f : 0.0f;
        ubo.dir_shadow_receiver = glm::vec4(
            (config_.shadow_receiver_plane_bias && !pcss_on_config) ? 1.0f : 0.0f,
            std::max(config_.shadow_receiver_max_slope, 0.0f), 0.0f, 0.0f);

        // .z is the cascade-SELECTION inset, in tile uv: a shading point is only accepted
        // into a cascade whose tile it sits at least this far inside, which is exactly what
        // keeps the widest PCF disk (and the PCSS blocker search around it) from reaching
        // across a tile border into the neighbouring cascade's depths. That guarantee is why
        // gfx/shadow_sampling.glsl's kernels need no atlas awareness at all.
        // .w is the dithered transition band, the outer slice of that accepted region where
        // calc_dir_shadow() randomly promotes a pixel to the next cascade so TAA can resolve
        // the resolution step into a gradient instead of a seam.
        const float inset = (pcf_max + pcss_search) / static_cast<float>(std::max(tile_res, 1u));
        ubo.dir_cascade_info = glm::vec4(
            static_cast<float>(cascades),
            static_cast<float>(coopa::gfx::engine::targets::ShadowMapTarget::grid_for(cascades).first),
            inset, 0.06f);
    }

    /**
     * @brief True when the global fog term is applied by volumetrics_pass_'s march
     *        rather than by a separate fog_pass_ draw.
     *
     * Both effects are fullscreen passes over the whole HDR frame, and the second reads
     * exactly what the first wrote -- so with both enabled the pair costs two full-resolution
     * HDR passes to produce a result one pass can compute. The composite applies fog to its
     * scene-colour sample before laying the march over it, which is arithmetically identical
     * to the two-pass order, through the same gfx_fog_apply() call fog.frag makes (see its doc).
     *
     * Derived, not stored: both inputs are startup-fixed, so every caller re-deriving it
     * agrees by construction, and there is no second copy to keep in sync.
     */
    /**
     * @brief True when this frame's record_scene_() would write a descriptor set: a Hi-Z /
     *        mip-chain pass whose sets don't yet hold the view it is about to be given, or
     *        ssr_pass_'s secondary source on the first ssr_reflect_transparent frame. The
     *        views mirror the execute() calls in record_scene_() exactly; all are fixed for
     *        the pipeline's lifetime, so this is true on the first frame and then stays false.
     */
    bool trace_inputs_need_rebind_() const {
        const coopa::gfx::TextureView depth = gbuffer_target_.depth_view_typed();
        if (hiz_pass_ && hiz_pass_->needs_descriptor_update(depth)) return true;
        if (config_.ssao_enabled && ao_depth_pyramid_pass_ &&
            ao_depth_pyramid_pass_->needs_descriptor_update(depth)) return true;
        if (scene_color_mip_pass_ &&
            scene_color_mip_pass_->needs_descriptor_update(offscreen_target_.color_view_typed())) return true;
        if (secondary_this_frame_) {
            if (!ssr_secondary_bound_) return true;
            if (transparent_hiz_pass_ && transparent_hiz_pass_->needs_descriptor_update(
                    coopa::gfx::detail::wrap(transparent_capture_target_.depth_view()))) return true;
            if (transparent_scene_color_mip_pass_ && transparent_scene_color_mip_pass_->needs_descriptor_update(
                    coopa::gfx::detail::wrap(transparent_capture_target_.shaded_color_view()))) return true;
        }
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

    bool fog_merged_into_volumetrics_() const {
        return config_.fog_enabled && config_.volumetrics_enabled;
    }

    /**
     * @brief One axis of volumetrics_march_target_: render_extent_ divided by
     *        volumetrics_resolution_scale (clamped to 1..4), rounded up so the march
     *        covers every edge pixel.
     */
    /// volumetrics_mode == "froxel" (anything else is the raymarch).
    bool froxel_volumetrics_() const { return config_.volumetrics_mode == "froxel"; }

    uint32_t volumetrics_march_dim_(uint32_t full) const {
        if (froxel_volumetrics_()) return 1u;   // froxel mode never marches: keep the target 1x1
        const uint32_t s = std::clamp<uint32_t>(config_.volumetrics_resolution_scale, 1u, 4u);
        return std::max<uint32_t>(1u, (full + s - 1) / s);
    }

    /**
     * @brief Every component this frame's render() reads, gathered by ONE hierarchy walk.
     *
     * Scene::get_components<T>() / find_first_component<T>() each walk the whole tree with
     * a dynamic_cast per component, and render() used to make about a dozen of them per
     * frame (point and spot lights twice each). snapshot_scene_() makes one, with exactly
     * get_components' semantics -- pre-order, inactive objects skipped (their children are
     * still visited), the first component of each type per object -- so every consumer
     * sees the same lists, in the same order, that its own walk would have produced.
     */
    struct FrameScene {
        coopa::gfx::engine::components::DirectionalLightComponent*  dir_light = nullptr;
        std::vector<coopa::gfx::engine::components::PointLightComponent*> point_lights;
        std::vector<coopa::gfx::engine::components::SpotLightComponent*>  spot_lights;
        std::vector<coopa::gfx::engine::components::MeshRenderer*>        mesh_renderers;
        std::vector<coopa::gfx::engine::components::SdfRenderer*>         sdf_renderers;
        std::vector<coopa::gfx::engine::components::VolumeComponent*>     volumes;
    };

    /** @brief Rebuilds frame_scene_ from `scene`; see FrameScene. Vectors keep their capacity. */
    void snapshot_scene_(coopa::scene::Scene& scene) {
        using namespace coopa::gfx::engine::components;
        FrameScene& fs = frame_scene_;
        fs.dir_light = nullptr;
        fs.point_lights.clear();
        fs.spot_lights.clear();
        fs.mesh_renderers.clear();
        fs.sdf_renderers.clear();
        fs.volumes.clear();
        for (const auto& root : scene.root_objects()) {
            root->for_each_recursive([&fs](coopa::scene::SceneObject& obj) {
                if (!obj.active()) return;
                bool dir = false, point = false, spot = false, mesh = false, sdf = false, vol = false;
                for (const auto& comp : obj.components()) {
                    coopa::scene::Component* c = comp.get();
                    if (!dir)   if (auto* x = dynamic_cast<DirectionalLightComponent*>(c)) { dir = true; if (!fs.dir_light) fs.dir_light = x; }
                    if (!point) if (auto* x = dynamic_cast<PointLightComponent*>(c)) { point = true; fs.point_lights.push_back(x); }
                    if (!spot)  if (auto* x = dynamic_cast<SpotLightComponent*>(c))  { spot = true;  fs.spot_lights.push_back(x); }
                    if (!mesh)  if (auto* x = dynamic_cast<MeshRenderer*>(c))        { mesh = true;  fs.mesh_renderers.push_back(x); }
                    if (!sdf)   if (auto* x = dynamic_cast<SdfRenderer*>(c))         { sdf = true;   fs.sdf_renderers.push_back(x); }
                    if (!vol)   if (auto* x = dynamic_cast<VolumeComponent*>(c))     { vol = true;   fs.volumes.push_back(x); }
                }
            });
        }
    }

    FrameScene frame_scene_;

    FrameProfile*                profile_ = nullptr;   ///< Profiling mode's sink; null = off.
    std::unique_ptr<GpuProfiler> gpu_profiler_;        ///< Exists only in profiling mode.

    /// Closes a GPU timing scope (see GpuProfiler::mark). A no-op unless profiling. Every call
    /// site sits between render passes.
    void gpu_mark_(coopa::gfx::command::CommandBuffer& cmd, GpuScope scope) {
        if (gpu_profiler_) gpu_profiler_->mark(cmd, scope);
    }

    /// Each LOD-switching renderer's level last frame, for select_lod()'s hysteresis. Rebuilt
    /// every frame from the renderers still present, so destroyed ones drop out.
    std::unordered_map<const coopa::gfx::engine::components::MeshRenderer*, int> lod_state_;

    MeshDrawStats frame_stats_;     ///< This frame's mesh draw counts (see mesh_draw_stats_summary()).
    MeshDrawStats stats_total_;     ///< Summed over stats_frames_ frames.
    uint64_t      stats_frames_ = 0;

    /** @brief The first PointLightComponent with cast_shadows set, or nullptr. */
    coopa::gfx::engine::components::PointLightComponent* find_first_shadow_casting_point_light_(coopa::scene::Scene& scene) {
        for (auto* pl : frame_scene_.point_lights) {
            if (pl->cast_shadows) return pl;
        }
        return nullptr;
    }

    /** @brief A shadow-casting SpotLightComponent plus its index into update_lights_()'s
     *         spots array (get_components<SpotLightComponent>() order), or {nullptr, 0}. The
     *         index is what light_counts.w names so the shader knows which spot_lights[] slot
     *         owns spot_light_space_matrix -- unlike the point-light path, which hardcodes
     *         slot 0, a spot doesn't get that shortcut since find_first_shadow_casting_point_light_'s
     *         "first casting light" and "slot 0" already silently disagree whenever a
     *         non-shadow-casting point light is authored before the shadow-casting one; this
     *         spot path is deliberately exact instead of repeating that bug. */
    struct SpotShadowCaster {
        coopa::gfx::engine::components::SpotLightComponent* light = nullptr;
        uint32_t index = 0;
    };
    SpotShadowCaster find_first_shadow_casting_spot_light_(coopa::scene::Scene& scene) {
        const auto& spots = frame_scene_.spot_lights;
        uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(spots.size()),
                                            coopa::gfx::engine::data::MAX_SPOT_LIGHTS);
        for (uint32_t i = 0; i < count; ++i) {
            if (spots[i]->cast_shadows) return {spots[i], i};
        }
        return {};
    }

    /**
     * @brief Draws the SDF shadow casters one shadow view can see, each scissored to its
     *        bounds' footprint in that view.
     *
     * An SDF caster draws a viewport-filling quad and raymarches every fragment of it, so
     * without the scissor each caster would cost a full cascade tile / cube face of marching
     * whether it covered ten texels or ten thousand. The frustum test drops casters outside
     * the view entirely (gather_sdf_() keeps off-camera casters precisely so the shadow views
     * can decide this for themselves).
     *
     * Shadow maps render with a POSITIVE-height viewport (see ShadowPipeline's raster note),
     * so NDC +y is DOWN in the framebuffer -- the opposite of the camera passes
     * sdf_clip_rect_to_pixels() was written for; mirroring y on the way in accounts for it.
     *
     * @param view_proj   The view's light matrix.
     * @param origin_x/y  The view's viewport origin in the framebuffer (a cascade's atlas tile).
     * @param resolution  The view's square viewport size in texels.
     * @param push        Pushes the pass's constants for one renderer index.
     */
    template <typename PushFn>
    void draw_sdf_shadow_casters_(coopa::gfx::command::CommandBuffer& cmd,
                                  const std::vector<SdfDrawItem>& sdf_draws, const glm::mat4& view_proj,
                                  uint32_t origin_x, uint32_t origin_y, uint32_t resolution, PushFn&& push) {
        const Frustum f = Frustum::from_matrix(view_proj);
        bool scissored = false;
        for (const auto& d : sdf_draws) {
            if (!d.cast_shadows) continue;
            // Same binary BLEND-at-full-opacity rule as the mesh casters.
            if (d.is_blend && d.comp->material.alpha < 1.0f) continue;
            const WorldBounds b{0.5f * (d.world_min + d.world_max), 0.5f * (d.world_max - d.world_min)};
            if (!f.intersects(b)) continue;
            const SdfClipRect clip = compute_sdf_clip_rect(view_proj, d.world_min, d.world_max);
            if (!clip.visible) continue;
            const PixelRect r = sdf_clip_rect_to_pixels(glm::vec2(clip.ndc_min.x, -clip.ndc_max.y),
                                                        glm::vec2(clip.ndc_max.x, -clip.ndc_min.y),
                                                        resolution, resolution);
            if (r.w == 0 || r.h == 0) continue;
            cmd.set_scissor(static_cast<int32_t>(origin_x) + r.x, static_cast<int32_t>(origin_y) + r.y,
                            r.w, r.h);
            scissored = true;
            push(d.gpu_index);
            cmd.draw(6);
        }
        if (scissored) {
            cmd.set_scissor(static_cast<int32_t>(origin_x), static_cast<int32_t>(origin_y), resolution, resolution);
        }
    }

    /**
     * @brief Draws one shadow view's batched casters (see MeshGather). Shared by the
     *        directional (per cascade), point (per face) and spot passes, which differ only in
     *        their push-constant type and pipeline bind -- passed in as `bind` / `push`.
     *
     * Rebinds the pipeline only when the shader variant or face culling changes, the material
     * set only when it changes, and the mesh only when it changes; push constants go out per
     * batch, since every batch can differ in cutoff/params.
     */
    template <typename PushConstants, typename BindFn, typename PushFn>
    void draw_shadow_batches_(coopa::gfx::command::CommandBuffer& cmd, const MeshGather& meshes,
                              const std::vector<MeshBatch>& batches, PushConstants pc,
                              BindFn&& bind, PushFn&& push) {
        const std::string* last_shader = nullptr;
        bool last_cull = false;
        const void* last_set = nullptr;
        const coopa::gfx::engine::data::Mesh* last_mesh = nullptr;
        for (const MeshBatch& b : batches) {
            const auto* mr = meshes.renderers[b.item];
            const auto& m  = meshes.material(b.item);
            if (!last_shader || m.shader != *last_shader || m.cull_backfaces != last_cull) {
                bind(m.shader, m.cull_backfaces);
                last_shader = &m.shader;
                last_cull   = m.cull_backfaces;
                last_set    = nullptr;   // a variant owns its own pipeline layout
            }
            // CUTOUT (AlphaMode::Mask): the mask texture punches through the shadow too, via
            // the same set/cutoff shadow_depth.frag tests against.
            pc.alpha_cutoff = m.gpu_alpha_cutoff();
            pc.gfx_params   = m.shader_params;
            const auto& set = material_cache_->set_for(m);
            if (&set != last_set) {
                cmd.bind_descriptor_set(set, 0);
                last_set = &set;
            }
            push(pc);
            const auto* mesh = mr->get_mesh().get();
            if (mesh != last_mesh) {
                mesh->bind(cmd);
                last_mesh = mesh;
            }
            mesh->draw_lod_part(cmd, b.lod, meshes.part[b.item], b.instance_count, b.first_instance);
            frame_stats_.shadow_draws     += 1;
            frame_stats_.shadow_instances += b.instance_count;
            frame_stats_.shadow_triangles += static_cast<uint64_t>(b.instance_count) *
                                             mesh->lods()[std::min<size_t>(b.lod, mesh->lods().size() - 1)].index_count / 3;
        }
    }

    /**
     * @brief Writes this frame's spot shadow matrix and shadow parameters into the current
     *        slot's LightUBO.
     *
     * Mirrors update_dir_shadow_matrix_(), but the spot map needs no camera-fit: its
     * frustum is entirely a function of the light itself (position/direction/cone/range),
     * via ShadowMapTarget::get_spot_matrix(). Fills ALL of spot_shadow_params (x/y/z/w) --
     * the penumbra scale in .y needs the casting spot's cone angle, which only this
     * function has. Must run after render() has set light_frame_ to this frame's slot,
     * same ordering requirement update_dir_shadow_matrix_() has.
     */
    void update_spot_shadow_matrix_(const coopa::gfx::engine::components::SpotLightComponent* spot,
                                    bool cast_spot_shadow) {
        auto& ubo = current_light_data();
        // .y is the PCF penumbra as a distance-scaled texel factor: the spot map is a
        // PERSPECTIVE projection, so its texel world size grows linearly with distance d
        // from the light -- texel_world(d) = 2*d*tan(outer_half)/resolution -- and there is
        // no single per-frame texel count the way update_dir_shadow_matrix_()'s ortho fit
        // has. Instead the CPU stores K = softness_world * resolution / (2*tan(outer_half))
        // and calc_spot_shadow() divides by the fragment's own light-space depth
        // (light_space_pos.w, the forward distance d for perspectiveRH_ZO * lookAt), giving
        // radius_texels = K / d -- i.e. config_.spot_shadow_softness WORLD units of
        // penumbra at every receiver distance, matching shadow_softness's behavior on the
        // directional map. The 12-texel practical clamp is applied per-pixel in the shader.
        // 0 (no caster, or soft_shadows off) selects the single hard compare.
        if (spot) {
            ubo.spot_light_space_matrix = coopa::gfx::engine::targets::ShadowMapTarget::get_spot_matrix(
                spot->get_world_position(), spot->get_world_direction(),
                spot->clamped_outer_angle(), spot->range);
            ubo.spot_shadow_params.y = config_.soft_shadows
                ? config_.spot_shadow_softness *
                      static_cast<float>(config_.spot_shadow_resolution) /
                      (2.0f * std::tan(glm::radians(spot->clamped_outer_angle())))
                : 0.0f;
        } else {
            ubo.spot_shadow_params.y = 0.0f;
        }
        ubo.spot_shadow_params.x = config_.shadow_bias;
        ubo.spot_shadow_params.z = cast_spot_shadow ? 1.0f : 0.0f;
        // The normal-offset bias stays a world-space constant, unlike the directional .w: the
        // per-pixel texel size the penumbra conversion above derives in-shader is not available
        // HERE, and the callers apply this offset before they have a light-space position to
        // derive it from. A constant is honest, and no scene in this repo lights blocky terrain
        // with a shadow-casting spot.
        ubo.spot_shadow_params.w = 0.05f;
    }

    void record_directional_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                                    const MeshGather& meshes,
                                    const std::vector<SdfDrawItem>& sdf_draws,
                                    bool cast_dir_shadow) {
        shadow_target_.begin_directional_pass(cmd);
        if (cast_dir_shadow) {
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

            // One pass over the casters per cascade, each scissored to its own atlas tile and
            // pushed that cascade's matrix. The atlas is cleared once by
            // begin_directional_pass() and transitioned once after the loop, so a cascade
            // costs exactly its draws -- no extra render pass, barrier or pipeline variant.
            const uint32_t cascades = shadow_target_.dir_cascade_count();
            for (uint32_t c = 0; c < cascades; ++c) {
                shadow_target_.set_cascade_viewport(cmd, c);
                shadow_pipeline_->bind_directional(cmd);

                coopa::gfx::engine::passes::DirectionalShadowPushConstants pc{};
                pc.light_space_matrix = current_light_data().dir_cascade_matrix[c];
                pc.gfx_time = surface_gfx_time_();

                // Same per-material shader-variant binding as record_gbuffer_() -- a caster's
                // shadow must displace identically to its G-buffer draw (see
                // gfx/surface/shadow_vs.glsl's doc), so it binds the SAME named variant.
                draw_shadow_batches_(cmd, meshes, meshes.cascade[std::min(c, 3u)], pc,
                    [&](const std::string& shader, bool cull) { shadow_pipeline_->bind_directional(cmd, shader, cull); },
                    [&](const auto& p) { shadow_pipeline_->push_directional(cmd, p); });

                if (!sdf_draws.empty()) {
                    sdf_shadow_pass_->bind_directional(cmd);
                    cmd.bind_descriptor_set(sdf_data_.current_set(), 0);

                    coopa::gfx::engine::passes::SdfDirectionalShadowPushConstants sdf_pc{};
                    sdf_pc.light_space_matrix = current_light_data().dir_cascade_matrix[c];
                    sdf_pc.shadow_max_steps   = config_.sdf_shadow_max_steps;
                    const uint32_t tile = shadow_target_.dir_tile_resolution();
                    const uint32_t grid = std::max(1u, shadow_target_.dir_grid_columns());
                    draw_sdf_shadow_casters_(cmd, sdf_draws, sdf_pc.light_space_matrix,
                                             (c % grid) * tile, (c / grid) * tile, tile,
                                             [&](uint32_t gpu_index) {
                                                 sdf_pc.renderer_index = gpu_index;
                                                 sdf_shadow_pass_->push_directional(cmd, sdf_pc);
                                             });
                }
            }
        }
        shadow_target_.end_directional_pass(cmd);
        shadow_target_.transition_dir_to_shader_read(cmd);
    }

    void record_point_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                              const MeshGather& meshes,
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
            pc.gfx_time = surface_gfx_time_();

            draw_shadow_batches_(cmd, meshes, meshes.cube[face], pc,
                [&](const std::string& shader, bool cull) { shadow_pipeline_->bind_cube(cmd, shader, cull); },
                [&](const auto& p) { shadow_pipeline_->push_cube(cmd, p); });

            if (!sdf_draws.empty()) {
                sdf_shadow_pass_->bind_cube(cmd);
                cmd.bind_descriptor_set(sdf_data_.current_set(), 0);

                coopa::gfx::engine::passes::SdfCubeShadowPushConstants sdf_pc{};
                sdf_pc.light_space_matrix = pc.light_space_matrix;
                sdf_pc.light_pos_range    = pc.light_pos_range;
                sdf_pc.shadow_max_steps   = config_.sdf_shadow_max_steps;
                draw_sdf_shadow_casters_(cmd, sdf_draws, sdf_pc.light_space_matrix, 0, 0,
                                         config_.cube_shadow_resolution,
                                         [&](uint32_t gpu_index) {
                                             sdf_pc.renderer_index = gpu_index;
                                             sdf_shadow_pass_->push_cube(cmd, sdf_pc);
                                         });
            }
            shadow_target_.end_cube_face_pass(cmd);
        }
    }

    /**
     * @brief Records the spot shadow map: a single perspective depth pass, structurally a
     *        copy of record_directional_shadow_() (same push-constant type, same per-material
     *        shader-variant transition guard, same BLEND-at-full-opacity and CUTOUT rules,
     *        same SDF loop) since a spot map, like the directional map, is one frustum -- only
     *        light_space_matrix differs. Reuses shadow_pipeline_->bind_directional()/
     *        push_directional() rather than adding a third named bind/push pair to
     *        ShadowPipeline for that reason.
     *
     *        Always begins/ends the pass, even with no caster -- exactly like
     *        record_directional_shadow_() -- so the image is never left UNDEFINED on frame 0
     *        and transition_spot_to_shader_read()'s oldLayout stays honest.
     */
    void record_spot_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                             const MeshGather& meshes,
                             const std::vector<SdfDrawItem>& sdf_draws,
                             bool cast_spot_shadow) {
        shadow_target_.begin_spot_pass(cmd);
        if (cast_spot_shadow) {
            shadow_pipeline_->bind_directional(cmd);
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

            coopa::gfx::engine::passes::DirectionalShadowPushConstants pc{};
            pc.light_space_matrix = current_light_data().spot_light_space_matrix;
            pc.gfx_time = surface_gfx_time_();

            draw_shadow_batches_(cmd, meshes, meshes.spot, pc,
                [&](const std::string& shader, bool cull) { shadow_pipeline_->bind_directional(cmd, shader, cull); },
                [&](const auto& p) { shadow_pipeline_->push_directional(cmd, p); });

            if (!sdf_draws.empty()) {
                sdf_shadow_pass_->bind_directional(cmd);
                cmd.bind_descriptor_set(sdf_data_.current_set(), 0);

                coopa::gfx::engine::passes::SdfDirectionalShadowPushConstants sdf_pc{};
                sdf_pc.light_space_matrix = current_light_data().spot_light_space_matrix;
                sdf_pc.shadow_max_steps   = config_.sdf_shadow_max_steps;
                draw_sdf_shadow_casters_(cmd, sdf_draws, sdf_pc.light_space_matrix, 0, 0,
                                         config_.spot_shadow_resolution,
                                         [&](uint32_t gpu_index) {
                                             sdf_pc.renderer_index = gpu_index;
                                             sdf_shadow_pass_->push_directional(cmd, sdf_pc);
                                         });
            }
        }
        shadow_target_.end_spot_pass(cmd);
        shadow_target_.transition_spot_to_shader_read(cmd);
    }

    /// Transitions the G-buffer depth image to SHADER_READ_ONLY_OPTIMAL for
    /// pixel_stylize_pass_'s pre-bound outline-detection descriptor, which cannot simply be
    /// rebound (file doc, rule 1).
    ///
    /// `from_layout` is the image's actual current layout: DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    /// right after record_gbuffer_() wrote it, or DEPTH_STENCIL_READ_ONLY_OPTIMAL after
    /// record_transparent_()'s own render pass read it.
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

        // ALL_COMMANDS_BIT rather than a hand-picked fragment-test stage. This barrier sits
        // downstream of a multi-pass chain (G-buffer write -> HiZPass's own transition -> optionally
        // TransparentPass's render pass), and a narrower mask that looked individually correct still
        // produced a WRITE_AFTER_WRITE hazard under synchronization validation once transparency ran
        // -- TransparentPass declares no explicit VK_SUBPASS_EXTERNAL dependency for depth, so which
        // stage its depth-test reads land at is not something this call site can determine. One
        // barrier per frame on one small image, so the conservative cost is negligible.
        vkCmdPipelineBarrier(cmd.handle(),
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    /**
     * @brief Copies a few G2 (world position) texels around the screen centre into this frame
     *        slot's readback buffer -- what the camera is looking at, for the "focus" shadow fit
     *        when the scene names no focus. read_focus_probe_() reads it back once the slot's
     *        fence has passed, a frame or two later, so nothing ever waits on it.
     */
    void record_focus_probe_(coopa::gfx::command::CommandBuffer& cmd) {
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = gbuffer_target_.g2_image_handle();
        barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        // The render pass left G2 in SHADER_READ_ONLY_OPTIMAL (its finalLayout).
        barrier.oldLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy regions[kFocusProbeSamples]{};
        for (uint32_t i = 0; i < kFocusProbeSamples; ++i) {
            regions[i].bufferOffset     = static_cast<VkDeviceSize>(i) * 8u; // one RGBA16F texel
            regions[i].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            regions[i].imageOffset      = {
                static_cast<int32_t>(render_extent_.width / 2),
                static_cast<int32_t>(static_cast<float>(render_extent_.height - 1) * kFocusProbeRows[i]), 0};
            regions[i].imageExtent      = {1, 1, 1};
        }
        vkCmdCopyImageToBuffer(cmd.handle(), gbuffer_target_.g2_image_handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               focus_probes_[camera_frame_]->handle(), kFocusProbeSamples, regions);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Make the copy visible to the host read after this slot's fence.
        VkMemoryBarrier host{};
        host.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host, 0, nullptr, 0, nullptr);
        focus_probe_pending_[camera_frame_] = true;
    }

public:
    /**
     * @brief The "focus" shadow fit's measured distance to what the camera is looking at
     *        (smoothed view depth of the screen-centre G-buffer hit); 0 until a probe has
     *        landed, and while the scene states its own focus (the probe then never runs).
     */
    float shadow_focus_probe_distance() const { return probed_focus_distance_; }

private:
    /**
     * @brief Reads back this slot's focus probe, if it recorded one, and moves
     *        probed_focus_distance_ toward the first sample that hit geometry.
     *
     * A sample hit geometry when its G2 alpha (roughness, floored at 0.045 by the G-buffer
     * backbone) is non-zero; sky leaves the cleared 0. The distance is the hit's view-space
     * depth from THIS frame's camera, smoothed at dof_focus_smoothing so the sharp shadow
     * region glides rather than jumps as the centre ray crosses a cliff edge. No hit leaves
     * the previous value in place.
     */
    void read_focus_probe_(uint32_t slot, const glm::mat4& view, float dt) {
        if (slot >= focus_probes_.size() || !focus_probe_pending_[slot]) return;
        focus_probe_pending_[slot] = false;
        uint16_t texels[kFocusProbeSamples * 4] = {};
        focus_probes_[slot]->download(texels, sizeof(texels));
        for (uint32_t i = 0; i < kFocusProbeSamples; ++i) {
            const float roughness = glm::unpackHalf1x16(texels[i * 4 + 3]);
            if (!(roughness > 0.01f)) continue;
            const glm::vec3 hit(glm::unpackHalf1x16(texels[i * 4 + 0]),
                                glm::unpackHalf1x16(texels[i * 4 + 1]),
                                glm::unpackHalf1x16(texels[i * 4 + 2]));
            const float depth = view_space_depth(view, hit);
            if (!(depth > 0.0f)) continue;
            probed_focus_distance_ = probed_focus_distance_ > 0.0f
                ? exp_smooth_toward(probed_focus_distance_, depth, config_.dof_focus_smoothing, dt)
                : depth;
            return;
        }
    }

    void record_gbuffer_(coopa::gfx::command::CommandBuffer& cmd,
                         const MeshGather& meshes,
                         const std::vector<SdfDrawItem>& sdf_draws) {
        gbuffer_target_.begin(cmd);
        // Stock pipeline bound first so a scene with no derived shaders (the overwhelming
        // common case) pays for exactly one bind, as before this pass gained variants.
        gbuffer_pipeline_->bind(cmd);
        cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), current_camera_set(), 0);
        cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

        // Tracks which named variant ("" for stock) is bound, so a run of same-shader batches only
        // rebinds at a transition. gather_meshes_() sorts each view by shader first, so every
        // variant is bound at most once per pass.
        //
        // last_cull_backfaces must be tracked alongside last_shader: the stock ("") key resolves to
        // one of TWO pipelines (see PBRMaterial::cull_backfaces), so the shader name alone does
        // not say which is bound.
        const DebugView gview = parse_debug_view(config_.debug_view);
        const bool untextured_view = gview == DebugView::Solid || gview == DebugView::Wireframe;
        std::string last_shader;
        bool last_cull_backfaces = true; // matches the initial pipeline_ bind above (Back)
        bool have_bound = true; // stock, bound just above

        const void* last_set = nullptr;
        const coopa::gfx::engine::data::Mesh* last_mesh = nullptr;
        // Already frustum-culled and batched (see gather_meshes_): one instanced draw per run
        // of identical mesh + LOD + material. BLEND materials never appear here -- they are
        // drawn by transparent_pass_ instead, after SSR compositing (see record_transparent_).
        for (const MeshBatch& batch : meshes.gbuffer) {
            auto* mr = meshes.renderers[batch.item];
            const auto& mr_mat = meshes.material(batch.item);

            if (!have_bound || mr_mat.shader != last_shader ||
                mr_mat.cull_backfaces != last_cull_backfaces) {
                gbuffer_pipeline_->bind(cmd, mr_mat.shader, mr_mat.cull_backfaces);
                last_shader         = mr_mat.shader;
                last_cull_backfaces = mr_mat.cull_backfaces;
                have_bound          = true;
                last_set            = nullptr;
            }

            coopa::gfx::engine::passes::GBufferPipeline::PushConstants pc;
            pc.albedo       = glm::vec4(mr_mat.albedo, mr_mat.alpha);
            pc.metallic     = mr_mat.metallic;
            pc.roughness    = mr_mat.roughness;
            pc.ao           = mr_mat.ao;
            pc.alpha_cutoff = mr_mat.gpu_alpha_cutoff();
            pc.emissive     = mr_mat.gpu_emissive();
            pc.gfx_time     = surface_gfx_time_();
            pc.gfx_params   = mr_mat.shader_params;
            gbuffer_pipeline_->push(cmd, pc);
            // Set 1: alpha-mask sampler (white 1x1 fallback unless this is a CUTOUT material
            // with a loaded texture_alpha_mask) -- see MaterialTextureCache.
            // The editor's Solid / Wireframe shading shows material colours, not textures.
            const auto& set = untextured_view ? material_cache_->untextured_set() : material_cache_->set_for(mr_mat);
            if (&set != last_set) {
                cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), set, 1);
                last_set = &set;
            }

            const auto* mesh = mr->get_mesh().get();
            if (mesh != last_mesh) {
                mesh->bind(cmd);
                last_mesh = mesh;
            }
            mesh->draw_lod_part(cmd, batch.lod, meshes.part[batch.item], batch.instance_count, batch.first_instance);
            frame_stats_.camera_draws     += 1;
            frame_stats_.camera_instances += batch.instance_count;
            frame_stats_.camera_triangles += static_cast<uint64_t>(batch.instance_count) *
                mesh->lods()[std::min<size_t>(batch.lod, mesh->lods().size() - 1)].index_count / 3;
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

    /// Draws every BLEND-material renderer's depth/normal/position/shaded colour into
    /// transparent_capture_target_, feeding ssr_pass_'s secondary trace source -- NOT a visible
    /// draw. No back-to-front sort is needed, unlike record_transparent_(): this is a normal
    /// depth-tested pass, so the front-most transparent surface wins per pixel regardless of
    /// order.
    ///
    /// Always begins and ends the render pass, even with nothing to draw: every attachment
    /// declares initialLayout = UNDEFINED, so an empty frame transitions as safely as a populated
    /// one, and transparent_hiz_pass_->execute() right afterwards unconditionally expects depth
    /// in DEPTH_STENCIL_ATTACHMENT_OPTIMAL -- only this render pass's finalLayout guarantees that
    /// every frame.
    void record_transparent_capture_(coopa::gfx::command::CommandBuffer& cmd,
                                     const MeshGather& meshes,
                                     const std::vector<SdfDrawItem>& sdf_draws) {
        transparent_capture_target_.begin(cmd);
        transparent_capture_pass_->bind(cmd);
        cmd.bind_descriptor_set(transparent_capture_pass_->layout(), current_camera_set(), 0);
        cmd.bind_descriptor_set(transparent_capture_pass_->layout(), current_light_set(),  1);
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

        for (const MeshBatch& batch : meshes.capture) {
            auto* mr = meshes.renderers[batch.item];
            const auto& mr_mat = meshes.material(batch.item);
            if (!have_bound || mr_mat.shader != last_shader) {
                transparent_capture_pass_->bind(cmd, mr_mat.shader);
                last_shader = mr_mat.shader;
                have_bound  = true;
            }

            coopa::gfx::engine::passes::TransparentCapturePass::PushConstants pc;
            pc.albedo     = glm::vec4(mr_mat.albedo, mr_mat.alpha);
            pc.metallic   = mr_mat.metallic;
            pc.roughness  = mr_mat.roughness;
            pc.ao         = mr_mat.ao;
            pc.gfx_time   = surface_gfx_time_();
            pc.gfx_params = mr_mat.shader_params;
            transparent_capture_pass_->push(cmd, pc);
            // Set 3: albedo/normal/metallic-roughness -- see gfx/surface/capture_fs.glsl and
            // engine::util::MaterialTextureCache.
            transparent_capture_pass_->bind_material(cmd, material_cache_->set_for(mr_mat));

            const auto* mesh = mr->get_mesh().get();
            mesh->bind(cmd);
            mesh->draw_lod_part(cmd, batch.lod, meshes.part[batch.item], batch.instance_count, batch.first_instance);
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
                cmd.bind_descriptor_set(current_light_set(),  1);
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

    /**
     * @brief The editor shading modes' BLEND meshes, back-to-front, over the debug-view image
     *        (inside post_target_'s open bracket). BLEND SDFs are not drawn here.
     */
    void record_transparent_preview_(coopa::gfx::command::CommandBuffer& cmd, const MeshGather& meshes,
                                     DebugView view, const glm::vec3& camera_pos) {
        std::vector<std::pair<float, size_t>> order;
        for (size_t i = 0; i < meshes.renderers.size(); ++i) {
            if (meshes.instance_idx[i] == InstanceStream::kInvalidIndex) continue;
            if (!meshes.material(i).is_blended()) continue;
            const glm::vec3 d = meshes.bounds[i].center - camera_pos;
            order.push_back({glm::dot(d, d), i});
        }
        if (order.empty()) return;
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        transparent_preview_pass_->begin(cmd, current_camera_set(), LetterboxRect{0, 0, render_extent_.width, render_extent_.height});
        cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);
        const float mode = view == DebugView::MaterialPreview ? 2.0f : view == DebugView::Wireframe ? 3.0f : 1.0f;
        for (const auto& [dist, i] : order) {
            auto* mr = meshes.renderers[i];
            const auto& mr_mat = meshes.material(i);
            passes::TransparentPreviewPass::PushConstants pc;
            pc.albedo    = glm::vec4(mr_mat.albedo, mr_mat.alpha);
            pc.metallic  = mr_mat.metallic;
            pc.roughness = mr_mat.roughness;
            if (mr_mat.shader == "water") {
                // Real water is a near-mirror at low opacity: under the preview modes' studio
                // lighting it reads as a white sheet. Give it enough body and roughness that its
                // own colour shows -- these modes are for seeing what is where, not for the look.
                pc.albedo.a  = std::max(pc.albedo.a, 0.75f);
                pc.roughness = std::max(pc.roughness, 0.45f);
            }
            pc.ao        = mr_mat.ao;
            pc.view      = glm::vec4(mode, 0.0f, 0.0f, 0.0f);
            pc.emissive  = mr_mat.gpu_emissive();
            transparent_preview_pass_->push(cmd, pc, view == DebugView::MaterialPreview ? material_cache_->set_for(mr_mat)
                                                                                        : material_cache_->untextured_set());
            mr->get_mesh()->bind(cmd);
            mr->get_mesh()->draw_lod_part(cmd, meshes.lod[i], meshes.part[i], 1, meshes.instance_idx[i]);
        }
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

        // Water ripples: the newest kMaxWaterRipples rings.
        const auto& rs = water_state_.ripples;
        const std::size_t first = rs.size() > static_cast<std::size_t>(kMaxWaterRipples)
                                      ? rs.size() - kMaxWaterRipples : 0;
        int n = 0;
        for (std::size_t i = first; i < rs.size(); ++i, ++n) {
            g.water_ripples[2 * n]     = glm::vec4(rs[i].position, rs[i].age, rs[i].strength);
            g.water_ripples[2 * n + 1] = glm::vec4(rs[i].radius, 0.0f, 0.0f, 0.0f);
        }
        g.water_ripple_info = glm::vec4(static_cast<float>(n), static_cast<float>(water_state_.ripple_layers),
                                        water_state_.detail_distance, water_state_.ripple_range);
    }

    /// One particle batch's push block: its look, plus the frame's clock, depth range and ambient.
    passes::ParticlePass::PushConstants particle_push_constants_(const ParticleDrawBatch& b) const {
        const ParticleLook& l = b.look;
        passes::ParticlePass::PushConstants pc;
        pc.mode    = glm::vec4(static_cast<float>(l.mode), static_cast<float>(l.sprite), l.flipbook.x, l.flipbook.y);
        pc.shading = glm::vec4(l.lit, l.toon_bands, l.emissive, glm::clamp(l.additive, 0.0f, 1.0f));
        pc.shape   = glm::vec4(l.softness, l.soft_distance, l.camera_fade, l.aspect);
        pc.stretch = glm::vec4(l.stretch_speed, l.stretch_length, l.distortion, elapsed_time_);
        pc.misc    = glm::vec4(l.opacity, b.texture_material ? 1.0f : 0.0f, l.pivot_z, particle_depth_.z);
        pc.depth   = glm::vec4(particle_depth_.x, particle_depth_.y,
                               1.0f / static_cast<float>(std::max(1u, render_extent_.width)),
                               1.0f / static_cast<float>(std::max(1u, render_extent_.height)));
        pc.ambient = glm::vec4(config_.indirect.ambient_intensity, 0.0f, 0.0f, 0.0f);
        return pc;
    }

    /// Draws every BLEND-material renderer AND every BLEND SdfRenderer, back-to-front by squared
    /// distance from the camera, into hdr_target_view.
    ///
    /// Must run after SSR compositing: ssr_composite.frag derives reflections from the G-buffer,
    /// which describes opaque geometry only, so compositing it on top of already-blended glass
    /// would add the OCCLUDED surface's reflection to the glass.
    ///
    /// Meshes and SDFs are merged into ONE sorted list, switching between transparent_pass_'s and
    /// sdf_forward_pass_'s pipelines as the item kind changes. Both draw into the SAME render pass
    /// instance, so a glass mesh and a glass SDF blob composite in correct depth order instead of
    /// one kind always landing on top of the other.
    ///
    /// @return true if the pass ran (at least one BLEND item) and therefore left the G-buffer
    ///         depth image in DEPTH_STENCIL_READ_ONLY_OPTIMAL; false if it early-returned with
    ///         depth untouched -- see the call site.
    bool record_transparent_(coopa::gfx::command::CommandBuffer& cmd,
                             const MeshGather& meshes,
                             const std::vector<SdfDrawItem>& sdf_draws,
                             VkImageView hdr_target_view,
                             VkImageLayout depth_layout,
                             const glm::vec3& camera_pos) {
        struct Item {
            bool      is_sdf;
            size_t    index; // into `renderers`/`world_matrices` if !is_sdf, else into `sdf_draws`
                             // (or particle_state_.quads, for a particle batch)
            glm::vec3 pos;
            bool      is_particle = false;
        };
        std::vector<Item> order;
        for (size_t i = 0; i < meshes.renderers.size(); ++i) {
            if (meshes.instance_idx[i] == InstanceStream::kInvalidIndex) continue;
            if (!meshes.material(i).is_blended()) continue;
            // Sorted on the world-bounds centre, not the object origin: a mesh whose pivot sits
            // at one end would otherwise sort by a point it barely occupies.
            order.push_back({false, i, meshes.bounds[i].center});
        }
        for (size_t i = 0; i < sdf_draws.size(); ++i) {
            if (!sdf_draws[i].is_blend) continue;
            if (sdf_draws[i].px_rect.w == 0 || sdf_draws[i].px_rect.h == 0) continue;
            order.push_back({true, i, sdf_draws[i].world_center});
        }
        // Particle batches sort as a whole by their bounds centre (each is already sorted
        // internally, back to front, by toy::particles).
        if (particle_pass_ && particle_pass_->total_instances() > 0) {
            for (size_t i = 0; i < particle_state_.quads.size(); ++i) {
                if (particle_state_.quads[i].count == 0) continue;
                order.push_back({false, i, particle_state_.quads[i].sort_center, true});
            }
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

        // Frame-level lighting/indirect/SSR/refraction tuning lives in forward_globals_'s UBO
        // (set 6, bound below via bind_extra()), filled and uploaded once per frame in
        // render(). As a descriptor set rather than push-constant contents, it survives a bind
        // to a pipeline with an incompatible layout, so it needs no re-issue at every
        // mesh-kind transition.

        // -1 = nothing yet bound, 0 = mesh, 1 = sdf, 2 = particles -- tracks which pipeline+sets are current so
        // a run of same-kind items in `order` only rebinds once, at the transition.
        int last_kind = -1;
        // Independent of last_kind: which named variant transparent_pass_ currently has bound. A
        // mesh-kind run is not sorted by shader (back-to-front depth is the only order that matters
        // here), so two consecutive mesh items can need different pipelines even though last_kind
        // does not change. Switching pipelines mid-run is safe without rebinding sets 0-2 or the
        // extras, since every variant shares the same descriptor set layouts.
        std::string last_mesh_shader;

        for (const auto& item : order) {
            if (item.is_particle) {
                // -- a particle batch: ParticlePass's pipeline, sets 0-2 once per run of batches,
                // the material set (sprite texture) per batch.
                if (last_kind != 2) {
                    particle_pass_->bind(cmd);
                    cmd.set_scissor(0, 0, render_extent_.width, render_extent_.height);
                    cmd.bind_descriptor_set(current_camera_set(), 0);
                    cmd.bind_descriptor_set(current_light_set(), 1);
                    cmd.bind_descriptor_set(ssr_pass_->hiz_set(), 2);
                    last_kind = 2;
                }
                const ParticleDrawBatch& pb = particle_state_.quads[item.index];
                cmd.bind_descriptor_set(material_cache_->set_for(pb.texture_material ? *pb.texture_material
                                                                                      : particle_default_material_), 3);
                particle_pass_->draw(cmd, item.index, pb.count, particle_push_constants_(pb));
                frame_stats_.camera_draws     += 1;
                frame_stats_.camera_instances += pb.count;
                continue;
            }
            if (!item.is_sdf) {
                auto* mr = meshes.renderers[item.index];
                const auto& mr_mat = meshes.material(item.index);

                // The pipeline MUST be (re)bound before the 2-argument bind_descriptor_set()
                // calls below -- that overload derives its pipeline layout from whatever
                // CommandBuffer last had bound (see command_buffer.h's bound_pipeline_ cache),
                // which after an SDF run (or nothing, at the very start of the frame) is NOT
                // transparent_pass_'s layout. Binding descriptor sets first and the pipeline
                // second, even briefly, resolves set 0 against the wrong layout and is a
                // real validation error (descriptor type mismatch), not just a style issue.
                if (last_kind != 0 || mr_mat.shader != last_mesh_shader) {
                    transparent_pass_->bind(cmd, mr_mat.shader);
                    last_mesh_shader = mr_mat.shader;
                }

                if (last_kind != 0) {
                    // An earlier SDF item in this back-to-front list narrows the scissor to
                    // its own px_rect (see the sdf branch below) and nothing widens it again
                    // until the loop ends -- reset here or this mesh draw inherits that SDF's
                    // bounding box and gets clipped to it.
                    cmd.set_scissor(0, 0, render_extent_.width, render_extent_.height);
                    cmd.bind_descriptor_set(current_camera_set(), 0);
                    cmd.bind_descriptor_set(current_light_set(),  1);
                    cmd.bind_descriptor_set(*shadow_set_, 2);
                    // Sets 3/4/5 -- ssr_pass_'s own trace-input sets; set 6 -- forward_globals_'s
                    // UBO (see the ExtraSets bind lambda in the ctor). All bound via the
                    // ExtraSets callback configured at construction.
                    transparent_pass_->bind_extra(cmd);
                    cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);
                    last_kind = 0;
                }

                coopa::gfx::engine::passes::TransparentPass::PushConstants pc;
                pc.albedo     = glm::vec4(mr_mat.albedo, mr_mat.alpha);
                pc.metallic   = mr_mat.metallic;
                pc.roughness  = mr_mat.roughness;
                pc.ao         = mr_mat.ao;
                pc.gfx_time   = surface_gfx_time_();
                pc.gfx_params = mr_mat.shader_params;
                transparent_pass_->push(cmd, pc);

                // Pushed into transparent.frag's PushConstants' [32, 64) region (see the ctor's
                // extra_pc_bytes comment) -- per-object, unlike forward_globals_'s per-frame UBO.
                // Each field falls back to its PixelRenderConfig engine-wide default when the
                // material left it at PBRMaterial's negative sentinel (see that struct's own
                // doc for why -1 rather than baking the default straight into PBRMaterial).
                TransparentRefractionPushConstants refract_pc;
                float refract_ior       = mr_mat.ior >= 0.0f ? mr_mat.ior : config_.refraction_ior;
                float refract_thickness = mr_mat.refraction_thickness >= 0.0f
                                              ? mr_mat.refraction_thickness : config_.refraction_thickness;
                glm::vec3 refract_tint  = mr_mat.refraction_tint.r >= 0.0f
                                              ? mr_mat.refraction_tint : config_.refraction_tint;
                refract_pc.tint_thickness = glm::vec4(refract_tint, refract_thickness);
                refract_pc.ior_flags      = glm::vec4(refract_ior, mr_mat.has_refraction() ? 1.0f : 0.0f, 0.0f, 0.0f);
                refract_pc.shader_ext0    = mr_mat.shader_params_ext[0];
                refract_pc.shader_ext1    = mr_mat.shader_params_ext[1];
                // VERTEX|FRAGMENT, not FRAGMENT alone: TransparentPass's push-constant range now
                // covers both stages (see its PushConstants' gfx_time/gfx_params doc), and
                // Vulkan requires a push call's stageFlags to match the declared range for
                // every byte it touches, including this trailing per-object refraction block.
                cmd.push_constants(transparent_pass_->layout(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   sizeof(coopa::gfx::engine::passes::TransparentPass::PushConstants),
                                   sizeof(TransparentRefractionPushConstants), &refract_pc);

                // Set 7: albedo/normal/metallic-roughness -- see gfx/surface/transparent_fs.glsl
                // and engine::util::MaterialTextureCache. Bound per-item, not just at the
                // mesh/sdf transition above: two consecutive mesh items in this back-to-front
                // list can have different textures even when neither the pipeline nor sets 0-6
                // need rebinding.
                transparent_pass_->bind_material(cmd, material_cache_->set_for(mr_mat));

                mr->get_mesh()->bind(cmd);
                mr->get_mesh()->draw_lod_part(cmd, meshes.lod[item.index], meshes.part[item.index], 1, meshes.instance_idx[item.index]);
                frame_stats_.camera_draws     += 1;
                frame_stats_.camera_instances += 1;
            } else {
                if (last_kind != 1) {
                    sdf_forward_pass_->bind(cmd);
                    cmd.bind_descriptor_set(current_camera_set(), 0);
                    cmd.bind_descriptor_set(current_light_set(),  1);
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
    /// Retained because rebuild_overlay_chain_() constructs a UiPass outside the ctor, on
    /// every swapchain resize -- see overlay_target_'s own doc.
    coopa::gfx::command::CommandPool& cmd_pool_;
    PixelRenderConfig              config_;
    RenderExtent                   render_extent_;

    // gfx_time's components, fed to every surface-shader backbone: elapsed_time_ accumulates
    // render()'s dt and is never reset, so a displacement hook gets a monotonic clock regardless
    // of frame rate; frame_dt_ is y; z reuses frame_index_ below.
    float elapsed_time_ = 0.0f;
    float frame_dt_     = 0.0f;
    // The DISPLAY (letterboxed/fit) rect tilt_shift_pass_ and ui_world_target_ are sized to.
    // Unlike render_extent_ and every other target here, this one IS resize-aware -- render()
    // recomputes it each frame and rebuilds both when it changes. The residual limitation is
    // resolution_mode == "divisor", where render_extent_ itself stays startup-fixed, so a resize
    // moves the fit rect but not the internal render resolution.
    LetterboxRect                  upscaled_extent_;
    /// See set_display_region(); nullopt = the whole window.
    std::optional<LetterboxRect>   display_region_;
    /// See set_overlay_scene(); non-owning.
    coopa::scene::Scene*           overlay_scene_ = nullptr;

    coopa::gfx::engine::targets::GBufferTarget   gbuffer_target_;
    coopa::gfx::engine::targets::OffscreenTarget offscreen_target_; // lit + sky, pre-post, HDR
    coopa::gfx::engine::targets::OffscreenTarget post_target_;      // final low-res LDR, post PixelStylizePass
    // The world-space UI layer, and that separation is the whole point: drawn as a guest inside
    // post_target_ it would sit upstream of AA and tilt shift, so TAA ghosted a canvas moving
    // under an orbiting camera and tilt shift smeared the UI with the scene. Here it is
    // alpha-0-cleared and ui_composite_pass_ puts it back on top once every display-space effect
    // has run.
    //
    // Sized to upscaled_extent_ (the letterbox rect), not render_extent_, so the composite
    // samples it 1:1 -- at render resolution the composite needed a non-integer NEAREST upscale
    // that duplicated roughly every fifth row and column.
    //
    // A unique_ptr rebuilt by rebuild_overlay_chain_(), because upscaled_extent_ tracks the live
    // swapchain. Declared BEFORE world_ui_pass_ so it outlives the pass holding its RenderPass&.
    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> ui_world_target_;
    /// ui_world_target_'s last write had no canvases, so it already holds transparent black.
    bool ui_world_layer_clear_ = false;
    // Forward capture of transparent geometry -- a second reflection SOURCE for opaque
    // reflectors' SSR (see record_transparent_capture_()), not a visible target. Always
    // constructed (mirrors gbuffer_target_'s own always-on policy); render() checks
    // config_.ssr_reflect_transparent per frame to decide whether to draw into/read from it.
    coopa::gfx::engine::targets::TransparentCaptureTarget transparent_capture_target_;
    coopa::gfx::engine::targets::OffscreenTarget fog_target_; // fog composite, pre-post, HDR
    coopa::gfx::engine::targets::OffscreenTarget underwater_target_; // UnderwaterPass output, HDR
    coopa::gfx::engine::targets::OffscreenTarget volumetrics_target_; // wind composite, after fog, pre-post, HDR
    coopa::gfx::engine::targets::OffscreenTarget volumetrics_march_target_; // reduced-res march: in-scatter + transmittance
    bool ssr_secondary_bound_ = false;
    /// This frame has transparent geometry for SSR's second source (set after the gathers in
    /// render()); false skips the transparent capture and the second trace.
    bool secondary_this_frame_ = false; // ssr_pass_->set_secondary_source() done (once; see record_scene_)
    bool refraction_this_frame_ = false; // refraction's scene-colour chain is built + sampled this frame
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

    // Per-frame-in-flight, same policy and reason as camera_ubos_/camera_sets_ (file doc, rule
    // 2). This one matters most for dir_light_space_matrix, which update_dir_shadow_matrix_()
    // makes a fast-changing function of the camera: a raced write there shows up as directional
    // shadows visibly lagging the rest of the scene under camera motion. light_frame_ is set
    // from the same frame_slot as camera_frame_, so the two can never disagree.
    std::vector<std::unique_ptr<coopa::gfx::engine::data::LightData>> light_datas_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout> light_layout_; // one shared layout
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>      light_pool_;
    std::vector<std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>> light_sets_;
    uint32_t light_frame_ = 0;

    coopa::gfx::engine::data::FogData fog_data_;
    std::unique_ptr<coopa::gfx::engine::passes::FogPass> fog_pass_;
    std::unique_ptr<passes::UnderwaterPass> underwater_pass_;   // null when !underwater_enabled
    passes::UnderwaterPass::Params underwater_params_;           // filled in render()
    WaterFrameState water_state_;                                // set_water_state(), per frame
    ParticleFrameState particle_state_;                          // set_particle_state(), per frame
    glm::vec4 particle_depth_{0.1f, 1000.0f, 1.0f, 0.0f};         ///< near, far, is_perspective -- render()
    bool warned_particles_need_transparency_ = false;
    /// Quads drawn inside transparent_pass_'s bracket (see record_transparent_()). Built with it.
    std::unique_ptr<passes::ParticlePass> particle_pass_;
    /// The white-texture material set untextured particle batches bind at set 3.
    coopa::gfx::engine::components::PBRMaterial particle_default_material_;
    // Fixed source view fog_pass_ reads from -- chosen once from config_.ssr_enabled's startup
    // value, same policy as pixel_stylize_pass_'s own binding (see that construction-time
    // comment). Kept as a member (not a local) so pixel_stylize_pass_'s own construction, later
    // in the ctor body, can fall back to it when fog is disabled.
    coopa::gfx::TextureView pre_fog_view_typed_;

    // Volumetric wind -- the raymarched, moving, sparse counterpart to fog_pass_'s
    // analytic everywhere-medium (see gfxcoopa's VolumetricsPass / gfx/volumetrics.glsl). Always
    // constructed, runtime-gated on config_.volumetrics_enabled, same policy as fog_pass_.
    coopa::gfx::engine::data::VolumetricsData volumetrics_data_;
    std::unique_ptr<coopa::gfx::engine::passes::VolumetricsPass> volumetrics_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::FroxelVolumetricsPass> froxel_volumetrics_pass_;
    glm::vec3 prev_vol_cam_pos_{0.0f};      ///< Last frame's camera, for froxel reprojection.
    bool      froxel_history_valid_ = false; ///< A previous frame filled the froxel grid.
    uint32_t  froxel_last_frame_    = 0;     ///< frame_index_ of the last frame that filled it.

    coopa::gfx::engine::targets::ShadowMapTarget shadow_target_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   shadow_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        shadow_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         shadow_set_;
    // Material set (alpha-mask sampler) shared by gbuffer_pipeline_ and shadow_pipeline_ -- see
    // MaterialTextureCache's own doc. Constructed before both in the ctor.
    std::unique_ptr<coopa::gfx::engine::util::MaterialTextureCache>           material_cache_;
    std::unique_ptr<coopa::gfx::engine::passes::ShadowPipeline>  shadow_pipeline_;

    coopa::gfx::engine::data::PaletteLut palette_lut_;
    // Colour-grading strip LUT, applied by pixel_stylize_pass_ after the tonemap. Always a
    // valid texture (a 1x1 dummy when grading_lut_path is empty), so its binding is
    // unconditional -- size() == 0 is what disables the lookup. Startup-fixed, like
    // palette_lut_: the image is loaded once and bound once.
    coopa::gfx::engine::data::GradingLut grading_lut_;
    // Auto-exposure metering (eye adaptation). Null unless config_.auto_exposure_enabled was
    // set at construction -- see that flag's doc and the binding-5 comment at its call site.
    std::unique_ptr<coopa::gfx::engine::passes::ExposurePass> exposure_pass_;

    std::unique_ptr<coopa::gfx::engine::passes::GBufferPipeline> gbuffer_pipeline_;
    std::unique_ptr<coopa::gfx::engine::passes::SsaoPass>        ssao_pass_;       // always constructed
    // Shared accumulation-count buffer every temporally accumulated screen-space effect reads
    // (SSR, traced SSGI, contact shadows). Always constructed -- contact shadows are a runtime
    // toggle, so the buffer must exist even when SSR was never built.
    std::unique_ptr<coopa::gfx::engine::passes::TemporalHistoryPass> temporal_history_pass_;
    /// Screen-space contact shadows and their temporal resolve. Always constructed and always
    /// executed -- see the construction site for why the runtime toggle does not gate it.
    std::unique_ptr<passes::ContactShadowPass>                   contact_shadow_pass_;
    /** The contact march's runtime knobs as of last frame. A change to any of them invalidates
     *  the accumulated history, so a live tweak shows up on the next frame rather than fading
     *  in over the accumulation window. */
    struct ContactShadowKnobs {
        float length = 0.0f, strength = 0.0f, thickness = 0.0f;
        int   steps = 0;
        bool  enabled = false;
        bool operator==(const ContactShadowKnobs& o) const {
            return length == o.length && strength == o.strength && thickness == o.thickness
                && steps == o.steps && enabled == o.enabled;
        }
    };
    ContactShadowKnobs                                           contact_shadow_knobs_{};
    /// contact_shadow_pass_'s output already holds the disabled-state clear (see record_scene_).
    bool                                                         contact_output_cleared_ = false;
    /// pixel_lighting_pass_'s extra set (set 4): contact_shadow_pass_'s resolved occlusion.
    /// Declared BEFORE pixel_lighting_pass_ so it outlives the pass holding it in its ExtraSets.
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   contact_extra_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        contact_extra_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         contact_extra_set_;
    std::unique_ptr<coopa::gfx::engine::passes::DeferredLightingPass> pixel_lighting_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::PixelStylizePass> pixel_stylize_pass_;
    // debug_view diagnostic (see build_post_chain_()'s own doc on this instance) -- the
    // one extra descriptor set (SSR reflection / traced-SSGI / G-buffer depth) debug_view.frag
    // needs on top of the camera/light/shadow/G-buffer+SSAO sets it shares with
    // pixel_lighting_pass_ above.
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   debug_view_extra_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        debug_view_extra_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         debug_view_extra_set_;
    std::unique_ptr<coopa::gfx::engine::passes::DeferredLightingPass> debug_view_pass_;
    // Physically-based depth of field. Runs at RENDER resolution (unlike tilt_shift_pass_,
    // which runs at display resolution), between volumetrics and bloom. Needs no resize rebuild:
    // it is sized to the startup-fixed render_extent_, and a window resize only moves the
    // letterbox rect downstream.
    std::unique_ptr<coopa::gfx::engine::passes::DofPass> dof_pass_;
    // resolve_dof_focus_()'s object-focus smoothing state (see that method's own doc).
    // <= 0 means "unseeded" -- the first object-focus frame snaps to the resolved
    // depth rather than racking up from zero.
    float smoothed_dof_focus_ = -1.0f;
    // The "focus" shadow fit's screen-centre probe (record_focus_probe_/read_focus_probe_).
    static constexpr uint32_t kFocusProbeSamples = 5;
    /// Screen rows the probe samples (at the centre column), as fractions of the render
    /// height: the centre first, then alternately below and above it, so a centre ray into the
    /// sky still finds the ground the camera is framing.
    static constexpr float kFocusProbeRows[kFocusProbeSamples] = {0.5f, 0.6f, 0.4f, 0.7f, 0.3f};
    std::vector<std::unique_ptr<coopa::gfx::memory::Buffer>> focus_probes_; // one per frame slot
    std::array<bool, kCameraFrames> focus_probe_pending_{};
    bool  focus_probe_wanted_   = false;
    float probed_focus_distance_ = 0.0f; ///< Smoothed view depth of the probe's hit; 0 = none yet.
    /** Smoothed forced-sharp half-width, metres. Negative = unseeded (0 is a legal value). */
    float smoothed_dof_range_ = -1.0f;
    bool  warned_missing_dof_object_ = false;
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

    /**
     * The final, WINDOW-sized composite target and the two passes that fill it.
     *
     * Everything above this point works at render_extent_ or at the letterbox content rect;
     * overlay_target_ is the full swapchain extent, so the bars are part of it (cleared black)
     * and a screen-space HUD can draw over them. ui_composite_pass_ draws the post-processed
     * scene into the letterbox sub-rect with the world-UI layer on top -- performing, when tilt
     * shift is off, the nearest upscale itself -- and screen_ui_pass_ then draws as a guest in
     * the same bracket at full window resolution. upscale_pass_ is left as a 1:1 blit into the
     * swapchain.
     *
     * Rebuilt together by rebuild_overlay_chain_() on every swapchain resize: TexturedQuad2DPass
     * holds a RenderPass& that OffscreenTarget::recreate() would dangle.
     */
    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> overlay_target_;
    VkExtent2D                                                   overlay_extent_{0, 0};
    std::unique_ptr<passes::UiCompositePass>                     ui_composite_pass_;
    /// Screen-space UI (uicoopa's UiPass). Null when config_.screen_ui_enabled was false at
    /// construction -- startup-fixed, same reasoning as world_ui_pass_.
    std::unique_ptr<coopa::ui::UiPass>                           screen_ui_pass_;
    /// This frame's screen-space canvases, gathered in render() alongside world_canvases_.
    std::vector<coopa::ui::CanvasComponent*>                     screen_canvases_;
    /// The image ui_composite_pass_ reads when tilt shift is off (post_target_, or aa_target_
    /// once AA is on). Cached at construction because rebuild_overlay_chain_() has to rebind
    /// it on a resize and must make exactly the same choice the ctor did.
    coopa::gfx::TextureView                                      display_source_view_;

    /**
     * Anti-aliasing. Unlike the passes above, these are NOT always constructed -- all four are
     * nullptr whenever aa_mode == "off" (the default), so a scene that never opts in allocates no
     * extra target and pays no extra draw call. When aa_mode != "off" all three passes are built
     * together and all three write into aa_target_, so render() can switch among fxaa/smaa/taa
     * every frame with no rebuild.
     *
     * aa_target_ sits between post_target_ (pixel_stylize_pass_'s output) and everything that
     * would otherwise read post_target_ directly: tilt shift's source, the composite's base image
     * and low_res_color_image()'s screenshot accessor all redirect to it once AA is on. Same
     * RGBA8_Unorm format as post_target_ -- FXAA/SMAA/TAA are LDR spatial or temporal filters,
     * not tonemap steps.
     */
    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> aa_target_;
    std::unique_ptr<coopa::gfx::engine::passes::FxaaPass> fxaa_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SmaaPass> smaa_pass_;
    // Owns its own RGBA16F accumulation ping-pong (see TaaPass's member doc for why the
    // shared RGBA8 aa_target_ cannot hold the history) and writes aa_target_ through a
    // passthrough present draw.
    std::unique_ptr<coopa::gfx::engine::passes::TaaPass> taa_pass_;
    // Halton(2,3) jitter phase for "taa" mode, ADVANCED ONLY when config_.aa_mode == "taa" --
    // deliberately separate from frame_index_ below, which must keep advancing every frame
    // regardless of aa_mode (SSAO/SSR/gfx_time all depend on it). Mirrors blendy's own
    // frame_index_/ssao_frame_index_ split (see PbrRenderPipeline).
    uint32_t taa_jitter_index_ = 0;
    // This frame's jitter as the NDC displacement it applies to the projection, written by
    // apply_taa_jitter_() each frame (zero when aa_mode != "taa"). The TAA resolve subtracts
    // it so its velocity is measured between UNjittered positions.
    glm::vec2 taa_jitter_ndc_ = glm::vec2(0.0f);
    // SSAO kernel-rotation phase, advanced only on frames where the camera's UNJITTERED
    // view-projection changed -- see record_scene_()'s SSAO block. Separate from frame_index_
    // for the same reason taa_jitter_index_ is: that counter must keep advancing every frame
    // because SSR and gfx_time depend on it, while this one must be able to stand still.
    //
    // Standing still is what lets ssao_resolve.frag's exponential history blend converge. With a
    // constant raw input the blend is a geometric series onto a fixed point (byte-static well
    // inside a second at 0.85); with an input that changes every frame it can only ever orbit,
    // which reads as AO that never finishes settling.
    uint32_t  ssao_rotation_index_ = 0;
    /** The pose the stillness deadband measures against: reset to the current pose whenever
     *  visible motion exceeds the deadband, held while within it. Anchoring (rather than a
     *  per-frame delta) makes hand tremor -- sub-pixel oscillation around a point -- count
     *  as still, while sustained drift accumulates deviation and exits. */
    glm::mat3 ssao_freeze_anchor_view_{1.0f};
    glm::vec3 ssao_freeze_anchor_pos_{0.0f};
    /** Deadband radii: rotation as pixels swept at the screen centre, translation in world
     *  units (~0.3 px for geometry nearer than ~2 wu). Below these, a pose change cannot
     *  visibly move the image, so it must not restart the stochastic redraw either. */
    static constexpr float kFreezeDeadbandPx = 0.35f;
    static constexpr float kFreezeDeadbandWu = 0.005f;
    /** Consecutive frames the camera has been still, and whether that has crossed the freeze
     *  threshold. While frozen, each temporal resolve holds its accepted-history pixels
     *  VERBATIM, which is what lets the image reach byte-static; the AO sample jitter also
     *  holds. What holds is the accumulated AVERAGE, so there is no look change at the freeze
     *  boundary, just a stop to sub-level residual updates. SSR/SSGI's ray jitter deliberately
     *  keeps advancing: its resolve is a converging average that needs a fresh draw per frame,
     *  and the verbatim hold is what makes its output static. */
    uint32_t  camera_frames_still_ = 0;
    bool      temporal_frozen_     = false;
    /** Still frames before freezing. Small enough that image_settles_after_camera_stops'
     *  eight-frame budget is met with margin; the pre-freeze frames only add 1/(count+1)-scale
     *  residuals, which that test's threshold tolerates. */
    static constexpr uint32_t kTemporalFreezeAfter = 4;
    /** Rotation between consecutive views, as pixels swept at the screen centre -- drives the
     *  AO blur's velocity widening (SsaoPass::Params::motion_px). */
    float     ssao_motion_px_      = 0.0f;
    glm::mat4 ssao_prev_view_{1.0f};
    // Physics collider/contact-normal gizmo overlay. Always constructed; config_.debug_view
    // == "lines" gates only whether render() calls draw(). debug_lines_ is filled by the
    // caller once per frame before render() -- this pass is kept physxcoopa-free by design,
    // so Engine is what bridges PhysicsWorld::debug_draw() into that vector.
    std::unique_ptr<passes::DebugLinePass>                       debug_line_pass_;
    /// BLEND meshes in the editor shading modes (see transparent_preview_pass.h).
    std::unique_ptr<passes::TransparentPreviewPass>              transparent_preview_pass_;
    /// This frame's gather, for record_post_chain_()'s transparent preview (valid during recording).
    const MeshGather*                                            frame_meshes_ = nullptr;

    /// The G-buffer depth, as a set-1 sampler for world_ui_pass_'s occlusion compare. Declared
    /// BEFORE world_ui_pass_ so it outlives it: the pass holds this layout in its ExtraSets and
    /// is rebuilt on every resize, while the set itself is bound once, to the startup-fixed
    /// gbuffer_target_, and survives untouched.
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   world_ui_depth_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        world_ui_depth_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         world_ui_depth_set_;
    /// World-space UI (uicoopa). Null when config_.world_ui_enabled is false -- that flag is
    /// startup-fixed, because the scene-depth descriptor above is built once and never rebound.
    /// The PASS is not startup-fixed: it is built against ui_world_target_'s render pass, which
    /// changes size with the window, so rebuild_overlay_chain_() replaces it.
    std::unique_ptr<coopa::ui::UiWorldPass>                      world_ui_pass_;
    /// This frame's world-space canvases, gathered from the scene in render(). A member
    /// rather than a local so the per-frame allocation is reused.
    std::vector<coopa::ui::CanvasComponent*>                     world_canvases_;
    std::vector<DebugLine>                                       debug_lines_;

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

    // The SSAO march's own depth pyramid: a HiZPass instance running
    // ao_depth_downsample.frag's weighted-average reduction instead of the SSR chain's
    // min(), and capped at the few levels the march's pixel-radius clamp can reach.
    // Always constructed (same policy as hiz_pass_); executed only when ssao_enabled.
    std::unique_ptr<coopa::gfx::engine::passes::HiZPass>           ao_depth_pyramid_pass_;

    // --- ssr_reflect_transparent: opaque surfaces also reflect transparent geometry ---
    // Always constructed, gated per frame. transparent_hiz_pass_ and
    // transparent_scene_color_mip_pass_ are second, independent instances of the same classes
    // hiz_pass_/scene_color_mip_pass_ use -- both are generic over any depth/colour image and
    // hold no GBufferTarget reference.
    std::unique_ptr<coopa::gfx::engine::passes::TransparentCapturePass>  transparent_capture_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::HiZPass>                 transparent_hiz_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SceneColorMipPass>       transparent_scene_color_mip_pass_;

    // --- Refraction: independent post-SSR scene-colour chain for the MESH forward pass's
    // u_scene_color, so a refracting object's background sample -- and its own
    // gfx_ssr_trace()/SSGI lookup -- sees SSR reflections rather than the pre-SSR image
    // scene_color_mip_pass_ still holds at that point.
    //
    // A THIRD, independent SceneColorMipPass instance, not a second execute() on the shared one:
    // that pass rebinds its internal per-mip descriptor sets on every execute(), and doing so
    // twice in one not-yet-submitted command buffer invalidates it (caught by validation as
    // "descriptor set was destroyed or updated"). Its own descriptor set for the same reason --
    // reusing ssr_pass_->scene_color_set() would mean rebinding that per frame.
    std::unique_ptr<coopa::gfx::engine::passes::SceneColorMipPass> refraction_scene_color_mip_pass_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>     refraction_scene_color_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>          refraction_scene_color_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>           refraction_scene_color_set_;

    // Previous frame's proj * view, for SSAO's and SSR's temporal resolve passes. When
    // aa_mode == "taa" this IS the jittered projection -- an unjittered prev-VP would give those
    // resolves an 8-frame crawl against TAA's own jittered current frame.
    glm::mat4 prev_view_proj_       = glm::mat4(1.0f);
    bool      prev_view_proj_valid_ = false;
    // Previous frame's UNjittered proj * view, for the TAA resolve's reprojection matrix --
    // which must measure velocity between unjittered poses so a still camera measures exactly
    // zero (the current frame's jitter is removed separately, via taa_jitter_ndc_).
    glm::mat4 prev_unjittered_view_proj_ = glm::mat4(1.0f);
    // Monotonic per-rendered-frame counter, incremented once at the end of render(). Shared
    // by SSAO's noise-tile rotation and SSR's stochastic ray jitter (see each pass's own
    // Params::*frame_index* doc) -- both are per-pixel noise sources that need decorrelating
    // frame to frame, and there is no reason for the two to disagree about which frame it is.
    uint32_t  frame_index_          = 0;

    InstanceStream instance_stream_;
    bool           warned_perspective_snap_ = false;

    // sdf_data_ must be constructed before the four passes below, which bind its layout;
    // declaration order here matches the initializer list.
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
