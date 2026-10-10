#include <toyengine/render/toy_render_pipeline.h>

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

glm::vec4 ToyRenderPipeline::surface_gfx_time_() const {
    const float water_time = water_state_.time >= 0.0f ? water_state_.time : elapsed_time_;
    return glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), water_time);
}

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

    // One hierarchy walk for every component type this frame's gathers read -- see
    // FrameScene. Everything below reads frame_scene_ instead of walking the scene again.
    snapshot_scene_(scene);

    // Where this frame's cloud shadow map lies, around the camera (update_lights_() hands it
    // to the shaders, record_clouds_() renders it).
    cloud_shadow_place_ = passes::CloudShadowPass::place(glm::vec2(cam_pos), cloud_state_.shadow_distance);

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
    // For the velocity attachment: the view as uploaded (snapped), and the unjittered proj.
    prev_snapped_view_             = camera_ubos_[frame_slot]->data().view;
    prev_unjittered_proj_          = unjittered_proj;
    prev_view_proj_valid_          = true;
    ++frame_index_;

    return frame_presented;
}

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
        std::vector<VkDescriptorSetLayout>{surface_world_->layout().handle()});

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
            sd.cull, opt(dir_tese), opt(cube_tese));
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
            config_.shaders("fullscreen.vert"), config_.shaders("underwater.frag"));
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
    // over the finished pixel-art frame the way tilt shift is.
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
        grading_lut_.view_typed(), &grading_lut_.sampler_object());

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
        config_.shaders("transparent.vert"), config_.shaders("transparent_preview.frag"));
    transparent_preview_pass_->set_scene_depth(gbuffer_target_.depth_view_typed(), nearest_sampler_,
                                               render_extent_.width, render_extent_.height);
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

void ToyRenderPipeline::record_physical_sky_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
    sky_atmosphere_pass_->initialize(cmd);
    sky_cloud_pass_->initialize(cmd);
    cloud_shadow_pass_->initialize(cmd);
    if (physical_sky_active()) {
        const SkyFrameState& st = sky_state_;
        sky_atmosphere_pass_->update_luts(cmd, st.media);
        passes::SkyAtmospherePass::ViewParams vp;
        vp.atmo = passes::SkyAtmospherePass::AtmosphereGpu::of(st.media);
        vp.sun = glm::vec4(st.sun_to, st.moon_ratio);
        vp.view = glm::vec4(st.media.bottom_radius + std::max(ctx.cam_pos.z, 0.0f) * 0.001f + 0.01f,
                            static_cast<float>(sky_quality_steps_()), 0.0f, 0.0f);
        sky_atmosphere_pass_->render_view(cmd, vp);
        gpu_mark_(cmd, GpuScope::Sky);
    }
    record_clouds_(cmd, ctx);
}

void ToyRenderPipeline::record_clouds_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
    const CloudFrameState& cs = cloud_state_;
    if (!cs.active) return;
    const bool volumetric = cs.type == CloudType::Volumetric;
    auto* dir = frame_scene_.dir_light;
    // The map only while the shadows show: update_lights_() fades them out as the light
    // sinks (cloud_shadow_layer.z), so below that point it would render for nothing.
    const bool shadows = cloud_shadows_active() && dir && current_light_data().cloud_shadow_layer.z > 0.0f;
    if (!volumetric) {
        sky_cloud_pass_->ensure_noise(cmd);
        cloud_overlay_pass_->upload_flat(ctx.frame_slot, passes::CloudOverlayPass::flat_frame_of(cs));
        if (shadows) {
            cloud_shadow_pass_->render_flat(cmd, ctx.frame_slot, cloud_shadow_place_, -glm::normalize(dir->direction));
            gpu_mark_(cmd, GpuScope::CloudShadow);
        }
        return;
    }

    const CloudQualitySteps q = cloud_quality_steps_();
    const bool trace = volumetric_clouds_traced();
    passes::SkyCloudPass::Params cp;
    // Unjittered: the clouds reconstruct temporally on their own (sky_cloud_resolve.frag),
    // and a jittered ray would swim against the history it reprojects.
    cp.view_proj = ctx.unjittered_proj * ctx.view;
    cp.prev_view_proj = prev_view_proj_valid_ ? prev_unjittered_view_proj_ : cp.view_proj;
    cp.camera_pos = ctx.cam_pos;
    cp.proj_y = std::abs(ctx.unjittered_proj[1][1]);
    cp.slab = glm::vec4(cs.altitude, std::max(cs.thickness, 0.1f), glm::clamp(cs.coverage, 0.0f, 1.0f),
                        std::max(cs.density, 0.0f));
    cp.scale = std::max(cs.scale, 1e-4f);
    cp.fade = cs.fade;
    cp.atmosphere_lut = cs.atmosphere_lut;
    // The drift in cloud space, wrapped to whole periods of both weather-map reads
    // (sky_cloud_density.glsl), in double precision so it never jumps.
    {
        const glm::dvec2 c = cs.offset / static_cast<double>(cp.scale);
        const double period = 13000.0 * 53.0;
        cp.wind_offset = glm::vec2(c - glm::floor(c / period) * period);
    }
    // The clouds' world movement since last frame: what the reconstruction follows. A scale
    // edit rescales the whole offset, which reads as a jump: anything faster than 1 km per
    // frame is treated as no drift (the anti-flicker cap is lifted by the edit anyway).
    const glm::vec2 drift = glm::vec2(cs.offset - cloud_prev_offset_);
    cp.drift = glm::length(drift) < 1000.0f ? drift : glm::vec2(0.0f);
    cloud_prev_offset_ = cs.offset;
    cp.time = cs.time;
    cp.frame = frame_index_;
    cp.light_dir = cs.light_to;
    cp.light_color = cs.light_color;
    cp.view_steps = q.cloud_steps;
    cp.shadow_steps = q.cloud_light_steps;
    // History is valid only if the clouds were traced last frame too.
    cp.history_valid = trace && prev_view_proj_valid_ && cloud_last_frame_ + 1 == frame_index_;
    if (trace) cloud_last_frame_ = frame_index_;
    // How much the clouds' light changed since last frame (relative): the resolve's
    // anti-flicker cap lets a real lighting change through at its own pace.
    const auto lum = [](glm::vec3 c) { return glm::dot(c, glm::vec3(0.2126f, 0.7152f, 0.0722f)); };
    const glm::vec2 light_now(lum(cs.light_color), lum(config_.indirect.sky_zenith));
    const glm::vec2 rel = glm::abs(light_now - cloud_prev_light_) / glm::max(glm::max(light_now, cloud_prev_light_), glm::vec2(1e-6f));
    // An edit to the layer itself (coverage, density, altitude, thickness, scale, the camera
    // fade) counts too, so dragging a slider -- or the camera through the fade -- updates the
    // clouds at once instead of easing in.
    const glm::vec4 slab_now(cp.slab.x / cp.scale, cp.slab.y / cp.scale, cp.slab.z, cp.slab.w);
    const glm::vec4 dslab = glm::abs(slab_now - cloud_prev_slab_) /
                            glm::max(glm::abs(cloud_prev_slab_), glm::vec4(100.0f, 100.0f, 0.05f, 0.05f));
    const float dfade = std::abs(cp.fade - cloud_prev_fade_) / 0.05f;
    const float layer_change = std::max(std::max(std::max(dslab.x, dslab.y), std::max(dslab.z, dslab.w)), dfade);
    cp.light_change = cp.history_valid ? std::max(std::max(rel.x, rel.y), layer_change) : 0.0f;
    cloud_prev_light_ = light_now;
    cloud_prev_slab_ = slab_now;
    cloud_prev_fade_ = cp.fade;
    sky_cloud_pass_->execute(cmd, ctx.frame_slot, current_camera_set(), current_light_set(), cp, q.cloud_scale, trace);
    if (trace) gpu_mark_(cmd, GpuScope::Clouds);
    if (shadows) {
        cloud_shadow_pass_->render_volumetric(cmd, ctx.frame_slot, cloud_shadow_place_, -glm::normalize(dir->direction));
        gpu_mark_(cmd, GpuScope::CloudShadow);
    }
}

int ToyRenderPipeline::sky_quality_steps_() const {
    switch (config_.sky_quality) {
        case RenderQuality::Low:    return 16;
        case RenderQuality::Medium: return 24;
        case RenderQuality::High:   break;
        case RenderQuality::Ultra:  return 40;
    }
    return 30;
}

ToyRenderPipeline::CloudQualitySteps ToyRenderPipeline::cloud_quality_steps_() const {
    // The cloud march traces a quarter of the region's pixels per frame (checkerboarded,
    // SkyCloudPass), so these step counts cost what a quarter of them would per pixel.
    switch (config_.cloud_quality) {
        case RenderQuality::Low:    return {24, 4, 0.5f};
        case RenderQuality::Medium: return {32, 5, 0.75f};
        case RenderQuality::High:   break;
        case RenderQuality::Ultra:  return {64, 6, 1.0f};
    }
    return {40, 4, 1.0f};
}

