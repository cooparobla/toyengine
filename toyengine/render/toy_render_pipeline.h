/**
 * @file toy_render_pipeline.h
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
 * says so in ToyRenderConfig, and apply_live_config() refuses to change them.
 *
 * The one exception: HiZPass and SceneColorMipPass (gfxcoopa) bind their own descriptors inside
 * execute(). Both bind lazily and only once -- the passes skip the write when the source view
 * is unchanged -- so only a frame that would actually write one (the first) pays a
 * device_.wait_idle(); see trace_inputs_need_rebind_().
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
 * Point and spot shadows share one depth atlas (LocalShadowAtlas): every cast_shadows light
 * competes for a slot by screen importance, up to max_shadowed_point_lights /
 * max_shadowed_spot_lights, and a point light's six cube faces are guard-banded tiles rendered in
 * the same pass as every spot. Static casters are cached per light and copied in each frame
 * (shadow_cache_enabled). See update_local_shadows_() / record_local_shadows_().
 */

#ifndef TOYENGINE_RENDER_TOY_RENDER_PIPELINE_H
#define TOYENGINE_RENDER_TOY_RENDER_PIPELINE_H

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

#include <gfxcoopa/engine/targets/shadow_map_target.h>
#include <gfxcoopa/engine/passes/gbuffer_pipeline.h>
#include <gfxcoopa/engine/passes/shadow_pipeline.h>
#include <gfxcoopa/engine/passes/scene_color_mip_pass.h>
#include <gfxcoopa/engine/passes/ssao_pass.h>
#include <gfxcoopa/engine/passes/temporal_history_pass.h>
#include "toyengine/render/passes/contact_shadow_pass.h"
#include "toyengine/render/passes/motion_blur_pass.h"
#include <gfxcoopa/engine/data/camera_ubo.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <gfxcoopa/engine/components/directional_light.h>
#include <gfxcoopa/engine/components/point_light.h>
#include <gfxcoopa/engine/components/spot_light.h>
#include <gfxcoopa/engine/passes/fog_pass.h>
#include <gfxcoopa/engine/passes/volumetrics_pass.h>
#include <gfxcoopa/engine/passes/froxel_volumetrics_pass.h>
#include <gfxcoopa/engine/data/volumetrics_data.h>
#include <gfxcoopa/engine/components/volume.h>
#include <gfxcoopa/engine/data/sdf_data.h>
#include <gfxcoopa/engine/passes/sdf_gbuffer_pass.h>
#include <gfxcoopa/engine/passes/sdf_forward_pass.h>
#include <gfxcoopa/engine/passes/sdf_shadow_pass.h>


#include <uicoopa/ui_yaml.h>
#include <uicoopa/render/ui_world_pass.h>

#include <toyengine/render/toy_render_types.h>
#include <toyengine/render/passes/ui_composite_pass.h>
#include <toyengine/render/instance_stream.h>
#include <toyengine/render/local_shadow_atlas.h>
#include <unordered_set>
#include <toyengine/render/visibility.h>
#include <toyengine/render/gpu_profiler.h>
#include <toyengine/render/forward_globals.h>
#include <gfxcoopa/engine/util/material_texture_cache.h>
#include <gfxcoopa/engine/data/palette_lut.h>
#include <gfxcoopa/engine/data/grading_lut.h>
#include <gfxcoopa/engine/passes/exposure_pass.h>
#include <gfxcoopa/engine/passes/deferred_lighting_pass.h>
#include <gfxcoopa/engine/passes/stylize_pass.h>
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
#include <toyengine/render/passes/sky_atmosphere_pass.h>
#include <toyengine/render/passes/sky_cloud_pass.h>
#include <toyengine/render/passes/cloud_overlay_pass.h>
#include <toyengine/render/passes/cloud_shadow_pass.h>
#include <toyengine/render/passes/skinning_pass.h>
#include <toyengine/render/surface_world.h>
#include <set>
#include <glm/gtc/packing.hpp>

/**
 * @brief The ToyRenderConfig fields fixed at pipeline construction: toggles that fix a
 *        descriptor binding or a source view, and sizes baked into targets, SSBOs and the
 *        palette LUT. apply_live_config() keeps them; changing one means a rebuild (see
 *        ToyRenderPipeline::needs_rebuild() and Engine::rebuild_pipeline()). sdf_enabled
 *        and shadows_enabled are deliberately absent: both are read fresh every frame.
 */
#define TOY_STARTUP_FIXED_FIELDS(X) \
    X(volumetrics_enabled) \
    X(bloom_enabled) \
    X(dof_enabled) \
    X(tilt_shift_enabled) \
    X(ssr_enabled) \
    X(ssgi_traced) \
    X(ssao_enabled) \
    X(transparency_enabled) \
    X(refraction_enabled) \
    X(aa_mode) \
    X(skinning) \
    X(world_ui_enabled) \
    X(screen_ui_enabled) \
    X(resolution_mode) \
    X(render_width) \
    X(render_height) \
    X(fill_aspect) \
    X(scale_divisor) \
    X(ssr_half_res) \
    X(ssgi_resolution_scale) \
    X(ssao_half_res) \
    X(volumetrics_resolution_scale) \
    X(volumetrics_mode) \
    X(volumetrics_froxel_tile) \
    X(volumetrics_froxel_slices) \
    X(shadow_map_resolution) \
    X(shadow_cascades) \
    X(cube_shadow_resolution) \
    X(spot_shadow_resolution) \
    X(local_shadow_atlas_resolution) \
    X(shadow_cache_enabled) \
    X(sdf_max_renderers) \
    X(sdf_max_shapes) \
    X(palette_path) \
    X(grading_lut_path) \
    X(auto_exposure_enabled)

namespace toy {
namespace render {

/**
 * @class ToyRenderPipeline
 * @brief Renders a scene through the low-resolution deferred pixel-art frame graph and
 *        upscales it into the swapchain.
 *
 * Not copyable or movable: passes hold references to sibling members, and the ExtraSets
 * callbacks capture `this`.
 */
class ToyRenderPipeline {
public:
    ToyRenderPipeline(coopa::gfx::core::Device& device,
                        coopa::gfx::memory::Allocator& allocator,
                        coopa::gfx::core::Swapchain& swapchain,
                        coopa::gfx::pipeline::RenderPass& swapchain_pass,
                        coopa::gfx::command::CommandPool& cmd_pool,
                        ToyRenderConfig config);

