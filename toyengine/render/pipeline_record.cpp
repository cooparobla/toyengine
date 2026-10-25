/**
 * @file pipeline_record.cpp
 * @brief Per-frame command recording: G-buffer, transparency, the post chain and the
 *        overlay, plus the small per-frame parameter fills they read.
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
    // On a planet the sky is drawn in the camera's own frame there (sky_frame_()).
    if (planet_) lighting_pc.sky_inv_view_proj = glm::mat4(sky_frame_()) * lighting_pc.sky_inv_view_proj;
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
        // `toon` restricts the outline to materials that asked for one
        // (PBRMaterial::toon_outline); anything else, including an unknown value, outlines
        // every edge as it always has.
        post_pc.outline_mode     = (config_.outline_mode == "toon") ? 1.0f : 0.0f;
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
        ui_world_target_->begin(cmd, coopa::gfx::ClearColor{0.0f, 0.0f, 0.0f, 0.0f});
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
    // an overlay, not part of the upscaled scene image, so it is not quantised to the render grid. It
    // sets its own viewport and per-batch scissor.
    overlay_target_->begin(cmd, coopa::gfx::ClearColor{0.0f, 0.0f, 0.0f, 1.0f});
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
    const void* last_terrain_set = nullptr;
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
            last_terrain_set    = nullptr;
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
        // Set 3: a heightmap terrain chunk's splats and layer arrays, found by its height grid
        // (the material's displacement map) -- see terrain_gpu.h.
        if (mr_mat.shader == "heightmap_terrain") {
            const auto& tset = terrain_gpu_->set_for(mr_mat.displacement_handle.is_loaded()
                                                         ? mr_mat.displacement_handle.get() : nullptr);
            if (&tset != last_terrain_set) {
                cmd.bind_descriptor_set(gbuffer_pipeline_->layout(), tset, 3);
                last_terrain_set = &tset;
            }
        }

        const auto* mesh = mr->get_mesh().get();
        if (mesh != last_mesh) {
            mesh->bind(cmd);
            last_mesh = mesh;
        }
        if (batch.own_instances) cmd.bind_vertex_buffer(*batch.own_instances, 0, 1);
        mesh->draw_lod_part(cmd, batch.lod, meshes.part[batch.item], batch.instance_count, batch.first_instance);
        if (batch.own_instances) {
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);
            frame_stats_.static_instances += batch.instance_count;
        }
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
        if (mr_mat.shader == "water" || mr_mat.shader == "water_planet") {
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

void ToyRenderPipeline::record_reactive_mask_(coopa::gfx::command::CommandBuffer& cmd) {
    if (!reactive_target_) return;
    reactive_target_->begin(cmd, coopa::gfx::ClearColor{0.0f, 0.0f, 0.0f, 0.0f});
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
        regions[i].bufferOffset     = static_cast<VkDeviceSize>(i) * 16u; // one RGBA32F texel
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
    float texels[kFocusProbeSamples * 4] = {};
    focus_probes_[slot]->download(texels, sizeof(texels));
    for (uint32_t i = 0; i < kFocusProbeSamples; ++i) {
        const float roughness = texels[i * 4 + 3];
        if (!(roughness > 0.01f)) continue;
        const glm::vec3 hit(texels[i * 4 + 0], texels[i * 4 + 1], texels[i * 4 + 2]);
        const float depth = view_space_depth(view, hit);
        if (!(depth > 0.0f)) continue;
        probed_focus_distance_ = probed_focus_distance_ > 0.0f
            ? exp_smooth_toward(probed_focus_distance_, depth, config_.dof_focus_smoothing, dt)
            : depth;
        return;
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
    if (w.underwater && w.surface_up) {
        p.camera_time = glm::vec4(*w.surface_up, elapsed_time_);
        p.water.w     = 2.0f;
    }
    p.fog_color     = glm::vec4(w.fog_color, 1.0f);
    p.absorption    = glm::vec4(w.absorption, light);
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

bool ToyRenderPipeline::motion_blur_active_(const FrameContext& ctx, DebugView view) const {
    return config_.motion_blur && config_.motion_blur_intensity > 0.0f &&
           (!ctx.cam || ctx.cam->motion_blur) && (view == DebugView::Off || view == DebugView::Lines);
}
} // namespace render
} // namespace toy
