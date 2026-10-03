/**
 * @file settings_schema.h
 * @brief config.yaml's sections, grouped for the Render Settings and Project Settings tabs.
 *
 * Keys and spellings mirror AppConfig::from_node() (toyengine/core/config.h). Fields marked
 * startup() are those PixelRenderPipeline::apply_live_config() refuses to change on a live
 * renderer; the tab shows them with a restart badge.
 */

#ifndef TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H
#define TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H

#include "component_schema.h"

#include <string>
#include <vector>

namespace toy::editor {

struct SettingsGroup {
    std::string title;
    std::vector<FieldDesc> fields;
};

inline const std::vector<SettingsGroup>& render_settings_groups() {
    static const std::vector<SettingsGroup> groups = [] {
        const std::vector<std::string> q = {"low", "medium", "high", "ultra"};
        std::vector<SettingsGroup> g;
        g.push_back({"Viewport & Resolution", {
            startup(f_enum("resolution_mode", {"fixed", "divisor"})),
            startup(f_int("render_width", 480, 16, 7680)),
            startup(f_int("render_height", 270, 16, 4320)),
            startup(f_int("scale_divisor", 4, 1, 16)),
            f_enum("upscale_mode", {"fit", "integer"}),
            startup(f_enum("aa_mode", {"off", "fxaa", "smaa", "taa"})),
        }});
        g.push_back({"Quality Tiers", {
            f_enum("shadow_quality", q), f_enum("ssao_quality", q), f_enum("ssr_quality", q),
            f_enum("ssgi_quality", q), f_enum("dof_quality", q), f_enum("volumetrics_quality", q),
            f_enum("sdf_quality", q),
        }});
        g.push_back({"Features", {
            f_bool("shadows_enabled", true),
            startup(f_bool("ssao_enabled", true)),
            startup(f_bool("ssr_enabled", true)),
            startup(f_bool("transparency_enabled", false)),
            startup(f_bool("refraction_enabled", true)),
            f_bool("sdf_enabled", true),
            startup(f_bool("bloom_enabled", true)),
            startup(f_bool("fog_enabled", false)),
            startup(f_bool("volumetrics_enabled", false)),
            startup(f_bool("dof_enabled", false)),
            startup(f_bool("tilt_shift_enabled", false)),
            startup(f_bool("auto_exposure_enabled", false)),
            f_bool("grading_enabled", false),
            f_bool("outline_enabled", true),
            f_bool("palette_enabled", false),
            f_bool("dither_enabled", false),
            f_bool("soft_lighting", false),
            f_bool("texel_aa", true),
            f_bool("camera_pixel_snap", false),
        }});
        g.push_back({"Lighting & Sky", {
            f_float("exposure", 1.0f, 0.01f, 0.0f, 64.0f),
            f_float("light_bands", 4.0f, 0.1f, 0.0f, 64.0f),
            f_float("spec_threshold", 0.55f, 0.005f, 0.0f, 1.0f),
            f_float("rim_strength", 0.0f, 0.005f, 0.0f, 4.0f),
            f_float("ambient_intensity", 1.0f, 0.01f, 0.0f, 16.0f),
            f_float("sky_intensity", 1.0f, 0.01f, 0.0f, 16.0f),
            listed(f_color("sky_zenith", glm::vec3(0.05f, 0.18f, 0.55f))),
            listed(f_color("sky_horizon", glm::vec3(0.25f, 0.35f, 0.45f))),
            listed(f_color("sky_ground", glm::vec3(0.05f, 0.045f, 0.04f))),
        }});
        g.push_back({"Shadows", {
            f_float("shadow_distance", 60.0f, 0.2f, 1.0f, 10000.0f),
            startup(f_int("shadow_cascades", 4, 1, 4)),
            f_float("shadow_bias", 0.005f, 0.0005f, 0.0f, 1.0f),
            f_float("shadow_normal_bias", 1.0f, 0.01f, 0.0f, 16.0f),
            f_float("shadow_softness", 0.15f, 0.005f, 0.0f, 4.0f),
            f_bool("soft_shadows", true),
            f_bool("shadow_pcss_enabled", false),
            f_bool("contact_shadows_enabled", false),
            f_float("contact_shadow_strength", 1.0f, 0.01f, 0.0f, 1.0f),
            f_float("contact_shadow_length", 0.5f, 0.01f, 0.0f, 10.0f),
        }});
        g.push_back({"Fog", {
            f_int_enum("fog_mode", {"Linear", "Exponential", "Exponential Squared"}, 2, {
                "Fog ramps evenly from none at Linear Start to full at Linear End (density is ignored)",
                "Fog thickens as exp(-density * distance): starts building right away, soft tail",
                "Fog follows exp(-(density * distance)^2): clear up close, then closes in quickly (Unity's default)",
            }),
            f_float("fog_density", 0.03f, 0.001f, 0.0f, 10.0f),
            f_float("fog_linear_start", 5.0f, 0.1f, 0.0f, 100000.0f),
            f_float("fog_linear_end", 60.0f, 0.1f, 0.0f, 100000.0f),
            listed(f_color("fog_color", glm::vec3(0.6f, 0.66f, 0.75f))),
            f_float("fog_height_base", -0.5f, 0.05f),
            f_float("fog_height_falloff", 2.0f, 0.05f),
            f_float("fog_sky_blend", 0.5f, 0.01f, 0.0f, 1.0f),
            f_float("fog_max_opacity", 1.0f, 0.01f, 0.0f, 1.0f),
            f_float("fog_max_distance", 60.0f, 0.5f, 0.0f, 100000.0f),
        }});
        g.push_back({"Bloom & Exposure", {
            f_float("bloom_threshold", 1.0f, 0.01f, 0.0f, 64.0f),
            f_float("bloom_soft_knee", 0.5f, 0.01f, 0.0f, 1.0f),
            f_float("bloom_intensity", 5.0f, 0.05f, 0.0f, 100.0f),
            f_float("bloom_scatter", 0.7f, 0.01f, 0.0f, 1.0f),
            f_float("auto_exposure_compensation", 0.0f, 0.01f, -16.0f, 16.0f),
        }});
        g.push_back({"Stylize", {
            f_float("outline_thickness", 1.0f, 0.05f, 0.0f, 8.0f),
            listed(f_color("outline_color", glm::vec3(0.0f))),
            f_float("depth_threshold", 0.1f, 0.001f, 0.0f, 10.0f),
            f_float("normal_threshold", 0.5f, 0.005f, 0.0f, 2.0f),
            f_float("dither_strength", 0.08f, 0.005f, 0.0f, 1.0f),
            startup(root_relative(f_asset("palette", "palettes", ".png"))),
            startup(root_relative(f_asset("grading_lut", "textures", ".png"))),
        }});
        g.push_back({"Debug", {
            f_enum("debug_view", {"off", "albedo", "normals", "roughness", "metallic", "emissive", "material_ao",
                                  "world_pos", "depth", "direct", "indirect", "shadows", "contact_shadows", "ssao",
                                  "ssr", "ssr_confidence", "ssgi", "dof", "volumetrics", "lines", "solid", "wireframe", "material_preview"}),
        }});
        return g;
    }();
    return groups;
}

/** @brief Every render key the groups above cover (the rest are shown generically). */
inline std::vector<std::string> render_settings_keys() {
    std::vector<std::string> keys;
    for (const auto& g : render_settings_groups()) for (const auto& f : g.fields) keys.push_back(f.key);
    return keys;
}

inline const std::vector<SettingsGroup>& project_settings_groups() {
    static const std::vector<SettingsGroup> groups = {
        {"Window", {f_string("title", "toyengine"), f_int("width", 1280, 64, 16384), f_int("height", 720, 64, 16384),
                    f_bool("vsync", true)}},
        {"Physics", {f_vec3("gravity", glm::vec3(0.0f, 0.0f, -9.81f), 0.05f), f_float("fixed_timestep", 1.0f / 60.0f, 0.0005f, 0.0001f, 1.0f)}},
        {"Jobs", {f_int("worker_threads", 0, 0, 256), f_int("parallel_threshold", 64, 1, 1 << 20)}},
        {"Output", {f_bool("save_on_exit", false), f_string("filepath", "output/screenshot.png"), f_bool("save_low_res", true)}},
    };
    return groups;
}

/** @brief The config.yaml section each project settings group lives in. */
inline std::string project_section_key(const std::string& group_title) {
    if (group_title == "Window") return "window";
    if (group_title == "Physics") return "physics";
    if (group_title == "Jobs") return "jobs";
    return "output";
}

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H
