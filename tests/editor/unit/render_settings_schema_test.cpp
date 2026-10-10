/**
 * @file render_settings_schema_test.cpp
 * @brief The Render tab's schema against the engine's config parser: every `render.` key AppConfig
 * reads is listed exactly once, with the engine's own default (the panel shows a field's schema
 * default while its key is absent), and colours are written in the form the engine reads.
 */

#include <coopa/testing/test.h>

#include <functional>
#include <map>

#include <glm/gtc/epsilon.hpp>
#include <toyengine/core/config.h>

#include "editor/schema/settings_schema.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("render_settings_schema");

namespace toy::editor::testing {

namespace {

/** @brief An engine default, numeric (as a vec4) or a string, for comparing with a schema field. */
struct EngineDefault {
    glm::vec4 v{0.0f};

} // namespace
    std::string s;
    bool text = false;
    EngineDefault(bool b) : v(b ? 1.0f : 0.0f) {}
    EngineDefault(int i) : v(static_cast<float>(i)) {}
    EngineDefault(uint32_t i) : v(static_cast<float>(i)) {}
    EngineDefault(float f) : v(f) {}
    EngineDefault(const glm::vec3& c) : v(c, 0.0f) {}
    EngineDefault(const glm::vec4& c) : v(c) {}
    EngineDefault(const std::string& t) : s(t), text(true) {}
    EngineDefault(toy::render::RenderQuality q)
        : s(q == toy::render::RenderQuality::Low ? "low" : q == toy::render::RenderQuality::Medium ? "medium"
            : q == toy::render::RenderQuality::Ultra ? "ultra" : "high"), text(true) {}
};

/**
 * @brief The Render tab covers every render key the engine parses, once, and every default it
 *        shows is the engine's (AppConfig::from_node() of an empty file: High quality presets).
 *        The table is every `render.` key AppConfig reads, with the field it lands in.
 */
COOPA_TEST(render_tab_lists_every_engine_key_once_with_its_default) {
    using PR = toy::render::PixelRenderConfig;
    const std::vector<std::pair<std::string, std::function<EngineDefault(const PR&)>>> engine_keys = {
        {"shadow_quality", [](const PR& r) { return EngineDefault(r.shadow_quality); }},
        {"ssao_quality", [](const PR& r) { return EngineDefault(r.ssao_quality); }},
        {"ssr_quality", [](const PR& r) { return EngineDefault(r.ssr_quality); }},
        {"ssgi_quality", [](const PR& r) { return EngineDefault(r.ssgi_quality); }},
        {"dof_quality", [](const PR& r) { return EngineDefault(r.dof_quality); }},
        {"volumetrics_quality", [](const PR& r) { return EngineDefault(r.volumetrics_quality); }},
        {"sdf_quality", [](const PR& r) { return EngineDefault(r.sdf_quality); }},
        {"water_quality", [](const PR& r) { return EngineDefault(r.water_quality); }},
        {"outline_enabled", [](const PR& r) { return EngineDefault(r.outline_enabled); }},
        {"palette_enabled", [](const PR& r) { return EngineDefault(r.palette_enabled); }},
        {"dither_enabled", [](const PR& r) { return EngineDefault(r.dither_enabled); }},
        {"camera_pixel_snap", [](const PR& r) { return EngineDefault(r.camera_pixel_snap); }},
        {"soft_lighting", [](const PR& r) { return EngineDefault(r.soft_lighting); }},
        {"ssao_enabled", [](const PR& r) { return EngineDefault(r.ssao_enabled); }},
        {"ssr_enabled", [](const PR& r) { return EngineDefault(r.ssr_enabled); }},
        {"transparency_enabled", [](const PR& r) { return EngineDefault(r.transparency_enabled); }},
        {"refraction_enabled", [](const PR& r) { return EngineDefault(r.refraction_enabled); }},
        {"fog_enabled", [](const PR& r) { return EngineDefault(r.fog_enabled); }},
        {"underwater_enabled", [](const PR& r) { return EngineDefault(r.underwater_enabled); }},
        {"volumetrics_enabled", [](const PR& r) { return EngineDefault(r.volumetrics_enabled); }},
        {"sdf_enabled", [](const PR& r) { return EngineDefault(r.sdf_enabled); }},
        {"sdf_shadows_enabled", [](const PR& r) { return EngineDefault(r.sdf_shadows_enabled); }},
        {"bloom_enabled", [](const PR& r) { return EngineDefault(r.bloom_enabled); }},
        {"tilt_shift_enabled", [](const PR& r) { return EngineDefault(r.tilt_shift_enabled); }},
        {"dof_enabled", [](const PR& r) { return EngineDefault(r.dof_enabled); }},
        {"motion_blur", [](const PR& r) { return EngineDefault(r.motion_blur); }},
        {"motion_blur_intensity", [](const PR& r) { return EngineDefault(r.motion_blur_intensity); }},
        {"debug_view", [](const PR& r) { return EngineDefault(r.debug_view); }},
        {"world_ui_enabled", [](const PR& r) { return EngineDefault(r.world_ui_enabled); }},
        {"screen_ui_enabled", [](const PR& r) { return EngineDefault(r.screen_ui_enabled); }},
        {"resolution_mode", [](const PR& r) { return EngineDefault(r.resolution_mode); }},
        {"render_width", [](const PR& r) { return EngineDefault(r.render_width); }},
        {"render_height", [](const PR& r) { return EngineDefault(r.render_height); }},
        {"scale_divisor", [](const PR& r) { return EngineDefault(r.scale_divisor); }},
        {"upscale_mode", [](const PR& r) { return EngineDefault(r.upscale_mode); }},
        {"exposure", [](const PR& r) { return EngineDefault(r.exposure); }},
        {"auto_exposure_enabled", [](const PR& r) { return EngineDefault(r.auto_exposure_enabled); }},
        {"auto_exposure_compensation", [](const PR& r) { return EngineDefault(r.auto_exposure_compensation); }},
        {"auto_exposure_speed_up", [](const PR& r) { return EngineDefault(r.auto_exposure_speed_up); }},
        {"auto_exposure_speed_down", [](const PR& r) { return EngineDefault(r.auto_exposure_speed_down); }},
        {"auto_exposure_min", [](const PR& r) { return EngineDefault(r.auto_exposure_min); }},
        {"auto_exposure_max", [](const PR& r) { return EngineDefault(r.auto_exposure_max); }},
        {"grading_enabled", [](const PR& r) { return EngineDefault(r.grading_enabled); }},
        {"light_bands", [](const PR& r) { return EngineDefault(r.light_bands); }},
        {"spec_threshold", [](const PR& r) { return EngineDefault(r.spec_threshold); }},
        {"rim_strength", [](const PR& r) { return EngineDefault(r.rim_strength); }},
        {"ambient_intensity", [](const PR& r) { return EngineDefault(r.indirect.ambient_intensity); }},
        {"sky_intensity", [](const PR& r) { return EngineDefault(r.indirect.sky_intensity); }},
        {"sky_zenith", [](const PR& r) { return EngineDefault(r.indirect.sky_zenith); }},
        {"sky_horizon", [](const PR& r) { return EngineDefault(r.indirect.sky_horizon); }},
        {"sky_ground", [](const PR& r) { return EngineDefault(r.indirect.sky_ground); }},
        {"sky_model", [](const PR& r) { return EngineDefault(r.sky_model); }},
        {"sky_quality", [](const PR& r) { return EngineDefault(r.sky_quality); }},
        {"atmosphere_density", [](const PR& r) { return EngineDefault(r.atmosphere_density); }},
        {"ozone", [](const PR& r) { return EngineDefault(r.ozone); }},
        {"sun_disc_size", [](const PR& r) { return EngineDefault(r.sun_disc_size); }},
        {"moon_disc_size", [](const PR& r) { return EngineDefault(r.moon_disc_size); }},
        {"sky_stars", [](const PR& r) { return EngineDefault(r.sky_stars); }},
        {"clouds", [](const PR& r) { return EngineDefault(r.clouds); }},
        {"cloud_coverage", [](const PR& r) { return EngineDefault(r.cloud_coverage); }},
        {"cloud_altitude", [](const PR& r) { return EngineDefault(r.cloud_altitude); }},
        {"cloud_thickness", [](const PR& r) { return EngineDefault(r.cloud_thickness); }},
        {"cloud_density", [](const PR& r) { return EngineDefault(r.cloud_density); }},
        {"cloud_wind_speed", [](const PR& r) { return EngineDefault(r.cloud_wind_speed); }},
        {"topdown_mode", [](const PR& r) { return EngineDefault(r.topdown_mode); }},
        {"topdown_cloud_height", [](const PR& r) { return EngineDefault(r.topdown_cloud_height); }},
        {"topdown_cloud_size", [](const PR& r) { return EngineDefault(r.topdown_cloud_size); }},
        {"topdown_cloud_thickness", [](const PR& r) { return EngineDefault(r.topdown_cloud_thickness); }},
        {"topdown_cloud_opacity", [](const PR& r) { return EngineDefault(r.topdown_cloud_opacity); }},
        {"topdown_fade_start", [](const PR& r) { return EngineDefault(r.topdown_fade_start); }},
        {"topdown_fade_end", [](const PR& r) { return EngineDefault(r.topdown_fade_end); }},
        {"topdown_shadow_strength", [](const PR& r) { return EngineDefault(r.topdown_shadow_strength); }},
        {"topdown_light_bands", [](const PR& r) { return EngineDefault(r.topdown_light_bands); }},
        {"topdown_outline", [](const PR& r) { return EngineDefault(r.topdown_outline); }},
        {"shadows_enabled", [](const PR& r) { return EngineDefault(r.shadows_enabled); }},
        {"shadow_map_resolution", [](const PR& r) { return EngineDefault(r.shadow_map_resolution); }},
        {"shadow_cascades", [](const PR& r) { return EngineDefault(r.shadow_cascades); }},
        {"shadow_cascade_split_lambda", [](const PR& r) { return EngineDefault(r.shadow_cascade_split_lambda); }},
        {"shadow_fit", [](const PR& r) { return EngineDefault(r.shadow_fit); }},
        {"shadow_focus_radius", [](const PR& r) { return EngineDefault(r.shadow_focus_radius); }},
        {"shadow_focus_distance", [](const PR& r) { return EngineDefault(r.shadow_focus_distance); }},
        {"shadow_pcf_max_texels", [](const PR& r) { return EngineDefault(r.shadow_pcf_max_texels); }},
        {"shadow_receiver_plane_bias", [](const PR& r) { return EngineDefault(r.shadow_receiver_plane_bias); }},
        {"shadow_receiver_max_slope", [](const PR& r) { return EngineDefault(r.shadow_receiver_max_slope); }},
        {"cube_shadow_resolution", [](const PR& r) { return EngineDefault(r.cube_shadow_resolution); }},
        {"spot_shadow_resolution", [](const PR& r) { return EngineDefault(r.spot_shadow_resolution); }},
        {"local_shadow_atlas_resolution", [](const PR& r) { return EngineDefault(r.local_shadow_atlas_resolution); }},
        {"max_shadowed_point_lights", [](const PR& r) { return EngineDefault(r.max_shadowed_point_lights); }},
        {"max_shadowed_spot_lights", [](const PR& r) { return EngineDefault(r.max_shadowed_spot_lights); }},
        {"shadow_cache_enabled", [](const PR& r) { return EngineDefault(r.shadow_cache_enabled); }},
        {"shadow_bias", [](const PR& r) { return EngineDefault(r.shadow_bias); }},
        {"shadow_depth_bias_texels", [](const PR& r) { return EngineDefault(r.shadow_depth_bias_texels); }},
        {"shadow_slope_bias_texels", [](const PR& r) { return EngineDefault(r.shadow_slope_bias_texels); }},
        {"shadow_slope_bias_max", [](const PR& r) { return EngineDefault(r.shadow_slope_bias_max); }},
        {"shadow_fade_fraction", [](const PR& r) { return EngineDefault(r.shadow_fade_fraction); }},
        {"shadow_normal_bias", [](const PR& r) { return EngineDefault(r.shadow_normal_bias); }},
        {"shadow_distance", [](const PR& r) { return EngineDefault(r.shadow_distance); }},
        {"soft_shadows", [](const PR& r) { return EngineDefault(r.soft_shadows); }},
        {"shadow_softness", [](const PR& r) { return EngineDefault(r.shadow_softness); }},
        {"point_shadow_softness", [](const PR& r) { return EngineDefault(r.point_shadow_softness); }},
        {"spot_shadow_softness", [](const PR& r) { return EngineDefault(r.spot_shadow_softness); }},
        {"shadow_pcf_samples", [](const PR& r) { return EngineDefault(r.shadow_pcf_samples); }},
        {"shadow_pcss_enabled", [](const PR& r) { return EngineDefault(r.shadow_pcss_enabled); }},
        {"shadow_pcss_light_size", [](const PR& r) { return EngineDefault(r.shadow_pcss_light_size); }},
        {"shadow_pcss_search_texels", [](const PR& r) { return EngineDefault(r.shadow_pcss_search_texels); }},
        {"shadow_pcss_taps", [](const PR& r) { return EngineDefault(r.shadow_pcss_taps); }},
        {"contact_shadows_enabled", [](const PR& r) { return EngineDefault(r.contact_shadows_enabled); }},
        {"contact_shadow_length", [](const PR& r) { return EngineDefault(r.contact_shadow_length); }},
        {"contact_shadow_strength", [](const PR& r) { return EngineDefault(r.contact_shadow_strength); }},
        {"contact_shadow_thickness", [](const PR& r) { return EngineDefault(r.contact_shadow_thickness); }},
        {"contact_shadow_steps", [](const PR& r) { return EngineDefault(r.contact_shadow_steps); }},
        {"contact_shadow_temporal_enabled", [](const PR& r) { return EngineDefault(r.contact_shadow_temporal_enabled); }},
        {"contact_shadow_temporal_frames", [](const PR& r) { return EngineDefault(r.contact_shadow_temporal_frames); }},
        {"outline_thickness", [](const PR& r) { return EngineDefault(r.outline_thickness); }},
        {"outline_color", [](const PR& r) { return EngineDefault(r.outline_color); }},
        {"depth_threshold", [](const PR& r) { return EngineDefault(r.depth_threshold); }},
        {"normal_threshold", [](const PR& r) { return EngineDefault(r.normal_threshold); }},
        {"palette", [](const PR& r) { return EngineDefault(r.palette_path); }},
        {"grading_lut", [](const PR& r) { return EngineDefault(r.grading_lut_path); }},
        {"dither_strength", [](const PR& r) { return EngineDefault(r.dither_strength); }},
        {"texel_aa", [](const PR& r) { return EngineDefault(r.texel_aa); }},
        {"ssao_radius", [](const PR& r) { return EngineDefault(r.ssao_radius); }},
        {"ssao_bias", [](const PR& r) { return EngineDefault(r.ssao_bias); }},
        {"ssao_power", [](const PR& r) { return EngineDefault(r.ssao_power); }},
        {"ssao_slices", [](const PR& r) { return EngineDefault(r.ssao_slices); }},
        {"ssao_steps", [](const PR& r) { return EngineDefault(r.ssao_steps); }},
        {"ssao_max_radius_px", [](const PR& r) { return EngineDefault(r.ssao_max_radius_px); }},
        {"ssao_blur_plane_sigma", [](const PR& r) { return EngineDefault(r.ssao_blur_plane_sigma); }},
        {"ssao_half_res", [](const PR& r) { return EngineDefault(r.ssao_half_res); }},
        {"ssao_blur_light", [](const PR& r) { return EngineDefault(r.ssao_blur_light); }},
        {"ssao_direct_lighting_strength", [](const PR& r) { return EngineDefault(r.ssao_direct_lighting_strength); }},
        {"ssao_temporal_enabled", [](const PR& r) { return EngineDefault(r.ssao_temporal_enabled); }},
        {"ssao_temporal_frames", [](const PR& r) { return EngineDefault(r.ssao_temporal_frames); }},
        {"ssao_temporal_gamma", [](const PR& r) { return EngineDefault(r.ssao_temporal_gamma); }},
        {"ssao_intensity", [](const PR& r) { return EngineDefault(r.ssao_intensity); }},
        {"ssr_max_distance", [](const PR& r) { return EngineDefault(r.ssr_max_distance); }},
        {"ssr_max_iterations", [](const PR& r) { return EngineDefault(r.ssr_max_iterations); }},
        {"ssr_thickness", [](const PR& r) { return EngineDefault(r.ssr_thickness); }},
        {"ssr_thickness_scale", [](const PR& r) { return EngineDefault(r.ssr_thickness_scale); }},
        {"ssr_bias_texels", [](const PR& r) { return EngineDefault(r.ssr_bias_texels); }},
        {"ssr_roughness_cutoff", [](const PR& r) { return EngineDefault(r.ssr_roughness_cutoff); }},
        {"ssr_start_mip", [](const PR& r) { return EngineDefault(r.ssr_start_mip); }},
        {"ssr_min_mip0_steps", [](const PR& r) { return EngineDefault(r.ssr_min_mip0_steps); }},
        {"ssr_temporal_enabled", [](const PR& r) { return EngineDefault(r.ssr_temporal_enabled); }},
        {"ssr_temporal_frames", [](const PR& r) { return EngineDefault(r.ssr_temporal_frames); }},
        {"ssgi_temporal_frames", [](const PR& r) { return EngineDefault(r.ssgi_temporal_frames); }},
        {"ssr_temporal_blend", [](const PR& r) { return EngineDefault(r.ssr_temporal_blend); }},
        {"ssr_blur_radius", [](const PR& r) { return EngineDefault(r.ssr_blur_radius); }},
        {"ssr_blur_light", [](const PR& r) { return EngineDefault(r.ssr_blur_light); }},
        {"ssr_blur_zero_skip", [](const PR& r) { return EngineDefault(r.ssr_blur_zero_skip); }},
        {"ssr_jitter", [](const PR& r) { return EngineDefault(r.ssr_jitter); }},
        {"ssr_rays_per_pixel", [](const PR& r) { return EngineDefault(r.ssr_rays_per_pixel); }},
        {"ssr_cone_prefilter", [](const PR& r) { return EngineDefault(r.ssr_cone_prefilter); }},
        {"ssr_skip_behind", [](const PR& r) { return EngineDefault(r.ssr_skip_behind); }},
        {"ssr_half_res", [](const PR& r) { return EngineDefault(r.ssr_half_res); }},
        {"ssgi_resolution_scale", [](const PR& r) { return EngineDefault(r.ssgi_resolution_scale); }},
        {"ssr_skip_negligible", [](const PR& r) { return EngineDefault(r.ssr_skip_negligible); }},
        {"ssr_skip_threshold", [](const PR& r) { return EngineDefault(r.ssr_skip_threshold); }},
        {"ssr_temporal_gamma", [](const PR& r) { return EngineDefault(r.ssr_temporal_gamma); }},
        {"ssgi_traced", [](const PR& r) { return EngineDefault(r.ssgi_traced); }},
        {"ssgi_max_distance", [](const PR& r) { return EngineDefault(r.ssgi_max_distance); }},
        {"ssgi_blur_radius", [](const PR& r) { return EngineDefault(r.ssgi_blur_radius); }},
        {"ssgi_blur_light", [](const PR& r) { return EngineDefault(r.ssgi_blur_light); }},
        {"ssgi_max_iterations", [](const PR& r) { return EngineDefault(r.ssgi_max_iterations); }},
        {"ssgi_intensity", [](const PR& r) { return EngineDefault(r.indirect.ssgi_intensity); }},
        {"ssgi_distance", [](const PR& r) { return EngineDefault(r.indirect.ssgi_distance); }},
        {"refraction_ior", [](const PR& r) { return EngineDefault(r.refraction_ior); }},
        {"refraction_thickness", [](const PR& r) { return EngineDefault(r.refraction_thickness); }},
        {"refraction_strength", [](const PR& r) { return EngineDefault(r.refraction_strength); }},
        {"refraction_max_offset", [](const PR& r) { return EngineDefault(r.refraction_max_offset); }},
        {"refraction_chromatic", [](const PR& r) { return EngineDefault(r.refraction_chromatic); }},
        {"refraction_blur", [](const PR& r) { return EngineDefault(r.refraction_blur); }},
        {"refraction_density", [](const PR& r) { return EngineDefault(r.refraction_density); }},
        {"refraction_fresnel", [](const PR& r) { return EngineDefault(r.refraction_fresnel); }},
        {"refraction_tint", [](const PR& r) { return EngineDefault(r.refraction_tint); }},
        {"refraction_include_reflections", [](const PR& r) { return EngineDefault(r.refraction_include_reflections); }},
        {"fog_mode", [](const PR& r) { return EngineDefault(r.fog_mode); }},
        {"fog_density", [](const PR& r) { return EngineDefault(r.fog_density); }},
        {"fog_linear_start", [](const PR& r) { return EngineDefault(r.fog_linear_start); }},
        {"fog_linear_end", [](const PR& r) { return EngineDefault(r.fog_linear_end); }},
        {"fog_color", [](const PR& r) { return EngineDefault(r.fog_color); }},
        {"fog_height_base", [](const PR& r) { return EngineDefault(r.fog_height_base); }},
        {"fog_height_falloff", [](const PR& r) { return EngineDefault(r.fog_height_falloff); }},
        {"fog_sky_blend", [](const PR& r) { return EngineDefault(r.fog_sky_blend); }},
        {"fog_sun_amount", [](const PR& r) { return EngineDefault(r.fog_sun_amount); }},
        {"fog_sun_anisotropy", [](const PR& r) { return EngineDefault(r.fog_sun_anisotropy); }},
        {"fog_max_opacity", [](const PR& r) { return EngineDefault(r.fog_max_opacity); }},
        {"fog_sun_start_distance", [](const PR& r) { return EngineDefault(r.fog_sun_start_distance); }},
        {"fog_start_distance", [](const PR& r) { return EngineDefault(r.fog_start_distance); }},
        {"fog_cutoff_distance", [](const PR& r) { return EngineDefault(r.fog_cutoff_distance); }},
        {"fog_sky_distance", [](const PR& r) { return EngineDefault(r.fog_sky_distance); }},
        {"volumetrics_step_count", [](const PR& r) { return EngineDefault(r.volumetrics_step_count); }},
        {"volumetrics_max_distance", [](const PR& r) { return EngineDefault(r.volumetrics_max_distance); }},
        {"volumetrics_resolution_scale", [](const PR& r) { return EngineDefault(r.volumetrics_resolution_scale); }},
        {"volumetrics_mode", [](const PR& r) { return EngineDefault(r.volumetrics_mode); }},
        {"volumetrics_froxel_tile", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_tile); }},
        {"volumetrics_froxel_slices", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_slices); }},
        {"volumetrics_froxel_history", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_history); }},
        {"volumetrics_froxel_miss_samples", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_miss_samples); }},
        {"volumetrics_froxel_lookup_jitter", [](const PR& r) { return EngineDefault(r.volumetrics_froxel_lookup_jitter); }},
        {"volumetrics_max_opacity", [](const PR& r) { return EngineDefault(r.volumetrics_max_opacity); }},
        {"volumetrics_sun_anisotropy", [](const PR& r) { return EngineDefault(r.volumetrics_sun_anisotropy); }},
        {"volumetrics_shadows_enabled", [](const PR& r) { return EngineDefault(r.volumetrics_shadows_enabled); }},
        {"volumetrics_light_scatter", [](const PR& r) { return EngineDefault(r.volumetrics_light_scatter); }},
        {"volumetrics_max_scatter_lights", [](const PR& r) { return EngineDefault(r.volumetrics_max_scatter_lights); }},
        {"mesh_lod_bias", [](const PR& r) { return EngineDefault(r.mesh_lod_bias); }},
        {"shadow_min_caster_texels", [](const PR& r) { return EngineDefault(r.shadow_min_caster_texels); }},
        {"bloom_threshold", [](const PR& r) { return EngineDefault(r.bloom_threshold); }},
        {"bloom_soft_knee", [](const PR& r) { return EngineDefault(r.bloom_soft_knee); }},
        {"bloom_intensity", [](const PR& r) { return EngineDefault(r.bloom_intensity); }},
        {"bloom_scatter", [](const PR& r) { return EngineDefault(r.bloom_scatter); }},
        {"bloom_radius", [](const PR& r) { return EngineDefault(r.bloom_radius); }},
        {"bloom_clamp", [](const PR& r) { return EngineDefault(r.bloom_clamp); }},
        {"tilt_shift_focus_center", [](const PR& r) { return EngineDefault(r.tilt_shift_focus_center); }},
        {"tilt_shift_focus_width", [](const PR& r) { return EngineDefault(r.tilt_shift_focus_width); }},
        {"tilt_shift_ramp_width", [](const PR& r) { return EngineDefault(r.tilt_shift_ramp_width); }},
        {"tilt_shift_blur_top", [](const PR& r) { return EngineDefault(r.tilt_shift_blur_top); }},
        {"tilt_shift_blur_bottom", [](const PR& r) { return EngineDefault(r.tilt_shift_blur_bottom); }},
        {"tilt_shift_max_radius", [](const PR& r) { return EngineDefault(r.tilt_shift_max_radius); }},
        {"tilt_shift_angle", [](const PR& r) { return EngineDefault(r.tilt_shift_angle); }},
        {"dof_focus_mode", [](const PR& r) { return EngineDefault(r.dof_focus_mode); }},
        {"dof_focus_object", [](const PR& r) { return EngineDefault(r.dof_focus_object); }},
        {"dof_focus_smoothing", [](const PR& r) { return EngineDefault(r.dof_focus_smoothing); }},
        {"dof_focus_distance", [](const PR& r) { return EngineDefault(r.dof_focus_distance); }},
        {"dof_focus_range", [](const PR& r) { return EngineDefault(r.dof_focus_range); }},
        {"dof_focus_cover_object", [](const PR& r) { return EngineDefault(r.dof_focus_cover_object); }},
        {"dof_blur_scale", [](const PR& r) { return EngineDefault(r.dof_blur_scale); }},
        {"dof_aperture", [](const PR& r) { return EngineDefault(r.dof_aperture); }},
        {"dof_focal_length", [](const PR& r) { return EngineDefault(r.dof_focal_length); }},
        {"dof_sensor_width", [](const PR& r) { return EngineDefault(r.dof_sensor_width); }},
        {"dof_max_radius", [](const PR& r) { return EngineDefault(r.dof_max_radius); }},
        {"dof_sample_count", [](const PR& r) { return EngineDefault(r.dof_sample_count); }},
        {"dof_blade_count", [](const PR& r) { return EngineDefault(r.dof_blade_count); }},
        {"dof_blade_rotation", [](const PR& r) { return EngineDefault(r.dof_blade_rotation); }},
        {"aa_mode", [](const PR& r) { return EngineDefault(r.aa_mode); }},
        {"fxaa_subpixel", [](const PR& r) { return EngineDefault(r.fxaa_subpixel); }},
        {"fxaa_edge_threshold", [](const PR& r) { return EngineDefault(r.fxaa_edge_threshold); }},
        {"fxaa_edge_threshold_min", [](const PR& r) { return EngineDefault(r.fxaa_edge_threshold_min); }},
        {"smaa_threshold", [](const PR& r) { return EngineDefault(r.smaa_threshold); }},
        {"smaa_max_search_steps", [](const PR& r) { return EngineDefault(r.smaa_max_search_steps); }},
        {"taa_blending_weight", [](const PR& r) { return EngineDefault(r.taa_blending_weight); }},
        {"taa_weight_scale", [](const PR& r) { return EngineDefault(r.taa_weight_scale); }},
        {"taa_feedback_motion", [](const PR& r) { return EngineDefault(r.taa_feedback_motion); }},
        {"taa_sharpness", [](const PR& r) { return EngineDefault(r.taa_sharpness); }},
        {"taa_variance_gamma", [](const PR& r) { return EngineDefault(r.taa_variance_gamma); }},
        {"sdf_max_steps", [](const PR& r) { return EngineDefault(r.sdf_max_steps); }},
        {"sdf_shadow_max_steps", [](const PR& r) { return EngineDefault(r.sdf_shadow_max_steps); }},
        {"sdf_max_renderers", [](const PR& r) { return EngineDefault(r.sdf_max_renderers); }},
        {"sdf_max_shapes", [](const PR& r) { return EngineDefault(r.sdf_max_shapes); }},
    };
    std::map<std::string, int> seen;
    std::map<std::string, FieldDesc> fields;
    for (const auto& g : render_settings_groups()) {
        for (const auto& f : g.all_fields()) { ++seen[f.key]; fields[f.key] = f; }
        for (const auto& f : g.all_fields()) {
            expect(!f.label.empty() && !f.tooltip.empty(), "render." + f.key + " has a label and a tooltip");
        }
        expect(!g.tip.empty() && !g.category.empty(), g.title + " has a category and a description");
    }
    for (const auto& [k, n] : seen) expect(n == 1, "render." + k + " appears once in the Render tab (" + std::to_string(n) + ")");
    Node root = Node::mapping();
    root["render"] = Node::mapping();
    const PR defaults = toy::core::AppConfig::from_node(root).render;
    for (const auto& [key, get] : engine_keys) {
        auto it = fields.find(key);
        expect(it != fields.end(), "render." + key + " (parsed by the engine) is in the Render tab");
        if (it == fields.end()) continue;
        const FieldDesc& f = it->second;
        const EngineDefault d = get(defaults);
        if (d.text) {
            if (f.kind == FieldKind::AssetRef) {
                expect(d.s.empty(), "render." + key + ": an asset path defaults to none (engine: '" + d.s + "')");
                continue;
            }
            const std::string shown = !f.default_string.empty() ? f.default_string
                                    : f.kind == FieldKind::Enum && !f.options.empty() ? f.options.front() : std::string();
            expect(shown == d.s, "render." + key + " shows the engine default '" + d.s + "' (schema: '" + shown + "')");
            if (f.kind == FieldKind::Enum) {
                expect(std::find(f.options.begin(), f.options.end(), d.s) != f.options.end(), "render." + key + " offers its default");
            }
        } else {
            const int n = f.kind == FieldKind::Color4 ? 4 : (f.kind == FieldKind::Color || f.kind == FieldKind::Vec3) ? 3 : 1;
            bool same = true;
            for (int i = 0; i < n; ++i) same &= std::abs(f.def[i] - d.v[i]) <= 1e-5f * std::max(1.0f, std::abs(d.v[i]));
            char buf[160];
            std::snprintf(buf, sizeof buf, " (engine %g %g %g %g, schema %g %g %g %g)", d.v.x, d.v.y, d.v.z, d.v.w, f.def.x, f.def.y, f.def.z, f.def.w);
            expect(same, "render." + key + " shows the engine default" + buf);
            if (n == 1 && f.kind != FieldKind::Bool) {
                expect(d.v.x >= f.min - 1e-6f && d.v.x <= f.max + 1e-6f, "render." + key + "'s default is inside its range");
            }
        }
    }
    for (const auto& entry : seen) {
        const std::string& k = entry.first;
        const bool parsed = std::any_of(engine_keys.begin(), engine_keys.end(), [&](const auto& e) { return e.first == k; });
        expect(parsed, "render." + k + " is a key the engine reads");
    }
}

