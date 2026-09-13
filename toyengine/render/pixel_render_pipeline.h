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
 * The one exception: HiZPass, SceneColorMipPass and TransparentCapturePass (reused unmodified
 * from gfxcoopa) rebind their own descriptors on every execute(). Frames that run them pay for
 * a single device_.wait_idle() -- see FrameContext::need_ssr_trace_inputs.
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
 * point light still lights the scene, without occlusion.
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
#include <gfxcoopa/engine/passes/volumetrics_pass.h>
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
#include <toyengine/render/forward_globals.h>
#include <gfxcoopa/engine/util/material_texture_cache.h>
#include <gfxcoopa/engine/data/palette_lut.h>
#include <gfxcoopa/engine/passes/deferred_lighting_pass.h>
#include <gfxcoopa/engine/passes/ssr_pass.h>
#include <toyengine/render/passes/fullscreen_blit_pass.h>
#include <gfxcoopa/engine/passes/pixel_stylize_pass.h>
#include <gfxcoopa/engine/passes/dof_pass.h>
#include <gfxcoopa/engine/passes/bloom_pass.h>
#include <gfxcoopa/engine/passes/tilt_shift_pass.h>
#include <gfxcoopa/engine/passes/fxaa_pass.h>
#include <gfxcoopa/engine/passes/smaa_pass.h>
#include <gfxcoopa/engine/passes/taa_pass.h>
#include <toyengine/render/passes/upscale_pass.h>
#include <toyengine/render/passes/debug_line_pass.h>
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
          offscreen_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat),
          post_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA8_Unorm),
          transparent_capture_target_(device, allocator, render_extent_.width, render_extent_.height),
          // Fog composite target -- HDR, same reasoning as offscreen_target_ above: fog belongs
          // in linear HDR (Unity applies it there too), ahead of pixel_stylize_pass_'s tonemap
          // step. A separate target is mandatory, not a style choice: pipeline::RenderPass
          // hardcodes LOAD_OP_CLEAR, so FogPass can't reopen and composite in place onto the
          // image it reads from.
          fog_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat),
          // Wind composite target -- same HDR format and the same LOAD_OP_CLEAR-forced
          // separation as fog_target_ above. Wind reads fog's output and writes its own.
          volumetrics_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat),
          nearest_sampler_(coopa::gfx::engine::util::Sampler::nearest(device)),
          linear_sampler_(coopa::gfx::engine::util::Sampler::linear(device)),
          // Hardware compareEnable, not a plain linear sampler -- see gfx/shadow_sampling.glsl's
          // *Shadow-family doc (pixel_lighting.frag/transparent.frag's dir_shadow_map and
          // point_shadow_map are sampler2DShadow/samplerCubeShadow to match).
          shadow_sampler_(coopa::gfx::engine::util::Sampler::shadow(device)),
          fog_data_(device, allocator),
          volumetrics_data_(device, allocator),
          shadow_target_(device, allocator, config_.shadow_map_resolution, config_.cube_shadow_resolution),
          palette_lut_(coopa::gfx::engine::data::PaletteLut::load(device, allocator, cmd_pool, config_.palette_path)),
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
     *        Scene::update()/late_update() and render(); drawn when config_.debug_lines_enabled
     *        is set, cleared by the caller each frame (this pipeline never clears it itself,
     *        matching how it never owns the scene it reads).
     */
    std::vector<DebugLine>& debug_lines() { return debug_lines_; }

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
     * @brief Read-only access to the live render config.
     */
    const PixelRenderConfig& render_config() const { return config_; }

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
        TOY_KEEP_STARTUP_FIXED(scale_divisor);
        TOY_KEEP_STARTUP_FIXED(shadow_map_resolution);
        TOY_KEEP_STARTUP_FIXED(cube_shadow_resolution);
        TOY_KEEP_STARTUP_FIXED(sdf_max_renderers);
        TOY_KEEP_STARTUP_FIXED(sdf_max_shapes);
        TOY_KEEP_STARTUP_FIXED(palette_path);
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
        float dof_focus_distance = resolve_dof_focus_(cam, view, scene, dt);

        // TAA sub-pixel jitter: an 8-frame Halton(2,3) sequence added to the projection's
        // jitter terms (proj[2][0]/[2][1]), not a [3][*] translation.
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
        renderer.wait_for_current_frame();
        camera_frame_ = frame_slot;
        light_frame_ = frame_slot;
        camera_ubos_[frame_slot]->update(view, proj, cam_pos, pixel_density);

        // Must follow the light_frame_ assignment above: update_lights_() writes through
        // current_light_data(), which reads light_frame_. Writing first would put this frame's
        // light colours in the other slot while the shadow-matrix write below landed correctly.
        update_lights_(scene);

        // debug_lines_ was filled by the caller before render() ran (see debug_lines()'s
        // doc); upload it unconditionally -- cheap when empty, and it keeps this slot's buffer
        // valid if debug_lines_enabled is flipped on between frames.
        debug_line_pass_->upload(frame_slot, debug_lines_);

        gather_ui_canvases_(scene, frame_slot, view);
        const MeshGather meshes = gather_meshes_(scene, frame_slot);
        const std::vector<SdfDrawItem> sdf_draws = gather_sdf_(scene, view, proj, cam_pos, frame_slot);

        auto* dir_light = scene.find_first_component<DirectionalLightComponent>();
        bool cast_dir_shadow = config_.shadows_enabled && dir_light && dir_light->cast_shadows;
        if (dir_light) {
            update_dir_shadow_matrix_(dir_light->direction, cam, cast_dir_shadow);
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
        light_datas_[light_frame_]->upload();

        if (config_.fog_enabled) {
            update_fog_data_(unjittered_proj, view, cam_pos, dir_light);
        }

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
                                    || config_.ssr_reflect_transparent;

        // ssao_debug_view is a runtime flag (re-read every frame, same policy as ssr_enabled):
        // when set, ssao_debug_pass_ replaces pixel_lighting_pass_/skybox_pass_ below, and the
        // SSR composite / forward transparent pass are skipped for this frame -- there is
        // nothing left to composite reflections or transparent geometry onto once lighting
        // itself has been replaced by a raw occlusion-buffer visualization.
        const bool ao_debug = config_.ssao_debug_view;

        if (need_ssr_trace_inputs) {
            // HiZPass and SceneColorMipPass rebind their own mip-0 descriptor set inside every
            // execute(). That is safe only under a per-frame wait, which this pipeline otherwise does
            // not do -- see rule 1 in the file doc. Rather than fork two sizeable gfxcoopa passes to
            // remove an internal rebind, frames that need them pay for one full wait here.
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
        ctx.dof_focus_distance    = dof_focus_distance;
        ctx.need_ssr_trace_inputs = need_ssr_trace_inputs;
        ctx.ao_debug              = ao_debug;
        ctx.cast_dir_shadow       = cast_dir_shadow;
        ctx.cast_point_shadow     = cast_point_shadow;
        ctx.shadow_point          = shadow_point;

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
                record_scene_(cmd, ctx, meshes, sdf_draws);
                record_post_chain_(cmd, ctx);
                record_overlay_(cmd, ctx);
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
     * @struct MeshGather
     * @brief This frame's MeshRenderers with their resolved world matrices and instance-buffer
     *        indices, all in the same order.
     *
     * One list shared by the instance upload and every recording site, so no two of them can
     * disagree about which renderer is which. instance_idx[i] is InstanceStream::kInvalidIndex
     * for a renderer that was not uploaded (not ready, no transform, or the buffer was full);
     * every draw loop skips on exactly that.
     */
    struct MeshGather {
        std::vector<coopa::gfx::engine::components::MeshRenderer*> renderers;
        std::vector<glm::mat4> world_matrices;
        std::vector<uint32_t>  instance_idx;
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
        /** True when the Hi-Z pyramid and scene-colour mip chain must be built this frame --
         *  transparent.frag traces the same ones ssr.frag does, so this is wider than
         *  ssr_enabled alone. Also the flag that pays for the one per-frame wait_idle(). */
        bool need_ssr_trace_inputs = false;
        /** ssao_debug_view: replaces lighting with a raw occlusion visualization, which leaves
         *  the SSR composite and the forward transparent pass with nothing to composite onto. */
        bool ao_debug = false;
        bool cast_dir_shadow   = false;
        bool cast_point_shadow = false;
        coopa::gfx::engine::components::PointLightComponent* shadow_point = nullptr;
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
            light_datas_.push_back(std::make_unique<coopa::gfx::engine::data::LightData>(device_, allocator_));
            light_sets_.push_back(std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *light_pool_, *light_layout_));
            light_sets_[i]->bind_buffer(0, light_datas_[i]->buffer());
        }

        shadow_layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
            coopa::gfx::pipeline::DescriptorLayoutBuilder()
                .combined_sampler(0, coopa::gfx::ShaderStage::Fragment)
                .combined_sampler(1, coopa::gfx::ShaderStage::Fragment)
                .build(device_));
        shadow_pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
            coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*shadow_layout_, 1).build(device_));
        shadow_set_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *shadow_pool_, *shadow_layout_);
        shadow_set_->bind_image(0, shadow_target_.dir_shadow_view(), shadow_sampler_.handle());
        shadow_set_->bind_image(1, shadow_target_.cube_shadow_view(), shadow_sampler_.handle());
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
                config_.shaders(sd.shadow_cube_frag.empty() ? "shadow_cube.frag" : sd.shadow_cube_frag));
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
     * @brief Builds the skybox, SSAO, SSAO debug view and deferred lighting passes.
     */
    void build_lighting_passes_() {
        skybox_pass_ = std::make_unique<coopa::gfx::engine::passes::SkyboxPass>(
            device_, offscreen_target_.render_pass_object(), *camera_layout_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("skybox.frag"));
        skybox_pass_->set_gbuffer_normal_image(gbuffer_target_.g1_view_typed(), linear_sampler_);

        // SsaoPass is always constructed: pixel_lighting.frag (and the SSR composite, when
        // that's built below) always has a g_ssao binding to fill, toggle or not -- cheap
        // either way, since SsaoPass's ctor already builds a permanent 1x1 neutral texture.
        ssao_pass_ = std::make_unique<coopa::gfx::engine::passes::SsaoPass>(
            device_, allocator_, cmd_pool_, *camera_layout_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("ssao.frag"),
            config_.shaders("ssao_resolve.frag"),
            config_.shaders("ssao_blur.frag"));
        ssao_pass_->recreate(render_extent_.width, render_extent_.height);
        // Bound once: update_descriptors() calls bind_image(), and g1/g2 never change anyway
        // (render_extent_ is startup-fixed). See rule 1 in the file doc.
        ssao_pass_->update_descriptors(gbuffer_target_.g1_view_typed(), gbuffer_target_.g2_view_typed(), linear_sampler_);

        // ssao_enabled is a load-time config value (no live reload), so which image to bind is
        // decided once here rather than every frame -- see the update_descriptors comment above
        // for why a per-frame rebind would be unsafe anyway.
        const VkImageView ssao_view = ssao_source_view_();

        // Draws the exact ssao_view bound into pixel_lighting_pass_/ssr_pass_ below, so the
        // debug view always reflects what lighting actually consumes -- including the neutral
        // 1x1 texture when ssao_enabled is off.
        ssao_debug_pass_ = std::make_unique<passes::FullscreenBlitPass>(
            device_, offscreen_target_.render_pass_object(),
            config_.shaders("fullscreen.vert"),
            config_.shaders("ssao_debug.frag"));
        ssao_debug_pass_->set_source_image(coopa::gfx::detail::wrap(ssao_view), ssao_pass_->sampler());

        // No ExtraSets (toyengine has neither GI nor reflection probes to plumb through), so
        // this collapses to the same {camera=0, light=1, shadow=2, gbuffer=3} layout the old
        // fork hardcoded -- gbuffer_set_index_ is derived, not hardcoded, so this is correct
        // whether or not extras are ever added later (see the fix in deferred_lighting_pass.h).
        pixel_lighting_pass_ = std::make_unique<coopa::gfx::engine::passes::DeferredLightingPass>(
            device_, offscreen_target_.render_pass_object(), *camera_layout_, *light_layout_,
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

        scene_color_mip_pass_ = std::make_unique<coopa::gfx::engine::passes::SceneColorMipPass>(
            device_, allocator_,
            config_.shaders("fullscreen.vert"),
            config_.shaders("scene_color_downsample.frag"));
        scene_color_mip_pass_->recreate(render_extent_.width, render_extent_.height);

        // No ExtraSets (toyengine has neither GI nor reflection probes to plumb through)
        // and half_res left at its false default -- toyengine already renders at the
        // pipeline's low internal resolution, so there is no separate "trace at half of
        // that" tier worth the bilateral-upsample cost.
        ssr_pass_ = std::make_unique<coopa::gfx::engine::passes::SsrPass>(
            device_, allocator_, *camera_layout_,
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
        ssr_pass_->set_ssao_image(ssao_source_view_(), ssao_pass_->sampler().handle());
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
        // policy elsewhere -- render() gates execute() per frame on config_.refraction_enabled
        // && config_.transparency_enabled; its image simply never gets sampled if
        // config_.refraction_enabled was false at construction (see the transparent_extra
        // bind lambda just below, which is the only thing that ever reads this set, and only
        // does so when that same condition held at startup).
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
                config_.shaders(sd.frag.empty() ? "transparent.frag" : sd.frag));
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
        coopa::gfx::TextureView pre_volumetrics_view =
            config_.fog_enabled ? fog_target_.color_view_typed() : pre_fog_view_typed_;

        // Volumetric wind -- always constructed (same always-on-but-runtime-gated policy as
        // fog_pass_ above); render() checks config_.volumetrics_enabled per frame. Where fog
        // integrates an analytic everywhere-medium in one sample, this raymarches a sparse
        // noise field advected along a wind vector, which is what makes it read as moving
        // air rather than haze (see gfx/volumetrics.glsl's header for the three ideas involved).
        volumetrics_pass_ = std::make_unique<coopa::gfx::engine::passes::VolumetricsPass>(
            device_, volumetrics_target_.render_pass_object(), volumetrics_data_.buffer(),
            config_.shaders("fullscreen.vert"),
            config_.shaders("volumetrics.frag"));
        volumetrics_pass_->set_source_images(pre_volumetrics_view, gbuffer_target_.g1_view_typed(),
                                      gbuffer_target_.g2_view_typed(), linear_sampler_);

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
            config_.bloom_enabled ? &linear_sampler_ : nullptr);

        // Anti-aliasing. Conditionally constructed, unlike the passes above: aa_mode == "off"
        // must allocate nothing and leave every downstream binding as it was, so the mode is
        // checked here rather than per frame. All three passes are built together and share
        // aa_target_, so switching AMONG fxaa/smaa/taa at runtime is safe; going to or from
        // "off" is not. Each binds once, so no per-frame wait is needed.
        if (config_.aa_mode != "off") {
            aa_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
                device_, allocator_, render_extent_.width, render_extent_.height,
                coopa::gfx::Format::RGBA8_Unorm);

            fxaa_pass_ = std::make_unique<coopa::gfx::engine::passes::FxaaPass>(
                device_, aa_target_->render_pass_object(),
                config_.shaders("fullscreen.vert"),
                config_.shaders("fxaa.frag"));
            fxaa_pass_->set_source_image(post_target_.color_image_object()->view_typed(), linear_sampler_);

            smaa_pass_ = std::make_unique<coopa::gfx::engine::passes::SmaaPass>(
                device_, allocator_, aa_target_->render_pass_object(), cmd_pool_,
                render_extent_.width, render_extent_.height, linear_sampler_, config_.shaders);
            smaa_pass_->set_source_image(post_target_.color_image_object()->view_typed(), linear_sampler_);

            // history_format = UNORM, not gfxcoopa's SRGB default -- see taa_pass_'s own
            // member doc for why post_target_'s color-space convention demands it.
            taa_pass_ = std::make_unique<coopa::gfx::engine::passes::TaaPass>(
                device_, allocator_, aa_target_->render_pass_object(), linear_sampler_,
                render_extent_.width, render_extent_.height,
                config_.shaders("taa.vert"), config_.shaders("taa.frag"),
                VK_FORMAT_R8G8B8A8_UNORM);
            taa_pass_->set_source_image(post_target_.color_image_object()->view_typed());
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
        // debug_lines_enabled is checked per frame, not here: this pass binds no descriptors, so
        // there is nothing for a startup-fixed flag to lock in.
        debug_line_pass_ = std::make_unique<passes::DebugLinePass>(
            device_, allocator_, post_target_.render_pass_object(),
            config_.shaders("debug_line.vert"),
            config_.shaders("debug_line.frag"));
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
        record_point_shadow_(cmd, meshes, sdf_draws, ctx.shadow_point, ctx.cast_point_shadow);
        record_gbuffer_(cmd, meshes, sdf_draws);

        if (ctx.need_ssr_trace_inputs) {
            hiz_pass_->execute(cmd, gbuffer_target_.depth_image_handle(), gbuffer_target_.depth_view_typed());
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
        if (config_.ssr_reflect_transparent) {
            record_transparent_capture_(cmd, meshes, sdf_draws);
            // TransparentCaptureTarget is out of scope for the Vulkan-sealing refactor
            // (MRT, no sealed equivalent -- see gfxcoopa's plan), so its raw VkImageView
            // accessors are wrapped here via detail::wrap() rather than gaining
            // TextureView-returning siblings themselves.
            transparent_hiz_pass_->execute(cmd, transparent_capture_target_.depth_image_handle(),
                                           coopa::gfx::detail::wrap(transparent_capture_target_.depth_view()));
            transparent_scene_color_mip_pass_->execute(
                cmd, coopa::gfx::detail::wrap(transparent_capture_target_.shaded_color_view()));

            // Rebind ssr_pass_'s secondary source (sets 4-6) to these now-populated, correctly
            // laid-out images -- deliberately not done at construction, see that call site. Safe
            // mid-recording only because this block runs solely when need_ssr_trace_inputs already
            // paid for a device_.wait_idle() before begin_frame().
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

        // Lighting and skybox draw into the SAME open render pass instance: pipeline::RenderPass
        // always uses LOAD_OP_CLEAR, so a target can never be reopened to composite onto. The
        // skybox's discard of already-lit pixels works only because it is a second draw inside
        // offscreen_target_'s single begin()/end().
        offscreen_target_.begin(cmd);

        if (ctx.ao_debug) {
            // Replaces lighting+skybox entirely -- draws SsaoPass's bound output
            // (the exact ssao_view bound at construction, see that comment) fullscreen.
            ssao_debug_pass_->draw(cmd, render_extent_.width, render_extent_.height);
        } else {
            PixelLightingPushConstants lighting_pc;
            lighting_pc.light_bands       = config_.light_bands;
            lighting_pc.spec_threshold    = config_.spec_threshold;
            lighting_pc.rim_strength      = config_.rim_strength;
            lighting_pc.ambient_intensity = config_.indirect.ambient_intensity;
            lighting_pc.sky_intensity     = config_.indirect.sky_intensity;
            lighting_pc.soft_lighting     = config_.soft_lighting ? 1.0f : 0.0f;
            // gfxcoopa's DeferredLightingPass::draw() pushes this internally now (the
            // templated overload), after its own bind_pipeline() -- no separate push needed.
            pixel_lighting_pass_->draw(cmd, current_camera_set(), current_light_set(), *shadow_set_, lighting_pc,
                                      render_extent_.width, render_extent_.height);

            skybox_pass_->draw(cmd, current_camera_set(), ctx.view, ctx.proj, render_extent_.width, render_extent_.height,
                               config_.indirect);
        }

        offscreen_target_.end(cmd);

        if (ctx.need_ssr_trace_inputs) {
            // Prefiltered scene-colour mip chain: also feeds transparent.frag's
            // gfx_ssr_trace() cone-LOD taps and SSGI bounce lookup, not just ssr.frag's
            // own -- see need_ssr_trace_inputs' own doc.
            scene_color_mip_pass_->execute(cmd, offscreen_target_.color_view_typed());
        }

        if (config_.ssr_enabled && !ctx.ao_debug) {
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
            ssr_params.sky_zenith        = config_.indirect.sky_zenith;
            ssr_params.sky_horizon       = config_.indirect.sky_horizon;
            ssr_params.sky_ground        = config_.indirect.sky_ground;
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

        if (config_.transparency_enabled && config_.refraction_enabled && !ctx.ao_debug) {
            // Builds refraction's own scene-colour chain -- the dedicated instance transparent.frag's
            // u_scene_color reads whenever refraction is active. It must be a SEPARATE instance, not a
            // second execute() on scene_color_mip_pass_: that pass rebinds its internal per-mip
            // descriptor sets on every execute(), and doing so twice in one not-yet-submitted command
            // buffer invalidates it. MESH-only -- BLEND SDFs keep reading the original chain.
            coopa::gfx::TextureView refraction_source =
                (config_.refraction_include_reflections && config_.ssr_enabled)
                    ? ssr_pass_->output_view_typed() : offscreen_target_.color_view_typed();
            refraction_scene_color_mip_pass_->execute(cmd, refraction_source);
        }

        if (config_.transparency_enabled && !ctx.ao_debug) {
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

        // Fog composite. After the transparent pass, so BLEND geometry is fogged too (it was drawn
        // in place into the same image), and before pixel_stylize_pass_, so fog sits in linear HDR
        // ahead of tonemap/outline/dither/palette. Gated on the startup-fixed flag both this pass's
        // source and pixel_stylize_pass_'s were chosen from.
        if (config_.fog_enabled) {
            fog_target_.begin(cmd);
            fog_pass_->draw(cmd, render_extent_.width, render_extent_.height);
            fog_target_.end(cmd);
        }

        // Volumetric wind. After fog, so wisps layer over fogged geometry, and before DOF and
        // bloom, so they defocus with everything else and sun-lit ones glow. Gated on the same
        // startup-fixed flag its source view was chosen from.
        if (config_.volumetrics_enabled) {
            volumetrics_target_.begin(cmd);
            volumetrics_pass_->draw(cmd, render_extent_.width, render_extent_.height);
            volumetrics_target_.end(cmd);
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

            dof_params.max_radius        = config_.dof_max_radius;
            dof_params.sample_count      = config_.dof_sample_count;
            dof_params.blade_count       = config_.dof_blade_count;
            dof_params.blade_rotation_deg = config_.dof_blade_rotation;
            // Same three-line camera idiom pixel_stylize_pass_'s push constants use
            // below, for the same linearization formula (see gfx/depth.glsl).
            dof_params.camera_near           = ctx.cam ? ctx.cam->clip_start : 0.1f;
            dof_params.camera_far            = ctx.cam ? ctx.cam->clip_end : 1000.0f;
            dof_params.camera_is_perspective = (!ctx.cam || ctx.cam->type == CameraType::Perspective);
            dof_params.debug_view = config_.dof_debug_view;

            dof_pass_->execute(cmd, dof_params);
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
        post_pc.camera_near           = ctx.cam ? ctx.cam->clip_start : 0.1f;
        post_pc.camera_far            = ctx.cam ? ctx.cam->clip_end : 1000.0f;
        post_pc.camera_is_perspective = (!ctx.cam || ctx.cam->type == CameraType::Perspective) ? 1.0f : 0.0f;
        // This pipeline has no separate tonemap pass, unlike blendy -- exposure > 0
        // keeps pixel_stylize.frag's tonemap step live (see PushConstants::exposure's doc).
        post_pc.exposure         = config_.exposure;
        post_pc.bloom_intensity  = config_.bloom_enabled ? config_.bloom_intensity : 0.0f;
        pixel_stylize_pass_->draw(cmd, post_pc, render_extent_.width, render_extent_.height);
        if (config_.debug_lines_enabled) {
            debug_line_pass_->draw(cmd, ctx.proj * ctx.view,
                LetterboxRect{0, 0, render_extent_.width, render_extent_.height});
        }
        post_target_.end(cmd);

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
        // is unaffected either way -- the jitter lands in proj[2][0]/[2][1], a constant NDC x/y
        // shift, so gl_FragCoord.z is identical with or without it.
        //
        // Drawn at the DISPLAY rect, not render_extent_, so the composite samples it 1:1; at render
        // resolution it needed a non-integer NEAREST upscale and came out visibly stepped. The
        // residual cost is that the depth texture it compares against is lower-resolution than the
        // layer, so an occluded edge stair-steps on the render grid while the canvas's own edges
        // stay crisp.
        //
        // Recorded unconditionally, even with no canvases: the target must be written (and so
        // transitioned to SHADER_READ_ONLY_OPTIMAL) every frame, because ui_composite_pass_ samples
        // it every frame regardless.
        if (ui_world_target_) {
            ui_world_target_->begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
            if (world_ui_pass_) {
                for (coopa::ui::CanvasComponent* canvas : world_canvases_) {
                    world_ui_pass_->draw(cmd, ctx.frame_slot,
                                         upscaled_extent_.w, upscaled_extent_.h,
                                         ctx.unjittered_proj * ctx.view, *canvas);
                }
            }
            ui_world_target_->end(cmd);
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
                // -> neighborhood-into-output_target) -- unlike fxaa_pass_/taa_pass_
                // above/below, the caller doesn't bracket this one itself.
                smaa_pass_->draw(cmd, *aa_target_, /*exposure (unused by the shader body,
                                 see SmaaPass::NeighborhoodPush's doc)*/ 1.0f,
                                 config_.smaa_threshold, config_.smaa_max_search_steps,
                                 render_extent_.width, render_extent_.height);
            } else if (config_.aa_mode == "taa") {
                taa_pass_->prepare_history(cmd);
                taa_pass_->set_taa_config(config_.taa_blending_weight, config_.taa_weight_scale);
                aa_target_->begin(cmd);
                taa_pass_->draw(cmd, render_extent_.width, render_extent_.height);
                aa_target_->end(cmd);
                // Copies aa_target_'s just-drawn color image into history for next
                // frame -- OffscreenTarget::end() above leaves it in
                // SHADER_READ_ONLY_OPTIMAL, matching update_history()'s expected
                // oldLayout (see TaaPass::update_history's barriers).
                taa_pass_->update_history(cmd, *aa_target_);
            }
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
                screen_ui_pass_->draw(cmd, ctx.frame_slot,
                                      overlay_extent_.width, overlay_extent_.height,
                                      canvas->scale_factor(), canvas->draw_list());
            }
        }
        overlay_target_->end(cmd);
    }

    /**
     * @brief Gathers every MeshRenderer's world matrix and uploads it into this frame's
     *        instance-buffer slot.
     *
     * The per-index world_matrix() reads are a pure read (safe for concurrent readers, since
     * TransformSystem resolved every dirty transform earlier this frame) and each writes only
     * its own slot, so the gather parallelizes; the merge that follows stays serial to preserve
     * InstanceStream::add()'s monotonic index order.
     */
    MeshGather gather_meshes_(coopa::scene::Scene& scene, uint32_t frame_slot) {
        using coopa::gfx::engine::components::MeshRenderer;
        // Gather renderables once; the instance upload, the shadow AABB fit, and
        // the draw loops below all need the same list (and world matrices) in
        // the same order.
        MeshGather out;
        out.renderers = scene.get_components<MeshRenderer>();
        out.world_matrices.assign(out.renderers.size(), glm::mat4(1.0f));
        std::vector<uint8_t>   renderer_valid(out.renderers.size(), 0);
        instance_stream_.begin(frame_slot);
        out.instance_idx.assign(out.renderers.size(), InstanceStream::kInvalidIndex);

        // world_matrix() is a pure read, safe for any number of concurrent readers (unlike
        // get_world_matrix()), because TransformSystem already resolved every dirty transform
        // earlier this frame. Each index writes only its own slot.
        auto gather_mesh = [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) {
                if (!out.renderers[i]->is_ready() || !out.renderers[i]->owner) continue;
                auto* tc = out.renderers[i]->owner->get_transform();
                if (!tc) continue;
                out.world_matrices[i] = tc->transform().world_matrix();
                renderer_valid[i] = 1;
            }
        };
        if (should_parallelize_(out.renderers.size())) {
            jobs_->parallel_for_blocking(out.renderers.size(), 0 /* auto grain */, gather_mesh);
        } else {
            gather_mesh(0, out.renderers.size());
        }

        for (size_t i = 0; i < out.renderers.size(); ++i) {
            if (renderer_valid[i]) out.instance_idx[i] = instance_stream_.add(out.world_matrices[i]);
        }
        instance_stream_.upload();
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
        const std::vector<coopa::ui::CanvasComponent*> all_canvases = coopa::ui::collect_canvases(scene);

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
            auto sdf_renderer_comps = scene.get_components<SdfRenderer>();

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
        const LetterboxRect letterbox = compute_display_rect(
            config_, swapchain_.extent().width, swapchain_.extent().height,
            render_extent_.width, render_extent_.height);

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
            // rebuild above has to come first.
            rebuild_overlay_chain_(swapchain_extent.width, swapchain_extent.height);
        }
        return letterbox;
    }

    /**
     * @brief Adds this frame's TAA sub-pixel jitter to `proj`, when aa_mode == "taa".
     *
     * An 8-frame Halton(2,3) sequence added to the projection's jitter terms
     * (proj[2][0]/[2][1]), not a [3][*] translation. Callers keep an unjittered copy for the
     * consumers that must not see it -- see render()'s own comment on that rule.
     */
    void apply_taa_jitter_(glm::mat4& proj) {
        if (config_.aa_mode == "taa") {
            static const float halton_offset[8][2] = {
                {1.0f / 2.0f, 1.0f / 3.0f}, {1.0f / 4.0f, 2.0f / 3.0f},
                {3.0f / 4.0f, 1.0f / 9.0f}, {1.0f / 8.0f, 4.0f / 9.0f},
                {5.0f / 8.0f, 7.0f / 9.0f}, {3.0f / 8.0f, 2.0f / 9.0f},
                {7.0f / 8.0f, 5.0f / 9.0f}, {1.0f / 16.0f, 8.0f / 9.0f},
            };
            proj[2][0] += (2.0f * halton_offset[taa_jitter_index_][0] - 1.0f) / static_cast<float>(render_extent_.width);
            proj[2][1] += (2.0f * halton_offset[taa_jitter_index_][1] - 1.0f) / static_cast<float>(render_extent_.height);
            taa_jitter_index_ = (taa_jitter_index_ + 1) & 0x7u;
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
        vol.camera_pos    = glm::vec4(cam_pos, config_.volumetrics_debug_view ? 1.0f : 0.0f);
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

        auto volumes = scene.get_components<VolumeComponent>();
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

    /**
     * @brief Resolves this frame's DOF focus distance in metres, before command recording.
     *
     * Precedence:
     *   1. CameraComponent::focus_distance (per-camera; > 0 overrides everything below)
     *   2. config_.dof_focus_distance (manual, global fallback)
     *   3a. CameraComponent::focus_object, if non-empty, self-activates object focus for THIS
     *       camera regardless of config_.dof_focus_mode
     *   3b. otherwise dof_focus_mode == "orbit_target" or "object" activates that global mode
     *
     * Object focus resolves `path` through Scene::find_object_by_path() and takes the view-space
     * depth of its Transform -- exactly the quantity DofPass::dof_signed_coc() consumes, unlike
     * orbit_target's RADIAL distance to the pivot. A depth <= 0 (object behind the eye, or
     * unresolved) falls through to steps 1-2 rather than being trusted.
     *
     * Only object focus is smoothed: orbit_target is already smoothed twice over by
     * CameraController's own follow_smoothing/movement_smoothing.
     *
     * Called once per frame from render(), OUTSIDE the recording lambda -- smoothed_dof_focus_
     * is dt-driven mutable state, and advancing it during command recording is a hazard the rest
     * of this pipeline avoids.
     *
     * @param cam   Active camera, or nullptr (config-only values).
     * @param view  This frame's UNJITTERED view matrix.
     * @param scene Scene to resolve the focus object's path against.
     * @param dt    Frame delta, for the object-focus smoothing step.
     * @return Focus distance in metres, unclamped; the DOF block applies its own 0.01 floor.
     */
    float resolve_dof_focus_(const coopa::gfx::engine::components::CameraComponent* cam,
                             const glm::mat4& view, coopa::scene::Scene& scene, float dt) {
        float focus = (cam && cam->focus_distance > 0.0f)
            ? cam->focus_distance : config_.dof_focus_distance;

        std::string_view path = (cam && !cam->focus_object.empty())
            ? std::string_view(cam->focus_object)
            : (config_.dof_focus_mode == "object" ? std::string_view(config_.dof_focus_object)
                                                   : std::string_view());

        bool object_mode = false;
        if (!path.empty()) {
            object_mode = true;
            if (auto* obj = scene.find_object_by_path(path)) {
                if (auto* tc = obj->get_transform()) {
                    float depth = view_space_depth(view, glm::vec3(tc->get_world_matrix()[3]));
                    if (depth > 0.0f) focus = depth;
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
                if (orbit_dist > 0.0f) focus = orbit_dist;
            }
        }

        if (object_mode) {
            if (smoothed_dof_focus_ <= 0.0f) smoothed_dof_focus_ = focus; // seed, don't rack from 0
            smoothed_dof_focus_ = exp_smooth_toward(smoothed_dof_focus_, focus,
                                                    config_.dof_focus_smoothing, dt);
            focus = smoothed_dof_focus_;
        }
        return focus;
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
        // --- The world-UI layer, at the DISPLAY rect ---
        // Reset the pass BEFORE replacing the target it was built against: TexturedQuad2DPass
        // stores the RenderPass& it was constructed from, and this closes the window in which
        // that reference dangles. (device_.wait_idle() is the caller's job and has already run.)
        world_ui_pass_.reset();
        ui_world_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
            device_, allocator_, upscaled_extent_.w, upscaled_extent_.h,
            coopa::gfx::Format::RGBA8_Unorm);
        rebuild_world_ui_pass_();

        overlay_target_ = std::make_unique<coopa::gfx::engine::targets::OffscreenTarget>(
            device_, allocator_, w, h, coopa::gfx::Format::RGBA8_Unorm);
        overlay_extent_ = VkExtent2D{w, h};

        ui_composite_pass_ = std::make_unique<passes::UiCompositePass>(
            device_, overlay_target_->render_pass_object(),
            config_.shaders("fullscreen.vert"),
            config_.shaders("ui_composite.frag"));
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

        auto& ubo = current_light_data();

        // Configurable sky/ambient colour (see IndirectParams, config_.indirect) -- not tied
        // to the directional light's presence, so set unconditionally every frame, same as
        // the lighting pass's own ambient_intensity/sky_intensity push-constant fields.
        ubo.sky_zenith  = glm::vec4(config_.indirect.sky_zenith, 0.0f);
        ubo.sky_horizon = glm::vec4(config_.indirect.sky_horizon, 0.0f);
        ubo.sky_ground  = glm::vec4(config_.indirect.sky_ground, 0.0f);

        auto* dir = scene.find_first_component<DirectionalLightComponent>();
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
     * @brief Writes this frame's directional shadow matrix and shadow parameters into the
     *        current slot's LightUBO.
     *
     * The fit itself is pure math (pixel_math.h's compute_dir_shadow_fit()); this adds the
     * two things that need pipeline state: the world-to-texel conversion for the PCF radius,
     * and the write through current_light_data(). Must run after render() has set
     * light_frame_ to this frame's slot, same requirement as update_lights_(); the single
     * upload() happens in render() once this and the point-light flag are both set.
     */
    void update_dir_shadow_matrix_(const glm::vec3& direction,
                                   const coopa::gfx::engine::components::CameraComponent* cam,
                                   bool cast_dir_shadow) {
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

        const DirShadowFit fit = compute_dir_shadow_fit(
            direction, cam ? &fit_cam : nullptr,
            config_.shadow_distance, config_.shadow_map_resolution);

        auto& ubo = current_light_data();
        ubo.dir_light_space_matrix = fit.light_space_matrix;
        // .y is the PCF radius in shadow-map TEXELS, converted here from the world-space
        // config_.shadow_softness against this frame's actual texel size, so the penumbra
        // stays visually constant in world units as the box refits to the camera. Clamped to
        // 12: the radius is unbounded above (a small scene at high resolution asks for
        // hundreds) while calc_dir_shadow's Vogel disk is tuned for single-digit radii. 0
        // (soft_shadows off) selects the single hard compare.
        const float dir_pcf_radius_texels = config_.soft_shadows
            ? std::min(config_.shadow_softness / std::max(fit.texel_world, 1e-6f), 12.0f)
            : 0.0f;
        ubo.dir_shadow_params = glm::vec4(config_.shadow_bias, dir_pcf_radius_texels,
                                          cast_dir_shadow ? 1.0f : 0.0f, 0.05f);
    }

    /** @brief The first PointLightComponent with cast_shadows set, or nullptr. */
    coopa::gfx::engine::components::PointLightComponent* find_first_shadow_casting_point_light_(coopa::scene::Scene& scene) {
        for (auto* pl : scene.get_components<coopa::gfx::engine::components::PointLightComponent>()) {
            if (pl->cast_shadows) return pl;
        }
        return nullptr;
    }

    void record_directional_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                                    const MeshGather& meshes,
                                    const std::vector<SdfDrawItem>& sdf_draws,
                                    bool cast_dir_shadow) {
        shadow_target_.begin_directional_pass(cmd);
        if (cast_dir_shadow) {
            shadow_pipeline_->bind_directional(cmd);
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

            coopa::gfx::engine::passes::DirectionalShadowPushConstants pc{};
            pc.light_space_matrix = current_light_data().dir_light_space_matrix;
            pc.gfx_time = glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), 0.0f);

            // Same last-shader transition guard as record_gbuffer_() -- a caster's shadow
            // must displace identically to its G-buffer draw (see gfx/surface/shadow_vs.glsl's
            // doc), which means binding the SAME named variant here, not just the stock pass.
            std::string last_shader;
            bool have_bound = true; // stock, bound just above

            for (size_t i = 0; i < meshes.renderers.size(); ++i) {
                if (meshes.instance_idx[i] == InstanceStream::kInvalidIndex) continue;
                // A BLEND material only casts a shadow at full opacity -- this pass has no
                // per-fragment discard (single hard depth compare, no PCF to average a partial
                // alpha into a partial shadow -- see gfx/shadow_dither.glsl's doc), so there is
                // no way to draw a *partial* shadow for a translucent object; it's binary,
                // caster or not.
                if (meshes.renderers[i]->material.is_blended() && meshes.renderers[i]->material.alpha < 1.0f) continue;

                if (!have_bound || meshes.renderers[i]->material.shader != last_shader) {
                    shadow_pipeline_->bind_directional(cmd, meshes.renderers[i]->material.shader);
                    last_shader = meshes.renderers[i]->material.shader;
                    have_bound  = true;
                }

                // CUTOUT (AlphaMode::Mask): the mask texture punches through the shadow too,
                // via the same set/cutoff shadow_depth.frag tests against.
                pc.alpha_cutoff = meshes.renderers[i]->material.gpu_alpha_cutoff();
                pc.gfx_params   = meshes.renderers[i]->material.shader_params;
                cmd.bind_descriptor_set(material_cache_->set_for(meshes.renderers[i]->material), 0);
                shadow_pipeline_->push_directional(cmd, pc);
                meshes.renderers[i]->get_mesh()->bind(cmd);
                meshes.renderers[i]->get_mesh()->draw(cmd, 1, meshes.instance_idx[i]);
            }

            if (!sdf_draws.empty()) {
                sdf_shadow_pass_->bind_directional(cmd);
                cmd.bind_descriptor_set(sdf_data_.current_set(), 0);

                coopa::gfx::engine::passes::SdfDirectionalShadowPushConstants sdf_pc{};
                sdf_pc.light_space_matrix = current_light_data().dir_light_space_matrix;
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
            pc.gfx_time = glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), 0.0f);

            // See record_directional_shadow_'s identical transition guard.
            std::string last_shader;
            bool have_bound = true; // stock, bound just above

            for (size_t i = 0; i < meshes.renderers.size(); ++i) {
                if (meshes.instance_idx[i] == InstanceStream::kInvalidIndex) continue;
                // See record_directional_shadow_'s identical check -- a BLEND material only
                // casts a shadow at full opacity, since this pass has no partial-alpha discard.
                if (meshes.renderers[i]->material.is_blended() && meshes.renderers[i]->material.alpha < 1.0f) continue;

                if (!have_bound || meshes.renderers[i]->material.shader != last_shader) {
                    shadow_pipeline_->bind_cube(cmd, meshes.renderers[i]->material.shader);
                    last_shader = meshes.renderers[i]->material.shader;
                    have_bound  = true;
                }

                // See record_directional_shadow_'s identical CUTOUT handling.
                pc.alpha_cutoff = meshes.renderers[i]->material.gpu_alpha_cutoff();
                pc.gfx_params   = meshes.renderers[i]->material.shader_params;
                cmd.bind_descriptor_set(material_cache_->set_for(meshes.renderers[i]->material), 0);
                shadow_pipeline_->push_cube(cmd, pc);
                meshes.renderers[i]->get_mesh()->bind(cmd);
                meshes.renderers[i]->get_mesh()->draw(cmd, 1, meshes.instance_idx[i]);
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

    void record_gbuffer_(coopa::gfx::command::CommandBuffer& cmd,
                         const MeshGather& meshes,
                         const std::vector<SdfDrawItem>& sdf_draws) {
        gbuffer_target_.begin(cmd);
        // Stock pipeline bound first so a scene with no derived shaders (the overwhelming
        // common case) pays for exactly one bind, as before this pass gained variants.
        gbuffer_pipeline_->bind(cmd);
        cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), current_camera_set(), 0);
        cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);

        // Tracks which named variant ("" for stock) is bound, so a run of same-shader renderers
        // only rebinds at a transition. Opaque draw order is unconstrained, so this pays off only
        // when a scene's derived-shader objects happen to be contiguous; interleaved shaders still
        // render correctly, just with more binds.
        //
        // last_cull_backfaces must be tracked alongside last_shader: the stock ("") key resolves to
        // one of TWO pipelines (see PBRMaterial::cull_backfaces), so the shader name alone does
        // not say which is bound.
        std::string last_shader;
        bool last_cull_backfaces = true; // matches the initial pipeline_ bind above (Back)
        bool have_bound = true; // stock, bound just above

        for (size_t i = 0; i < meshes.renderers.size(); ++i) {
            if (meshes.instance_idx[i] == InstanceStream::kInvalidIndex) continue;
            auto* mr = meshes.renderers[i];
            // BLEND materials are drawn by transparent_pass_ instead, after SSR compositing --
            // never into the opaque G-buffer (see record_transparent_).
            if (mr->material.is_blended()) continue;

            if (!have_bound || mr->material.shader != last_shader ||
                mr->material.cull_backfaces != last_cull_backfaces) {
                gbuffer_pipeline_->bind(cmd, mr->material.shader, mr->material.cull_backfaces);
                last_shader         = mr->material.shader;
                last_cull_backfaces = mr->material.cull_backfaces;
                have_bound          = true;
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
            mr->get_mesh()->draw(cmd, 1, meshes.instance_idx[i]);
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

        for (size_t i = 0; i < meshes.renderers.size(); ++i) {
            if (meshes.instance_idx[i] == InstanceStream::kInvalidIndex) continue;
            if (!meshes.renderers[i]->material.is_blended()) continue;

            auto* mr = meshes.renderers[i];
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
            // Set 3: albedo/normal/metallic-roughness -- see gfx/surface/capture_fs.glsl and
            // engine::util::MaterialTextureCache.
            transparent_capture_pass_->bind_material(cmd, material_cache_->set_for(mr->material));

            mr->get_mesh()->bind(cmd);
            mr->get_mesh()->draw(cmd, 1, meshes.instance_idx[i]);
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
            glm::vec3 pos;
        };
        std::vector<Item> order;
        for (size_t i = 0; i < meshes.renderers.size(); ++i) {
            if (meshes.instance_idx[i] == InstanceStream::kInvalidIndex) continue;
            if (!meshes.renderers[i]->material.is_blended()) continue;
            order.push_back({false, i, glm::vec3(meshes.world_matrices[i][3])});
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

        // Frame-level lighting/indirect/SSR/refraction tuning lives in forward_globals_'s UBO
        // (set 6, bound below via bind_extra()), filled and uploaded once per frame in
        // render(). As a descriptor set rather than push-constant contents, it survives a bind
        // to a pipeline with an incompatible layout, so it needs no re-issue at every
        // mesh-kind transition.

        // -1 = neither yet bound, 0 = mesh, 1 = sdf -- tracks which pipeline+sets are current so
        // a run of same-kind items in `order` only rebinds once, at the transition.
        int last_kind = -1;
        // Independent of last_kind: which named variant transparent_pass_ currently has bound. A
        // mesh-kind run is not sorted by shader (back-to-front depth is the only order that matters
        // here), so two consecutive mesh items can need different pipelines even though last_kind
        // does not change. Switching pipelines mid-run is safe without rebinding sets 0-2 or the
        // extras, since every variant shares the same descriptor set layouts.
        std::string last_mesh_shader;

        for (const auto& item : order) {
            if (!item.is_sdf) {
                auto* mr = meshes.renderers[item.index];

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

                // Set 7: albedo/normal/metallic-roughness -- see gfx/surface/transparent_fs.glsl
                // and engine::util::MaterialTextureCache. Bound per-item, not just at the
                // mesh/sdf transition above: two consecutive mesh items in this back-to-front
                // list can have different textures even when neither the pipeline nor sets 0-6
                // need rebinding.
                transparent_pass_->bind_material(cmd, material_cache_->set_for(mr->material));

                mr->get_mesh()->bind(cmd);
                mr->get_mesh()->draw(cmd, 1, meshes.instance_idx[item.index]);
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
    // Forward capture of transparent geometry -- a second reflection SOURCE for opaque
    // reflectors' SSR (see record_transparent_capture_()), not a visible target. Always
    // constructed (mirrors gbuffer_target_'s own always-on policy); render() checks
    // config_.ssr_reflect_transparent per frame to decide whether to draw into/read from it.
    coopa::gfx::engine::targets::TransparentCaptureTarget transparent_capture_target_;
    coopa::gfx::engine::targets::OffscreenTarget fog_target_; // fog composite, pre-post, HDR
    coopa::gfx::engine::targets::OffscreenTarget volumetrics_target_; // wind composite, after fog, pre-post, HDR
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

    coopa::gfx::engine::targets::ShadowMapTarget shadow_target_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   shadow_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        shadow_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         shadow_set_;
    // Material set (alpha-mask sampler) shared by gbuffer_pipeline_ and shadow_pipeline_ -- see
    // MaterialTextureCache's own doc. Constructed before both in the ctor.
    std::unique_ptr<coopa::gfx::engine::util::MaterialTextureCache>           material_cache_;
    std::unique_ptr<coopa::gfx::engine::passes::ShadowPipeline>  shadow_pipeline_;

    coopa::gfx::engine::data::PaletteLut palette_lut_;

    std::unique_ptr<coopa::gfx::engine::passes::GBufferPipeline> gbuffer_pipeline_;
    /// ssao_debug_view diagnostic -- draws the same occlusion image lighting consumes.
    std::unique_ptr<passes::FullscreenBlitPass>                  ssao_debug_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SsaoPass>        ssao_pass_;       // always constructed
    std::unique_ptr<coopa::gfx::engine::passes::DeferredLightingPass> pixel_lighting_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SkyboxPass>      skybox_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::PixelStylizePass> pixel_stylize_pass_;
    // Physically-based depth of field. Runs at RENDER resolution (unlike tilt_shift_pass_,
    // which runs at display resolution), between volumetrics and bloom. Needs no resize rebuild:
    // it is sized to the startup-fixed render_extent_, and a window resize only moves the
    // letterbox rect downstream.
    std::unique_ptr<coopa::gfx::engine::passes::DofPass> dof_pass_;
    // resolve_dof_focus_()'s object-focus smoothing state (see that method's own doc).
    // <= 0 means "unseeded" -- the first object-focus frame snaps to the resolved
    // depth rather than racking up from zero.
    float smoothed_dof_focus_ = -1.0f;
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
    // history_format is UNORM, not gfxcoopa's default SRGB: post_target_/aa_target_ are
    // RGBA8_Unorm holding sRGB-ENCODED bytes (see upscale.frag's srgb_decode()), so an SRGB
    // history view would re-decode on every sample and drift the temporal blend dark.
    std::unique_ptr<coopa::gfx::engine::passes::TaaPass> taa_pass_;
    // Halton(2,3) jitter phase for "taa" mode, ADVANCED ONLY when config_.aa_mode == "taa" --
    // deliberately separate from frame_index_ below, which must keep advancing every frame
    // regardless of aa_mode (SSAO/SSR/gfx_time all depend on it). Mirrors blendy's own
    // frame_index_/ssao_frame_index_ split (see PbrRenderPipeline).
    uint32_t taa_jitter_index_ = 0;
    // Physics collider/contact-normal gizmo overlay. Always constructed;
    // config_.debug_lines_enabled gates only whether render() calls draw(). debug_lines_ is
    // filled by the caller once per frame before render() -- this pass is kept physxcoopa-free
    // by design, so Engine is what bridges PhysicsWorld::debug_draw() into that vector.
    std::unique_ptr<passes::DebugLinePass>                       debug_line_pass_;

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