    ToyRenderPipeline(const ToyRenderPipeline&) = delete;
    ToyRenderPipeline& operator=(const ToyRenderPipeline&) = delete;

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
     * @brief Lens blur off while set: depth of field and tilt shift draw sharp (their passes
     *        still run, as exact passthroughs -- blur_scale 0 and zero strength -- since the
     *        passes downstream read their outputs). Runtime, no rebuild: Engine sets it every
     *        frame from edit mode, so an editor shows them only while a scene is playing.
     */
    void set_lens_blur_suppressed(bool suppressed) { lens_blur_suppressed_ = suppressed; }
    bool lens_blur_suppressed() const { return lens_blur_suppressed_; }

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
    void set_display_region(std::optional<LetterboxRect> region);
    const std::optional<LetterboxRect>& display_region() const { return display_region_; }

    /** @brief The window-pixel rect the scene image lands in for a swapchain of sw x sh. */
    LetterboxRect display_rect_for(uint32_t sw, uint32_t sh) const;

    /**
     * @brief A second scene whose screen-space canvases draw over the main scene's (the
     *        editor UI). Null removes it. Needs screen_ui_enabled.
     */
    void set_overlay_scene(coopa::scene::Scene* scene) { overlay_scene_ = scene; }

    /**
     * @brief The engine's own overlay scenes (debug HUD, scene transition), in draw order:
     *        their screen-space canvases draw after -- over -- the overlay scene's. Needs
     *        screen_ui_enabled; an empty list draws nothing extra.
     */
    void set_overlay_layers(std::vector<coopa::scene::Scene*> layers) { overlay_layers_ = std::move(layers); }

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

    /** @brief How many frames have recorded the motion blur pass (0 while motion_blur is off). */
    uint64_t motion_blur_frames() const { return motion_blur_frames_; }

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
    /**
     * @brief This frame's surface world state -- snow, wetness, wind, the precipitation occlusion
     *        map and the snow trench field (see surface_world.h). Engine sets it every frame.
     */
    void set_surface_state(SurfaceFrameState state) { surface_state_ = std::move(state); }
    /** @brief Whether this device runs tessellation (MeshRenderer::tessellation; otherwise drawn as authored). */
    bool supports_tessellation() const { return device_.supports_tessellation(); }
    const SurfaceFrameState& surface_state() const { return surface_state_; }
    /** @brief What the surface world UBO held at the last render() (tests / diagnostics). */
    const SurfaceWorldUBO& surface_world_ubo() const { return surface_world_->last(); }

    void set_water_state(WaterFrameState state) { water_state_ = std::move(state); }

    /**
     * @brief This frame's physical sky (render sky_model: physical): the sun and moon, the
     *        atmosphere's media and the light levels the CPU atmosphere model worked out (see
     *        sky_state.h). Engine sets it every frame; an inactive state draws the gradient sky.
     */
    void set_sky_state(const SkyFrameState& state) { sky_state_ = state; }
    /** @brief This frame's cloud layer (render clouds), with either sky model (see sky_state.h). */
    void set_cloud_state(const CloudFrameState& state) { cloud_state_ = state; }
    const CloudFrameState& cloud_state() const { return cloud_state_; }
    const SkyFrameState& sky_state() const { return sky_state_; }
    /** @brief True when this frame draws the volumetric cloud layer's march (on, and not faded out). */
    bool volumetric_clouds_traced() const;
    /** @brief True when this frame casts cloud shadows. */
    bool cloud_shadows_active() const { return cloud_state_.active && cloud_state_.shadows && cloud_state_.shadow_strength > 0.0f; }
    /** @brief The cloud shadow map's pass (tests: how often it was rendered). */
    const passes::CloudShadowPass& cloud_shadow_pass() const { return *cloud_shadow_pass_; }
    /** @brief The GPU skinning pre-pass (render.skinning: gpu with compute), else null --
     *         SkinnedMeshRenderer::upload() then skins on the CPU. */
    passes::SkinningPass* skinning_pass() { return skinning_pass_.get(); }
    /** @brief True when this frame draws the physical sky. */
    bool physical_sky_active() const { return config_.sky_model == "physical" && sky_state_.active; }
    /** @brief The physical sky's LUT pass (tests: how often its tables were re-rendered). */
    const passes::SkyAtmospherePass& sky_atmosphere_pass() const { return *sky_atmosphere_pass_; }

    /**
     * @brief gfx_time for every surface-shader push: x = renderer clock, y = frame dt,
     *        z = frame index, w = the water clock (WaterFrameState::time) -- the wave phase the
     *        water shader must share with the CPU's buoyancy queries. The renderer clock runs
     *        from pipeline creation and the water clock from scene start, so they are never the
     *        same; w falls back to x when no water system reported a time.
     */
    glm::vec4 surface_gfx_time_() const;
    const WaterFrameState& water_state() const { return water_state_; }

    /**
     * @brief Hands over this frame's particle draw batches (toy::particles, via Engine). Quads
     *        draw inside the forward transparent pass, sorted with BLEND meshes and SDFs; mesh
     *        batches join the opaque G-buffer and shadow batching as instanced draws. The batch
     *        pointers must stay valid until render() returns.
     */
    void set_particle_state(ParticleFrameState state) { particle_state_ = std::move(state); }
    const ParticleFrameState& particle_state() const { return particle_state_; }
    /** @brief `simulation: gpu` particles (null without compute support). */
    passes::GpuParticlePass* gpu_particle_pass() { return gpu_particle_pass_.get(); }
    /** @brief A GPU particle system's last read-back alive count and its generation. */
    bool gpu_particle_alive(uint64_t id, uint32_t& count, uint32_t& generation) const {
        return gpu_particle_pass_ && gpu_particle_pass_->alive(id, count, generation);
    }

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
    void validate_material_shaders(coopa::scene::Scene& scene) const;

    /**
     * @brief Read-only access to the live render config.
     */
    const ToyRenderConfig& render_config() const { return config_; }

    /**
     * @brief Turns on profiling mode: CPU phase timers record into `profile`, and GPU
     *        timestamp queries time each feature's passes (see GpuProfiler). Null turns it
     *        off. Without it the frame loop records no queries and pays nothing.
     */
    void set_profiler(FrameProfile* profile);

    /**
     * @brief Per-frame mesh draw averages since startup -- MeshRenderers, how many the camera
     *        sees, and draw calls / instances / triangles for the camera passes and for all
     *        shadow views together. Empty before the second frame. Engine::run() prints it
     *        on exit, beside the frame time.
     */
    std::string mesh_draw_stats_summary() const;

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
     * doc in ToyRenderConfig): writing those changes the struct but not what renders, and can
     * leave descriptors pointing at targets nothing writes. Use apply_live_config() when setting
     * many fields at once -- it refuses those and names them.
     *
     * Mutate between frames, never mid-record.
     */
    ToyRenderConfig& render_config_mut() { return config_; }

