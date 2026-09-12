/**
 * @file config.h
 * @brief Runtime configuration loaded from assets/config.yaml.
 *
 * Mirrors blendy's blendy::core::AppConfig pattern (see
 * blendy/src/blendy/core/config.h): every field has an in-class default, every
 * YAML key is individually optional, unknown keys are silently ignored, and a
 * missing or malformed file falls back to defaults rather than failing
 * startup. Unlike blendy, this parses fkYAML directly instead of going
 * through caml::CAMLMap -- toyengine has no need for encrypted/compressed
 * config or scene files, so it avoids the OpenSSL and zstd dependencies
 * entirely.
 */

#ifndef TOYENGINE_CORE_CONFIG_H
#define TOYENGINE_CORE_CONFIG_H

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <coopa/scene/config.h>
#include <physxcoopa/util/physics_settings.h>
#include <toyengine/render/pixel_render_config.h>

namespace toy {
namespace core {

/**
 * @struct WindowConfig
 * @brief Window and presentation settings.
 */
struct WindowConfig {
    std::string title  = "toyengine";
    uint32_t    width  = 1920;
    uint32_t    height = 1080;
    bool        vsync  = true;
};

/**
 * @struct OutputConfig
 * @brief Headless frame export configuration (see util/screenshot.h).
 */
struct OutputConfig {
    bool        save_on_exit  = true;
    std::string filepath      = "./output/frame.png";
    /**
     * True saves the internal low-resolution buffer 1:1 (pixel-perfect, but BEFORE any
     * display-resolution-only effect such as tilt shift). False saves the final image
     * actually shown in the window, at display resolution. See Engine::save_screenshot().
     */
    bool        save_low_res  = true;
};

/**
 * @struct JobsConfig
 * @brief Job-system sizing and the per-frame parallel-dispatch threshold.
 */
struct JobsConfig {
    /// Worker threads for the shared JobEngine. 0 = std::thread::hardware_concurrency().
    unsigned int worker_threads = 0;
    /**
     * Minimum element count before a per-frame loop dispatches jobs instead of
     * running serially. Deliberately low so the pixel demo (8 MeshRenderers,
     * 4 SdfRenderers) exercises the parallel path; libcoopa's own house value for
     * production-sized workloads is 256 (see AnimationSystem::parallel_threshold_).
     */
    std::size_t parallel_threshold = 4;
};

/**
 * @struct AppConfig
 * @brief Aggregated runtime configuration for toyengine.
 */
struct AppConfig {
    coopa::scene::SceneConfig         scene;
    WindowConfig                      window;
    render::PixelRenderConfig         render;
    OutputConfig                      output;
    JobsConfig                        jobs;
    coopa::physx::util::PhysicsSettings physics;

