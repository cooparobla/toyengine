/**
 * @file toy_render_pipeline.cpp
 * @brief ToyRenderPipeline's construction and the per-frame spine, render().
 *
 * The class is split across sibling translation units, all against this one header:
 *
 * | File | Holds |
 * |---|---|
 * | toy_render_pipeline.cpp | construction, `render()` (the frame's spine), live config, accessors |
 * | pipeline_build.cpp      | building every pass/target/descriptor, and the resize rebuilds |
 * | pipeline_gather.cpp     | the per-frame CPU gather: meshes, UI canvases, SDF items |
 * | pipeline_shadows.cpp    | lights, directional cascades, the local shadow atlas |
 * | pipeline_sky_fog.cpp    | sky, clouds, fog, volumetrics |
 * | pipeline_record.cpp     | G-buffer, transparency, post chain, overlay recording |
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

ToyRenderPipeline::ToyRenderPipeline(coopa::gfx::core::Device& device,
                    coopa::gfx::memory::Allocator& allocator,
                    coopa::gfx::core::Swapchain& swapchain,
                    coopa::gfx::pipeline::RenderPass& swapchain_pass,
                    coopa::gfx::command::CommandPool& cmd_pool,
                    ToyRenderConfig config)
    : device_(device), allocator_(allocator), swapchain_(swapchain), cmd_pool_(cmd_pool),
      config_(std::move(config)),
      render_extent_(compute_render_extent(config_, swapchain.extent().width, swapchain.extent().height)),
      upscaled_extent_(compute_display_rect(config_, swapchain.extent().width, swapchain.extent().height,
                                            render_extent_.width, render_extent_.height)),
      gbuffer_target_(device, allocator, render_extent_.width, render_extent_.height),
      // HDR always -- the sky-based indirect lighting (see toy_lighting.frag) can exceed
      // 1.0 regardless of whether SSR/SSAO are toggled, and stylize.frag's tonemap
      // step (gated on PushConstants::exposure) always applies before dither/palette to
      // bring it back down.
      // Colour-only: the lighting draw is one depth-less fullscreen triangle, and nothing
      // ever reads this target's depth (record_transparent_() attaches gbuffer_target_'s
      // depth to its own render pass; stylize/debug sample gbuffer depth too). A D32
      // attachment here would be cleared and stored for nothing every frame.
      offscreen_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA16_Sfloat,
                        coopa::gfx::engine::targets::kColorOnly),
      post_target_(device, allocator, render_extent_.width, render_extent_.height, coopa::gfx::Format::RGBA8_Unorm),
      // Underwater composite target -- HDR like offscreen_target_, and separate because
      // pipeline::RenderPass hardcodes LOAD_OP_CLEAR (see UnderwaterPass's file doc).
      underwater_target_(device, allocator, render_extent_.width, render_extent_.height,
                         coopa::gfx::Format::RGBA16_Sfloat, coopa::gfx::engine::targets::kColorOnly),
      // Volumetrics composite target -- same HDR format and the same LOAD_OP_CLEAR-forced
      // separation as underwater_target_ above.
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
      // *Shadow-family doc (toy_lighting.frag/transparent.frag's dir_shadow_map and
      // point_shadow_map are sampler2DShadow/samplerCubeShadow to match).
      shadow_sampler_(coopa::gfx::engine::util::Sampler::shadow(device)),
      volumetrics_data_(device, allocator),
      // Only the directional cascade atlas is used: point and spot shadows live in
      // local_shadow_atlas_, so the target's cube and spot maps are allocated at a token size.
      shadow_target_(device, allocator, config_.shadow_map_resolution, 16u, 16u,
                    config_.shadow_cascades),
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

void ToyRenderPipeline::set_display_region(std::optional<LetterboxRect> region) {
    if (region && (region->w == 0 || region->h == 0)) region.reset();
    display_region_ = region;
}

LetterboxRect ToyRenderPipeline::display_rect_for(uint32_t sw, uint32_t sh) const {
    if (display_region_) {
        LetterboxRect box = compute_display_rect(config_, display_region_->w, display_region_->h,
                                                 render_extent_.width, render_extent_.height);
        box.x += display_region_->x;
        box.y += display_region_->y;
        return box;
    }
    return compute_display_rect(config_, sw, sh, render_extent_.width, render_extent_.height);
}

bool ToyRenderPipeline::volumetric_clouds_traced() const { return cloud_state_.active && cloud_state_.type == CloudType::Volumetric && cloud_state_.fade > 0.0f; }

void ToyRenderPipeline::validate_material_shaders(coopa::scene::Scene& scene) const {
    for (auto* mr : scene.get_components<coopa::gfx::engine::components::MeshRenderer>()) {
        config_.surface_shaders.require(mr->material.shader);
        for (const auto& sm : mr->slot_materials) config_.surface_shaders.require(sm.shader);
    }
    for (auto* sr : scene.get_components<coopa::gfx::engine::components::SdfRenderer>()) {
        config_.surface_shaders.require(sr->material.shader);
    }
}

void ToyRenderPipeline::set_profiler(FrameProfile* profile) {
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

std::string ToyRenderPipeline::mesh_draw_stats_summary() const {
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
    std::string out(buf);
    if (stats_total_.tess_draws > 0) {
        std::snprintf(buf, sizeof(buf), " | tessellated: %.0f draws", stats_total_.tess_draws / f);
        out += buf;
    }
    if (stats_total_.cull_items_tested > 0) {
        std::snprintf(buf, sizeof(buf),
                      " | cull: %.1fk tests, %.0f cells, %.1f list hits, %.2f sun levels redrawn",
                      stats_total_.cull_items_tested / f / 1000.0,
                      stats_total_.cull_cells_visited / f, stats_total_.dir_list_cache_hits / f,
                      stats_total_.dir_levels_rendered / f);
        out += buf;
    }
    return out;
}

bool ToyRenderPipeline::needs_rebuild(const ToyRenderConfig& live, ToyRenderConfig next) {
    next.fill_aspect = live.fill_aspect;
    bool differs = false;
#define TOY_STARTUP_DIFFERS(field) differs = differs || live.field != next.field;
    TOY_STARTUP_FIXED_FIELDS(TOY_STARTUP_DIFFERS)
#undef TOY_STARTUP_DIFFERS
    return differs;
}

void ToyRenderPipeline::apply_live_config(const ToyRenderConfig& next, bool warn_ignored) {
    ToyRenderConfig merged = next;
    std::vector<const char*> ignored;

    // Restore a startup-fixed field from the live config, remembering it if the
    // file tried to change it. The list is TOY_STARTUP_FIXED_FIELDS (toy_render_pipeline.h).
#define TOY_KEEP_STARTUP_FIXED(field)                             \
    do {                                                      \
        if (merged.field != config_.field) {                  \
            ignored.push_back(#field);                        \
        }                                                     \
        merged.field = config_.field;                         \
    } while (0);
    TOY_STARTUP_FIXED_FIELDS(TOY_KEEP_STARTUP_FIXED)
#undef TOY_KEEP_STARTUP_FIXED

    config_ = merged;

    if (warn_ignored && !ignored.empty()) {
        std::cerr << "[toy::render] Config reloaded, but these are startup-fixed and were "
                     "IGNORED (restart to apply):";
        for (const char* name : ignored) {
            std::cerr << ' ' << name;
        }
        std::cerr << '\n';
    }
}

bool ToyRenderPipeline::render(coopa::gfx::presentation::Renderer& renderer, coopa::scene::Scene& scene, float dt) {
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
    // The previous frame's matrices the G-buffer's velocity attachment reprojects with
    // (gfx/surface/gbuffer_fs.glsl). prev_snapped_view_ is what the previous update()
    // actually uploaded (pixel-snapped when snapping is on), read back from the UBO at the
    // end of render(); the projection is the unjittered one, and the jitter apply_taa_jitter_
    // put into THIS frame's proj is handed over so the velocity is measured unjittered-to-
    // unjittered. On the first frame there is no previous pose: this frame's own stands in,
    // which makes every velocity read as zero instead of a frame-wide spike.
    camera_ubos_[frame_slot]->set_reprojection(
        prev_view_proj_valid_ ? prev_snapped_view_   : view,
        prev_view_proj_valid_ ? prev_unjittered_proj_ : unjittered_proj,
        taa_jitter_ndc_);
    // Tessellation stages size edges in render pixels: pixels per metre at 1 m.
    camera_ubos_[frame_slot]->set_pixel_scale(0.5f * static_cast<float>(render_extent_.height) * std::abs(unjittered_proj[1][1]));
    camera_ubos_[frame_slot]->update(view, proj, cam_pos, pixel_density);
    // The surface world set for this slot: the tessellation view (main camera position and
    // its pixel scale -- render pixels per metre at 1 m), snow, occlusion and trenches.
    surface_world_->upload(frame_slot, surface_state_, cam_pos,
                           0.5f * static_cast<float>(render_extent_.height) * std::abs(unjittered_proj[1][1]),
                           elapsed_time_);
    // Heightmap terrains' set 3, and the material sets of height grids that went away (their
    // textures are still alive -- the asset manager's grace period -- so the addresses cannot
    // have been reused yet).
    terrain_gpu_->update(surface_state_.terrains);
    for (const auto* gone : terrain_gpu_->released()) material_cache_->release(gone);
    material_cache_->tick();
    tick_static_instances_();

    // One hierarchy walk for every component type this frame's gathers read -- see
    // FrameScene. Everything below reads frame_scene_ instead of walking the scene again.
    snapshot_scene_(scene);

    // Where this frame's cloud shadow map lies, around the camera (update_lights_() hands it
    // to the shaders, record_clouds_() renders it).
    cloud_shadow_place_ = passes::CloudShadowPass::place(glm::vec2(cam_pos), cloud_state_.shadow_distance);

    frame_cam_pos_ = cam_pos;
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

    // Point and spot shadows: the most important cast_shadows lights get slots in the
    // local-light shadow atlas, up to the per-type budgets (see update_local_shadows_()).
    update_local_shadows_(cam_pos, unjittered_proj * view);

    // After both shadow fits: every view's frustum is now known, so the gather can cull
    // and batch each view's draw list (see gather_meshes_()).
    const MeshGather meshes = gather_meshes_(frame_slot, view, unjittered_proj, cast_dir_shadow);

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

    // Refraction's scene-colour chain is only ever sampled by transparent.frag, i.e. by a
    // camera-visible BLEND mesh (BLEND SDFs read ssr_pass_'s chain instead). On a frame
    // with none in view -- every frame of a scene without transparency -- the seven-mip
    // build would be pure bandwidth, so it is skipped.
    refraction_this_frame_ = config_.transparency_enabled && config_.refraction_enabled
                          && meshes.has_blend_mesh;

    light_datas_[light_frame_]->upload();

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

    if (profile_) {
        profile_->add_cpu(CpuScope::Gather, std::chrono::duration<double, std::milli>(
                                                std::chrono::steady_clock::now() - gather_start).count());
    }
    const auto submit_start = std::chrono::steady_clock::now();
    double record_ms = 0.0;
    // Split of the frame's submit half, for TOY_PRESENT_LOG below: the recording callback runs
    // after the swapchain image has been acquired, so the time up to it is the acquire (plus
    // the per-image fence wait), and what is left after the callback is the submit and present.
    double acquire_ms = 0.0;
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
        coopa::gfx::ClearColor{0.0f, 0.0f, 0.0f, 1.0f},
        nullptr,
        [&](coopa::gfx::command::CommandBuffer& cmd) {
            const auto record_start = std::chrono::steady_clock::now();
            acquire_ms = std::chrono::duration<double, std::milli>(record_start - submit_start).count();
            if (gpu_profiler_) gpu_profiler_->begin_frame(cmd, frame_slot, profile_->current_frame());
            // GPU skinning first: every pass below (shadows, G-buffer...) draws its output.
            if (skinning_pass_) skinning_pass_->record(cmd);
            gpu_mark_(cmd, GpuScope::Skinning);
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
    const double submit_total_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - submit_start).count();
    if (profile_) {
        profile_->add_cpu(CpuScope::Record, record_ms);
        profile_->add_cpu(CpuScope::SubmitPresent, std::max(0.0, submit_total_ms - record_ms));
    }
    // TOY_PRESENT_LOG=<ms>: diagnostic. Reports every frame whose submit half ran longer than
    // that, split into the acquire and the submit+present, so a stall that the profile only
    // shows as `render.submit_present` can be told apart -- blocking in
    // vkAcquireNextImageKHR is the presentation engine holding every image (a compositor or
    // driver pacing the window), while blocking after the recording is the submit itself.
    // Independent of PROFILE, so it also answers whether profiling is what provoked the stall.
    {
        static const double log_over_ms = [] {
            const char* v = core::debug_env("TOY_PRESENT_LOG");
            return v != nullptr && *v != '\0' ? std::strtod(v, nullptr) : 0.0;
        }();
        if (log_over_ms > 0.0 && submit_total_ms > log_over_ms) {
            std::printf("[present] frame %llu: submit half %.2f ms = acquire %.2f + record %.2f"
                        " + queue_submit %.2f + queue_present %.2f (rest %.2f)\n",
                        static_cast<unsigned long long>(frame_index_), submit_total_ms, acquire_ms,
                        record_ms, renderer.last_submit_ms(), renderer.last_present_ms(),
                        std::max(0.0, submit_total_ms - acquire_ms - record_ms -
                                          renderer.last_submit_ms() - renderer.last_present_ms()));
            std::fflush(stdout);
        }
    }

    // Needed by both SSAO's and SSR's temporal resolve passes -- kept unconditional (not
    // gated on either toggle) since ssao_pass_ always exists and either toggle can be
    // re-enabled without a resize/reconstruct in between.
    prev_view_proj_                = proj * view;
    prev_unjittered_view_proj_     = unjittered_proj * view;
    // For the velocity attachment: the view as uploaded (snapped), and the unjittered proj.
    prev_snapped_view_             = camera_ubos_[frame_slot]->data().view;
    prev_unjittered_proj_          = unjittered_proj;
    prev_view_proj_valid_          = true;
    ++frame_index_;

    return frame_presented;
}

bool ToyRenderPipeline::ContactShadowKnobs::operator==(const ContactShadowKnobs& o) const {
    return length == o.length && strength == o.strength && thickness == o.thickness
        && steps == o.steps && enabled == o.enabled;
}
} // namespace render
} // namespace toy