    /**
     * @brief True if `next` differs from `live` in a field fixed at construction
     *        (TOY_STARTUP_FIXED_FIELDS), so applying it needs a new pipeline rather than
     *        apply_live_config(). fill_aspect is left out: the engine owns it and rebuilds for
     *        it itself (Engine::update_fill_extent_()).
     */
    static bool needs_rebuild(const ToyRenderConfig& live, ToyRenderConfig next);

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
     * @param warn_ignored Log the startup-fixed fields it kept (off when the caller rebuilds for them).
     */
    void apply_live_config(const ToyRenderConfig& next, bool warn_ignored = true);

    /**
     * @brief Renders one frame of the given scene.
     *
     * @param dt Seconds since the previous frame (or the FIXED_DT override). Accumulated into
     *           elapsed_time_ and pushed as gfx_time to every surface-shader backbone, so a
     *           derived shader's displacement hook (foliage sway, water waves) can animate. The
     *           stock hooks ignore it, so a scene with no derived shaders is unaffected.
     * @return True if the frame was presented, false if the window was minimized.
     */
    bool render(coopa::gfx::presentation::Renderer& renderer, coopa::scene::Scene& scene, float dt);

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
        /// Per item: the renderer's world matrix LAST frame (== world_matrices[i] when it has
        /// none), streamed as InstanceData::prev_model for the G-buffer's motion vectors.
        std::vector<glm::mat4>   prev_world_matrices;
        /// Per item: the pose its snow pattern is laid out in (data::InstanceData::snow_anchor):
        /// world_matrices[i] until the renderer first moves, then frozen (see snow_anchor_).
        std::vector<glm::mat4>   snow_anchors;
        /// Per item: its surface moved since last frame -- the world matrix changed, the mesh
        /// is dynamic (cloth, CPU skinning: re-uploaded vertices under a fixed matrix), or it
        /// is a particle batch. Drives scene_moved_, which keeps the temporal freeze off.
        std::vector<uint8_t>     moved;
        std::vector<WorldBounds> bounds;
        std::vector<uint8_t>     valid;   ///< Ready, has a transform, not LOD-culled.
        /// Per item: a particle mesh batch's world matrices (ParticleMeshBatch), null for an
        /// ordinary renderer. Such an item contributes `multi_count[i]` instances from these
        /// matrices to whichever batch it joins, instead of world_matrices[i]; its bounds are
        /// the whole particle batch's.
        std::vector<const glm::mat4*> multi;
        std::vector<uint32_t>         multi_count;
        std::vector<uint32_t>    lod;     ///< Chosen against the camera; shadow views reuse it.
        /// Per item: the surface push extension -- x/y packed tessellation params (0: drawn
        /// untessellated), z flags (bit 0: no snow cover). See GBufferPipeline::PushConstants.
        std::vector<glm::uvec4>  ext;
        /// Single-instance index for each camera-visible BLEND renderer -- the forward pass
        /// draws those one at a time, back to front, so they are never batched. Otherwise
        /// InstanceStream::kInvalidIndex.
        std::vector<uint32_t>    instance_idx;
        /// Any camera-visible BLEND renderer this frame (some instance_idx is valid) -- the
        /// only thing that samples refraction's scene-colour chain, so render() skips building
        /// it when this is false.
        bool                     has_blend_mesh = false;