    /**
     * @brief Loads application configuration from a YAML file.
     * @param path Path to the configuration file (e.g. assets/config.yaml).
     * @return Loaded AppConfig, with any missing/malformed fields left at default.
     */
    static AppConfig load(const std::string& path) {
        AppConfig config;
        try {
            if (!std::filesystem::exists(path)) {
                std::cerr << "[toy::core::AppConfig] Config file not found at " << path << ", using defaults.\n";
                return config;
            }

            std::ifstream in(path);
            fkyaml::node root = fkyaml::node::deserialize(in);

            if (root.contains("scene")) {
                const auto& s = root.at("scene");
                if (s.contains("default_scene")) {
                    config.scene.default_scene = s.at("default_scene").get_value<std::string>();
                }
            }

            if (root.contains("window")) {
                const auto& w = root.at("window");
                if (w.contains("title"))  config.window.title  = w.at("title").get_value<std::string>();
                if (w.contains("width"))  config.window.width  = w.at("width").get_value<uint32_t>();
                if (w.contains("height")) config.window.height = w.at("height").get_value<uint32_t>();
                if (w.contains("vsync"))  config.window.vsync  = w.at("vsync").get_value<bool>();
            }

            if (root.contains("render")) {
                const auto& r = root.at("render");

                // --- Feature toggles ---
                if (r.contains("outline_enabled"))   config.render.outline_enabled   = r.at("outline_enabled").get_value<bool>();
                if (r.contains("palette_enabled"))   config.render.palette_enabled   = r.at("palette_enabled").get_value<bool>();
                if (r.contains("dither_enabled"))    config.render.dither_enabled    = r.at("dither_enabled").get_value<bool>();
                if (r.contains("camera_pixel_snap")) config.render.camera_pixel_snap = r.at("camera_pixel_snap").get_value<bool>();
                if (r.contains("soft_lighting"))     config.render.soft_lighting     = r.at("soft_lighting").get_value<bool>();
                if (r.contains("ssao_enabled"))      config.render.ssao_enabled      = r.at("ssao_enabled").get_value<bool>();
                if (r.contains("ssao_debug_view"))   config.render.ssao_debug_view   = r.at("ssao_debug_view").get_value<bool>();
                if (r.contains("ssr_enabled"))       config.render.ssr_enabled       = r.at("ssr_enabled").get_value<bool>();
                if (r.contains("transparency_enabled")) config.render.transparency_enabled = r.at("transparency_enabled").get_value<bool>();
                if (r.contains("ssr_reflect_transparent")) config.render.ssr_reflect_transparent = r.at("ssr_reflect_transparent").get_value<bool>();
                if (r.contains("refraction_enabled")) config.render.refraction_enabled = r.at("refraction_enabled").get_value<bool>();
                if (r.contains("fog_enabled"))       config.render.fog_enabled       = r.at("fog_enabled").get_value<bool>();
                if (r.contains("volumetrics_enabled")) config.render.volumetrics_enabled = r.at("volumetrics_enabled").get_value<bool>();
                if (r.contains("sdf_enabled"))         config.render.sdf_enabled         = r.at("sdf_enabled").get_value<bool>();
                if (r.contains("sdf_shadows_enabled")) config.render.sdf_shadows_enabled = r.at("sdf_shadows_enabled").get_value<bool>();
                if (r.contains("bloom_enabled"))     config.render.bloom_enabled     = r.at("bloom_enabled").get_value<bool>();
                if (r.contains("tilt_shift_enabled")) config.render.tilt_shift_enabled = r.at("tilt_shift_enabled").get_value<bool>();
                if (r.contains("dof_enabled"))        config.render.dof_enabled        = r.at("dof_enabled").get_value<bool>();
                if (r.contains("dof_debug_view"))     config.render.dof_debug_view     = r.at("dof_debug_view").get_value<bool>();
                if (r.contains("debug_lines_enabled")) config.render.debug_lines_enabled = r.at("debug_lines_enabled").get_value<bool>();

                // --- Internal resolution ---
                if (r.contains("resolution_mode"))        config.render.resolution_mode = r.at("resolution_mode").get_value<std::string>();
                if (r.contains("render_width"))            config.render.render_width    = r.at("render_width").get_value<uint32_t>();
                if (r.contains("render_height"))           config.render.render_height   = r.at("render_height").get_value<uint32_t>();
                if (r.contains("scale_divisor"))           config.render.scale_divisor   = r.at("scale_divisor").get_value<uint32_t>();
                if (r.contains("upscale_mode"))            config.render.upscale_mode    = r.at("upscale_mode").get_value<std::string>();

                // --- Lighting ---
                if (r.contains("exposure"))          config.render.exposure          = r.at("exposure").get_value<float>();
                if (r.contains("light_bands"))       config.render.light_bands       = r.at("light_bands").get_value<float>();
                if (r.contains("spec_threshold"))    config.render.spec_threshold    = r.at("spec_threshold").get_value<float>();
                if (r.contains("rim_strength"))      config.render.rim_strength      = r.at("rim_strength").get_value<float>();
                if (r.contains("ambient_intensity")) config.render.indirect.ambient_intensity = r.at("ambient_intensity").get_value<float>();
                if (r.contains("sky_intensity"))     config.render.indirect.sky_intensity     = r.at("sky_intensity").get_value<float>();
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

                // --- Shadows ---
                if (r.contains("shadows_enabled"))        config.render.shadows_enabled        = r.at("shadows_enabled").get_value<bool>();
                if (r.contains("shadow_map_resolution"))  config.render.shadow_map_resolution  = r.at("shadow_map_resolution").get_value<uint32_t>();
                if (r.contains("cube_shadow_resolution")) config.render.cube_shadow_resolution = r.at("cube_shadow_resolution").get_value<uint32_t>();
                if (r.contains("shadow_bias"))            config.render.shadow_bias            = r.at("shadow_bias").get_value<float>();
                if (r.contains("shadow_distance"))        config.render.shadow_distance        = r.at("shadow_distance").get_value<float>();
                if (r.contains("soft_shadows"))            config.render.soft_shadows            = r.at("soft_shadows").get_value<bool>();
                if (r.contains("shadow_softness"))         config.render.shadow_softness         = r.at("shadow_softness").get_value<float>();
                if (r.contains("point_shadow_softness"))   config.render.point_shadow_softness   = r.at("point_shadow_softness").get_value<float>();
                if (r.contains("shadow_pcf_samples"))      config.render.shadow_pcf_samples      = r.at("shadow_pcf_samples").get_value<uint32_t>();

                // --- Outline ---
                if (r.contains("outline_thickness")) config.render.outline_thickness = r.at("outline_thickness").get_value<float>();
                if (r.contains("outline_color")) {
                    const auto& c = r.at("outline_color");
                    if (c.size() >= 4) {
                        config.render.outline_color = glm::vec4(
                            c.at(0).get_value<float>(), c.at(1).get_value<float>(),
                            c.at(2).get_value<float>(), c.at(3).get_value<float>());
                    }
                }
                if (r.contains("depth_threshold"))   config.render.depth_threshold   = r.at("depth_threshold").get_value<float>();
                if (r.contains("normal_threshold"))  config.render.normal_threshold  = r.at("normal_threshold").get_value<float>();

                // --- Palette ---
                if (r.contains("palette")) config.render.palette_path = r.at("palette").get_value<std::string>();

                // --- Dither ---
                if (r.contains("dither_strength")) config.render.dither_strength = r.at("dither_strength").get_value<float>();

                // --- SSAO ---
                if (r.contains("ssao_radius"))           config.render.ssao_radius           = r.at("ssao_radius").get_value<float>();
                if (r.contains("ssao_bias"))             config.render.ssao_bias             = r.at("ssao_bias").get_value<float>();
                if (r.contains("ssao_power"))            config.render.ssao_power            = r.at("ssao_power").get_value<float>();
                if (r.contains("ssao_kernel_size"))      config.render.ssao_kernel_size      = r.at("ssao_kernel_size").get_value<int>();
                if (r.contains("ssao_temporal_enabled")) config.render.ssao_temporal_enabled = r.at("ssao_temporal_enabled").get_value<bool>();
                if (r.contains("ssao_temporal_blend"))   config.render.ssao_temporal_blend   = r.at("ssao_temporal_blend").get_value<float>();

                // --- SSR + SSGI ---
                if (r.contains("ssr_max_distance"))     config.render.ssr_max_distance     = r.at("ssr_max_distance").get_value<float>();
                if (r.contains("ssr_max_iterations"))   config.render.ssr_max_iterations   = r.at("ssr_max_iterations").get_value<int>();
                if (r.contains("ssr_thickness"))        config.render.ssr_thickness        = r.at("ssr_thickness").get_value<float>();
                if (r.contains("ssr_thickness_scale"))  config.render.ssr_thickness_scale  = r.at("ssr_thickness_scale").get_value<float>();
                if (r.contains("ssr_bias_texels"))      config.render.ssr_bias_texels      = r.at("ssr_bias_texels").get_value<float>();
                if (r.contains("ssr_roughness_cutoff")) config.render.ssr_roughness_cutoff = r.at("ssr_roughness_cutoff").get_value<float>();
                if (r.contains("ssr_start_mip"))        config.render.ssr_start_mip        = r.at("ssr_start_mip").get_value<int>();
                if (r.contains("ssr_min_mip0_steps"))   config.render.ssr_min_mip0_steps   = r.at("ssr_min_mip0_steps").get_value<int>();
                if (r.contains("ssr_temporal_enabled")) config.render.ssr_temporal_enabled = r.at("ssr_temporal_enabled").get_value<bool>();
                if (r.contains("ssr_temporal_blend"))   config.render.ssr_temporal_blend   = r.at("ssr_temporal_blend").get_value<float>();
                if (r.contains("ssr_blur_radius"))      config.render.ssr_blur_radius      = r.at("ssr_blur_radius").get_value<float>();
                if (r.contains("ssr_jitter"))           config.render.ssr_jitter           = r.at("ssr_jitter").get_value<float>();
                if (r.contains("ssr_temporal_gamma"))   config.render.ssr_temporal_gamma   = r.at("ssr_temporal_gamma").get_value<float>();
                if (r.contains("ssgi_intensity"))       config.render.indirect.ssgi_intensity = r.at("ssgi_intensity").get_value<float>();
                if (r.contains("ssgi_distance"))        config.render.indirect.ssgi_distance  = r.at("ssgi_distance").get_value<float>();

                // --- Refraction (transparent/BLEND MESH objects only) ---
                if (r.contains("refraction_ior"))              config.render.refraction_ior           = r.at("refraction_ior").get_value<float>();
                if (r.contains("refraction_thickness"))        config.render.refraction_thickness     = r.at("refraction_thickness").get_value<float>();
                if (r.contains("refraction_strength"))         config.render.refraction_strength      = r.at("refraction_strength").get_value<float>();
                if (r.contains("refraction_max_offset"))       config.render.refraction_max_offset    = r.at("refraction_max_offset").get_value<float>();
                if (r.contains("refraction_chromatic"))        config.render.refraction_chromatic     = r.at("refraction_chromatic").get_value<float>();
                if (r.contains("refraction_blur"))             config.render.refraction_blur          = r.at("refraction_blur").get_value<float>();
                if (r.contains("refraction_density"))          config.render.refraction_density       = r.at("refraction_density").get_value<float>();
                if (r.contains("refraction_fresnel"))          config.render.refraction_fresnel       = r.at("refraction_fresnel").get_value<bool>();
                if (r.contains("refraction_tint")) {
                    const auto& c = r.at("refraction_tint");
                    if (c.size() >= 3) {
                        config.render.refraction_tint = glm::vec3(
                            c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                    }
                }
                if (r.contains("refraction_include_reflections")) config.render.refraction_include_reflections = r.at("refraction_include_reflections").get_value<bool>();

                // --- Fog ---
                if (r.contains("fog_mode"))           config.render.fog_mode           = r.at("fog_mode").get_value<int>();
                if (r.contains("fog_density"))        config.render.fog_density        = r.at("fog_density").get_value<float>();
                if (r.contains("fog_linear_start"))   config.render.fog_linear_start   = r.at("fog_linear_start").get_value<float>();
                if (r.contains("fog_linear_end"))     config.render.fog_linear_end     = r.at("fog_linear_end").get_value<float>();
                if (r.contains("fog_color")) {
                    const auto& c = r.at("fog_color");
                    if (c.size() >= 3) {
                        config.render.fog_color = glm::vec3(
                            c.at(0).get_value<float>(), c.at(1).get_value<float>(), c.at(2).get_value<float>());
                    }
                }
                if (r.contains("fog_height_base"))    config.render.fog_height_base    = r.at("fog_height_base").get_value<float>();
                if (r.contains("fog_height_falloff")) config.render.fog_height_falloff = r.at("fog_height_falloff").get_value<float>();
                if (r.contains("fog_sky_blend"))      config.render.fog_sky_blend      = r.at("fog_sky_blend").get_value<float>();
                if (r.contains("fog_sun_amount"))     config.render.fog_sun_amount     = r.at("fog_sun_amount").get_value<float>();
                if (r.contains("fog_sun_anisotropy")) config.render.fog_sun_anisotropy = r.at("fog_sun_anisotropy").get_value<float>();
                if (r.contains("fog_max_opacity"))    config.render.fog_max_opacity    = r.at("fog_max_opacity").get_value<float>();
                if (r.contains("fog_max_distance"))   config.render.fog_max_distance   = r.at("fog_max_distance").get_value<float>();

                // --- Volumetrics (shared march settings; per-volume look lives on VolumeComponent) ---
                if (r.contains("volumetrics_step_count"))     config.render.volumetrics_step_count     = r.at("volumetrics_step_count").get_value<int>();
                if (r.contains("volumetrics_max_distance"))   config.render.volumetrics_max_distance   = r.at("volumetrics_max_distance").get_value<float>();
                if (r.contains("volumetrics_max_opacity"))    config.render.volumetrics_max_opacity    = r.at("volumetrics_max_opacity").get_value<float>();
                if (r.contains("volumetrics_sun_anisotropy")) config.render.volumetrics_sun_anisotropy = r.at("volumetrics_sun_anisotropy").get_value<float>();
                if (r.contains("volumetrics_debug_view"))     config.render.volumetrics_debug_view     = r.at("volumetrics_debug_view").get_value<bool>();

                if (r.contains("bloom_threshold")) config.render.bloom_threshold = r.at("bloom_threshold").get_value<float>();
                if (r.contains("bloom_soft_knee")) config.render.bloom_soft_knee = r.at("bloom_soft_knee").get_value<float>();
                if (r.contains("bloom_intensity")) config.render.bloom_intensity = r.at("bloom_intensity").get_value<float>();
                if (r.contains("bloom_scatter"))   config.render.bloom_scatter   = r.at("bloom_scatter").get_value<float>();
                if (r.contains("bloom_radius"))    config.render.bloom_radius    = r.at("bloom_radius").get_value<float>();
                if (r.contains("bloom_clamp"))     config.render.bloom_clamp     = r.at("bloom_clamp").get_value<float>();

                // --- Tilt shift ---
                if (r.contains("tilt_shift_focus_center")) config.render.tilt_shift_focus_center = r.at("tilt_shift_focus_center").get_value<float>();
                if (r.contains("tilt_shift_focus_width"))  config.render.tilt_shift_focus_width  = r.at("tilt_shift_focus_width").get_value<float>();
                if (r.contains("tilt_shift_ramp_width"))   config.render.tilt_shift_ramp_width   = r.at("tilt_shift_ramp_width").get_value<float>();
                if (r.contains("tilt_shift_blur_top"))     config.render.tilt_shift_blur_top     = r.at("tilt_shift_blur_top").get_value<float>();
                if (r.contains("tilt_shift_blur_bottom"))  config.render.tilt_shift_blur_bottom  = r.at("tilt_shift_blur_bottom").get_value<float>();
                if (r.contains("tilt_shift_max_radius"))   config.render.tilt_shift_max_radius   = r.at("tilt_shift_max_radius").get_value<float>();
                if (r.contains("tilt_shift_angle"))        config.render.tilt_shift_angle        = r.at("tilt_shift_angle").get_value<float>();

                // --- Depth of field ---
                if (r.contains("dof_focus_mode"))      config.render.dof_focus_mode      = r.at("dof_focus_mode").get_value<std::string>();
                if (r.contains("dof_focus_object"))    config.render.dof_focus_object    = r.at("dof_focus_object").get_value<std::string>();
                if (r.contains("dof_focus_smoothing")) config.render.dof_focus_smoothing = r.at("dof_focus_smoothing").get_value<float>();
                if (r.contains("dof_focus_distance")) config.render.dof_focus_distance = r.at("dof_focus_distance").get_value<float>();
                if (r.contains("dof_aperture"))       config.render.dof_aperture       = r.at("dof_aperture").get_value<float>();
                if (r.contains("dof_focal_length"))   config.render.dof_focal_length   = r.at("dof_focal_length").get_value<float>();
                if (r.contains("dof_sensor_width"))   config.render.dof_sensor_width   = r.at("dof_sensor_width").get_value<float>();
                if (r.contains("dof_max_radius"))     config.render.dof_max_radius     = r.at("dof_max_radius").get_value<float>();
                if (r.contains("dof_sample_count"))   config.render.dof_sample_count   = r.at("dof_sample_count").get_value<int>();
                if (r.contains("dof_blade_count"))    config.render.dof_blade_count    = r.at("dof_blade_count").get_value<int>();
                if (r.contains("dof_blade_rotation")) config.render.dof_blade_rotation = r.at("dof_blade_rotation").get_value<float>();

                // --- Anti-aliasing ---
                if (r.contains("aa_mode"))                  config.render.aa_mode                  = r.at("aa_mode").get_value<std::string>();
                if (r.contains("fxaa_subpixel"))             config.render.fxaa_subpixel             = r.at("fxaa_subpixel").get_value<float>();
                if (r.contains("fxaa_edge_threshold"))       config.render.fxaa_edge_threshold       = r.at("fxaa_edge_threshold").get_value<float>();
                if (r.contains("fxaa_edge_threshold_min"))   config.render.fxaa_edge_threshold_min   = r.at("fxaa_edge_threshold_min").get_value<float>();
                if (r.contains("smaa_threshold"))            config.render.smaa_threshold            = r.at("smaa_threshold").get_value<float>();
                if (r.contains("smaa_max_search_steps"))     config.render.smaa_max_search_steps     = r.at("smaa_max_search_steps").get_value<int>();
                if (r.contains("taa_blending_weight"))       config.render.taa_blending_weight       = r.at("taa_blending_weight").get_value<float>();
                if (r.contains("taa_weight_scale"))          config.render.taa_weight_scale          = r.at("taa_weight_scale").get_value<float>();

                // --- SDF ---
                if (r.contains("sdf_max_steps"))        config.render.sdf_max_steps        = r.at("sdf_max_steps").get_value<uint32_t>();
                if (r.contains("sdf_shadow_max_steps")) config.render.sdf_shadow_max_steps = r.at("sdf_shadow_max_steps").get_value<uint32_t>();
                if (r.contains("sdf_max_renderers"))    config.render.sdf_max_renderers    = r.at("sdf_max_renderers").get_value<uint32_t>();
                if (r.contains("sdf_max_shapes"))       config.render.sdf_max_shapes       = r.at("sdf_max_shapes").get_value<uint32_t>();
            }

            if (root.contains("jobs")) {
                const auto& j = root.at("jobs");
                if (j.contains("worker_threads"))    config.jobs.worker_threads    = j.at("worker_threads").get_value<unsigned int>();
                if (j.contains("parallel_threshold")) config.jobs.parallel_threshold = j.at("parallel_threshold").get_value<std::size_t>();
            }

            // Schema owned by physxcoopa itself (see util/physics_settings.h's doc) rather than
            // hand-parsed field-by-field here, unlike every other block above -- physics settings
            // are physxcoopa's concept end to end (PhysicsWorld/PhysicsSystem consume the parsed
            // struct directly), so duplicating its YAML shape in toyengine would just be a second
            // place to keep in sync.
            if (root.contains("physics")) {
                config.physics = coopa::physx::util::parse_physics_settings(root.at("physics"));
            }

            if (root.contains("output")) {
                const auto& o = root.at("output");
                if (o.contains("save_on_exit")) config.output.save_on_exit = o.at("save_on_exit").get_value<bool>();
                if (o.contains("filepath"))     config.output.filepath     = o.at("filepath").get_value<std::string>();
                if (o.contains("save_low_res")) config.output.save_low_res = o.at("save_low_res").get_value<bool>();
            }
        } catch (const std::exception& e) {
            std::cerr << "[toy::core::AppConfig] Warning: Failed to parse config file (" << e.what() << "), using defaults.\n";
        }
        return config;
    }
};

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_CONFIG_H