void ToyRenderPipeline::record_scene_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx,
                   const MeshGather& meshes, const std::vector<SdfDrawItem>& sdf_draws) {
    if (gpu_particle_pass_ && !particle_state_.gpu.empty()) {
        gpu_particle_pass_->record(cmd, ctx.frame_slot, particle_state_.gpu);
        gpu_mark_(cmd, GpuScope::ParticlesSim);
    }
    record_directional_shadow_(cmd, meshes, sdf_draws, ctx.cast_dir_shadow);
    gpu_mark_(cmd, GpuScope::ShadowDirectional);
    record_local_shadows_(cmd, meshes, sdf_draws);
    gpu_mark_(cmd, GpuScope::ShadowLocal);
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
        // colour attachments end in SHADER_READ_ONLY_OPTIMAL), but stylize.frag samples
        // depth for the outline edge detector. When Hi-Z runs, HiZPass::execute() performs this
        // same transition instead -- doing both would present a stale oldLayout on the second. Not
        // transitioned back: GBufferTarget declares depth initialLayout = UNDEFINED, so next
        // frame's begin() does not care what it is left in.
        transition_gbuffer_depth_to_shader_read_(cmd);
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
        // The scene counts too: an object that moved (gather_meshes_()'s scene_moved_)
        // has changed what every temporally accumulated pass sees, so a still camera
        // watching it must keep resolving every frame rather than hold a stale image.
        camera_frames_still_  = (moved || scene_moved_) ? 0u : camera_frames_still_ + 1u;
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
        // Per-object motion: a moving object keeps its accumulated SSR/SSGI/contact-shadow
        // history instead of resetting every frame along its path.
        th_params.use_velocity    = true;
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
        ssao_params.temporal_gamma       = config_.ssao_temporal_gamma;
        ssao_params.reproject        = temporal_reproject;
        ssao_params.reproject_valid  = prev_view_proj_valid_;
        ssao_params.frozen               = temporal_frozen_;
        ssao_params.motion_px            = ssao_motion_px_;
        ssao_pass_->execute(cmd, current_camera_set(), ssao_params);
        gpu_mark_(cmd, GpuScope::Ssao);
    } else {
        ssao_pass_->invalidate_history();
    }
    // Note: the SSAO image bound into toy_lighting_pass_/ssr_pass_ is decided
    // once at construction (config_.ssao_enabled doesn't change at runtime), not
    // here -- see the constructor's comment on why a per-frame rebind would violate
    // this pipeline's frame-overlap model.

    record_physical_sky_(cmd, ctx);

    // One fullscreen draw writes every pixel of offscreen_target_: lit surfaces, and the
    // procedural sky at background pixels (toy_lighting.frag -- rather than a second
    // skybox draw that would re-read every pixel's normal just to discard the lit ones).
    offscreen_target_.begin(cmd);

    ToyLightingPushConstants lighting_pc;
    lighting_pc.light_bands       = config_.light_bands;
    lighting_pc.spec_threshold    = config_.spec_threshold;
    lighting_pc.rim_strength      = config_.rim_strength;
    lighting_pc.ambient_intensity = config_.indirect.ambient_intensity;
    lighting_pc.sky_intensity     = config_.indirect.sky_intensity;
    lighting_pc.soft_lighting     = config_.soft_lighting ? 1.0f : 0.0f;
    lighting_pc.ssao_direct_strength = config_.ssao_direct_lighting_strength;
    lighting_pc.ssao_intensity       = config_.ssao_intensity;
    // The sky is drawn by this same pass at background pixels (see toy_lighting.frag),
    // so sky and lighting share one fullscreen draw.
    lighting_pc.sky_inv_view_proj    = glm::inverse(ctx.proj * ctx.view);
    // gfxcoopa's DeferredLightingPass::draw() pushes this internally (the templated
    // overload), after its own bind_pipeline() -- no separate push needed.
    toy_lighting_pass_->draw(cmd, current_camera_set(), current_light_set(), *shadow_set_, lighting_pc,
                              render_extent_.width, render_extent_.height);


    offscreen_target_.end(cmd);
    gpu_mark_(cmd, GpuScope::LightingSky);

    if (ctx.need_ssr_trace_inputs) {
        // Prefiltered scene-colour mip chain: also feeds transparent.frag's
        // gfx_ssr_trace() cone-LOD taps and SSGI bounce lookup, not just ssr.frag's
        // own -- see need_ssr_trace_inputs' own doc.
        // Once a previous frame has copied its final HDR into mip 0 (record_post_chain_()),
        // only mips 1+ are rebuilt from it; the first frame draws mip 0 from this frame's
        // lit opaque image instead.
        scene_color_mip_pass_->execute(cmd, offscreen_target_.color_view_typed(),
                                       scene_color_history_valid_);
        gpu_mark_(cmd, GpuScope::SceneColorMips);
    }

    // Runs on debug_view frames too: with ssr_enabled, the whole post chain reads
    // ssr_pass_'s composite output (see pre_volumetrics_view_typed_ in build_post_chain_), so
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

        ssr_params.skip_threshold  = config_.ssr_skip_negligible ? config_.ssr_skip_threshold : 0.0f;
        // Hits read the previous frame's final colour at the hit's reprojected uv.
        ssr_params.prev_frame_color = scene_color_history_valid_;
        ssr_params.rays_per_pixel   = glm::clamp(config_.ssr_rays_per_pixel, 1, 8);
        ssr_params.cone_prefilter   = config_.ssr_cone_prefilter;
        ssr_params.skip_behind      = config_.ssr_skip_behind;

        ssr_pass_->execute(cmd, current_camera_set(), ssr_params);
        // GPU scopes for SsrPass come from its stage hook (see set_profiler()).
    }

    // Global fog over the opaque scene, before refraction's chain and translucency.
    if (config_.fog_enabled) record_fog_(cmd, ctx);

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
        // stylize_pass_'s pre-bound descriptor expects. record_transparent_() early-returns
        // without touching depth when the scene has no BLEND renderers this frame, in which case
        // depth is still where the branch above left it and this transition must be skipped --
        // issuing it anyway asserts a false oldLayout and trips synchronization validation.
        if (transparent_ran) {
            transition_gbuffer_depth_to_shader_read_(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        }
        gpu_mark_(cmd, GpuScope::Transparent);
    }

    record_cloud_overlay_(cmd, ctx);
}

