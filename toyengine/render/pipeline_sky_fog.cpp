/**
 * @file pipeline_sky_fog.cpp
 * @brief Sky, clouds, fog and volumetrics -- the atmosphere half of the frame.
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

void ToyRenderPipeline::record_physical_sky_(coopa::gfx::command::CommandBuffer& cmd, const FrameContext& ctx) {
    sky_atmosphere_pass_->initialize(cmd);
    sky_cloud_pass_->initialize(cmd);
    cloud_shadow_pass_->initialize(cmd);
    if (physical_sky_active()) {
        const SkyFrameState& st = sky_state_;
        sky_atmosphere_pass_->update_luts(cmd, st.media);
        passes::SkyAtmospherePass::ViewParams vp;
        vp.atmo = passes::SkyAtmospherePass::AtmosphereGpu::of(st.media);
        vp.sun = glm::vec4(planet_ ? sky_frame_() * st.sun_to : st.sun_to, st.moon_ratio);
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
    if (planet_) {
        // On a planet world Z is no height: the fog is plain distance fog there (no height
        // falloff, no blend toward the world-oriented sky gradient, no world-Z water cut). The
        // shaders are untouched -- these are values they already handle.
        ubo.fog_height.y = 0.0f;
        ubo.fog_height.z = 0.0f;
        ubo.fog_water    = glm::vec4(0.0f);
    }
}

glm::mat3 ToyRenderPipeline::sky_frame_() const {
    if (!planet_) return glm::mat3(1.0f);
    const glm::vec3 d = frame_cam_pos_ - planet_->centre;
    if (glm::length(d) < 1e-4f) return glm::mat3(1.0f);
    const glm::vec3 up = glm::normalize(d);
    // The shortest turn taking `up` to +Z (any turn about +Z after it would do: the sky only
    // depends on directions relative to the up and the sun, which turn together).
    const glm::vec3 z(0.0f, 0.0f, 1.0f);
    if (glm::dot(up, z) < -0.9999f) return glm::mat3(glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f)));
    return glm::mat3(glm::rotation(up, z));
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
        glm::vec3 dir = (dir_len > 1e-5f) ? vc->direction / dir_len
                                          : glm::vec3(1.0f, 0.0f, 0.0f);
        float speed = vc->speed;
        // The weather's wind joins the authored drift (VolumeComponent::wind_influence).
        if (vc->wind_influence != 0.0f) {
            // The MEAN weather wind (deliberately ungusted: gusts would make every existing
            // volume's drift wobble) plus local influences, so a rotor stirs a smoke pocket.
            const glm::vec3 drift = dir * speed +
                (surface_state_.wind + env::local_wind_at(surface_state_.env, glm::vec3(world[3]))) *
                vc->wind_influence;
            const float dl = glm::length(drift);
            if (dl > 1e-4f) { dir = drift / dl; speed = dl; }
            else speed = 0.0f;
        }
        gpu.direction_speed = glm::vec4(dir, speed);
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
    // Shadow levels too, not just the near matrix: a sun shaft marches the whole
    // volumetrics_max_distance, so a march point past the near level must find its own tile
    // of the atlas or it would come back unshadowed. volumetrics_march.frag runs the same
    // containment select calc_dir_shadow() does -- see vol_shadow_vis_cascaded().
    //
    // Only the NEAREST MAX_DIR_CASCADES levels are forwarded, even when more are live. The
    // march hoists two vec4s per level per pixel (volumetrics_march.frag's cso/csd), so
    // carrying all eight would double that register pressure for shaft shadows that are a
    // near-camera effect; its loop already clamps to min(dir_cascade_info.x, VOL_MAX_CASCADES).
    // dir_cascade_info is forwarded VERBATIM rather than with a reduced count, because the
    // tile-uv remap derives grid_y from count / grid_x -- a reduced count there would compute
    // the wrong tile height and sample the wrong half of the atlas.
    for (uint32_t c = 0; c < coopa::gfx::engine::data::MAX_DIR_CASCADES; ++c) {
        vol.dir_cascade_matrix[c] = lubo.dir_level_matrix[c];
    }
    vol.dir_cascade_info = lubo.dir_level_info;
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
    // Set before the tessellation early-returns below: the toon look has nothing to do with
    // whether this draw is tessellated, and an untessellated toon material must still carry
    // its code to the G-buffer.
    ext.w = m.gpu_toon_code();
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

glm::vec4 ToyRenderPipeline::surface_gfx_time_() const {
    const float water_time = water_state_.time >= 0.0f ? water_state_.time : elapsed_time_;
    return glm::vec4(elapsed_time_, frame_dt_, static_cast<float>(frame_index_), water_time);
}

uint32_t ToyRenderPipeline::volumetrics_march_dim_(uint32_t full) const {
    if (froxel_volumetrics_()) return 1u;   // froxel mode never marches: keep the target 1x1
    const uint32_t s = std::clamp<uint32_t>(config_.volumetrics_resolution_scale, 1u, 4u);
    return std::max<uint32_t>(1u, (full + s - 1) / s);
}
} // namespace render
} // namespace toy