        std::vector<MeshBatch>                gbuffer;   ///< Camera; opaque + mask.
        std::array<std::vector<MeshBatch>, 4> cascade;   ///< Directional shadow, per cascade.
        /// Per local-shadow VIEW (LocalShadowSlot::first_view + v): the static casters drawn into
        /// the cache atlas -- filled only for views whose light re-renders its cache this frame.
        std::vector<std::vector<MeshBatch>>   local_static;
        /// Per local-shadow view: the casters drawn live (every caster when caching is off).
        std::vector<std::vector<MeshBatch>>   local_dynamic;
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
        uint64_t tess_draws = 0;   ///< Camera draws (G-buffer + transparent) that ran tessellated.
    };

public:
    /** @brief Mesh draw statistics of the last frame (tests / diagnostics). */
    const MeshDrawStats& last_frame_stats() const { return frame_stats_; }

private:

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
    };

    /**
     * @brief Builds the per-frame-in-flight camera and light UBOs plus their descriptor sets, and the
     * shared shadow-map sampler set. Every bind happens here, once -- see the class doc.
     */
    void build_frame_descriptors_();
    /**
     * @brief Builds the material texture cache and the shadow/G-buffer pipelines, plus one named
     * pipeline variant per Opaque-domain surface shader.
     */
    void build_geometry_passes_();
    /**
     * @brief Builds the three SDF passes that need nothing beyond what the initializer list and the
     * blocks above already provide. sdf_forward_pass_ is built with the transparency passes,
     * since it shares transparent_pass_'s render pass and ssr_pass_'s trace sets.
     */
    void build_sdf_passes_();
    /**
     * @brief Builds the SSAO, SSAO debug view and deferred lighting passes. (The sky is drawn
     *        by the lighting pass itself at background pixels -- see toy_lighting.frag.)
     */
    void build_lighting_passes_();
    /**
     * @brief Builds the Hi-Z pyramid, the prefiltered scene-colour mip chain and the SSR pass that
     * consumes both. Must follow build_lighting_passes_(), whose ssao_pass_ this binds.
     */
    void build_ssr_passes_();
    /**
     * @brief Builds refraction's own scene-colour chain, and the forward
     * transparent and SDF forward passes. Must follow build_ssr_passes_(), whose trace-input
     * layouts and sets these borrow.
     */
    void build_transparency_passes_();
    /**
     * @brief Builds the post-process chain in frame-graph order -- fog, volumetrics, DOF, bloom,
     * stylize, AA, tilt shift, upscale, debug lines -- resolving each stage's source view from
     * the previous stage's startup-fixed toggle.
     *
     * @param swapchain_pass The swapchain's render pass; upscale_pass_ is built against it.
     */
    void build_post_chain_(coopa::gfx::pipeline::RenderPass& swapchain_pass);
    /**
     * @brief Builds the scene-depth descriptor set the world-space UI pass compares against. The PASS
     * itself is built by rebuild_world_ui_pass_(), since it follows the window size.
     */
    void build_world_ui_descriptor_();
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
     * @brief The sky's and the clouds' per-frame work, ahead of the lighting pass that draws
     *        them: the physical sky's atmosphere tables when the atmosphere changed and its
     *        sky-view table, then the cloud layer (record_clouds_()). Records nothing but a
     *        one-time clear of the bound images while the gradient sky is on and the clouds are
     *        off (so neither costs anything off).
     */
    void record_physical_sky_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx);

    /**
     * @brief The cloud layer's per-frame work ahead of the lighting pass (render clouds, either
     *        sky model): the volumetric march and reconstruction (skipped while the camera fade
     *        hides the layer), then the cloud shadow map from whichever look is on. The flat
     *        look's draw itself happens after the translucent pass (record_cloud_overlay_()).
     */
    void record_clouds_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx);

    /** @brief render sky_quality as the sky-view table's integration steps (live). */
    int sky_quality_steps_() const;

    /** @brief render cloud_quality as step counts and the cloud march's resolution (live):
     *         cloud_scale is the fraction (per axis) of the half-resolution cloud target marched. */
    struct CloudQualitySteps { int cloud_steps, cloud_light_steps; float cloud_scale; };
    CloudQualitySteps cloud_quality_steps_() const;

    /**
     * @brief The directional light's colour as every pass sees it: the component's, times the
     *        physical sky's tint (the air's transmittance toward it and the cloud cover's
     *        dimming) while that sky is on.
     */
    glm::vec3 dir_light_color_(const coopa::gfx::engine::components::DirectionalLightComponent& l) const {
        return physical_sky_active() ? l.color * sky_state_.light_tint : l.color;
    }

    /**
     * @brief Records the scene: shadow maps, G-buffer, Hi-Z, SSAO,
     *        deferred lighting and sky, SSR, and the forward transparent pass.
     */
    void record_scene_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx,
                       const MeshGather& meshes, const std::vector<SdfDrawItem>& sdf_draws);

    /**
     * @brief The cloud layer over the finished HDR scene's geometry (after the translucent pass,
     *        so clouds below a high camera cover water too): the flat look, or the volumetric
     *        layer's depth-tested composite. Reopens the live HDR image through
     *        transparent_pass_'s render pass like record_fog_() does; depth arrives and leaves
     *        SHADER_READ_ONLY_OPTIMAL.
     */
    void record_cloud_overlay_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx);

    /**
     * @brief Records the post-process chain: fog, volumetrics, DOF, bloom, the stylize pass
     *        (with the debug-line overlay as its guest), the world-UI layer, AA and tilt shift.
     *
     * Every stage is gated on a startup-fixed toggle, because each one's source image was
     * chosen from that toggle when the pipeline was built.
     */
    void record_post_chain_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx);

    /**
     * @brief Records the window-sized overlay: the letterboxed composite of scene + world UI,
     *        then the screen-space UI as a guest in the same bracket at full window resolution.
     */
    void record_overlay_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx);

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
            glm::uvec4 ext;         // tessellation params + surface flags (MeshGather::ext)
        } camera{};
        /// The subset the shadow passes push -- so casters that differ only in colour still
        /// share one instanced shadow draw.
        struct Shadow {
            glm::vec4 params;
            float     alpha_cutoff;
            uint32_t  tess_a, tess_b;   // a tessellated caster batches only with identical settings
            float     pad;
        } shadow{};
    };

    /**
     * @brief Resolves every MeshRenderer's transform, bounds and LOD, then frustum-culls and
     *        batches it into each view's draw list, uploading all transforms in batch order.
     *
     * Must run after update_dir_shadow_matrix_()/update_local_shadows_(): the cascade and
     * local-light view frusta come from the matrices those write.
     *
     * Per view: frustum test on the world AABB; for shadow views also the small-caster test
     * (config shadow_min_caster_texels); then a sort on MeshBatchKey so identical
     * mesh + LOD + material runs become one instanced draw, with their transforms appended
     * contiguously. A transform is therefore uploaded once per view that sees it.
     */
    MeshGather gather_meshes_(uint32_t frame_slot, const glm::mat4& view, const glm::mat4& unjittered_proj,
                              bool cast_dir_shadow);

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
    void gather_ui_canvases_(coopa::scene::Scene& scene, uint32_t frame_slot, const glm::mat4& view);

    /**
     * @brief Gathers every visible SdfRenderer into the draw list each recording site shares,
     *        uploading this frame's shape/renderer SSBO records and globals.
     *
     * The single gate for the whole SDF system: when sdf_enabled is off the list comes back
     * empty and every downstream site is naturally a no-op.
     */
    std::vector<SdfDrawItem> gather_sdf_(coopa::scene::Scene& scene, const glm::mat4& view,
                                         const glm::mat4& proj, const glm::vec3& cam_pos,
                                         uint32_t frame_slot);

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
    LetterboxRect handle_resize_();

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
    void apply_taa_jitter_(glm::mat4& proj);

    /** @brief Fills UnderwaterPass's push constants from water_state_ (see set_water_state()). */
    void update_underwater_params_(const glm::mat4& unjittered_proj, const glm::mat4& view, const glm::vec3& cam_pos,
                                   const coopa::gfx::engine::components::DirectionalLightComponent* dir_light);

    /**
     * @brief Writes the global fog parameters into this frame's LightUBO (its fog block).
     *
     * One source for every fog consumer: FogPass (opaque scene and sky) and each forward shader
     * (BLEND meshes, water, particles, SDF glass), which all read `lights.fog` -- see
     * gfx/fog.glsl. Per frame-in-flight like the rest of the light UBO. Fog is GLOBAL ONLY and
     * comes from config_ (which the weather system drives); bounded fog is a VolumeComponent.
     *
     * The water fields come from the WaterSystem's frame state: with the camera under a water
     * surface, fog integrates only the part of each ray above it, so the in-water part is left
     * to UnderwaterPass instead of being washed over with air fog.
     */
    void fill_fog_block_(coopa::gfx::engine::data::LightUBO& ubo) const;

    /**
     * @brief Global fog over the opaque scene and sky, composited in place (FogPass).
     *
     * Runs after lighting/SSR and BEFORE refraction's scene-colour chain and the translucent
     * draws -- the Unreal/HDRP order: refraction and alpha blending then see an already-fogged
     * background, and each translucent fragment fogs only its own radiance at its own distance.
     * Reopens the live HDR image through transparent_pass_'s render pass (LOAD on colour and
     * depth); FogPass blends premultiplied, so nothing samples the image being written.
     */
    void record_fog_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx);

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
                                  const coopa::gfx::engine::components::DirectionalLightComponent* dir_light);

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

    /** @brief The frame-in-flight slot render() most recently selected (camera_frame_). */
    uint32_t current_frame_slot_() const { return camera_frame_; }

    /**
     * @brief A draw item's surface push extension (MeshGather::ext): its renderer's
     *        tessellation packed into x/y when it is on, the device has tessellation and the
     *        material's shader has a tessellated pipeline in its pass -- else 0 (drawn as
     *        authored; each missing case warns once) -- and the snow opt-out in z.
     */
    glm::uvec4 surface_ext_(const coopa::gfx::engine::components::MeshRenderer& mr,
                            const coopa::gfx::engine::components::PBRMaterial& m);

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
     * its centre -- the kitchen_sink test scene's pillar spans [0,1] in plan, so its origin is a CORNER, 0.2 m in
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
                                const glm::mat4& view, coopa::scene::Scene& scene, float dt);

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
    void rebuild_overlay_chain_(uint32_t w, uint32_t h);

    /**
     * @brief (Re)builds the display-rect-sized world-UI layer and its pass. Reads
     *        upscaled_extent_; the caller has waited for the device to idle.
     */
    void rebuild_display_layer_();

    /** @brief Points ui_composite_pass_ at the current scene image and world-UI layer. */
    void bind_composite_inputs_();

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
    void rebuild_world_ui_pass_();

    /**
     * @brief Populates this frame's slot's LightUBO colour/intensity/count fields.
     *
     * Not the shadow matrix or the cast_shadows flags -- update_dir_shadow_matrix_() and
     * render()'s point-light handling write those into the same slot afterwards, and all three
     * share the single upload() at the end of render(). Must run after light_frame_ is set to
     * this frame's slot.
     */
    void update_lights_(coopa::scene::Scene& scene);

    /**
     * @brief Writes this frame's directional cascade matrices and shadow parameters into the
     *        current slot's LightUBO.
     *
     * One ortho fit per cascade, each to its own slice of the camera's depth range
     * (toy_render_math.h's compute_cascade_splits() + compute_dir_shadow_fit_slice()); this adds
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
                                   bool cast_dir_shadow, float focus_distance);

    /**
     * @brief True when this frame's record_scene_() would write a descriptor set: a Hi-Z /
     *        mip-chain pass whose sets don't yet hold the view it is about to be given. The
     *        views mirror the execute() calls in record_scene_() exactly; all are fixed for
     *        the pipeline's lifetime, so this is true on the first frame and then stays false.
     */
    bool trace_inputs_need_rebind_() const;

    /** @brief Whether this frame records motion blur: the runtime toggle, a non-zero shutter and
     *         radius, a camera that has not opted out, and the normal image (or "lines" over it). */
    bool motion_blur_active_(const FrameContext& ctx, DebugView view) const;

    /**
     * @brief One axis of volumetrics_march_target_: render_extent_ divided by
     *        volumetrics_resolution_scale (clamped to 1..4), rounded up so the march
     *        covers every edge pixel.
     */
    /// volumetrics_mode == "froxel" (anything else is the raymarch).
    bool froxel_volumetrics_() const { return config_.volumetrics_mode == "froxel"; }

    uint32_t volumetrics_march_dim_(uint32_t full) const;

    /**
     * @brief Every component this frame's render() reads, gathered by ONE hierarchy walk.
     *
     * Scene::get_components<T>() / find_first_component<T>() each walk the whole tree with
     * a dynamic_cast per component, and render() needs about a dozen such lists per frame
     * (point and spot lights twice each). snapshot_scene_() makes one walk, with exactly
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
    void snapshot_scene_(coopa::scene::Scene& scene);

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
    /// Last frame's world matrix per renderer drawn last frame (same keying and per-frame
    /// rebuild as lod_state_): streamed as data::InstanceData::prev_model so the G-buffer
    /// writes per-object motion vectors. A renderer absent here (new, or not drawn last
    /// frame) streams prev == current, i.e. zero object motion.
    std::unordered_map<const coopa::gfx::engine::components::MeshRenderer*, glm::mat4> prev_world_;
    /// Frozen snow anchors (data::InstanceData::snow_anchor): one per renderer whose transform
    /// has moved since it appeared -- its pose from just before it first moved, kept while the
    /// renderer exists, so its lying-snow pattern travels with it rather than sliding under a
    /// world-fixed one (which TAA smears into trails). A renderer absent here has never moved and
    /// anchors at its current pose, keeping the world's snow one continuous pattern.
    std::unordered_map<const coopa::gfx::engine::components::MeshRenderer*, glm::mat4> snow_anchor_;

    MeshDrawStats frame_stats_;     ///< This frame's mesh draw counts (see mesh_draw_stats_summary()).
    MeshDrawStats stats_total_;     ///< Summed over stats_frames_ frames.
    uint64_t      stats_frames_ = 0;

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
        bool last_tess = false;
        const void* last_set = nullptr;
        const coopa::gfx::engine::data::Mesh* last_mesh = nullptr;
        for (const MeshBatch& b : batches) {
            const auto* mr = meshes.renderers[b.item];
            const auto& m  = meshes.material(b.item);
            const bool tess = meshes.ext[b.item].x != 0u;
            if (!last_shader || m.shader != *last_shader || m.cull_backfaces != last_cull || tess != last_tess) {
                bind(m.shader, m.cull_backfaces, tess);
                last_shader = &m.shader;
                last_cull   = m.cull_backfaces;
                last_tess   = tess;
                last_set    = nullptr;   // a variant owns its own pipeline layout
                // Set 1: the surface world (a displacement hook / the tessellation view reads it).
                cmd.bind_descriptor_set(surface_world_->set(current_frame_slot_()), 1);
            }
            pc.tess_a = meshes.ext[b.item].x;
            pc.tess_b = meshes.ext[b.item].y;
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

    void record_directional_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                                    const MeshGather& meshes,
                                    const std::vector<SdfDrawItem>& sdf_draws,
                                    bool cast_dir_shadow);

    /**
     * @brief Chooses this frame's shadowed point and spot lights, packs them into the local
     *        shadow atlas, and writes their records (LightUBO::local_shadows) plus each light's
     *        1-based slot (PointLightGPU::attenuation.w / SpotLightGPU::params.z).
     *
     * Any number of lights may set cast_shadows; the ones that get a shadow are those with the
     * highest screen importance -- the influence sphere's size over its distance from the camera,
     * times the square root of brightness, culled to the camera frustum, with a 25% bonus for
     * last frame's choices so the selection does not flicker between near-equal lights (Unreal
     * ranks shadowed local lights the same way, by screen-space size). Up to
     * max_shadowed_point_lights / max_shadowed_spot_lights of each win; those that do not fit
     * the atlas are left unshadowed, least important first.
     *
     * Packing order is by scene index, not importance, so a light keeps its tile -- and its
     * static-cache content -- while the set of shadowed lights is unchanged.
     *
     * Each point light gets six views with a guard-banded field of view: tan(half fov) =
     * res / (res - 2 * guard), which puts a cube edge `guard` texels inside its tile so a PCF
     * kernel of up to `guard - 1.5` texels never crosses into the next face.
     */
    void update_local_shadows_(const glm::vec3& cam_pos, const glm::mat4& camera_vp);

    /**
     * @brief Records every point/spot shadow into the local-light atlas.
     *
     * With the static cache on (LocalShadowAtlas::caching()): lights whose static casters,
     * light pose or tile changed re-render those casters into the cache atlas; every light's tiles
     * are then copied cache -> live in one transfer; and only the casters that moved recently
     * (plus SDF casters) draw live on top. With it off, every caster draws live into a cleared
     * tile. Either way it is ONE live render pass for every view of every light -- point faces
     * included -- at hardware depth, so no fragment writes gl_FragDepth and early-Z holds.
     */
    void record_local_shadows_(coopa::gfx::command::CommandBuffer& cmd,
                               const MeshGather& meshes,
                               const std::vector<SdfDrawItem>& sdf_draws);

    /// Transitions the G-buffer depth image to SHADER_READ_ONLY_OPTIMAL for
    /// stylize_pass_'s pre-bound outline-detection descriptor, which cannot simply be
    /// rebound (file doc, rule 1).
    ///
    /// `from_layout` is the image's actual current layout: DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    /// right after record_gbuffer_() wrote it, or DEPTH_STENCIL_READ_ONLY_OPTIMAL after
    /// record_transparent_()'s own render pass read it.
    void transition_gbuffer_depth_to_shader_read_(
        coopa::gfx::command::CommandBuffer& cmd,
        VkImageLayout from_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    /**
     * @brief Copies a few G2 (world position) texels around the screen centre into this frame
     *        slot's readback buffer -- what the camera is looking at, for the "focus" shadow fit
     *        when the scene names no focus. read_focus_probe_() reads it back once the slot's
     *        fence has passed, a frame or two later, so nothing ever waits on it.
     */
    void record_focus_probe_(coopa::gfx::command::CommandBuffer& cmd);

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
    void read_focus_probe_(uint32_t slot, const glm::mat4& view, float dt);

    void record_gbuffer_(coopa::gfx::command::CommandBuffer& cmd,
                         const MeshGather& meshes,
                         const std::vector<SdfDrawItem>& sdf_draws);

    /**
     * @brief The editor shading modes' BLEND meshes, back-to-front, over the debug-view image
     *        (inside post_target_'s open bracket). BLEND SDFs are not drawn here.
     */
    void record_transparent_preview_(coopa::gfx::command::CommandBuffer& cmd, const MeshGather& meshes,
                                     DebugView view, const glm::vec3& camera_pos);

    /// Fills the forward MESH pass's per-frame globals (lighting/indirect/SSR/refraction)
    /// from config_ -- the exact same fields sdf_data_'s own globals() fill (in render()'s
    /// SDF gather block) sources, kept in sync by hand since the two are independent UBOs
    /// (see ForwardGlobals's own doc for why the duplication is accepted, not removed).
    void fill_forward_globals_(ForwardGlobals& g) const;

    /// One particle batch's push block: its look, plus the frame's clock, depth range and ambient.
    passes::ParticlePass::PushConstants particle_push_constants_(const ParticleDrawBatch& b) const;

    /**
     * @brief The TAA reactive mask for this frame: cleared, then every batch with `reactive` > 0
     *        drawn again into it as coverage (ParticlePass's reactive pipeline; the fragment
     *        depth-tests itself against the Hi-Z copy). Recorded right before the resolve that
     *        reads it, every TAA frame, so it is never stale.
     */
    void record_reactive_mask_(coopa::gfx::command::CommandBuffer& cmd);

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
                             const glm::vec3& camera_pos);

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::gfx::core::Swapchain&   swapchain_;
    /// Retained because rebuild_overlay_chain_() constructs a UiPass outside the ctor, on
    /// every swapchain resize -- see overlay_target_'s own doc.
    coopa::gfx::command::CommandPool& cmd_pool_;
    ToyRenderConfig              config_;
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
    /// See set_overlay_layers(); non-owning, in draw order.
    std::vector<coopa::scene::Scene*> overlay_layers_;

    coopa::gfx::engine::targets::GBufferTarget   gbuffer_target_;
    coopa::gfx::engine::targets::OffscreenTarget offscreen_target_; // lit + sky, pre-post, HDR
    coopa::gfx::engine::targets::OffscreenTarget post_target_;      // final low-res LDR, post StylizePass
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
    coopa::gfx::engine::targets::OffscreenTarget underwater_target_; // UnderwaterPass output, HDR
    coopa::gfx::engine::targets::OffscreenTarget volumetrics_target_; // wind composite, after fog, pre-post, HDR
    coopa::gfx::engine::targets::OffscreenTarget volumetrics_march_target_; // reduced-res march: in-scatter + transmittance
    bool refraction_this_frame_ = false; // refraction's scene-colour chain is built + sampled this frame
    /// The final pre-DOF HDR image (see build_post_chain_()), copied each frame into the SSR
    /// scene-colour chain's mip 0 as the next frame's reflection colour.
    VkImage scene_color_history_src_ = VK_NULL_HANDLE;
    /// A previous frame's copy is in scene_color_mip_pass_'s mip 0.
    bool scene_color_history_valid_ = false;
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
    /// The surface world set (surface_world.h) and what the engine handed in for it this frame.
    std::unique_ptr<SurfaceWorldData> surface_world_;
    SurfaceFrameState surface_state_;
    bool tess_warned_unsupported_ = false;
    std::set<std::string> tess_warned_shaders_;

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

    // Global fog over the opaque scene, drawn in place inside transparent_pass_'s render pass
    // before translucency (see record_fog_()); forward shaders fog themselves. Its parameters
    // ride in the light UBO (fill_fog_block_()).
    std::unique_ptr<coopa::gfx::engine::passes::FogPass> fog_pass_;
    std::unique_ptr<passes::UnderwaterPass> underwater_pass_;   // null when !underwater_enabled
    passes::UnderwaterPass::Params underwater_params_;           // filled in render()
    WaterFrameState water_state_;                                // set_water_state(), per frame
    SkyFrameState sky_state_;                                    // set_sky_state(), per frame
    ParticleFrameState particle_state_;                          // set_particle_state(), per frame
    glm::vec4 particle_depth_{0.1f, 1000.0f, 1.0f, 0.0f};         ///< near, far, is_perspective -- render()
    bool warned_particles_need_transparency_ = false;
    /// Quads drawn inside transparent_pass_'s bracket (see record_transparent_()). Built with it.
    std::unique_ptr<passes::ParticlePass> particle_pass_;
    std::unique_ptr<passes::GpuParticlePass> gpu_particle_pass_;   ///< `simulation: gpu` systems; null without compute.
    /// The white-texture material set untextured particle batches bind at set 3.
    coopa::gfx::engine::components::PBRMaterial particle_default_material_;
    // The lit scene with fog and translucency composited in place (and the underwater look,
    // when built) -- the first image the post chain reads. Chosen once from the startup toggles,
    // same policy as stylize_pass_'s own binding. A member so later constructions in the
    // ctor body can fall back to it when volumetrics is disabled.
    coopa::gfx::TextureView pre_volumetrics_view_typed_;

    // Local volumes -- the raymarched/froxel, bounded counterpart to the global fog's analytic
    // everywhere-medium (see gfxcoopa's VolumetricsPass / gfx/volumetrics.glsl). Constructed
    // when built, runtime-gated on config_.volumetrics_enabled.
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
    std::unique_ptr<passes::CloudShadowPass>                     cloud_shadow_pass_;   ///< shadow_set_'s binding 4 (render cloud_shadows)
    // Material set (alpha-mask sampler) shared by gbuffer_pipeline_ and shadow_pipeline_ -- see
    // MaterialTextureCache's own doc. Constructed before both in the ctor.
    std::unique_ptr<coopa::gfx::engine::util::MaterialTextureCache>           material_cache_;
    std::unique_ptr<coopa::gfx::engine::passes::ShadowPipeline>  shadow_pipeline_;

    coopa::gfx::engine::data::PaletteLut palette_lut_;
    // Colour-grading strip LUT, applied by stylize_pass_ after the tonemap. Always a
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
        bool operator==(const ContactShadowKnobs& o) const;
    };
    ContactShadowKnobs                                           contact_shadow_knobs_{};
    /// contact_shadow_pass_'s output already holds the disabled-state clear (see record_scene_).
    bool                                                         contact_output_cleared_ = false;
    /// The physical sky (render sky_model: physical): its LUTs and the cloud layer. Declared
    /// before the extra set below, which binds their images.
    std::unique_ptr<passes::SkyAtmospherePass>                   sky_atmosphere_pass_;
    std::unique_ptr<passes::SkinningPass>                        skinning_pass_;   ///< null: CPU skinning
    std::unique_ptr<passes::SkyCloudPass>                        sky_cloud_pass_;
    std::unique_ptr<passes::CloudOverlayPass>                    cloud_overlay_pass_;   ///< the cloud layer over geometry
    CloudFrameState                                              cloud_state_;          // set_cloud_state(), per frame
    passes::CloudShadowPass::Placement                           cloud_shadow_place_;   ///< where this frame's cloud shadow map lies
    glm::dvec2 cloud_prev_offset_{0.0};     ///< Last frame's cloud wind offset (the drift the reconstruction follows).
    glm::vec4 cloud_prev_slab_{0.0f};       ///< Last frame's layer parameters (an edit lifts the anti-flicker cap too).
    glm::vec2 cloud_prev_light_{0.0f};      ///< Last frame's cloud light / zenith luminance (SkyCloudPass::Params::light_change).
    float     cloud_prev_fade_ = 1.0f;      ///< Last frame's camera fade (a change lifts the anti-flicker cap too).
    uint32_t  cloud_last_frame_ = ~0u - 1;   ///< frame_index_ the clouds last ran (history valid only if it was the previous one).
    /// toy_lighting_pass_'s extra set (set 4): contact_shadow_pass_'s resolved occlusion.
    /// Declared BEFORE toy_lighting_pass_ so it outlives the pass holding it in its ExtraSets.
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   contact_extra_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        contact_extra_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         contact_extra_set_;
    std::unique_ptr<coopa::gfx::engine::passes::DeferredLightingPass> toy_lighting_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::StylizePass> stylize_pass_;
    // debug_view diagnostic (see build_post_chain_()'s own doc on this instance) -- the
    // one extra descriptor set (SSR reflection / traced-SSGI / G-buffer depth) debug_view.frag
    // needs on top of the camera/light/shadow/G-buffer+SSAO sets it shares with
    // toy_lighting_pass_ above.
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSetLayout>   debug_view_extra_layout_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorPool>        debug_view_extra_pool_;
    std::unique_ptr<coopa::gfx::pipeline::DescriptorSet>         debug_view_extra_set_;
    std::unique_ptr<coopa::gfx::engine::passes::DeferredLightingPass> debug_view_pass_;
    // Physically-based depth of field. Runs at RENDER resolution (unlike tilt_shift_pass_,
    // which runs at display resolution), between volumetrics and bloom. Needs no resize rebuild:
    // it is sized to the startup-fixed render_extent_, and a window resize only moves the
    // letterbox rect downstream.
    std::unique_ptr<coopa::gfx::engine::passes::DofPass> dof_pass_;
    std::unique_ptr<passes::MotionBlurPass> motion_blur_pass_;
    static constexpr float kMotionBlurMaxRadius1080 = 32.0f;   ///< Blur half-length ceiling, px at 1080p (scaled).
    static constexpr int   kMotionBlurSamples       = 16;      ///< Gather samples per pixel.
    uint64_t motion_blur_frames_ = 0;   ///< Frames motion_blur_pass_ was recorded in (motion_blur_frames()).
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
    /**
     * UI passes (and the targets they were built against) replaced by a resize, kept alive
     * for the one frame that still draws with them: canvases emitted their DrawLists -- and
     * captured the old pass's white_view() -- before render() ran handle_resize_(). Freeing
     * them there would draw that frame's UI from a destroyed texture (a black frame). Freed
     * at the next handle_resize_(). Targets first, so the passes are destroyed before them.
     */
    struct RetiredUi {
        std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> world_target;
        std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> overlay_target;
        std::unique_ptr<coopa::ui::UiWorldPass>                      world_pass;
        std::unique_ptr<coopa::ui::UiPass>                           screen_pass;
        bool empty() const { return !world_target && !overlay_target && !world_pass && !screen_pass; }
    } retired_ui_;
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
     * aa_target_ sits between post_target_ (stylize_pass_'s output) and everything that
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
    /// TAA reactive mask (R8, render extent): see record_reactive_mask_(). Null unless aa_mode is taa.
    std::unique_ptr<coopa::gfx::engine::targets::OffscreenTarget> reactive_target_;
public:
    /** @brief True when TAA runs with the particles' reactive mask (aa_mode taa). */
    bool has_reactive_mask() const { return reactive_target_ != nullptr && particle_pass_ && particle_pass_->has_reactive(); }
private:
    // Halton(2,3) jitter phase for "taa" mode, ADVANCED ONLY when config_.aa_mode == "taa" --
    // deliberately separate from frame_index_ below, which must keep advancing every frame
    // regardless of aa_mode (SSAO/SSR/gfx_time all depend on it). Mirrors blendy's own
    // frame_index_/ssao_frame_index_ split (see PbrRenderPipeline).
    uint32_t taa_jitter_index_ = 0;
    // This frame's jitter as the NDC displacement it applies to the projection, written by
    // apply_taa_jitter_() each frame (zero when aa_mode != "taa"). The TAA resolve subtracts
    // it so its velocity is measured between UNjittered positions.
    glm::vec2 taa_jitter_ndc_ = glm::vec2(0.0f);
    // SSAO sample-pattern phase, advanced every frame the temporal resolve is accumulating and
    // held while it is frozen -- see record_scene_()'s stillness block. Separate from
    // frame_index_ for the same reason taa_jitter_index_ is: that counter must keep advancing
    // every frame because SSR and gfx_time depend on it, while this one must be able to stand
    // still. Holding it is what keeps a frozen image byte-static: ssao_resolve.frag then
    // copies accepted history verbatim, and the raw pass under it redraws the same pattern.
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
    /** Any drawn surface moved this frame (world matrix changed, dynamic mesh, particle
     *  batch) -- set by gather_meshes_(), read by the stillness block. A frame in which the
     *  scene moved resets camera_frames_still_ exactly as camera motion does. */
    bool      scene_moved_         = false;
    /** Still frames before freezing. Matches the AO resolve's accumulation cap (8 frames at
     *  the High tier) so the held average has a full window of draws behind it; still inside
     *  image_settles_after_camera_stops' eight-frame budget, since the pre-freeze frames only
     *  add 1/(count+1)-scale residuals that its threshold tolerates. */
    static constexpr uint32_t kTemporalFreezeAfter = 8;
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
    // Previous frame's view matrix exactly as the camera UBO uploaded it (pixel-snapped when
    // snapping is on) and its UNjittered projection, handed to CameraUBO::set_reprojection()
    // so the G-buffer's velocity attachment projects last frame's pose the way last frame did.
    // Kept separate from the premultiplied prev_*_view_proj_ above: the velocity shader needs
    // the view alone for a linear depth that is correct under orthographic cameras too.
    glm::mat4 prev_snapped_view_         = glm::mat4(1.0f);
    glm::mat4 prev_unjittered_proj_      = glm::mat4(1.0f);
    // Monotonic per-rendered-frame counter, incremented once at the end of render(). Shared
    // by SSAO's noise-tile rotation and SSR's stochastic ray jitter (see each pass's own
    // Params::*frame_index* doc) -- both are per-pixel noise sources that need decorrelating
    // frame to frame, and there is no reason for the two to disagree about which frame it is.
    uint32_t  frame_index_          = 0;

    InstanceStream instance_stream_;

    // --- Point/spot shadows: the local-light shadow atlas -------------------------------------
    /** One shadowed point or spot light this frame (update_local_shadows_()). */
    struct LocalShadowSlot {
        uint32_t    kind = 0;            ///< 1 = spot, 2 = point (LocalShadowGPU::tile.w).
        const void* light = nullptr;     ///< The component -- the static-cache key.
        uint32_t    gpu_index = 0;       ///< Index into LightUBO::point_lights / spot_lights.
        glm::vec3   pos{0.0f};
        float       range = 0.0f;
        uint32_t    first_view = 0;      ///< Into LocalShadowBlock::view_proj / MeshGather::local_*.
        uint32_t    view_count = 0;      ///< 6 for a point, 1 for a spot.
        AtlasRect   block;               ///< The light's whole region of the atlas.
        uint32_t    tile_res = 0;        ///< One view's tile edge.
        bool        rerender_static = true; ///< Its static-cache tiles must be redrawn this frame.
        /// Atlas rectangle of view `v` (0..view_count-1): face f at column f%3, row f/3.
        AtlasRect view_rect(uint32_t v) const {
            return {block.x + (v % 3u) * tile_res, block.y + (v / 3u) * tile_res, tile_res, tile_res};
        }
    };
    std::unique_ptr<LocalShadowAtlas> local_shadow_atlas_;
    std::vector<LocalShadowSlot>      local_slots_;
    /** What each light's static-cache tiles hold: the static casters' hash and the block. */
    struct LocalCacheEntry { uint64_t hash = 0; AtlasRect block; };
    std::unordered_map<const void*, LocalCacheEntry> local_cache_;
    /** Lights that held a shadow last frame -- a small score bonus keeps the choice stable. */
    std::unordered_set<const void*> local_selected_prev_;
    /** Consecutive frames each renderer has not moved; static-cache eligibility. */
    std::unordered_map<const coopa::gfx::engine::components::MeshRenderer*, uint32_t> still_frames_;
    /** Surface shaders with their own shadow vertex stage (possibly animated). */
    std::unordered_set<std::string> animated_shadow_shaders_;
    /** Still frames before a caster's depth may be cached as static. */
    static constexpr uint32_t kStaticCasterFrames = 8;
    bool warned_local_atlas_full_ = false;
    bool           warned_perspective_snap_ = false;

    // sdf_data_ must be constructed before the four passes below, which bind its layout;
    // declaration order here matches the initializer list.
    coopa::gfx::engine::data::SdfData sdf_data_;
    std::unique_ptr<coopa::gfx::engine::passes::SdfGBufferPass> sdf_gbuffer_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SdfForwardPass> sdf_forward_pass_;
    std::unique_ptr<coopa::gfx::engine::passes::SdfShadowPass>  sdf_shadow_pass_;

    // --- Job dispatch for the per-frame gathers -- see should_parallelize_()'s doc ---
    coopa::job::JobEngine* jobs_ = nullptr;      // non-owning; nullptr = always serial
    std::size_t            parallel_threshold_ = 256;
    bool                   lens_blur_suppressed_ = false;   ///< see set_lens_blur_suppressed()
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_TOY_RENDER_PIPELINE_H