void ToyRenderPipeline::record_cloud_overlay_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
    const CloudFrameState& cs = cloud_state_;
    const bool flat = cs.active && cs.type == CloudType::Flat && cs.fade > 0.0f;
    if (!flat && !volumetric_clouds_traced()) return;
    VkImageView hdr = config_.ssr_enabled ? ssr_pass_->output_view() : offscreen_target_.color_view();
    transparent_pass_->set_targets(hdr, gbuffer_target_.depth_view(), render_extent_.width, render_extent_.height);
    transparent_pass_->begin(cmd, gbuffer_target_.depth_image_handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (flat) {
        // Jittered, like every surface: TAA then resolves the clouds' silhouettes with the scene's.
        cloud_overlay_pass_->draw_flat(cmd, ctx.frame_slot, current_camera_set(), current_light_set(),
                                       glm::inverse(ctx.proj * ctx.view), render_extent_.width, render_extent_.height);
    } else {
        passes::CloudOverlayPass::CompositePush p;
        p.camera = glm::vec4(ctx.cam_pos, std::max(cs.scale, 1e-4f));
        p.region = glm::vec4(cloud_quality_steps_().cloud_scale, 0.0f, 0.0f, 0.0f);
        cloud_overlay_pass_->draw_composite(cmd, p, render_extent_.width, render_extent_.height);
    }
    transparent_pass_->end(cmd);
    transition_gbuffer_depth_to_shader_read_(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    gpu_mark_(cmd, GpuScope::Clouds);
}

void ToyRenderPipeline::record_post_chain_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
    using coopa::gfx::engine::components::CameraType;

    // Parsed once per frame -- debug_view is runtime (file doc, rule 1's exception list),
    // so every branch below that reads it must see the SAME frame's value.
    const DebugView active_view = parse_debug_view(config_.debug_view);

    // Global fog already ran in record_scene_() (record_fog_(), before translucency).
    // Underwater -- every frame once built (see UnderwaterPass's file doc); a copy unless the
    // camera is below a water surface.
    if (underwater_pass_) {
        underwater_target_.begin(cmd);
        underwater_pass_->draw(cmd, render_extent_.width, render_extent_.height, underwater_params_);
        underwater_target_.end(cmd);
        gpu_mark_(cmd, GpuScope::Underwater);
    }

    // Local volumes. After fog, so wisps layer over fogged geometry, and before DOF and
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

    // Previous-frame scene colour: copy the final pre-lens HDR into the SSR scene-colour
    // chain's mip 0, where next frame's trace reads it (reprojected by the G-buffer
    // velocity). After volumetrics -- the last pass that writes the image -- and before DOF.
    if (ctx.need_ssr_trace_inputs && scene_color_history_src_ != VK_NULL_HANDLE) {
        scene_color_mip_pass_->copy_level0_from(cmd, scene_color_history_src_);
        scene_color_history_valid_ = true;
        gpu_mark_(cmd, GpuScope::SceneColorHistory);
    }

    // Motion blur, in place on the final pre-lens HDR image. After the history copy above, so
    // next frame's reflections see an unblurred scene (Unreal's PrevSceneColor is pre-blur
    // too), and before DoF and bloom, so blurred highlights still defocus and glow. TAA here
    // is a display-space resolve after tonemap (step 18), so it necessarily comes after this;
    // the velocity it reads is unjittered, so the jittered input blurs the same way. Nothing
    // is recorded when off, for this camera, or under a raw debug channel.
    if (motion_blur_active_(ctx, active_view)) {
        passes::MotionBlurPass::Params mb{};
        // Double precision for the same reason TAA's reprojection uses it (see below).
        mb.sky_reproject = glm::mat4(glm::dmat4(prev_unjittered_view_proj_) *
                                     glm::inverse(glm::dmat4(ctx.unjittered_proj * ctx.view)));
        mb.sky_valid   = prev_view_proj_valid_;
        mb.shutter     = config_.motion_blur_intensity;
        mb.max_radius  = kMotionBlurMaxRadius1080 * static_cast<float>(render_extent_.height) / 1080.0f;
        mb.samples     = kMotionBlurSamples;
        // TAA averages a per-frame jitter away; without it a fixed pattern reads as grain, not crawl.
        mb.noise_frame = config_.aa_mode == "taa" ? static_cast<uint32_t>(frame_index_) : 0u;
        motion_blur_pass_->execute(cmd, mb);
        ++motion_blur_frames_;
        gpu_mark_(cmd, GpuScope::MotionBlur);
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
        dof_params.blur_scale     = lens_blur_suppressed_ ? 0.0f : config_.dof_blur_scale;   // 0: exact bypass

        dof_params.max_radius        = config_.dof_max_radius;
        dof_params.sample_count      = config_.dof_sample_count;
        dof_params.blade_count       = config_.dof_blade_count;
        dof_params.blade_rotation_deg = config_.dof_blade_rotation;
        // Same three-line camera idiom stylize_pass_'s push constants use
        // below, for the same linearization formula (see gfx/depth.glsl).
        dof_params.camera_near           = ctx.cam ? ctx.cam->clip_start : 0.1f;
        dof_params.camera_far            = ctx.cam ? ctx.cam->clip_end : 1000.0f;
        dof_params.camera_is_perspective = (!ctx.cam || ctx.cam->type == CameraType::Perspective);
        dof_params.debug_view = (active_view == DebugView::Dof);

        dof_pass_->execute(cmd, dof_params);
        gpu_mark_(cmd, GpuScope::Dof);
    }

    // Bloom pyramid. After the fog branch (so fog and everything before it
    // bloom too) and before post_target_, whose stylize_pass_ draw below
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

        coopa::gfx::engine::passes::StylizePass::PushConstants post_pc;
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
        // keeps stylize.frag's tonemap step live (see PushConstants::exposure's doc).
        post_pc.exposure         = raw_passthrough ? 0.0f : config_.exposure;
        post_pc.bloom_intensity  = (config_.bloom_enabled && !raw_passthrough) ? config_.bloom_intensity : 0.0f;
        post_pc.auto_exposure    = (exposure_pass_ && !raw_passthrough) ? 1.0f : 0.0f;
        post_pc.grading_size     = (config_.grading_enabled && !raw_passthrough)
            ? static_cast<float>(grading_lut_.size()) : 0.0f;
        stylize_pass_->draw(cmd, post_pc, render_extent_.width, render_extent_.height);
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

    // Anti-aliasing, after stylize_pass_ and before tilt shift. A no-op when aa_mode is
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
            record_reactive_mask_(cmd);
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
        // Suppressed: zero strength, so every CoC is 0 -- the pass's exact passthrough.
        ts_params.blur_top      = lens_blur_suppressed_ ? 0.0f : config_.tilt_shift_blur_top;
        ts_params.blur_bottom   = lens_blur_suppressed_ ? 0.0f : config_.tilt_shift_blur_bottom;
        ts_params.angle_degrees = config_.tilt_shift_angle;
        tilt_shift_pass_->execute(cmd, ts_params);
        gpu_mark_(cmd, GpuScope::TiltShift);
    }

}

void ToyRenderPipeline::record_overlay_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
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

