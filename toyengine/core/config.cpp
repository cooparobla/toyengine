#include <toyengine/core/config.h>
#include <toyengine/core/yaml_read.h>

#include <coopa/yaml/document.h>
#include <physxcoopa/util/physics_settings.h>

namespace toy {
namespace core {

using yaml::read;
using yaml::num;

render::RenderQuality parse_render_quality(const std::string& value) {
    if (value == "low")                        return render::RenderQuality::Low;
    if (value == "med" || value == "medium")   return render::RenderQuality::Medium;
    if (value == "ultra")                      return render::RenderQuality::Ultra;
    return render::RenderQuality::High;
}

AppConfig AppConfig::load(const std::string& path) {
    AppConfig config;
    try {
        const std::filesystem::path resolved = coopa::yaml::resolve_variant(path);
        if (!std::filesystem::exists(resolved)) {
            std::cerr << "[toy::core::AppConfig] Config file not found at " << path << ", using defaults.\n";
            return config;
        }

        return from_node(coopa::yaml::load_document(resolved));
    } catch (const std::exception& e) {
        std::cerr << "[toy::core::AppConfig] Warning: Failed to parse config file (" << e.what() << "), using defaults.\n";
    }
    return config;
}

AppConfig AppConfig::from_node(const fkyaml::node& root) {
    AppConfig config;
    if (root.is_mapping()) config.source = root;
    try {
        if (root.contains("scene")) {
            const auto& s = root.at("scene");
            if (s.contains("default_scene")) {
                config.scene.default_scene = s.at("default_scene").get_value<std::string>();
            }
        }

        if (root.contains("window")) {
            const auto& w = root.at("window");
            read(w, "title", config.window.title);
            read(w, "width", config.window.width);
            read(w, "height", config.window.height);
            read(w, "vsync", config.window.vsync);
            read(w, "visible", config.window.visible);
            read(w, "splash", config.window.splash);
            read(w, "splash_title", config.window.splash_title);
            read(w, "splash_image", config.window.splash_image);
            if (w.contains("splash_color")) {
                const auto& c = w.at("splash_color");
                if (c.size() >= 3) {
                    config.window.splash_color = glm::vec3(
                        c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                }
            }
        }

        if (root.contains("render")) {
            const auto& r = root.at("render");

            // --- Quality presets ---
            // Parsed and applied BEFORE every other render key, so the per-key
            // parsing below overrides preset-covered fields whenever a key is
            // written explicitly: defaults < quality preset < explicit key.
            if (r.contains("shadow_quality"))      config.render.shadow_quality      = parse_render_quality(r.at("shadow_quality").get_value<std::string>());
            if (r.contains("ssao_quality"))        config.render.ssao_quality        = parse_render_quality(r.at("ssao_quality").get_value<std::string>());
            if (r.contains("ssr_quality"))         config.render.ssr_quality         = parse_render_quality(r.at("ssr_quality").get_value<std::string>());
            if (r.contains("ssgi_quality"))        config.render.ssgi_quality        = parse_render_quality(r.at("ssgi_quality").get_value<std::string>());
            if (r.contains("dof_quality"))         config.render.dof_quality         = parse_render_quality(r.at("dof_quality").get_value<std::string>());
            if (r.contains("volumetrics_quality")) config.render.volumetrics_quality = parse_render_quality(r.at("volumetrics_quality").get_value<std::string>());
            if (r.contains("sdf_quality"))         config.render.sdf_quality         = parse_render_quality(r.at("sdf_quality").get_value<std::string>());
            if (r.contains("water_quality"))       config.render.water_quality       = parse_render_quality(r.at("water_quality").get_value<std::string>());
            if (r.contains("sky_quality"))         config.render.sky_quality         = parse_render_quality(r.at("sky_quality").get_value<std::string>());
            config.render.apply_quality_presets();

            // --- Feature toggles ---
            read(r, "outline_enabled", config.render.outline_enabled);
            read(r, "palette_enabled", config.render.palette_enabled);
            read(r, "dither_enabled", config.render.dither_enabled);
            read(r, "camera_pixel_snap", config.render.camera_pixel_snap);
            read(r, "soft_lighting", config.render.soft_lighting);
            read(r, "ssao_enabled", config.render.ssao_enabled);
            read(r, "ssr_enabled", config.render.ssr_enabled);
            read(r, "transparency_enabled", config.render.transparency_enabled);
            read(r, "xray_enabled", config.render.xray_enabled);
            if (r.contains("ssr_reflect_transparent")) std::cerr << "[toy::core::AppConfig] Warning: ssr_reflect_transparent is deprecated and ignored -- SSR now reflects transparent geometry through the previous frame's colour.\n";
            read(r, "refraction_enabled", config.render.refraction_enabled);
            read(r, "fog_enabled", config.render.fog_enabled);
            read(r, "underwater_enabled", config.render.underwater_enabled);
            read(r, "volumetrics_enabled", config.render.volumetrics_enabled);
            read(r, "sdf_enabled", config.render.sdf_enabled);
            read(r, "sdf_shadows_enabled", config.render.sdf_shadows_enabled);
            read(r, "bloom_enabled", config.render.bloom_enabled);
            read(r, "tilt_shift_enabled", config.render.tilt_shift_enabled);
            read(r, "dof_enabled", config.render.dof_enabled);
            read(r, "debug_view", config.render.debug_view);
            if (r.contains("skinning")) {
                const std::string v = r.at("skinning").get_value<std::string>();
                if (v == "cpu" || v == "gpu") config.render.skinning = v;
                else std::cerr << "[toy::core::AppConfig] Unknown render.skinning '" << v << "', expected cpu | gpu; using gpu.\n";
            }
            read(r, "world_ui_enabled", config.render.world_ui_enabled);
            read(r, "screen_ui_enabled", config.render.screen_ui_enabled);

            // --- Internal resolution ---
            read(r, "resolution_mode", config.render.resolution_mode);
            read(r, "render_width", config.render.render_width);
            read(r, "render_height", config.render.render_height);
            read(r, "scale_divisor", config.render.scale_divisor);
            read(r, "upscale_mode", config.render.upscale_mode);
            read(r, "upscale_filter", config.render.upscale_filter);

            // --- Lighting ---
            read(r, "exposure", config.render.exposure);
            read(r, "auto_exposure_enabled", config.render.auto_exposure_enabled);
            read(r, "auto_exposure_compensation", config.render.auto_exposure_compensation);
            read(r, "auto_exposure_speed_up", config.render.auto_exposure_speed_up);
            read(r, "auto_exposure_speed_down", config.render.auto_exposure_speed_down);
            read(r, "auto_exposure_min", config.render.auto_exposure_min);
            read(r, "auto_exposure_max", config.render.auto_exposure_max);
            read(r, "grading_enabled", config.render.grading_enabled);
            read(r, "light_bands", config.render.light_bands);
            read(r, "spec_threshold", config.render.spec_threshold);
            read(r, "rim_strength", config.render.rim_strength);
            // --- Per-material toon shading (scene-wide tuning; see PBRMaterial::toon_bands) ---
            read(r, "toon_strength", config.render.toon_strength);
            read(r, "toon_spec_threshold", config.render.toon_spec_threshold);
            read(r, "toon_spec_edge", config.render.toon_spec_edge);
            read(r, "toon_rim_power", config.render.toon_rim_power);
            read(r, "toon_band_attenuation", config.render.toon_band_attenuation);
            if (r.contains("toon_shadow_tint")) {
                const auto& c = r.at("toon_shadow_tint");
                if (c.size() >= 3) {
                    config.render.toon_shadow_tint = glm::vec3(
                        c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                }
            }
            read(r, "ambient_intensity", config.render.indirect.ambient_intensity);
            read(r, "sky_intensity", config.render.indirect.sky_intensity);
            if (r.contains("sky_zenith")) {
                const auto& c = r.at("sky_zenith");
                if (c.size() >= 3) {
                    config.render.indirect.sky_zenith = glm::vec3(
                        c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                }
            }
            if (r.contains("sky_horizon")) {
                const auto& c = r.at("sky_horizon");
                if (c.size() >= 3) {
                    config.render.indirect.sky_horizon = glm::vec3(
                        c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                }
            }
            if (r.contains("sky_ground")) {
                const auto& c = r.at("sky_ground");
                if (c.size() >= 3) {
                    config.render.indirect.sky_ground = glm::vec3(
                        c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                }
            }

            // --- Sky model (physical sky + clouds) ---
            // Numbers may be written as integers (cloud_altitude: 1500).
            auto sky_num = [](const fkyaml::node& v) {
                return v.is_integer() ? static_cast<float>(v.get_value<int64_t>()) : v.get_value<float>();
            };
            read(r, "sky_model", config.render.sky_model);
            if (r.contains("atmosphere_density")) config.render.atmosphere_density = sky_num(r.at("atmosphere_density"));
            if (r.contains("ozone"))              config.render.ozone              = sky_num(r.at("ozone"));
            if (r.contains("sun_disc_size"))      config.render.sun_disc_size      = sky_num(r.at("sun_disc_size"));
            if (r.contains("moon_disc_size"))     config.render.moon_disc_size     = sky_num(r.at("moon_disc_size"));
            read(r, "sky_stars", config.render.sky_stars);
            read(r, "clouds", config.render.clouds);
            read(r, "cloud_type", config.render.cloud_type);
            if (r.contains("cloud_quality"))      config.render.cloud_quality      = parse_render_quality(r.at("cloud_quality").get_value<std::string>());
            if (r.contains("cloud_coverage"))     config.render.cloud_coverage     = sky_num(r.at("cloud_coverage"));
            if (r.contains("cloud_altitude"))     config.render.cloud_altitude     = sky_num(r.at("cloud_altitude"));
            if (r.contains("cloud_thickness"))    config.render.cloud_thickness    = sky_num(r.at("cloud_thickness"));
            if (r.contains("cloud_density"))      config.render.cloud_density      = sky_num(r.at("cloud_density"));
            if (r.contains("cloud_wind_speed"))   config.render.cloud_wind_speed   = sky_num(r.at("cloud_wind_speed"));
            if (r.contains("cloud_scale"))        config.render.cloud_scale        = sky_num(r.at("cloud_scale"));
            if (r.contains("flat_cloud_size"))        config.render.flat_cloud_size        = sky_num(r.at("flat_cloud_size"));
            if (r.contains("flat_cloud_opacity"))     config.render.flat_cloud_opacity     = sky_num(r.at("flat_cloud_opacity"));
            if (r.contains("flat_cloud_light_bands")) config.render.flat_cloud_light_bands = sky_num(r.at("flat_cloud_light_bands"));
            if (r.contains("flat_cloud_outline"))     config.render.flat_cloud_outline     = sky_num(r.at("flat_cloud_outline"));
            if (r.contains("flat_cloud_turbulence"))  config.render.flat_cloud_turbulence  = sky_num(r.at("flat_cloud_turbulence"));
            if (r.contains("flat_cloud_evolve"))      config.render.flat_cloud_evolve      = sky_num(r.at("flat_cloud_evolve"));
            read(r, "cloud_shadows", config.render.cloud_shadows);
            if (r.contains("cloud_shadow_strength"))  config.render.cloud_shadow_strength  = sky_num(r.at("cloud_shadow_strength"));
            if (r.contains("cloud_shadow_distance"))  config.render.cloud_shadow_distance  = sky_num(r.at("cloud_shadow_distance"));
            read(r, "cloud_camera_fade", config.render.cloud_camera_fade);
            if (r.contains("cloud_fade_start"))       config.render.cloud_fade_start       = sky_num(r.at("cloud_fade_start"));
            if (r.contains("cloud_fade_end"))         config.render.cloud_fade_end         = sky_num(r.at("cloud_fade_end"));

            // --- Shadows ---
            read(r, "shadows_enabled", config.render.shadows_enabled);
            read(r, "shadow_map_resolution", config.render.shadow_map_resolution);
            read(r, "shadow_cascades", config.render.shadow_cascades);
            read(r, "shadow_cascade_split_lambda", config.render.shadow_cascade_split_lambda);
            read(r, "shadow_fit", config.render.shadow_fit);
            read(r, "shadow_focus_radius", config.render.shadow_focus_radius);
            for (int ci = 0; ci < 4; ++ci) {
                const std::string key = "shadow_cascade_radius_" + std::to_string(ci + 1);
                if (r.contains(key)) config.render.shadow_cascade_radius[ci] = r.at(key).get_value<float>();
            }
            read(r, "shadow_focus_distance", config.render.shadow_focus_distance);
            read(r, "shadow_pcf_max_texels", config.render.shadow_pcf_max_texels);
            read(r, "shadow_receiver_plane_bias", config.render.shadow_receiver_plane_bias);
            read(r, "shadow_receiver_max_slope", config.render.shadow_receiver_max_slope);
            read(r, "cube_shadow_resolution", config.render.cube_shadow_resolution);
            read(r, "spot_shadow_resolution", config.render.spot_shadow_resolution);
            read(r, "local_shadow_atlas_resolution", config.render.local_shadow_atlas_resolution);
            read(r, "max_shadowed_point_lights", config.render.max_shadowed_point_lights);
            read(r, "max_shadowed_spot_lights", config.render.max_shadowed_spot_lights);
            read(r, "shadow_cache_enabled", config.render.shadow_cache_enabled);
            read(r, "shadow_cull_cache_enabled", config.render.shadow_cull_cache_enabled);
            read(r, "shadow_clip_radius", config.render.shadow_clip_radius);
            read(r, "shadow_clip_ratio", config.render.shadow_clip_ratio);
            read(r, "shadow_clip_snap_texels", config.render.shadow_clip_snap_texels);
            read(r, "shadow_clip_fresh_levels", config.render.shadow_clip_fresh_levels);
            read(r, "shadow_clip_static_budget", config.render.shadow_clip_static_budget);
            read(r, "shadow_clip_sun_snap_degrees", config.render.shadow_clip_sun_snap_degrees);
            read(r, "shadow_clip_caster_reach", config.render.shadow_clip_caster_reach);
            read(r, "shadow_bias", config.render.shadow_bias);
            read(r, "shadow_depth_bias_texels", config.render.shadow_depth_bias_texels);
            read(r, "shadow_slope_bias_texels", config.render.shadow_slope_bias_texels);
            read(r, "shadow_slope_bias_max", config.render.shadow_slope_bias_max);
            read(r, "shadow_fade_fraction", config.render.shadow_fade_fraction);
            read(r, "shadow_normal_bias", config.render.shadow_normal_bias);
            read(r, "shadow_distance", config.render.shadow_distance);
            read(r, "soft_shadows", config.render.soft_shadows);
            read(r, "shadow_softness", config.render.shadow_softness);
            read(r, "point_shadow_softness", config.render.point_shadow_softness);
            read(r, "spot_shadow_softness", config.render.spot_shadow_softness);
            read(r, "shadow_pcf_samples", config.render.shadow_pcf_samples);
            read(r, "shadow_pcss_enabled", config.render.shadow_pcss_enabled);
            read(r, "shadow_pcss_light_size", config.render.shadow_pcss_light_size);
            read(r, "shadow_pcss_search_texels", config.render.shadow_pcss_search_texels);
            read(r, "shadow_pcss_taps", config.render.shadow_pcss_taps);
            read(r, "contact_shadows_enabled", config.render.contact_shadows_enabled);
            read(r, "contact_shadow_length", config.render.contact_shadow_length);
            read(r, "contact_shadow_strength", config.render.contact_shadow_strength);
            read(r, "contact_shadow_thickness", config.render.contact_shadow_thickness);
            read(r, "contact_shadow_steps", config.render.contact_shadow_steps);
            read(r, "contact_shadow_temporal_enabled", config.render.contact_shadow_temporal_enabled);
            read(r, "contact_shadow_temporal_frames", config.render.contact_shadow_temporal_frames);

            // --- Outline ---
            read(r, "outline_mode", config.render.outline_mode);
            read(r, "outline_thickness", config.render.outline_thickness);
            if (r.contains("outline_color")) {
                const auto& c = r.at("outline_color");
                if (c.size() >= 3) {   // [r, g, b] or [r, g, b, a]; alpha defaults to opaque
                    config.render.outline_color = glm::vec4(
                        c.at(0).get_value<float>(), c.at(1).get_value<float>(),
                        c.at(2).get_value<float>(), c.size() >= 4 ? c.at(3).get_value<float>() : 1.0f);
                }
            }
            read(r, "depth_threshold", config.render.depth_threshold);
            read(r, "normal_threshold", config.render.normal_threshold);

            // --- X-ray (see-through silhouette) ---
            read(r, "xray_mode", config.render.xray_mode);
            read(r, "xray_thickness", config.render.xray_thickness);
            read(r, "xray_depth_fade", config.render.xray_depth_fade);
            if (r.contains("xray_color")) {
                const auto& c = r.at("xray_color");
                if (c.size() >= 3) {   // [r, g, b] or [r, g, b, a]; alpha defaults to opaque
                    config.render.xray_color = glm::vec4(
                        num(c.at(0)), num(c.at(1)), num(c.at(2)),
                        c.size() >= 4 ? num(c.at(3)) : 1.0f);
                }
            }

            // --- Palette ---
            read(r, "palette", config.render.palette_path);
            read(r, "grading_lut", config.render.grading_lut_path);

            // --- Dither ---
            read(r, "dither_strength", config.render.dither_strength);
            read(r, "texel_aa", config.render.texel_aa);

            // --- SSAO ---
            read(r, "ssao_radius", config.render.ssao_radius);
            read(r, "ssao_bias", config.render.ssao_bias);
            read(r, "ssao_power", config.render.ssao_power);
            read(r, "ssao_slices", config.render.ssao_slices);
            read(r, "ssao_steps", config.render.ssao_steps);
            read(r, "ssao_max_radius_px", config.render.ssao_max_radius_px);
            read(r, "ssao_blur_plane_sigma", config.render.ssao_blur_plane_sigma);
            read(r, "ssao_half_res", config.render.ssao_half_res);
            read(r, "ssao_blur_light", config.render.ssao_blur_light);
            read(r, "ssao_direct_lighting_strength", config.render.ssao_direct_lighting_strength);

            read(r, "ssao_temporal_enabled", config.render.ssao_temporal_enabled);
            read(r, "ssao_temporal_frames", config.render.ssao_temporal_frames);
            read(r, "ssao_temporal_gamma", config.render.ssao_temporal_gamma);
            read(r, "ssao_intensity", config.render.ssao_intensity);

            // --- SSR + SSGI ---
            read(r, "ssr_max_distance", config.render.ssr_max_distance);
            read(r, "ssr_max_iterations", config.render.ssr_max_iterations);
            read(r, "ssr_thickness", config.render.ssr_thickness);
            read(r, "ssr_thickness_scale", config.render.ssr_thickness_scale);
            read(r, "ssr_bias_texels", config.render.ssr_bias_texels);
            read(r, "ssr_roughness_cutoff", config.render.ssr_roughness_cutoff);
            read(r, "ssr_start_mip", config.render.ssr_start_mip);
            read(r, "ssr_min_mip0_steps", config.render.ssr_min_mip0_steps);
            read(r, "ssr_temporal_enabled", config.render.ssr_temporal_enabled);
            read(r, "ssr_temporal_frames", config.render.ssr_temporal_frames);
            read(r, "ssgi_temporal_frames", config.render.ssgi_temporal_frames);
            read(r, "ssr_temporal_blend", config.render.ssr_temporal_blend);
            read(r, "ssr_blur_radius", config.render.ssr_blur_radius);
            read(r, "ssr_blur_light", config.render.ssr_blur_light);
            read(r, "ssr_blur_zero_skip", config.render.ssr_blur_zero_skip);
            read(r, "ssr_jitter", config.render.ssr_jitter);
            read(r, "ssr_rays_per_pixel", config.render.ssr_rays_per_pixel);
            read(r, "ssr_cone_prefilter", config.render.ssr_cone_prefilter);
            read(r, "ssr_skip_behind", config.render.ssr_skip_behind);
            read(r, "ssr_half_res", config.render.ssr_half_res);
            read(r, "ssgi_resolution_scale", config.render.ssgi_resolution_scale);
            read(r, "ssr_skip_negligible", config.render.ssr_skip_negligible);
            read(r, "ssr_skip_threshold", config.render.ssr_skip_threshold);
            read(r, "ssr_temporal_gamma", config.render.ssr_temporal_gamma);
            read(r, "ssgi_traced", config.render.ssgi_traced);
            read(r, "ssgi_max_distance", config.render.ssgi_max_distance);
            read(r, "ssgi_blur_radius", config.render.ssgi_blur_radius);
            read(r, "ssgi_blur_light", config.render.ssgi_blur_light);
            read(r, "ssgi_max_iterations", config.render.ssgi_max_iterations);
            read(r, "ssgi_intensity", config.render.indirect.ssgi_intensity);
            read(r, "ssgi_distance", config.render.indirect.ssgi_distance);

            // --- Refraction (transparent/BLEND MESH objects only) ---
            read(r, "refraction_ior", config.render.refraction_ior);
            read(r, "refraction_thickness", config.render.refraction_thickness);
            read(r, "refraction_strength", config.render.refraction_strength);
            read(r, "refraction_max_offset", config.render.refraction_max_offset);
            read(r, "refraction_chromatic", config.render.refraction_chromatic);
            read(r, "refraction_blur", config.render.refraction_blur);
            read(r, "refraction_density", config.render.refraction_density);
            read(r, "refraction_fresnel", config.render.refraction_fresnel);
            if (r.contains("refraction_tint")) {
                const auto& c = r.at("refraction_tint");
                if (c.size() >= 3) {
                    config.render.refraction_tint = glm::vec3(
                        c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                }
            }
            read(r, "refraction_include_reflections", config.render.refraction_include_reflections);

            // --- Fog ---
            if (r.contains("fog_mode")) {
                // Exp2 (2) is gone: squaring a height-integrated optical depth has no physical
                // meaning. It loads as Exponential, which needs a somewhat lower density for
                // the same look at mid distances.
                int mode = r.at("fog_mode").get_value<int>();
                if (mode == 2) {
                    std::cerr << "[toy::core::AppConfig] fog_mode 2 (Exp2) is deprecated; using 1 (Exponential height fog).\n";
                    mode = 1;
                }
                config.render.fog_mode = mode == 0 ? 0 : 1;
            }
            read(r, "fog_density", config.render.fog_density);
            read(r, "fog_linear_start", config.render.fog_linear_start);
            read(r, "fog_linear_end", config.render.fog_linear_end);
            if (r.contains("fog_color")) {
                const auto& c = r.at("fog_color");
                if (c.size() >= 3) {
                    config.render.fog_color = glm::vec3(
                        c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                }
            }
            read(r, "fog_height_base", config.render.fog_height_base);
            read(r, "fog_height_falloff", config.render.fog_height_falloff);
            read(r, "fog_sky_blend", config.render.fog_sky_blend);
            read(r, "fog_sun_amount", config.render.fog_sun_amount);
            read(r, "fog_sun_anisotropy", config.render.fog_sun_anisotropy);
            read(r, "fog_max_opacity", config.render.fog_max_opacity);
            read(r, "fog_sun_start_distance", config.render.fog_sun_start_distance);
            read(r, "fog_start_distance", config.render.fog_start_distance);
            read(r, "fog_cutoff_distance", config.render.fog_cutoff_distance);
            read(r, "fog_sky_distance", config.render.fog_sky_distance);
            if (r.contains("fog_max_distance"))
                std::cerr << "[toy::core::AppConfig] fog_max_distance is deprecated and ignored -- use fog_cutoff_distance / fog_sky_distance.\n";
            if (r.contains("snow_cover_override")) {
                const auto& v = r.at("snow_cover_override");
                if (v.is_float_number()) config.render.snow_cover_override = static_cast<float>(v.get_value<double>());
                else if (v.is_integer()) config.render.snow_cover_override = static_cast<float>(v.get_value<int64_t>());
            }

            // --- Volumetrics (shared march settings; per-volume look lives on VolumeComponent) ---
            read(r, "volumetrics_step_count", config.render.volumetrics_step_count);
            read(r, "volumetrics_max_distance", config.render.volumetrics_max_distance);
            read(r, "volumetrics_resolution_scale", config.render.volumetrics_resolution_scale);
            read(r, "volumetrics_mode", config.render.volumetrics_mode);
            read(r, "volumetrics_froxel_tile", config.render.volumetrics_froxel_tile);
            read(r, "volumetrics_froxel_slices", config.render.volumetrics_froxel_slices);
            read(r, "volumetrics_froxel_history", config.render.volumetrics_froxel_history);
            read(r, "volumetrics_froxel_miss_samples", config.render.volumetrics_froxel_miss_samples);
            read(r, "volumetrics_froxel_lookup_jitter", config.render.volumetrics_froxel_lookup_jitter);
            read(r, "volumetrics_max_opacity", config.render.volumetrics_max_opacity);
            read(r, "volumetrics_sun_anisotropy", config.render.volumetrics_sun_anisotropy);
            read(r, "volumetrics_shadows_enabled", config.render.volumetrics_shadows_enabled);
            read(r, "volumetrics_light_scatter", config.render.volumetrics_light_scatter);
            read(r, "volumetrics_max_scatter_lights", config.render.volumetrics_max_scatter_lights);

            // --- Mesh visibility ---
            read(r, "mesh_lod_bias", config.render.mesh_lod_bias);
            read(r, "shadow_min_caster_texels", config.render.shadow_min_caster_texels);

            read(r, "bloom_threshold", config.render.bloom_threshold);
            read(r, "bloom_soft_knee", config.render.bloom_soft_knee);
            read(r, "bloom_intensity", config.render.bloom_intensity);
            read(r, "bloom_scatter", config.render.bloom_scatter);
            read(r, "bloom_radius", config.render.bloom_radius);
            read(r, "bloom_clamp", config.render.bloom_clamp);

            // --- Tilt shift ---
            read(r, "tilt_shift_focus_center", config.render.tilt_shift_focus_center);
            read(r, "tilt_shift_focus_width", config.render.tilt_shift_focus_width);
            read(r, "tilt_shift_ramp_width", config.render.tilt_shift_ramp_width);
            read(r, "tilt_shift_blur_top", config.render.tilt_shift_blur_top);
            read(r, "tilt_shift_blur_bottom", config.render.tilt_shift_blur_bottom);
            read(r, "tilt_shift_max_radius", config.render.tilt_shift_max_radius);
            read(r, "tilt_shift_angle", config.render.tilt_shift_angle);

            // --- Depth of field ---
            read(r, "dof_focus_mode", config.render.dof_focus_mode);
            read(r, "dof_focus_object", config.render.dof_focus_object);
            read(r, "dof_focus_smoothing", config.render.dof_focus_smoothing);
            read(r, "dof_focus_distance", config.render.dof_focus_distance);
            read(r, "dof_focus_range", config.render.dof_focus_range);
            read(r, "dof_focus_cover_object", config.render.dof_focus_cover_object);
            read(r, "dof_blur_scale", config.render.dof_blur_scale);
            read(r, "dof_aperture", config.render.dof_aperture);
            read(r, "dof_focal_length", config.render.dof_focal_length);
            read(r, "dof_sensor_width", config.render.dof_sensor_width);
            read(r, "dof_max_radius", config.render.dof_max_radius);
            read(r, "dof_sample_count", config.render.dof_sample_count);
            read(r, "dof_blade_count", config.render.dof_blade_count);
            read(r, "dof_blade_rotation", config.render.dof_blade_rotation);

            // --- Motion blur ---
            read(r, "motion_blur", config.render.motion_blur);
            read(r, "motion_blur_intensity", config.render.motion_blur_intensity);

            // --- Anti-aliasing ---
            read(r, "aa_mode", config.render.aa_mode);
            read(r, "fxaa_subpixel", config.render.fxaa_subpixel);
            read(r, "fxaa_edge_threshold", config.render.fxaa_edge_threshold);
            read(r, "fxaa_edge_threshold_min", config.render.fxaa_edge_threshold_min);
            read(r, "smaa_threshold", config.render.smaa_threshold);
            read(r, "smaa_max_search_steps", config.render.smaa_max_search_steps);
            read(r, "taa_blending_weight", config.render.taa_blending_weight);
            read(r, "taa_weight_scale", config.render.taa_weight_scale);
            read(r, "taa_feedback_motion", config.render.taa_feedback_motion);
            read(r, "taa_sharpness", config.render.taa_sharpness);
            read(r, "taa_variance_gamma", config.render.taa_variance_gamma);

            // --- SDF ---
            read(r, "sdf_max_steps", config.render.sdf_max_steps);
            read(r, "sdf_shadow_max_steps", config.render.sdf_shadow_max_steps);
            read(r, "sdf_max_renderers", config.render.sdf_max_renderers);
            read(r, "sdf_max_shapes", config.render.sdf_max_shapes);
        }

        if (root.contains("jobs")) {
            const auto& j = root.at("jobs");
            read(j, "worker_threads", config.jobs.worker_threads);
            read(j, "parallel_threshold", config.jobs.parallel_threshold);
        }

        if (root.contains("debug")) {
            const auto& d = root.at("debug");
            if (d.contains("overlay")) {
                // A YAML 1.1 reader takes a bare `off` for a boolean; accept either spelling.
                const auto& o = d.at("overlay");
                config.debug.overlay = o.is_boolean() ? (o.get_value<bool>() ? "fps" : "off")
                                                      : o.get_value<std::string>();
            }
        }

        if (root.contains("particles")) {
            const auto& pn = root.at("particles");
            read(pn, "gpu_enabled", config.particles.gpu_enabled);
        }

        if (root.contains("save")) {
            const auto& sv = root.at("save");
            read(sv, "encode", config.save.encode);
            if (sv.contains("quick_slot")) {
                const auto& q = sv.at("quick_slot");
                config.save.quick_slot = q.is_null() ? std::string() : q.get_value<std::string>();
            }
        }

        // Schema owned by physxcoopa itself (see util/physics_settings.h's doc) rather than
        // hand-parsed field-by-field here, unlike every other block above -- physics settings
        // are physxcoopa's concept end to end (PhysicsWorld/PhysicsSystem consume the parsed
        // struct directly), so duplicating its YAML shape in toyengine would just be a second
        // place to keep in sync.
        if (root.contains("physics")) {
            config.physics = coopa::physx::util::parse_physics_settings(root.at("physics"));
        }
        // Also physxcoopa-owned. After physics: `layers:` may name physics layers.
        if (root.contains("navigation")) {
            config.navigation = coopa::physx::nav::parse_nav_settings(root.at("navigation"), config.physics.layer_names);
        }

        if (root.contains("audio")) {
            const auto& a = root.at("audio");
            auto num = [&](const char* k, float& out) {
                if (!a.contains(k)) return;
                const auto& v = a.at(k);
                if (v.is_float_number()) out = static_cast<float>(v.get_value<double>());
                else if (v.is_integer()) out = static_cast<float>(v.get_value<int64_t>());
            };
            read(a, "enabled", config.audio.enabled);
            read(a, "sample_rate", config.audio.sample_rate);
            read(a, "device", config.audio.device);
            num("master", config.audio.master);
            num("music", config.audio.music);
            num("sfx", config.audio.sfx);
            num("ui", config.audio.ui);
        }

        if (root.contains("output")) {
            const auto& o = root.at("output");
            read(o, "save_on_exit", config.output.save_on_exit);
            read(o, "filepath", config.output.filepath);
            read(o, "save_low_res", config.output.save_low_res);
        }
    } catch (const std::exception& e) {
        std::cerr << "[toy::core::AppConfig] Warning: Failed to parse config file (" << e.what() << "), using defaults.\n";
    }
    return config;
}

AppConfig AppConfig::with_scene_settings(const fkyaml::node& settings) const {
    if (!has_scene_overrides(settings)) return *this;
    fkyaml::node merged = source;
    for (const char* section : kSceneSettingsSections) {
        if (!settings.contains(section) || !settings.at(section).is_mapping()) continue;
        if (!merged.contains(section) || !merged.at(section).is_mapping()) merged[section] = fkyaml::node::mapping();
        for (auto item : settings.at(section).map_items()) {
            merged[section][item.key().get_value<std::string>()] = item.value();
        }
    }
    const AppConfig parsed = from_node(merged);
    AppConfig out = *this;
    out.render = parsed.render;
    out.physics = parsed.physics;
    out.navigation = parsed.navigation;
    return out;
}

bool AppConfig::has_scene_overrides(const fkyaml::node& settings) {
    if (!settings.is_mapping()) return false;
    for (const char* section : kSceneSettingsSections) {
        if (settings.contains(section) && settings.at(section).is_mapping() && settings.at(section).size() > 0) return true;
    }
    return false;
}

} // namespace core
} // namespace toy
