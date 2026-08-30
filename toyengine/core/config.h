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
    bool        save_low_res  = true;
};

/**
 * @struct AppConfig
 * @brief Aggregated runtime configuration for toyengine.
 */
struct AppConfig {
    coopa::scene::SceneConfig    scene;
    WindowConfig                 window;
    render::PixelRenderConfig    render;
    OutputConfig                 output;

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
                if (r.contains("ssr_enabled"))       config.render.ssr_enabled       = r.at("ssr_enabled").get_value<bool>();
                if (r.contains("transparency_enabled")) config.render.transparency_enabled = r.at("transparency_enabled").get_value<bool>();
                if (r.contains("ssr_reflect_transparent")) config.render.ssr_reflect_transparent = r.at("ssr_reflect_transparent").get_value<bool>();
                if (r.contains("fog_enabled"))       config.render.fog_enabled       = r.at("fog_enabled").get_value<bool>();
                if (r.contains("bloom_enabled"))     config.render.bloom_enabled     = r.at("bloom_enabled").get_value<bool>();

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

                // --- Shadows ---
                if (r.contains("shadows_enabled"))        config.render.shadows_enabled        = r.at("shadows_enabled").get_value<bool>();
                if (r.contains("shadow_map_resolution"))  config.render.shadow_map_resolution  = r.at("shadow_map_resolution").get_value<uint32_t>();
                if (r.contains("cube_shadow_resolution")) config.render.cube_shadow_resolution = r.at("cube_shadow_resolution").get_value<uint32_t>();
                if (r.contains("shadow_bias"))            config.render.shadow_bias            = r.at("shadow_bias").get_value<float>();
                if (r.contains("shadow_max_extent"))      config.render.shadow_max_extent      = r.at("shadow_max_extent").get_value<float>();

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

                if (r.contains("bloom_threshold")) config.render.bloom_threshold = r.at("bloom_threshold").get_value<float>();
                if (r.contains("bloom_soft_knee")) config.render.bloom_soft_knee = r.at("bloom_soft_knee").get_value<float>();
                if (r.contains("bloom_intensity")) config.render.bloom_intensity = r.at("bloom_intensity").get_value<float>();
                if (r.contains("bloom_scatter"))   config.render.bloom_scatter   = r.at("bloom_scatter").get_value<float>();
                if (r.contains("bloom_radius"))    config.render.bloom_radius    = r.at("bloom_radius").get_value<float>();
                if (r.contains("bloom_clamp"))     config.render.bloom_clamp     = r.at("bloom_clamp").get_value<float>();
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
