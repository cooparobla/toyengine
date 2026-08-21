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
                if (r.contains("resolution_mode"))       config.render.resolution_mode       = r.at("resolution_mode").get_value<std::string>();
                if (r.contains("render_width"))           config.render.render_width           = r.at("render_width").get_value<uint32_t>();
                if (r.contains("render_height"))          config.render.render_height          = r.at("render_height").get_value<uint32_t>();
                if (r.contains("scale_divisor"))          config.render.scale_divisor          = r.at("scale_divisor").get_value<uint32_t>();
                if (r.contains("upscale_mode"))           config.render.upscale_mode           = r.at("upscale_mode").get_value<std::string>();
                if (r.contains("exposure"))               config.render.exposure               = r.at("exposure").get_value<float>();
                if (r.contains("light_bands"))            config.render.light_bands            = r.at("light_bands").get_value<float>();
                if (r.contains("spec_threshold"))         config.render.spec_threshold         = r.at("spec_threshold").get_value<float>();
                if (r.contains("rim_strength"))           config.render.rim_strength           = r.at("rim_strength").get_value<float>();
                if (r.contains("shadows_enabled"))        config.render.shadows_enabled        = r.at("shadows_enabled").get_value<bool>();
                if (r.contains("shadow_map_resolution"))  config.render.shadow_map_resolution  = r.at("shadow_map_resolution").get_value<uint32_t>();
                if (r.contains("cube_shadow_resolution")) config.render.cube_shadow_resolution = r.at("cube_shadow_resolution").get_value<uint32_t>();
                if (r.contains("shadow_bias"))            config.render.shadow_bias            = r.at("shadow_bias").get_value<float>();
                if (r.contains("shadow_max_extent"))      config.render.shadow_max_extent      = r.at("shadow_max_extent").get_value<float>();
            }

            if (root.contains("pixel")) {
                const auto& p = root.at("pixel");
                if (p.contains("outline_enabled"))   config.render.outline_enabled   = p.at("outline_enabled").get_value<bool>();
                if (p.contains("outline_thickness")) config.render.outline_thickness = p.at("outline_thickness").get_value<float>();
                if (p.contains("outline_color")) {
                    const auto& c = p.at("outline_color");
                    if (c.size() >= 4) {
                        config.render.outline_color = glm::vec4(
                            c.at(0).get_value<float>(), c.at(1).get_value<float>(),
                            c.at(2).get_value<float>(), c.at(3).get_value<float>());
                    }
                }
                if (p.contains("depth_threshold"))   config.render.depth_threshold   = p.at("depth_threshold").get_value<float>();
                if (p.contains("normal_threshold"))  config.render.normal_threshold  = p.at("normal_threshold").get_value<float>();
                if (p.contains("palette"))           config.render.palette_path      = p.at("palette").get_value<std::string>();
                if (p.contains("palette_enabled"))   config.render.palette_enabled   = p.at("palette_enabled").get_value<bool>();
                if (p.contains("dither_strength"))   config.render.dither_strength   = p.at("dither_strength").get_value<float>();
                if (p.contains("dither_enabled"))    config.render.dither_enabled    = p.at("dither_enabled").get_value<bool>();
                if (p.contains("camera_pixel_snap")) config.render.camera_pixel_snap = p.at("camera_pixel_snap").get_value<bool>();
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