/** @brief An edited outline colour is written as the 4-element list the engine reads, and the
 *         engine still takes the 3-element form files were saved in before that fix. */
COOPA_TEST(outline_color_is_written_with_alpha_and_rgb_loads_opaque) {
    const toy::core::AppConfig eng = toy::core::AppConfig::from_node(Node::mapping());
    const FieldDesc* outline = nullptr;
    for (const auto& g : render_settings_groups()) for (const auto& f : g.all_fields()) if (f.key == "outline_color") outline = &f;
    ASSERT_TRUE(outline != nullptr);
    Node render = Node::mapping();
    render["outline_color"] = field_default_node(*outline);
    Node root = Node::mapping();
    root["render"] = render;
    expect(render.at("outline_color").size() == 4, "outline_color is written with alpha");
    const auto parsed = toy::core::AppConfig::from_node(root);
    expect(glm::all(glm::epsilonEqual(parsed.render.outline_color, eng.render.outline_color, 1e-6f)), "outline_color default matches");

    Node old = Node::mapping();
    old["render"] = Node::mapping();
    float rgb[3] = {1.0f, 0.0f, 0.0f};
    old["render"]["outline_color"] = make_float_seq(rgb, 3);
    expect(toy::core::AppConfig::from_node(old).render.outline_color == glm::vec4(1, 0, 0, 1), "rgb outline_color loads opaque");
}

} // namespace toy::editor::testing