ToyRenderPipeline::MeshGather ToyRenderPipeline::gather_meshes_(uint32_t frame_slot, const glm::mat4& view, const glm::mat4& unjittered_proj,
                          bool cast_dir_shadow) {
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
    out.prev_world_matrices.assign(n, glm::mat4(1.0f));
    out.snow_anchors.assign(n, glm::mat4(1.0f));
    out.moved.assign(n, 0);
    out.bounds.assign(n, WorldBounds{});
    out.valid.assign(n, 0);
    out.lod.assign(n, 0);
    out.ext.assign(n, glm::uvec4(0u));
    out.instance_idx.assign(n, InstanceStream::kInvalidIndex);
    instance_stream_.begin(frame_slot);

    // A world matrix counts as changed past any float churn a transform resolve can
    // produce for a resting object; a real move is orders of magnitude larger.
    auto matrix_moved = [](const glm::mat4& a, const glm::mat4& b) {
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                if (std::abs(a[c][r] - b[c][r]) > 1e-5f) return true;
            }
        }
        return false;
    };

    // world_matrix() is a pure read, safe for any number of concurrent readers (unlike
    // get_world_matrix()), because TransformSystem already resolved every dirty transform
    // earlier this frame. Each index writes only its own slot. prev_world_ is only READ
    // here (find on a map no one mutates until the serial section below), so that is
    // parallel-safe too.
    auto gather_mesh = [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            MeshRenderer* mr = out.renderers[i];
            if (out.multi[i]) {
                // A particle batch: world-space matrices already, bounds from the system.
                // No previous poses exist for its instances, so each streams prev == cur;
                // its presence alone counts as scene motion.
                const ParticleMeshBatch& pb = *particle_items[i - first_particle_item];
                out.bounds[i].center = 0.5f * (pb.bounds_min + pb.bounds_max);
                out.bounds[i].extent = 0.5f * (pb.bounds_max - pb.bounds_min);
                out.valid[i] = 1;
                out.moved[i] = 1;
                continue;
            }
            if (!mr->is_ready() || !mr->owner) continue;
            auto* tc = mr->owner->get_transform();
            if (!tc) continue;
            out.world_matrices[i] = tc->transform().world_matrix();
            const auto& mesh = mr->get_mesh();
            out.bounds[i] = world_aabb(out.world_matrices[i], mesh->bounds_min(), mesh->bounds_max());
            out.valid[i] = 1;
            const auto pit = prev_world_.find(mr);
            out.prev_world_matrices[i] = (pit != prev_world_.end()) ? pit->second : out.world_matrices[i];
            // A dynamic mesh re-uploads its vertices every frame under a fixed matrix
            // (cloth, CPU skinning): its surface moves even though prev == cur.
            const bool transform_moved = matrix_moved(out.prev_world_matrices[i], out.world_matrices[i]);
            out.moved[i] = (transform_moved || mesh->is_dynamic()) ? 1 : 0;
            // Snow anchor: frozen once the transform has moved (the pose from just before it
            // first did, so the pattern it showed at rest carries on with it).
            const auto ait = snow_anchor_.find(mr);
            out.snow_anchors[i] = ait != snow_anchor_.end() ? ait->second
                                : transform_moved           ? out.prev_world_matrices[i]
                                                            : out.world_matrices[i];
        }
    };
    if (should_parallelize_(n)) {
        jobs_->parallel_for_blocking(n, 0 /* auto grain */, gather_mesh);
    } else {
        gather_mesh(0, n);
    }

    // Rebuild last-frame poses for the next frame (serial: the map is written here only),
    // and record whether anything in the scene moved -- the temporal freeze (see
    // record_scene_()'s stillness block) must stay off while it did.
    {
        std::unordered_map<const MeshRenderer*, glm::mat4> next_prev_world;
        std::unordered_map<const MeshRenderer*, glm::mat4> next_snow_anchor;
        std::unordered_map<const MeshRenderer*, uint32_t>  next_still;
        next_prev_world.reserve(prev_world_.size() + 16);
        next_still.reserve(still_frames_.size() + 16);
        bool any_moved = false;
        for (size_t i = 0; i < n; ++i) {
            if (!out.valid[i]) continue;
            any_moved = any_moved || out.moved[i] != 0;
            if (out.multi[i]) continue;
            MeshRenderer* mr = out.renderers[i];
            next_prev_world[mr] = out.world_matrices[i];
            // Keep a frozen anchor; freeze one the frame the transform first moves.
            if (snow_anchor_.count(mr) || matrix_moved(out.prev_world_matrices[i], out.world_matrices[i])) {
                next_snow_anchor[mr] = out.snow_anchors[i];
            }
            // Frames without moving -- what makes a caster eligible for the local-shadow
            // static cache (see the local views below).
            const auto it = still_frames_.find(mr);
            const uint32_t prev = (it != still_frames_.end()) ? it->second : 0u;
            next_still[mr] = out.moved[i] ? 0u : std::min(prev + 1u, 1u << 20);
        }
        prev_world_.swap(next_prev_world);
        snow_anchor_.swap(next_snow_anchor);
        still_frames_.swap(next_still);
        scene_moved_ = any_moved;
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
        out.ext[i]            = surface_ext_(*mr, m);
        k.camera.ext          = out.ext[i];
        k.shadow.tess_a       = out.ext[i].x;
        k.shadow.tess_b       = out.ext[i].y;
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
                    instance_stream_.add(out.world_matrices[it], out.prev_world_matrices[it], out.snow_anchors[it]);
                    ++added;
                }
            }
            batch.instance_count = added;
            list.push_back(batch);
            r = e;
        }
    };

    // A BLEND material only casts a shadow at full opacity -- the shadow passes have no
    // per-fragment alpha discard beyond the CUTOUT mask (gfx/surface/shadow_fs.glsl), so a
    // translucent object is binary: caster or not.
    auto casts = [&](size_t i) {
        const auto& m = out.material(i);
        return !(m.is_blended() && m.alpha < 1.0f);
    };
    auto opaque = [&](size_t i) { return !out.material(i).is_blended(); };
    auto blended = [&](size_t i) { return out.material(i).is_blended(); };

    const glm::mat4 camera_vp = unjittered_proj * view;
    build(camera_vp, false, 0.0f, opaque, out.gbuffer);

    // The forward BLEND pass draws one renderer at a time in back-to-front order, so its
    // transforms are added singly.
    {
        const Frustum f = Frustum::from_matrix(camera_vp);
        for (size_t i = 0; i < n; ++i) {
            if (out.valid[i] && blended(i) && f.intersects(out.bounds[i])) {
                out.instance_idx[i] = instance_stream_.add(out.world_matrices[i], out.prev_world_matrices[i], out.snow_anchors[i]);
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
    // Point/spot shadows: every view of every local light update_local_shadows_() chose.
    // A caster must reach into the light's influence sphere as well as its view frustum
    // (light-bounds culling: a face frustum alone extends to the corners of the range cube).
    {
        uint32_t total_views = 0;
        for (const LocalShadowSlot& slot : local_slots_) total_views = std::max(total_views, slot.first_view + slot.view_count);
        out.local_static.assign(total_views, {});
        out.local_dynamic.assign(total_views, {});
        const bool caching = local_shadow_atlas_->caching();
        const auto& blk = current_light_data().local_shadows;

        // Static-cache eligibility: still for kStaticCasterFrames, not a dynamic mesh or a
        // particle batch, and not drawn with a shadow shader that may animate.
        auto static_ok = [&](size_t i) {
            if (out.multi[i]) return false;
            MeshRenderer* mr = out.renderers[i];
            if (mr->get_mesh()->is_dynamic()) return false;
            const auto it = still_frames_.find(mr);
            if (it == still_frames_.end() || it->second < kStaticCasterFrames) return false;
            // A tessellated caster's shape follows the CAMERA (its edge factors), so it is
            // never the same twice: never cached.
            if (out.ext[i].x != 0u) return false;
            return animated_shadow_shaders_.count(out.material(i).shader) == 0;
        };
        // FNV-1a over raw bytes, for the static-content hash.
        auto mix = [](uint64_t h, const void* data, size_t len) {
            const auto* b = static_cast<const unsigned char*>(data);
            for (size_t k = 0; k < len; ++k) { h ^= b[k]; h *= 1099511628211ull; }
            return h;
        };

        for (LocalShadowSlot& slot : local_slots_) {
            const glm::vec3 c = slot.pos;
            const float     r = slot.range;
            auto in_light = [&](size_t i) {
                const WorldBounds& b = out.bounds[i];
                const glm::vec3 d = glm::max(glm::abs(c - b.center) - b.extent, glm::vec3(0.0f));
                return glm::dot(d, d) <= r * r;
            };
            const float res = static_cast<float>(slot.tile_res);
            if (!caching) {
                for (uint32_t v = 0; v < slot.view_count; ++v) {
                    build(blk.view_proj[slot.first_view + v], true, res,
                          [&](size_t i) { return casts(i) && in_light(i); },
                          out.local_dynamic[slot.first_view + v]);
                }
                slot.rerender_static = false;
                continue;
            }

            // What the cache tiles must hold: every static caster any of this light's views
            // sees, at its current pose and material, plus the views and the tile block. Any
            // change re-renders the light's cache once; otherwise the copy is enough.
            uint64_t h = 1469598103934665603ull;
            h = mix(h, &slot.block, sizeof(slot.block));
            for (uint32_t v = 0; v < slot.view_count; ++v) {
                const glm::mat4& vp = blk.view_proj[slot.first_view + v];
                h = mix(h, &vp, sizeof(vp));
                const Frustum f = Frustum::from_matrix(vp);
                for (size_t i = 0; i < n; ++i) {
                    if (!out.valid[i] || !casts(i) || !static_ok(i) || !in_light(i)) continue;
                    if (!f.intersects(out.bounds[i])) continue;
                    const MeshBatchKey& k = keys[i];
                    const void* id = out.renderers[i];
                    h = mix(h, &id, sizeof(id));
                    h = mix(h, &out.world_matrices[i], sizeof(glm::mat4));
                    h = mix(h, &out.lod[i], sizeof(out.lod[i]));
                    h = mix(h, &out.part[i], sizeof(out.part[i]));
                    h = mix(h, &k.shader_hash, sizeof(k.shader_hash));
                    h = mix(h, &k.mesh, sizeof(k.mesh));
                    h = mix(h, &k.shadow, sizeof(k.shadow));
                }
            }
            const auto it = local_cache_.find(slot.light);
            slot.rerender_static = (it == local_cache_.end() || it->second.hash != h ||
                                    !(it->second.block == slot.block));
            local_cache_[slot.light] = LocalCacheEntry{h, slot.block};

            for (uint32_t v = 0; v < slot.view_count; ++v) {
                const glm::mat4& vp = blk.view_proj[slot.first_view + v];
                if (slot.rerender_static) {
                    build(vp, true, res, [&](size_t i) { return casts(i) && in_light(i) && static_ok(i); },
                          out.local_static[slot.first_view + v]);
                }
                build(vp, true, res, [&](size_t i) { return casts(i) && in_light(i) && !static_ok(i); },
                      out.local_dynamic[slot.first_view + v]);
            }
        }
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
        stats_total_.tess_draws       += frame_stats_.tess_draws;
        ++stats_frames_;
    }
    frame_stats_ = MeshDrawStats{};
    frame_stats_.renderers = n;
    for (const MeshBatch& b : out.gbuffer) frame_stats_.camera_visible += b.instance_count;
    return out;
}

void ToyRenderPipeline::gather_ui_canvases_(coopa::scene::Scene& scene, uint32_t frame_slot, const glm::mat4& view) {
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
    // The engine's overlay layers (Engine::add_overlay_layer()) after that, so they draw
    // over the editor UI as well as the game's.
    for (coopa::scene::Scene* layer : overlay_layers_) {
        for (coopa::ui::CanvasComponent* c : coopa::ui::collect_canvases(*layer)) {
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

std::vector<SdfDrawItem> ToyRenderPipeline::gather_sdf_(coopa::scene::Scene& scene, const glm::mat4& view,
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
        g.ssr1 = glm::vec4(config_.ssr_thickness_scale, config_.ssr_roughness_cutoff,
                      config_.ssr_cone_prefilter, 0.0f);
        g.ssr_steps = glm::ivec4(config_.ssr_max_iterations,
                                 static_cast<int>(hiz_pass_->max_mip_level()),
                                 config_.ssr_start_mip, config_.ssr_min_mip0_steps);
        // y: the SSR scene-colour chain (which SDF forward always reads) holds the previous
        // frame's final colour at mip 0 -- hits reproject by the G-buffer velocity.
        g.ssr_mip = glm::ivec4(static_cast<int>(scene_color_mip_pass_->max_mip_level()),
                               scene_color_history_valid_ ? 1 : 0, 0, 0);

        sdf_data_.upload();
    }
    return sdf_draws;
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

void ToyRenderPipeline::apply_taa_jitter_(glm::mat4& proj) {
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

void ToyRenderPipeline::update_underwater_params_(const glm::mat4& unjittered_proj, const glm::mat4& view, const glm::vec3& cam_pos,
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

void ToyRenderPipeline::fill_fog_block_(coopa::gfx::engine::data::LightUBO& ubo) const {
    const bool on = config_.fog_enabled;
    ubo.fog_color   = glm::vec4(config_.fog_color, on ? 1.0f : 0.0f);
    ubo.fog_density = glm::vec4(config_.fog_mode == 0 ? 0.0f : 1.0f, std::max(config_.fog_density, 0.0f),
                                config_.fog_linear_start, config_.fog_linear_end);
    ubo.fog_height  = glm::vec4(config_.fog_height_base, config_.fog_height_falloff,
                                glm::clamp(config_.fog_sky_blend, 0.0f, 1.0f),
                                glm::clamp(config_.fog_max_opacity, 0.0f, 1.0f));
    ubo.fog_range   = glm::vec4(std::max(config_.fog_start_distance, 0.0f),
                                std::max(config_.fog_cutoff_distance, 0.0f),
                                std::max(config_.fog_sky_distance, 1.0f), 0.0f);
    const auto* dir = frame_scene_.dir_light;
    if (dir && config_.fog_sun_amount > 0.0f) {
        ubo.fog_sun     = glm::vec4(dir_light_color_(*dir) * dir->intensity * config_.fog_sun_amount,
                                    glm::clamp(config_.fog_sun_anisotropy, -0.95f, 0.95f));
        ubo.fog_sun_dir = glm::vec4(glm::normalize(dir->direction), std::max(config_.fog_sun_start_distance, 0.0f));
    } else {
        ubo.fog_sun     = glm::vec4(0.0f);
        ubo.fog_sun_dir = glm::vec4(0.0f, 0.0f, -1.0f, 0.0f);
    }
    ubo.fog_water = glm::vec4(water_state_.surface_level, water_state_.underwater ? 1.0f : 0.0f, 0.0f, 0.0f);
}

void ToyRenderPipeline::record_fog_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
    VkImageView hdr = config_.ssr_enabled ? ssr_pass_->output_view() : offscreen_target_.color_view();
    transparent_pass_->set_targets(hdr, gbuffer_target_.depth_view(), render_extent_.width, render_extent_.height);
    transparent_pass_->begin(cmd, gbuffer_target_.depth_image_handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    coopa::gfx::engine::passes::FogPass::PushConstants pc;
    // Unjittered: the fog reprojects nothing, and a jittered ray would swim against the
    // world-space fog (the same rule the volumetrics UBO follows).
    pc.inv_view_proj = glm::inverse(ctx.unjittered_proj * ctx.view);
    pc.camera_pos    = glm::vec4(ctx.cam_pos, 1.0f);
    fog_pass_->draw(cmd, current_light_set(), pc, render_extent_.width, render_extent_.height);
    transparent_pass_->end(cmd);
    // TransparentPass leaves depth read-only-attachment; everything after expects shader-read.
    transition_gbuffer_depth_to_shader_read_(cmd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    gpu_mark_(cmd, GpuScope::Fog);
}

void ToyRenderPipeline::update_volumetrics_data_(coopa::scene::Scene& scene, const glm::mat4& unjittered_proj,
                              const glm::mat4& view, const glm::vec3& cam_pos,
                              const coopa::gfx::engine::components::DirectionalLightComponent* dir_light) {
    // Volumetrics UBO. Gated on volumetrics_enabled -- when off, record() skips the
    // draw entirely, so this upload would otherwise be wasted work.
    //
    // There is NO global term here: fog above is the global atmosphere, and
    // everything in this buffer is a bounded, scene-placed VolumeComponent that
    // carries its own complete field description. Single-buffered (see VolumetricsData's
    // doc).

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
        vol.sun_color     = glm::vec4(dir_light_color_(*dir_light) * dir_light->intensity, 1.0f);
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
    // update_local_shadows_() have all written the current slot.
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
    // The clouds' shadow on the sun's in-scatter: light shafts through gaps in the clouds.
    vol.cloud_shadow = lubo.cloud_shadow;
    vol.cloud_shadow_layer = lubo.cloud_shadow_layer;
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
    // Every scatter light that holds a local-atlas shadow slot is shadowed in the fog too --
    // point lights included (their shadows live in the same local atlas as spots).
    const bool local_shadowed = config_.volumetrics_shadows_enabled;
    vol.local_shadows = lubo.local_shadows;
    for (uint32_t k = 0; k < scatter_count; ++k) {
        auto& dst = vol.scatter_lights[k];
        if (cands[k].spot) {
            const auto& sl = lubo.spot_lights[cands[k].index];
            dst.position_range  = sl.position_range;
            dst.color_intensity = sl.color_intensity;
            dst.direction_cone  = sl.direction_cone;
            dst.params = glm::vec4(sl.params.x, sl.params.y, 1.0f,
                                   local_shadowed ? sl.params.z : 0.0f);
        } else {
            const auto& pl = lubo.point_lights[cands[k].index];
            dst.position_range  = pl.position_range;
            dst.color_intensity = pl.color_intensity;
            dst.direction_cone  = glm::vec4(0.0f);
            dst.params = glm::vec4(pl.attenuation.x, 0.0f, 0.0f,
                                   local_shadowed ? pl.attenuation.w : 0.0f);
        }
    }
    vol.counts.y = static_cast<float>(scatter_count);
    vol.counts.z = glm::max(config_.volumetrics_light_scatter, 0.0f);
    vol.counts.w = 0.0f;

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

glm::uvec4 ToyRenderPipeline::surface_ext_(const coopa::gfx::engine::components::MeshRenderer& mr,
                        const coopa::gfx::engine::components::PBRMaterial& m) {
    glm::uvec4 ext(0u);
    if (!m.snow) ext.z |= 1u;
    const auto& t = mr.effective_tessellation();
    if (!t.enabled) return ext;
    if (!device_.supports_tessellation()) {
        if (!tess_warned_unsupported_) {
            std::cerr << "[toy::render] tessellation requested but this device has no tessellation stages; drawing untessellated\n";
            tess_warned_unsupported_ = true;
        }
        return ext;
    }
    const bool has = m.is_blended() ? (transparent_pass_ && transparent_pass_->has_tessellated(m.shader))
                                    : gbuffer_pipeline_->has_tessellated(m.shader);
    if (!has) {
        if (tess_warned_shaders_.insert(m.shader).second) {
            std::cerr << "[toy::render] surface shader '" << (m.shader.empty() ? "(stock)" : m.shader)
                      << "' has no tessellated variant (SurfaceShaderDesc::tese); drawing untessellated\n";
        }
        return ext;
    }
    const float max_factor = std::clamp(t.max_factor, 1.0f, static_cast<float>(std::max(1u, device_.max_tessellation_level())));
    ext.x = glm::packHalf2x16(glm::vec2(std::max(t.edge_pixels, 0.5f), max_factor));
    ext.y = glm::packHalf2x16(glm::vec2(std::max(t.max_distance, 0.0f),
                                        m.has_displacement_map() ? m.displacement_scale : 0.0f));
    if (ext.x == 0u) ext.x = 1u;   // never collide with "untessellated"
    return ext;
}

ToyRenderPipeline::DofFocus ToyRenderPipeline::resolve_dof_focus_(const coopa::gfx::engine::components::CameraComponent* cam,
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
            std::cerr << "[toyengine] ToyRenderPipeline: dof focus_object \"" << path
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

void ToyRenderPipeline::update_lights_(coopa::scene::Scene& scene) {
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
    fill_fog_block_(ubo);

    auto* dir = frame_scene_.dir_light;
    ubo.light_counts.x = dir ? 1 : 0;
    if (dir) {
        ubo.dir_direction = glm::vec4(dir->direction, dir->intensity);
        ubo.dir_color     = glm::vec4(dir_light_color_(*dir), 0.0f);
    }

    // The physical sky (sky_physical.glsl); all zero selects the gradient. sky_params.y, the
    // fraction of the cloud target the march filled (0 = no clouds composited), applies to
    // either sky model.
    const float cloud_fill = volumetric_clouds_traced() ? cloud_quality_steps_().cloud_scale : 0.0f;
    if (physical_sky_active()) {
        const SkyFrameState& st = sky_state_;
        ubo.sky_sun    = glm::vec4(st.sun_to, st.sun_disc_cos);
        ubo.sky_moon   = glm::vec4(st.moon_to, st.moon_disc_cos);
        ubo.sky_params = glm::vec4(1.0f, cloud_fill, st.star_visibility, st.sky_illuminance);
        ubo.sky_extra  = glm::vec4(st.sun_disc_radiance, st.moon_disc_radiance, st.night_floor, st.time);
    } else {
        ubo.sky_params = glm::vec4(0.0f, cloud_fill, 0.0f, 0.0f);
    }

    // The cloud layer's shadow map (cloud_shadow.glsl): where it lies, how dark, and how it
    // fades as the light sinks -- sooner for the flat clouds, whose crisp outlines would
    // smear into long bands (and sweep across walls) under a grazing light.
    ubo.cloud_shadow = glm::vec4(0.0f);
    ubo.cloud_shadow_layer = glm::vec4(0.0f);
    if (dir && cloud_shadows_active() && glm::length(dir->direction) > 1e-6f) {
        const CloudFrameState& cs = cloud_state_;
        const float lz = -glm::normalize(dir->direction).z;
        const bool flat = cs.type == CloudType::Flat;
        const float lo = flat ? 0.15f : 0.03f, hi = flat ? 0.4f : 0.15f;
        const float k = std::clamp((lz - lo) / (hi - lo), 0.0f, 1.0f);
        ubo.cloud_shadow = glm::vec4(cloud_shadow_place_.corner, 1.0f / cloud_shadow_place_.side, cs.shadow_strength);
        ubo.cloud_shadow_layer = glm::vec4(cs.altitude, cs.thickness, k * k * (3.0f - 2.0f * k), 0.0f);
    }
    // Soft-shadow tuning shared by every calc_dir_shadow()/calc_local_shadow() call site.
    // Written UNCONDITIONALLY, not only when a directional light exists: .z (PCF taps) and
    // .w (the shared rotation offset) drive the point/spot kernels too, so a scene with only
    // local lights still needs them filled. .x (directional shadow intensity) is meaningless
    // without a directional light and keeps its 1.0 default there.
    //
    // .y is unused and left 0: point-light penumbrae are per light in
    // LightUBO::local_shadows (see update_local_shadows_()).
    // .w rotates the PCF kernel every frame ONLY under TAA, which averages the rotations into
    // a smooth penumbra. Without a temporal resolve after it, a rotating kernel is per-frame
    // noise along every soft shadow edge that never settles: the kernel then holds still.
    ubo.dir_shadow_extra = glm::vec4(
        dir ? glm::clamp(dir->shadow_intensity, 0.0f, 1.0f) : 1.0f,
        0.0f,
        static_cast<float>(std::clamp<uint32_t>(config_.shadow_pcf_samples, 1u, 32u)),
        config_.aa_mode == "taa" ? static_cast<float>(frame_index_ & 0xFFu) : 0.0f);

    // Screen-space contact shadows (toy_lighting.frag's directional block).
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
    // .y is the march's per-frame step rotation: the frame index while the contact resolve
    // accumulates (it integrates the rotating comb), 0 when it does not (nothing would
    // average it, so a still image would shimmer).
    ubo.contact_soft_params = glm::vec4(
        config_.soft_shadows ? std::max(config_.shadow_pcss_light_size, 0.0f) : 0.0f,
        config_.contact_shadow_temporal_enabled ? static_cast<float>(frame_index_ & 0xFFu) : 0.0f,
        0.0f, 0.0f);

    const auto& points = frame_scene_.point_lights;
    uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(points.size()), coopa::gfx::engine::data::MAX_POINT_LIGHTS);
    ubo.light_counts.y = count;
    for (uint32_t i = 0; i < count; ++i) {
        auto* pl = points[i];
        auto& gpu = ubo.point_lights[i];
        gpu.position_range  = glm::vec4(pl->get_world_position(), pl->range);
        gpu.color_intensity = glm::vec4(pl->color, pl->intensity);
        gpu.attenuation     = glm::vec4(pl->attenuation_constant, pl->attenuation_linear,
                                        pl->attenuation_quadratic, 0.0f); // .w: shadow slot, set by update_local_shadows_()
    }

    // Point and spot shadow slots (attenuation.w / params.z) are written afterward by
    // update_local_shadows_(), which picks the shadowed lights.

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
                               0.0f, 0.0f); // .z: shadow slot, set by update_local_shadows_()
    }
}

void ToyRenderPipeline::update_dir_shadow_matrix_(const glm::vec3& direction,
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
    // instead of slices of its frustum (see ToyRenderConfig::shadow_fit). The point is
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
        // offset does not have to clear the PCF disk -- only shadow_normal_bias texels.
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
        // One texel of THIS cascade in its [0,1] light depth: the shader multiplies its
        // constant + slope bias (counted in texels) by this, so the bias tracks each
        // cascade's resolution instead of being a fixed fraction of a depth range that
        // always spans the whole shadow distance toward the sun.
        ubo.dir_cascade_depth_bias[c]  = fit.texel_world / std::max(fit.depth_range_world, 1e-6f);

        if (c < cascades && pcf_texels > 0.0f) any_soft = true;

        if (c == 0) {
            // Cascade 0 mirrored into the pre-cascade fields of the LightUBO prefix
            // (dir_shadow_params.z gates the directional shadow in toy_shadow_body.glsl)
            // -- see light_data.h's cascade doc.
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
    // The PCSS blocker search only reaches past the PCF disk when PCSS is actually on;
    // reserving its radius otherwise just wastes the outer ring of every tile.
    const float inset_texels = (config_.soft_shadows ? pcf_max : 1.0f) +
                               (ubo.pcss_params.x > 0.5f ? pcss_search : 0.0f) + 1.0f;
    const float inset = inset_texels / static_cast<float>(std::max(tile_res, 1u));
    ubo.dir_cascade_info = glm::vec4(
        static_cast<float>(cascades),
        static_cast<float>(coopa::gfx::engine::targets::ShadowMapTarget::grid_for(cascades).first),
        inset, 0.06f);

    // Depth bias (texels; see toy_shadow_bias_texels in toy_shadow_body.glsl) -- shared
    // with the local-light maps, which convert it per pixel.
    ubo.dir_shadow_bias_texels = glm::vec4(std::max(config_.shadow_depth_bias_texels, 0.0f),
                                           std::max(config_.shadow_slope_bias_texels, 0.0f),
                                           std::max(config_.shadow_slope_bias_max, 0.0f), 0.0f);
    // Far fade. Measured from the point the cascades are centred on: the camera for the
    // frustum fit (whose last slice ends at shadow_distance), the focus point for the focus
    // fit (whose last sphere has radius shadow_distance around it -- the camera itself may
    // sit further away than that).
    const float fade = std::clamp(config_.shadow_fade_fraction, 0.0f, 1.0f);
    const glm::vec3 fade_origin = focus_fit ? focus_point
                                : (cam ? glm::vec3(glm::inverse(fit_cam.view)[3]) : glm::vec3(0.0f));
    ubo.dir_shadow_fade = glm::vec4(fade_origin, (cam && fade > 0.0f) ? config_.shadow_distance : 0.0f);
    // Without TAA nothing averages a dithered cascade switch, so blend the two cascades
    // across the band instead (two evaluations there, one everywhere else).
    ubo.dir_shadow_fade_params = glm::vec4(config_.shadow_distance * (1.0f - fade), fade,
                                           config_.aa_mode == "taa" ? 0.0f : 1.0f, 0.0f);
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

bool ToyRenderPipeline::motion_blur_active_(const FrameContext& ctx, DebugView view) const {
    return config_.motion_blur && config_.motion_blur_intensity > 0.0f &&
           (!ctx.cam || ctx.cam->motion_blur) && (view == DebugView::Off || view == DebugView::Lines);
}

uint32_t ToyRenderPipeline::volumetrics_march_dim_(uint32_t full) const {
    if (froxel_volumetrics_()) return 1u;   // froxel mode never marches: keep the target 1x1
    const uint32_t s = std::clamp<uint32_t>(config_.volumetrics_resolution_scale, 1u, 4u);
    return std::max<uint32_t>(1u, (full + s - 1) / s);
}

void ToyRenderPipeline::snapshot_scene_(coopa::scene::Scene& scene) {
    using namespace coopa::gfx::engine::components;
    FrameScene& fs = frame_scene_;
    fs.dir_light = nullptr;
    fs.point_lights.clear();
    fs.spot_lights.clear();
    fs.mesh_renderers.clear();
    fs.sdf_renderers.clear();
    fs.volumes.clear();
    // Depth-first, pruning at an inactive object: disabling a parent hides everything under
    // it (runtime children included -- a water body's tiles, the weather's effects).
    std::function<void(coopa::scene::SceneObject&)> visit = [&fs, &visit](coopa::scene::SceneObject& obj) {
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
            for (const auto& child : obj.children()) visit(*child);
    };
    for (const auto& root : scene.root_objects()) visit(*root);
}

void ToyRenderPipeline::record_directional_shadow_(coopa::gfx::command::CommandBuffer& cmd,
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
                [&](const std::string& shader, bool cull, bool tess) { shadow_pipeline_->bind_directional(cmd, shader, cull, tess); },
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

void ToyRenderPipeline::update_local_shadows_(const glm::vec3& cam_pos, const glm::mat4& camera_vp) {
    using namespace coopa::gfx::engine::data;
    auto& ubo = current_light_data();
    LocalShadowBlock& blk = ubo.local_shadows;
    blk = LocalShadowBlock{};
    local_slots_.clear();
    // The single-spot-shadow fields: no shader reads them (spot shadows come from
    // local_shadows), so they are written in their "no shadow" state.
    ubo.light_counts.w     = 0xFFFFFFFFu;
    ubo.spot_shadow_params = glm::vec4(0.0f);

    std::unordered_set<const void*> selected;
    if (config_.shadows_enabled) {
        const Frustum frustum = Frustum::from_matrix(camera_vp);
        struct Cand { uint32_t kind, index; const void* light; glm::vec3 pos; float range, score; };
        std::vector<Cand> points, spots;
        auto score_of = [&](const glm::vec3& pos, float range, const glm::vec3& color,
                            float intensity, const void* light) {
            WorldBounds b;
            b.center = pos;
            b.extent = glm::vec3(range);
            if (!frustum.intersects(b)) return -1.0f;
            const float d = glm::length(pos - cam_pos);
            float score = range / std::max(d - range, 0.25f * range);
            const float lum = glm::dot(color, glm::vec3(0.2126f, 0.7152f, 0.0722f)) * intensity;
            score *= std::sqrt(std::max(lum, 1e-3f));
            if (local_selected_prev_.count(light)) score *= 1.25f;
            return score;
        };
        const auto& pls = frame_scene_.point_lights;
        const uint32_t np = std::min<uint32_t>(static_cast<uint32_t>(pls.size()), MAX_POINT_LIGHTS);
        for (uint32_t i = 0; i < np; ++i) {
            auto* pl = pls[i];
            if (!pl->cast_shadows || pl->range <= 0.0f || pl->intensity <= 0.0f) continue;
            const glm::vec3 pos = pl->get_world_position();
            const float sc = score_of(pos, pl->range, pl->color, pl->intensity, pl);
            if (sc >= 0.0f) points.push_back({2u, i, pl, pos, pl->range, sc});
        }
        const auto& sls = frame_scene_.spot_lights;
        const uint32_t ns = std::min<uint32_t>(static_cast<uint32_t>(sls.size()), MAX_SPOT_LIGHTS);
        for (uint32_t i = 0; i < ns; ++i) {
            auto* sl = sls[i];
            if (!sl->cast_shadows || sl->range <= 0.0f || sl->intensity <= 0.0f) continue;
            const glm::vec3 pos = sl->get_world_position();
            const float sc = score_of(pos, sl->range, sl->color, sl->intensity, sl);
            if (sc >= 0.0f) spots.push_back({1u, i, sl, pos, sl->range, sc});
        }
        auto keep_best = [](std::vector<Cand>& v, uint32_t budget) {
            std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
            if (v.size() > budget) v.resize(budget);
            // Stable placement: pack in scene order, so tiles do not shuffle with the ranking.
            std::sort(v.begin(), v.end(), [](const Cand& a, const Cand& b) { return a.index < b.index; });
        };
        keep_best(points, config_.max_shadowed_point_lights);
        keep_best(spots,  config_.max_shadowed_spot_lights);

        std::vector<Cand> chosen = points;
        chosen.insert(chosen.end(), spots.begin(), spots.end());
        const uint32_t atlas = local_shadow_atlas_->size();
        const uint32_t cube_res = std::max(config_.cube_shadow_resolution, 16u);
        const uint32_t spot_res = std::max(config_.spot_shadow_resolution, 16u);
        std::vector<std::pair<uint32_t, uint32_t>> sizes;
        for (const Cand& c : chosen) {
            sizes.push_back(c.kind == 2 ? std::make_pair(3u * cube_res, 2u * cube_res)
                                        : std::make_pair(spot_res, spot_res));
        }
        const std::vector<AtlasRect> rects = pack_shadow_atlas(sizes, atlas);

        const bool  soft    = config_.soft_shadows;
        const float pcf_max = std::max(config_.shadow_pcf_max_texels, 1.0f);
        uint32_t next_view = 0;
        for (size_t k = 0; k < chosen.size(); ++k) {
            const Cand& c = chosen[k];
            const uint32_t views = (c.kind == 2) ? 6u : 1u;
            if (rects[k].w == 0 || local_slots_.size() >= MAX_LOCAL_SHADOWS ||
                next_view + views > MAX_LOCAL_SHADOW_VIEWS) {
                if (!warned_local_atlas_full_) {
                    std::cerr << "[toy::render] Local shadow atlas (" << atlas << "^2) is full: some "
                                 "cast_shadows point/spot lights are unshadowed this frame. Raise "
                                 "local_shadow_atlas_resolution or lower cube/spot_shadow_resolution.\n";
                    warned_local_atlas_full_ = true;
                }
                continue;
            }
            const uint32_t slot = static_cast<uint32_t>(local_slots_.size());
            LocalShadowSlot ls;
            ls.kind       = c.kind;
            ls.light      = c.light;
            ls.gpu_index  = c.index;
            ls.pos        = c.pos;
            ls.range      = c.range;
            ls.first_view = next_view;
            ls.view_count = views;
            ls.block      = rects[k];
            ls.tile_res   = (c.kind == 2) ? cube_res : spot_res;

            const float near_p = std::min(0.1f, c.range * 0.05f);
            const float far_p  = std::max(c.range, near_p * 2.0f);
            LocalShadowGPU& g = blk.shadows[slot];
            float tan_half = 1.0f;
            if (c.kind == 2) {
                const float softness = soft ? std::clamp(config_.point_shadow_softness, 0.0f, 8.0f) : 0.0f;
                const float guard    = std::ceil(softness) + 2.0f;
                tan_half = static_cast<float>(cube_res) / (static_cast<float>(cube_res) - 2.0f * guard);
                const glm::mat4 proj = glm::perspectiveRH_ZO(2.0f * std::atan(tan_half), 1.0f, near_p, far_p);
                for (uint32_t f = 0; f < 6; ++f) {
                    blk.view_proj[next_view + f] = proj * local_shadow_point_face_view(f, c.pos);
                }
                g.pcf = glm::vec4(0.0f, std::min(softness, guard - 1.5f), guard - 1.5f, 1.0f);
                ubo.point_lights[c.index].attenuation.w = static_cast<float>(slot + 1);
            } else {
                const auto* sl = static_cast<const coopa::gfx::engine::components::SpotLightComponent*>(c.light);
                const float outer = glm::radians(sl->clamped_outer_angle());
                tan_half = std::tan(outer);
                const glm::vec3 dir = sl->get_world_direction();
                // Z-up engine: a near-vertical aim needs another up axis for lookAt.
                const glm::vec3 up = (std::abs(dir.z) < 0.99f) ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                                : glm::vec3(0.0f, 1.0f, 0.0f);
                blk.view_proj[next_view] = glm::perspectiveRH_ZO(2.0f * outer, 1.0f, near_p, far_p) *
                                           glm::lookAt(c.pos, c.pos + dir, up);
                g.pcf = glm::vec4(soft ? std::max(config_.spot_shadow_softness, 0.0f) : 0.0f,
                                     0.0f, pcf_max, 1.0f);
                ubo.spot_lights[c.index].params.z = static_cast<float>(slot + 1);
            }
            g.tile  = glm::vec4(static_cast<float>(rects[k].x) / static_cast<float>(atlas),
                                static_cast<float>(rects[k].y) / static_cast<float>(atlas),
                                static_cast<float>(ls.tile_res) / static_cast<float>(atlas),
                                static_cast<float>(c.kind));
            g.light = glm::vec4(c.pos, far_p);
            g.proj  = glm::vec4(near_p, tan_half, static_cast<float>(ls.tile_res),
                                static_cast<float>(next_view));
            next_view += views;
            local_slots_.push_back(ls);
            selected.insert(c.light);
        }
        blk.info = glm::vec4(static_cast<float>(local_slots_.size()), static_cast<float>(atlas),
                             std::max(config_.shadow_normal_bias, 0.0f), 0.0f);
        blk.bias = glm::vec4(std::max(config_.shadow_depth_bias_texels, 0.0f),
                             std::max(config_.shadow_slope_bias_texels, 0.0f),
                             std::max(config_.shadow_slope_bias_max, 0.0f), 0.0f);
    }
    local_selected_prev_ = std::move(selected);
    // A light that lost its slot no longer owns its tiles: another light may be packed there
    // now, so its cached static depth must not be trusted when it comes back.
    for (auto it = local_cache_.begin(); it != local_cache_.end();) {
        if (!local_selected_prev_.count(it->first)) it = local_cache_.erase(it); else ++it;
    }
}

void ToyRenderPipeline::record_local_shadows_(coopa::gfx::command::CommandBuffer& cmd,
                           const MeshGather& meshes,
                           const std::vector<SdfDrawItem>& sdf_draws) {
    local_shadow_atlas_->initialize(cmd);
    if (local_slots_.empty()) return;   // nothing samples the atlas this frame
    const bool caching = local_shadow_atlas_->caching();
    const auto& blk = current_light_data().local_shadows;

    auto draw_view = [&](uint32_t view, const std::vector<MeshBatch>& batches, bool with_sdf,
                         const AtlasRect& rect) {
        local_shadow_atlas_->set_tile(cmd, rect);
        shadow_pipeline_->bind_directional(cmd);
        cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);
        coopa::gfx::engine::passes::DirectionalShadowPushConstants pc{};
        pc.light_space_matrix = blk.view_proj[view];
        pc.gfx_time = surface_gfx_time_();
        // Same per-material variant binding as the directional cascades, so a displaced
        // caster's shadow moves exactly as its G-buffer draw does.
        draw_shadow_batches_(cmd, meshes, batches, pc,
            [&](const std::string& shader, bool cull, bool tess) { shadow_pipeline_->bind_directional(cmd, shader, cull, tess); },
            [&](const auto& p) { shadow_pipeline_->push_directional(cmd, p); });
        if (with_sdf && !sdf_draws.empty()) {
            sdf_shadow_pass_->bind_directional(cmd);
            cmd.bind_descriptor_set(sdf_data_.current_set(), 0);
            coopa::gfx::engine::passes::SdfDirectionalShadowPushConstants sdf_pc{};
            sdf_pc.light_space_matrix = blk.view_proj[view];
            sdf_pc.shadow_max_steps   = config_.sdf_shadow_max_steps;
            draw_sdf_shadow_casters_(cmd, sdf_draws, sdf_pc.light_space_matrix, rect.x, rect.y, rect.w,
                                     [&](uint32_t gpu_index) {
                                         sdf_pc.renderer_index = gpu_index;
                                         sdf_shadow_pass_->push_directional(cmd, sdf_pc);
                                     });
        }
    };

    local_shadow_atlas_->begin_frame(cmd);
    if (caching) {
        const bool any_static = std::any_of(local_slots_.begin(), local_slots_.end(),
                                            [](const LocalShadowSlot& s) { return s.rerender_static; });
        if (any_static) {
            local_shadow_atlas_->begin_static_pass(cmd);
            for (const LocalShadowSlot& slot : local_slots_) {
                if (!slot.rerender_static) continue;
                local_shadow_atlas_->clear_tile(cmd, slot.block);
                for (uint32_t v = 0; v < slot.view_count; ++v) {
                    draw_view(slot.first_view + v, meshes.local_static[slot.first_view + v], false,
                              slot.view_rect(v));
                }
            }
            local_shadow_atlas_->end_static_pass(cmd);
        }
        std::vector<AtlasRect> blocks;
        for (const LocalShadowSlot& slot : local_slots_) blocks.push_back(slot.block);
        local_shadow_atlas_->copy_static(cmd, blocks);
    }
    local_shadow_atlas_->begin_dynamic_pass(cmd);
    for (const LocalShadowSlot& slot : local_slots_) {
        if (!caching) local_shadow_atlas_->clear_tile(cmd, slot.block);
        for (uint32_t v = 0; v < slot.view_count; ++v) {
            draw_view(slot.first_view + v, meshes.local_dynamic[slot.first_view + v], true,
                      slot.view_rect(v));
        }
    }
    local_shadow_atlas_->end_dynamic_pass(cmd);
}

void ToyRenderPipeline::transition_gbuffer_depth_to_shader_read_(
    coopa::gfx::command::CommandBuffer& cmd,
    VkImageLayout from_layout)
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

void ToyRenderPipeline::record_focus_probe_(coopa::gfx::command::CommandBuffer& cmd) {
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

void ToyRenderPipeline::read_focus_probe_(uint32_t slot, const glm::mat4& view, float dt) {
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

void ToyRenderPipeline::record_gbuffer_(coopa::gfx::command::CommandBuffer& cmd,
                     const MeshGather& meshes,
                     const std::vector<SdfDrawItem>& sdf_draws) {
    gbuffer_target_.begin(cmd);
    // Stock pipeline bound first so a scene with no derived shaders (the overwhelming
    // common case) pays for exactly one bind, as before this pass gained variants.
    gbuffer_pipeline_->bind(cmd);
    cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), current_camera_set(), 0);
    // Set 2: the surface world (snow, occlusion, trenches, tessellation view) -- every
    // G-buffer variant shares this layout, so one bind serves the whole pass.
    cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), surface_world_->set(current_frame_slot_()), 2);
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
    bool last_tess = false;
    bool have_bound = true; // stock, bound just above

    const void* last_set = nullptr;
    const coopa::gfx::engine::data::Mesh* last_mesh = nullptr;
    // Already frustum-culled and batched (see gather_meshes_): one instanced draw per run
    // of identical mesh + LOD + material. BLEND materials never appear here -- they are
    // drawn by transparent_pass_ instead, after SSR compositing (see record_transparent_).
    for (const MeshBatch& batch : meshes.gbuffer) {
        auto* mr = meshes.renderers[batch.item];
        const auto& mr_mat = meshes.material(batch.item);

        const bool tess = meshes.ext[batch.item].x != 0u;
        if (!have_bound || mr_mat.shader != last_shader ||
            mr_mat.cull_backfaces != last_cull_backfaces || tess != last_tess) {
            gbuffer_pipeline_->bind(cmd, mr_mat.shader, mr_mat.cull_backfaces, tess);
            last_shader         = mr_mat.shader;
            last_cull_backfaces = mr_mat.cull_backfaces;
            last_tess           = tess;
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
        pc.surface_ext  = meshes.ext[batch.item];
        gbuffer_pipeline_->push(cmd, pc);
        if (tess) frame_stats_.tess_draws += 1;
        // Set 1: alpha-mask sampler (white 1x1 fallback unless this is a CUTOUT material
        // with a loaded texture_alpha_mask) -- see MaterialTextureCache.
        // The editor's Solid / Wireframe shading shows material colours, not textures.
        const auto& set = untextured_view ? material_cache_->untextured_set_for(mr_mat) : material_cache_->set_for(mr_mat);
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

void ToyRenderPipeline::record_transparent_preview_(coopa::gfx::command::CommandBuffer& cmd, const MeshGather& meshes,
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

void ToyRenderPipeline::fill_forward_globals_(ForwardGlobals& g) const {
    g.lighting0 = glm::vec4(config_.light_bands, config_.spec_threshold,
                            config_.soft_lighting ? 1.0f : 0.0f, config_.rim_strength);
    g.lighting1 = glm::vec4(config_.indirect.ambient_intensity, config_.indirect.sky_intensity,
                            config_.ssr_enabled ? 1.0f : 0.0f, config_.indirect.ssgi_intensity);
    g.ssr0 = glm::vec4(config_.indirect.ssgi_distance, config_.ssr_max_distance,
                      config_.ssr_bias_texels, config_.ssr_thickness);
    g.ssr1 = glm::vec4(config_.ssr_thickness_scale, config_.ssr_roughness_cutoff,
                      config_.ssr_cone_prefilter, 0.0f);
    g.ssr_steps = glm::ivec4(config_.ssr_max_iterations,
                             static_cast<int>(hiz_pass_->max_mip_level()),
                             config_.ssr_start_mip, config_.ssr_min_mip0_steps);
    // y: u_scene_color holds the previous frame's final colour. Only for the SSR chain: with
    // refraction active the mesh forward pass binds refraction's own chain instead, built
    // from THIS frame's post-SSR image, whose hits must not be reprojected.
    const bool forward_prev_frame = scene_color_history_valid_ &&
        !(config_.transparency_enabled && config_.refraction_enabled);
    g.ssr_mip = glm::ivec4(static_cast<int>(scene_color_mip_pass_->max_mip_level()),
                           forward_prev_frame ? 1 : 0, 0, 0);
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

passes::ParticlePass::PushConstants ToyRenderPipeline::particle_push_constants_(const ParticleDrawBatch& b) const {
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
    pc.ambient = glm::vec4(config_.indirect.ambient_intensity,
                           (l.receive_shadows && config_.shadows_enabled) ? 1.0f : 0.0f, 0.0f, 0.0f);
    pc.extra   = glm::vec4(0.0f, glm::clamp(l.reactive, 0.0f, 1.0f), std::max(l.scatter, 0.0f), l.scatter_anisotropy);
    return pc;
}

void ToyRenderPipeline::record_reactive_mask_(coopa::gfx::command::CommandBuffer& cmd) {
    if (!reactive_target_) return;
    reactive_target_->begin(cmd, VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}});
    if (particle_pass_ && particle_pass_->has_reactive() && particle_pass_->total_instances() > 0) {
        bool bound = false;
        for (size_t i = 0; i < particle_state_.quads.size(); ++i) {
            const ParticleDrawBatch& pb = particle_state_.quads[i];
            if (pb.count == 0 || pb.look.reactive <= 0.0f) continue;
            if (!bound) {
                particle_pass_->bind_reactive(cmd);
                cmd.bind_descriptor_set(current_camera_set(), 0);
                cmd.bind_descriptor_set(current_light_set(), 1);
                cmd.bind_descriptor_set(ssr_pass_->hiz_set(), 2);
                cmd.bind_descriptor_set(*shadow_set_, 4);
                bound = true;
            }
            cmd.bind_descriptor_set(material_cache_->set_for(pb.texture_material ? *pb.texture_material
                                                                                  : particle_default_material_), 3);
            passes::ParticlePass::PushConstants pc = particle_push_constants_(pb);
            pc.extra.x = 1.0f;
            particle_pass_->draw(cmd, i, pb.count, pc);
        }
    }
    reactive_target_->end(cmd);
}

bool ToyRenderPipeline::record_transparent_(coopa::gfx::command::CommandBuffer& cmd,
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
    bool last_mesh_tess = false;

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
                cmd.bind_descriptor_set(*shadow_set_, 4);
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
            const glm::uvec4 ext = meshes.ext[item.index];
            const bool tess = ext.x != 0u;
            if (tess) frame_stats_.tess_draws += 1;
            if (last_kind != 0 || mr_mat.shader != last_mesh_shader || tess != last_mesh_tess) {
                transparent_pass_->bind(cmd, mr_mat.shader, tess);
                last_mesh_shader = mr_mat.shader;
                last_mesh_tess   = tess;
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
            // Each field falls back to its ToyRenderConfig engine-wide default when the
            // material left it at PBRMaterial's negative sentinel (see that struct's own
            // doc for why -1 rather than baking the default straight into PBRMaterial).
            TransparentRefractionPushConstants refract_pc;
            float refract_ior       = mr_mat.ior >= 0.0f ? mr_mat.ior : config_.refraction_ior;
            float refract_thickness = mr_mat.refraction_thickness >= 0.0f
                                          ? mr_mat.refraction_thickness : config_.refraction_thickness;
            glm::vec3 refract_tint  = mr_mat.refraction_tint.r >= 0.0f
                                          ? mr_mat.refraction_tint : config_.refraction_tint;
            refract_pc.tint_thickness = glm::vec4(refract_tint, refract_thickness);
            // zw: the packed tessellation params, as raw bits (gfx/surface/transparent_tes.glsl
            // reads them back with floatBitsToUint) -- never arithmetic on them on the way.
            refract_pc.ior_flags      = glm::vec4(refract_ior, mr_mat.has_refraction() ? 1.0f : 0.0f, 0.0f, 0.0f);
            std::memcpy(&refract_pc.ior_flags.z, &ext.x, sizeof(uint32_t));
            std::memcpy(&refract_pc.ior_flags.w, &ext.y, sizeof(uint32_t));
            refract_pc.shader_ext0    = mr_mat.shader_params_ext[0];
            refract_pc.shader_ext1    = mr_mat.shader_params_ext[1];
            // VERTEX|FRAGMENT, not FRAGMENT alone: TransparentPass's push-constant range
            // covers both stages (see its PushConstants' gfx_time/gfx_params doc), and
            // Vulkan requires a push call's stageFlags to match the declared range for
            // every byte it touches, including this trailing per-object refraction block.
            cmd.push_constants(transparent_pass_->layout(),
                               coopa::gfx::detail::to_vk(transparent_pass_->push_stages()),
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
    // stylize_pass_'s later outline/dither pass expects the whole render area.
    cmd.set_scissor(0, 0, render_extent_.width, render_extent_.height);

    transparent_pass_->end(cmd);
    return true;
}

bool ToyRenderPipeline::ContactShadowKnobs::operator==(const ContactShadowKnobs& o) const {
    return length == o.length && strength == o.strength && thickness == o.thickness
        && steps == o.steps && enabled == o.enabled;
}

} // namespace render
} // namespace toy
