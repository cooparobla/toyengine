/**
 * @file pipeline_shadows.cpp
 * @brief Lights and shadows: the light UBO, directional cascade fitting and recording,
 *        and the local (point/spot) shadow atlas.
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
        // (On a planet the sky's directions are in the camera's own frame: see sky_frame_().)
        const glm::mat3 frame = planet_ ? sky_frame_() : glm::mat3(1.0f);
        ubo.sky_sun    = glm::vec4(planet_ ? frame * st.sun_to : st.sun_to, st.sun_disc_cos);
        ubo.sky_moon   = glm::vec4(planet_ ? frame * st.moon_to : st.moon_to, st.moon_disc_cos);
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
    // Per-material toon shading, scene-wide half (gfx/toon.glsl). The per-material half rides
    // the G-buffer's emissive alpha instead -- see surface_ext_(). Written unconditionally:
    // toon_strength 0 is itself the meaningful value (every toon material shades as plain PBR),
    // and gfx_toon_active() tests it, so there is nothing to branch on here.
    ubo.toon_params0 = glm::vec4(config_.toon_spec_threshold, config_.toon_spec_edge,
                                 config_.toon_rim_power, std::clamp(config_.toon_strength, 0.0f, 1.0f));
    ubo.toon_params1 = glm::vec4(config_.toon_shadow_tint, config_.toon_band_attenuation ? 1.0f : 0.0f);

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
    // shadow_fit "clipmap": concentric rings CENTRED ON THE CAMERA, radius growing
    // geometrically, each snapped to a coarse multiple of its own texel size. Camera-position
    // anchoring (not the frustum, not the gaze point) is what makes every matrix invariant to
    // where the camera LOOKS and how far it is zoomed out, and the coarse snap is what holds
    // each matrix still between steps -- together those are what let a level's depth be reused
    // across frames (DirShadowCache, phase 3). The reach is an output here: the outermost ring
    // is shadow_clip_radius * ratio^(count-1), so shadow_distance does not bound the fit.
    const bool clipmap_fit = cam && config_.shadow_fit == "clipmap";
    std::vector<float> clip_radii;
    glm::vec3 clip_centre(0.0f);
    glm::vec3 clip_direction = direction;
    if (clipmap_fit) {
        clip_radii  = compute_clipmap_level_radii(config_.shadow_clip_radius,
                                                  config_.shadow_clip_ratio, cascades);
        clip_centre = glm::vec3(glm::inverse(fit_cam.view)[3]);
        // A sun that moves (a day/night cycle) otherwise re-bases every level every frame,
        // which dirties every cached level every frame -- see shadow_clip_sun_snap_degrees.
        // ONLY the fit is snapped; ubo.dir_direction below keeps the true direction, so
        // shading, specular and the sky are all still continuous.
        clip_direction = snap_light_direction(direction, config_.shadow_clip_sun_snap_degrees);
    }

    const bool focus_fit = cam && !clipmap_fit && config_.shadow_fit == "focus";
    glm::vec3 focus_point(0.0f);
    std::vector<float> focus_radii;
    if (focus_fit) {
        const glm::mat4 cam_to_world = glm::inverse(fit_cam.view);
        float d = config_.shadow_focus_distance > 0.0f ? config_.shadow_focus_distance : focus_distance;
        if (!(d > 0.0f)) d = config_.shadow_focus_radius;
        focus_point = glm::vec3(cam_to_world[3]) - glm::vec3(cam_to_world[2]) * d;
        focus_radii = compute_focus_cascade_radii(config_.shadow_focus_radius,
                                                  config_.shadow_distance, cascades);
        // Per-cascade overrides (ToyRenderConfig::shadow_cascade_radius). Applied in order and
        // only when they keep the radii increasing: selection walks finest-first and a slot that
        // did not grow would shadow the one before it, leaving a cascade that can never be hit.
        for (size_t ci = 0; ci < focus_radii.size() && ci < 4; ++ci) {
            const float wanted = config_.shadow_cascade_radius[ci];
            if (wanted <= 0.0f) continue;
            const float floor_r = ci == 0 ? 0.01f : focus_radii[ci - 1] * 1.01f;
            focus_radii[ci] = std::max(wanted, floor_r);
        }
    }
    const float pcf_max = std::max(config_.shadow_pcf_max_texels, 1.0f);

    const bool pcss_on_config = config_.shadow_pcss_enabled;
    const float pcss_search = glm::clamp(config_.shadow_pcss_search_texels, 1.0f, 16.0f);

    auto& ubo = current_light_data();

    bool any_soft = false;
    const uint32_t clip_snap = std::clamp(config_.shadow_clip_snap_texels, 1u, 128u);

    // --- Which levels re-render their static depth this frame (see DirLevelState) ---
    //
    // Two kinds of dirt, handled differently because they fail differently:
    //
    //  * The MATRIX moved (the camera crossed a snap cell). Frequent on the near rings, and
    //    a level left stale merely stops covering a sliver at its edge, which containment
    //    selection hands to the next ring out. Budget-limited and round-robined.
    //  * The static CONTENT changed (a terrain chunk streamed in or out, an LOD flipped, a
    //    material was edited). Rare, but a stale level there means a missing or a ghost
    //    shadow rather than a coverage sliver -- so every affected level re-renders at once.
    //
    // The decision is made HERE, before gather_meshes_, because gather culls and batches
    // against the matrix each level will actually publish. gather may still promote a level
    // (it recomputes the content hash while culling), which is safe: it runs before recording.
    const bool caching = dir_cache_active_();
    const uint32_t fresh_levels = std::min(config_.shadow_clip_fresh_levels, cascades);
    uint32_t static_budget = config_.shadow_clip_static_budget;
    for (uint32_t c = 0; c < coopa::gfx::engine::data::MAX_DIR_SHADOW_LEVELS; ++c) {
        // Slots past the live count repeat the last cascade rather than holding an
        // identity matrix: the shader's containment loop stops at the count, but a stale
        // identity here would project every world position into the same degenerate tile
        // if that bound were ever read wrong.
        const uint32_t i = std::min(c, cascades - 1u);
        const float slice_near = (i == 0) ? near_clip : splits.distance[i - 1];
        // For the slice and focus fits, caster_reach is the whole shadowed range: those
        // ranges are tens of metres, so the long box costs little. A clipmap's outermost ring
        // is kilometres, and sizing every level off THAT would have the finest 9 m ring
        // gather every caster in a kilometre-long column toward the sun. Each clipmap level
        // instead reaches past itself by its own size, floored by shadow_clip_caster_reach.
        const float level_reach = clipmap_fit
            ? std::max(2.0f * clip_radii[i], std::max(config_.shadow_clip_caster_reach, 0.0f))
            : config_.shadow_distance;
        const DirShadowFit fit = clipmap_fit
            ? compute_dir_shadow_fit_sphere(clip_direction, clip_centre, clip_radii[i],
                                            level_reach, tile_res, clip_snap)
            : focus_fit
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
        // Publish the fit whose depth the tile actually holds. Without caching that is
        // always this frame's fit; with caching a level that is dirty but not granted a
        // re-render keeps the fit it was last drawn with, so the matrix the shader samples
        // with and the content in the tile can never disagree.
        DirLevelState& lvl = dir_levels_[c];
        lvl.wanted = fit;
        bool grant = !caching || !lvl.valid;
        if (caching && lvl.valid) {
            // Sticky: a level dirtied by a matrix step or by a content change stays dirty
            // until it is actually granted a redraw, so the budget works through a backlog
            // instead of forgetting it the moment the matrix stops moving.
            lvl.dirty = lvl.dirty || lvl.published.light_space_matrix != fit.light_space_matrix;
            if (c < fresh_levels) {
                grant = lvl.dirty;        // the nearest levels are never deferred
            } else if (lvl.dirty && static_budget > 0) {
                // Round-robin from a rotating cursor so a near level dirtying every frame
                // cannot starve an outer one that has been waiting.
                const uint32_t turn = (dir_level_cursor_ + c) % std::max(cascades, 1u);
                grant = turn < static_budget || c == dir_level_cursor_;
                if (grant) --static_budget;
            }
        }
        if (grant) {
            lvl.published = fit;
            lvl.valid = true;
            lvl.dirty = false;
            lvl.age = 0;
        } else if (c < cascades) {
            lvl.age = std::min(lvl.age + 1u, 1u << 20);
        }
        lvl.render_static = grant && caching;
        const DirShadowFit& pub = lvl.published;

        const float pcf_texels = config_.soft_shadows
            ? std::min(config_.shadow_softness / std::max(pub.texel_world, 1e-6f), pcf_max)
            : 0.0f;

        // The extended block is what every toyengine shader reads; the legacy
        // dir_cascade_* block below keeps the first four levels for consumers compiled
        // against the shorter std140 prefix (see light_data.h).
        ubo.dir_level_matrix[c] = pub.light_space_matrix;
        ubo.dir_level_pcf_texels[c >> 2][c & 3] = pcf_texels;
        if (c < coopa::gfx::engine::data::MAX_DIR_CASCADES) {
            ubo.dir_cascade_matrix[c]     = pub.light_space_matrix;
            ubo.dir_cascade_pcf_texels[c] = pcf_texels;
        }
        // The normal-offset bias, in world units, derived from THIS cascade's texel size --
        // the same conversion the PCF radius just did, and for the same reason: a
        // world-space constant means a different number of texels in every cascade. See
        // compute_shadow_normal_bias().
        // With the receiver-plane bias on, the taps already follow the receiver, so the
        // offset does not have to clear the PCF disk -- only shadow_normal_bias texels.
        const bool plane_bias = config_.shadow_receiver_plane_bias && !pcss_on_config;
        const float normal_bias = compute_shadow_normal_bias(
            plane_bias ? 0.0f : pcf_texels, config_.shadow_normal_bias, pub.texel_world);
        ubo.dir_level_normal_bias[c >> 2][c & 3] = normal_bias;
        if (c < coopa::gfx::engine::data::MAX_DIR_CASCADES) {
            ubo.dir_cascade_normal_bias[c] = normal_bias;
        }
        // PCSS contact hardening (gfx_shadow_dir_pcss): the whole penumbra conversion
        // folded into one factor. A stored-vs-receiver gap of `g` in [0,1] light-space
        // depth spans g * depth_range_world metres, and a sun of angular size
        // shadow_pcss_light_size grows the penumbra by that many metres * size --
        // divided by texel_world to land in the texels gfx_shadow_dir_pcf_vogel wants.
        const float pcss_scale = pub.depth_range_world *
            config_.shadow_pcss_light_size / std::max(pub.texel_world, 1e-6f);
        ubo.dir_level_pcss_scale[c >> 2][c & 3] = pcss_scale;
        if (c < coopa::gfx::engine::data::MAX_DIR_CASCADES) {
            ubo.dir_cascade_pcss_scale[c] = pcss_scale;
        }
        // One texel of THIS cascade in its [0,1] light depth: the shader multiplies its
        // constant + slope bias (counted in texels) by this, so the bias tracks each
        // cascade's resolution instead of being a fixed fraction of a depth range that
        // always spans the whole shadow distance toward the sun.
        const float depth_bias = pub.texel_world / std::max(pub.depth_range_world, 1e-6f);
        ubo.dir_level_depth_bias[c >> 2][c & 3] = depth_bias;
        if (c < coopa::gfx::engine::data::MAX_DIR_CASCADES) {
            ubo.dir_cascade_depth_bias[c] = depth_bias;
        }

        if (c < cascades && pcf_texels > 0.0f) any_soft = true;

        if (c == 0) {
            // Cascade 0 mirrored into the pre-cascade fields of the LightUBO prefix
            // (dir_shadow_params.z gates the directional shadow in toy_shadow_body.glsl)
            // -- see light_data.h's cascade doc.
            ubo.dir_light_space_matrix = pub.light_space_matrix;
            ubo.dir_shadow_params = glm::vec4(
                config_.shadow_bias, pcf_texels, cast_dir_shadow ? 1.0f : 0.0f, normal_bias);
            ubo.pcss_params = glm::vec4(
                0.0f, pcss_scale, pcss_search,
                static_cast<float>(std::clamp<uint32_t>(config_.shadow_pcss_taps, 1u, 16u)));
        }
    }
    // PCSS requires the soft path (radius > 0) in at least one live cascade: the constant
    // radius above becomes PCSS's maximum, so shadow_softness keeps its role as the
    // artist's width dial.
    if (caching && cascades > 0) dir_level_cursor_ = (dir_level_cursor_ + 1u) % cascades;
    for (uint32_t c = 0; c < coopa::gfx::engine::data::MAX_DIR_SHADOW_LEVELS; ++c) {
        ubo.dir_level_age[c >> 2][c & 3] = static_cast<float>(dir_levels_[c].age);
    }
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
    const float grid_columns =
        static_cast<float>(coopa::gfx::engine::targets::ShadowMapTarget::grid_for(cascades).first);
    ubo.dir_level_info = glm::vec4(static_cast<float>(cascades), grid_columns, inset, 0.06f);
    // The legacy copy keeps the REAL grid and count: its consumers remap tile uv with
    // grid_y = ceil(count / grid_x), so a truncated count there would compute the wrong
    // tile height and sample the wrong band of the atlas.
    ubo.dir_cascade_info = ubo.dir_level_info;

    // Depth bias (texels; see toy_shadow_bias_texels in toy_shadow_body.glsl) -- shared
    // with the local-light maps, which convert it per pixel.
    ubo.dir_shadow_bias_texels = glm::vec4(std::max(config_.shadow_depth_bias_texels, 0.0f),
                                           std::max(config_.shadow_slope_bias_texels, 0.0f),
                                           std::max(config_.shadow_slope_bias_max, 0.0f), 0.0f);
    // Far fade. Measured from the point the levels are centred on: the camera for the frustum
    // and clipmap fits, the focus point for the focus fit (whose last sphere has radius
    // shadow_distance around it -- the camera itself may sit further away than that).
    //
    // The fade distance is the range the levels ACTUALLY cover, which for the clipmap is the
    // outermost ring's radius, NOT shadow_distance -- that field does not bound the clipmap
    // fit and is typically far shorter (55 m against rings reaching 288 m on habitat). Keying
    // the fade to it would render every outer ring and then fade its shadows away to nothing
    // before they were ever sampled, silently throwing away the whole point of the fit.
    const float fade = std::clamp(config_.shadow_fade_fraction, 0.0f, 1.0f);
    const float fade_range = (clipmap_fit && !clip_radii.empty()) ? clip_radii.back()
                                                                  : config_.shadow_distance;
    const glm::vec3 fade_origin = focus_fit ? focus_point
                                : (cam ? glm::vec3(glm::inverse(fit_cam.view)[3]) : glm::vec3(0.0f));
    ubo.dir_shadow_fade = glm::vec4(fade_origin, (cam && fade > 0.0f) ? fade_range : 0.0f);
    // Without TAA nothing averages a dithered level switch, so blend the two levels across
    // the band instead (two evaluations there, one everywhere else).
    ubo.dir_shadow_fade_params = glm::vec4(fade_range * (1.0f - fade), fade,
                                           config_.aa_mode == "taa" ? 0.0f : 1.0f, 0.0f);
}

void ToyRenderPipeline::record_directional_shadow_(coopa::gfx::command::CommandBuffer& cmd,
                                const MeshGather& meshes,
                                const std::vector<SdfDrawItem>& sdf_draws,
                                bool cast_dir_shadow) {
    const uint32_t cascades = shadow_target_.dir_cascade_count();
    const uint32_t tile     = shadow_target_.dir_tile_resolution();
    const uint32_t grid     = std::max(1u, shadow_target_.dir_grid_columns());

    // The cache is built the first time the clipmap fit asks for it -- only the frustum and
    // focus fits refit every level every frame, so only the clipmap can ever reuse depth, and
    // `shadow_fit` is a per-scene setting rather than a startup-fixed one. Deliberately NOT on
    // the frame it is first wanted: creating it is safe at any time (nothing samples the cache
    // image, and the live image's view is unchanged, so no descriptor moves), but its first
    // composite has to start from a live atlas already in SHADER_READ, which is exactly what
    // the uncached path below leaves behind. So frame N allocates and draws uncached, and
    // caching begins on N+1.
    if (!dir_shadow_cache_ && dir_cache_wanted_() && frame_index_ > 0) {
        dir_shadow_cache_ = std::make_unique<DirShadowCache>(
            device_, allocator_, shadow_target_.dir_shadow_image(), shadow_target_.dir_shadow_view(),
            shadow_target_.dir_atlas_width(), shadow_target_.dir_atlas_height());
    }

    // The casters of one level, into whichever pass is currently open. Shared by the cached
    // and uncached paths so a caster's shadow is drawn the same way either way -- including
    // the per-material shader variant, which must match its G-buffer draw or a displaced
    // surface would cast the silhouette of its undisplaced self (gfx/surface/shadow_vs.glsl).
    auto draw_level = [&](uint32_t c, const std::vector<MeshBatch>& batches, bool with_sdf) {
        shadow_pipeline_->bind_directional(cmd);
        coopa::gfx::engine::passes::DirectionalShadowPushConstants pc{};
        pc.light_space_matrix = current_light_data().dir_level_matrix[c];
        pc.gfx_time = surface_gfx_time_();
        draw_shadow_batches_(cmd, meshes, batches, pc,
            [&](const std::string& shader, bool cull, bool tess) { shadow_pipeline_->bind_directional(cmd, shader, cull, tess); },
            [&](const auto& p) { shadow_pipeline_->push_directional(cmd, p); });

        if (with_sdf && !sdf_draws.empty()) {
            sdf_shadow_pass_->bind_directional(cmd);
            cmd.bind_descriptor_set(sdf_data_.current_set(), 0);
            coopa::gfx::engine::passes::SdfDirectionalShadowPushConstants sdf_pc{};
            sdf_pc.light_space_matrix = current_light_data().dir_level_matrix[c];
            sdf_pc.shadow_max_steps   = config_.sdf_shadow_max_steps;
            draw_sdf_shadow_casters_(cmd, sdf_draws, sdf_pc.light_space_matrix,
                                     (c % grid) * tile, (c / grid) * tile, tile,
                                     [&](uint32_t gpu_index) {
                                         sdf_pc.renderer_index = gpu_index;
                                         sdf_shadow_pass_->push_directional(cmd, sdf_pc);
                                     });
        }
    };

    if (dir_cache_active_()) {
        // --- Cached path: static depth rendered rarely, copied in, movers drawn on top ---
        dir_shadow_cache_->initialize(cmd);
        dir_shadow_cache_->begin_frame(cmd);   // live atlas shader-read -> transfer-dst

        bool any_static = false;
        for (uint32_t c = 0; c < cascades; ++c) any_static = any_static || dir_levels_[c].render_static;
        if (cast_dir_shadow && any_static) {
            dir_shadow_cache_->begin_static_pass(cmd);
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);
            for (uint32_t c = 0; c < cascades; ++c) {
                if (!dir_levels_[c].render_static) continue;
                const uint32_t x = (c % grid) * tile, y = (c / grid) * tile;
                dir_shadow_cache_->set_tile(cmd, x, y, tile, tile);
                // The tile must not keep the depth it held under its previous matrix.
                dir_shadow_cache_->clear_tile(cmd, x, y, tile, tile);
                // SDF casters are raymarched against live shape data, never cacheable, so
                // they are drawn in the dynamic pass instead.
                draw_level(c, meshes.dir_static[c], false);
            }
            dir_shadow_cache_->end_static_pass(cmd);
        }

        // Only the tiles whose live depth can differ from the cache. A tile needs the copy
        // when it was just re-rendered, when movers are about to be drawn into it, or when
        // movers were drawn into it LAST frame (their silhouettes are still burned in and
        // must be reset). A tile that is none of those -- a quiet outer ring with nothing
        // moving in it -- already holds exactly its static depth, and copying it back is
        // pure bandwidth: measured at ~0.97 ms a frame for six 2048 tiles, which was 80% of
        // the whole shadow budget.
        std::vector<VkImageCopy> regions;
        regions.reserve(cascades);
        for (uint32_t c = 0; c < cascades; ++c) {
            const bool has_dynamic = cast_dir_shadow && !meshes.cascade[c].empty();
            DirLevelState& lvl = dir_levels_[c];
            if (lvl.render_static || has_dynamic || lvl.had_dynamic) {
                regions.push_back(DirShadowCache::tile_region((c % grid) * tile, (c / grid) * tile, tile, tile));
            }
            lvl.had_dynamic = has_dynamic;
        }
        dir_shadow_cache_->copy_levels(cmd, regions);
        gpu_mark_(cmd, GpuScope::ShadowDirCache);

        dir_shadow_cache_->begin_dynamic_pass(cmd);   // LOAD: keeps the copied static depth
        if (cast_dir_shadow) {
            cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);
            for (uint32_t c = 0; c < cascades; ++c) {
                const uint32_t x = (c % grid) * tile, y = (c / grid) * tile;
                dir_shadow_cache_->set_tile(cmd, x, y, tile, tile);
                draw_level(c, meshes.cascade[c], true);
            }
        }
        dir_shadow_cache_->end_dynamic_pass(cmd);   // ends SHADER_READ for the lighting pass
        return;
    }

    // --- Uncached path: one pass over every caster per level, exactly as before ---
    //
    // The atlas is cleared once by begin_directional_pass() and transitioned once after the
    // loop, so a level costs exactly its draws -- no extra render pass, barrier or variant.
    shadow_target_.begin_directional_pass(cmd);
    if (cast_dir_shadow) {
        cmd.bind_vertex_buffer(instance_stream_.buffer(), 0, 1);
        for (uint32_t c = 0; c < cascades; ++c) {
            shadow_target_.set_cascade_viewport(cmd, c);
            draw_level(c, meshes.cascade[c], true);
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
} // namespace render
} // namespace toy
