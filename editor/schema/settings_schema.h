/**
 * @file settings_schema.h
 * @brief config.yaml's sections, grouped for the Render Settings and Project Settings tabs.
 *
 * Render settings are organized by FEATURE: each group is one collapsible section holding
 * every key that shapes that feature -- its quality tier, its tunables and its startup sizes --
 * with the feature's master switch (`toggle`) in the section header. Groups sit under a
 * `category` heading (General, Render Features, Stylize, Debug) and may split their rows with
 * sub-headings, some shown only for one value of a mode key (FXAA's rows only when aa_mode is
 * fxaa). Every field has a label and a tooltip.
 *
 * Keys and spellings mirror AppConfig::from_node() (toyengine/core/config.h); every render key it
 * parses appears exactly once here, and each default matches PixelRenderConfig at High quality
 * (editor tests check both). Fields marked startup() are those
 * PixelRenderPipeline::apply_live_config() refuses to change on a live renderer; the tab marks
 * them with * (Restart Renderer applies them).
 */

#ifndef TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H
#define TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H

#include "component_schema.h"

#include <string>
#include <utility>
#include <vector>

namespace toy::editor {

/** @brief A sub-heading inside a settings group, drawn before fields[index]. */
struct SettingsSubhead {
    size_t index = 0;
    std::string title;
    std::string show_key;     ///< Non-empty: these rows show only while show_key == show_value.
    std::string show_value;
};

struct SettingsGroup {
    std::string title;
    std::vector<FieldDesc> fields;
    std::string category;                  ///< Render settings: the heading the group sits under.
    std::string tip;                       ///< Header tooltip: what the feature is.
    FieldDesc toggle;                      ///< The feature's on/off switch, in the header (key "" = none).
    std::string needs;                     ///< A bool key the feature also needs on (SSGI needs ssr_enabled).
    std::vector<SettingsSubhead> subheads;
    bool open = false;                     ///< Open the first time it is shown.

    bool has_toggle() const { return !toggle.key.empty(); }
    /** @brief The toggle (if any) and every field: all the keys this group edits. */
    std::vector<FieldDesc> all_fields() const {
        std::vector<FieldDesc> out;
        if (has_toggle()) out.push_back(toggle);
        out.insert(out.end(), fields.begin(), fields.end());
        return out;
    }
};

namespace settings_detail {

/** @brief Builds one group: rows, sub-headings, header toggle. */
class GroupBuilder {
public:
    GroupBuilder(std::string category, std::string title, std::string tip) {
        g_.category = std::move(category);
        g_.title = std::move(title);
        g_.tip = std::move(tip);
    }
    GroupBuilder& toggle(FieldDesc f) { g_.toggle = std::move(f); return *this; }
    GroupBuilder& needs(std::string key) { g_.needs = std::move(key); return *this; }
    GroupBuilder& open() { g_.open = true; return *this; }
    GroupBuilder& sub(std::string title, std::string show_key = {}, std::string show_value = {}) {
        g_.subheads.push_back({g_.fields.size(), std::move(title), std::move(show_key), std::move(show_value)});
        return *this;
    }
    GroupBuilder& add(FieldDesc f) { g_.fields.push_back(std::move(f)); return *this; }
    SettingsGroup done() { return std::move(g_); }

private:
    SettingsGroup g_;
};

/** @brief A labelled field with its tooltip. */
inline FieldDesc F(FieldDesc f, std::string label, std::string tip) {
    f.label = std::move(label);
    f.tooltip = std::move(tip);
    return f;
}
/** @brief A field a quality tier sets: the tooltip says which, and that editing it overrides the tier. */
inline FieldDesc tier(FieldDesc f, const std::string& tier_name) {
    f.tooltip += "\nSet by " + tier_name + " Quality (shown: High); editing it overrides the tier.";
    f.tier_driven = true;
    return f;
}
/** @brief A feature's quality tier. */
inline FieldDesc quality(const std::string& key, std::string tip) {
    return F(with_default(f_enum(key, {"low", "medium", "high", "ultra"}), "high"), "Quality",
             "Preset for this feature's resolutions and sample / step counts: " + std::move(tip) +
                 ". High is the engine default; a key set explicitly below still overrides its preset.");
}

} // namespace settings_detail

inline const std::vector<SettingsGroup>& render_settings_groups() {
    static const std::vector<SettingsGroup> groups = [] {
        using settings_detail::F;
        using settings_detail::GroupBuilder;
        using settings_detail::quality;
        using settings_detail::tier;
        std::vector<SettingsGroup> g;
        const std::string general = "General", features = "Render Features", stylize = "Stylize", debug = "Debug";

        // =====================================================================================
        // General
        // =====================================================================================
        g.push_back(GroupBuilder(general, "Resolution & Detail", "The internal render resolution, how it reaches the window, "
                                 "and mesh level of detail")
            .add(startup(F(f_enum("resolution_mode", {"fixed", "divisor", "fill"}), "Resolution Mode",
                           "fixed: render at Render Width x Height. divisor: the window size / Scale Divisor. "
                           "fill: Render Height rows, width following the window's aspect")))
            .add(startup(F(f_int("render_width", 480, 16, 7680), "Render Width", "Internal render width in pixels (fixed mode)")))
            .add(startup(F(f_int("render_height", 270, 16, 4320), "Render Height", "Internal render height in pixels (fixed and fill modes)")))
            .add(startup(F(f_int("scale_divisor", 4, 1, 16), "Scale Divisor", "divisor mode: render at the window size divided by this")))
            .add(F(f_enum("upscale_mode", {"fit", "integer"}), "Upscale",
                   "How the render reaches the window. fit: scale to fill (letterboxed); integer: whole-number scale only (crisp pixels)"))
            .add(F(f_float("mesh_lod_bias", 1.0f, 0.01f, 0.01f, 16.0f), "Mesh LOD Bias",
                   "Above 1 keeps mesh LOD detail longer, below 1 drops it sooner. Only meshes with `lods` are affected"))
            .done());

        g.push_back(GroupBuilder(general, "Lighting & Sky", "Shading style, exposure and the sky gradient that lights the scene "
                                 "indirectly")
            .add(F(f_float("exposure", 1.0f, 0.01f, 0.0f, 64.0f), "Exposure",
                   "Baseline brightness multiplier before tonemapping. Auto Exposure scales this rather than replacing it"))
            .add(F(f_bool("soft_lighting", false), "Smooth Shading",
                   "On: smooth physically-based (Cook-Torrance) lighting. Off: a banded, cel-shaded look"))
            .add(F(f_float("light_bands", 4.0f, 0.1f, 0.0f, 64.0f), "Light Bands", "Banded shading: how many light steps (Smooth Shading off only)"))
            .add(F(f_float("spec_threshold", 0.55f, 0.005f, 0.0f, 1.0f), "Specular Threshold",
                   "Banded shading: brightness a specular highlight must reach to show (hard cutoff)"))
            .add(F(f_float("rim_strength", 0.0f, 0.005f, 0.0f, 4.0f), "Rim Light", "Strength of a rim highlight on silhouettes; 0 disables it"))
            .sub("Sky")
            .add(with_default(F(f_enum("sky_model", {"gradient", "physical"}), "Sky Model",
                                "gradient: the three colours below, drawn as is (cheap, stylised). physical: a physically "
                                "based atmosphere -- sunsets, a sun and moon, stars at night, optional clouds -- that follows "
                                "the scene's sun (or the weather's clock) and sets the sky colours, ambient and the sun's "
                                "colour to match"), "gradient"))
            .add(F(f_float("ambient_intensity", 1.0f, 0.01f, 0.0f, 16.0f), "Ambient Intensity",
                   "How strongly the sky gradient lights surfaces as indirect diffuse light"))
            .add(F(f_float("sky_intensity", 1.0f, 0.01f, 0.0f, 16.0f), "Sky Reflection Intensity",
                   "Strength of the sky gradient as indirect specular (what surfaces reflect where nothing else is)"))
            .sub("Sky Gradient", "sky_model", "gradient")
            .add(F(listed(f_color("sky_zenith", glm::vec3(0.05f, 0.18f, 0.55f))), "Zenith Colour", "Sky gradient colour straight up"))
            .add(F(listed(f_color("sky_horizon", glm::vec3(0.25f, 0.35f, 0.45f))), "Horizon Colour", "Sky gradient colour at the horizon"))
            .add(F(listed(f_color("sky_ground", glm::vec3(0.05f, 0.045f, 0.04f))), "Ground Colour", "Sky gradient colour straight down"))
            .sub("Physical Sky", "sky_model", "physical")
            .add(quality("sky_quality", "the sky's integration steps and the cloud march's view and shadow steps"))
            .add(F(f_float("atmosphere_density", 1.0f, 0.01f, 0.0f, 20.0f), "Haze",
                   "Aerosols in the air: 1 = a clear day; higher = a hazier, whiter sky, a wider glow around the sun "
                   "and redder sunsets"))
            .add(F(f_float("ozone", 1.0f, 0.01f, 0.0f, 10.0f), "Ozone", "The ozone layer: deepens the blue of the sky at dawn and dusk"))
            .add(F(f_float("sun_disc_size", 0.53f, 0.01f, 0.0f, 20.0f), "Sun Size", "The sun disc's angular diameter in degrees (0.53 is real; 0 hides it)"))
            .add(F(f_float("moon_disc_size", 0.6f, 0.01f, 0.0f, 20.0f), "Moon Size", "The moon disc's angular diameter in degrees (0 hides it)"))
            .add(F(f_bool("sky_stars", true), "Stars", "Stars fade in as the sky darkens, behind any clouds"))
            .done());

        g.push_back(GroupBuilder(general, "Clouds", "The sky's cloud layers. Volumetric: clouds lit by the sun and moon, "
                                 "drawn over the physical sky (sky_model: physical), raymarched a quarter of the pixels per "
                                 "frame and reconstructed temporally (Sky Quality sets the steps and resolution). Topdown: a "
                                 "toon layer for topdown games -- puffy, cel-shaded clouds floating between a zoomed-out "
                                 "camera and the ground, fading in as the camera rises, their shadows drifting over the "
                                 "ground; works with either sky model. The scene's weather drives coverage and drift while "
                                 "it is on")
            .add(F(f_float("cloud_coverage", 0.4f, 0.005f, 0.0f, 1.0f), "Coverage",
                   "0 = a clear sky, 1 = overcast. Clouds also dim the sun and the ambient light. Drives both layers"))
            .add(F(f_float("cloud_wind_speed", 8.0f, 0.1f, 0.0f, 200.0f), "Wind Speed",
                   "How fast the clouds drift, m/s (along the weather's wind while it is on). Drives both layers"))
            .sub("Volumetric")
            .add(F(f_bool("clouds", false), "Volumetric Clouds", "The volumetric cloud layer (needs Sky Model: physical)"))
            .add(F(f_float("cloud_altitude", 1500.0f, 10.0f, 0.0f, 20000.0f), "Altitude", "Height of the layer's base, metres"))
            .add(F(f_float("cloud_thickness", 1500.0f, 10.0f, 10.0f, 10000.0f), "Thickness", "The layer's depth, metres"))
            .add(F(f_float("cloud_density", 1.0f, 0.01f, 0.0f, 10.0f), "Density", "Higher = darker, more solid clouds; lower = wispy"))
            .sub("Topdown")
            .add(F(f_bool("topdown_mode", false), "Topdown Clouds", "The toon cloud layer and its ground shadows"))
            .add(F(f_float("topdown_cloud_height", 60.0f, 0.5f, -1000.0f, 10000.0f), "Cloud Height",
                   "World height (z) of the layer, metres"))
            .add(F(f_float("topdown_cloud_size", 30.0f, 0.5f, 1.0f, 1000.0f), "Cloud Size", "Typical diameter of one puff, metres"))
            .add(F(f_float("topdown_cloud_thickness", 8.0f, 0.1f, 0.1f, 200.0f), "Cloud Thickness",
                   "How tall the puffs stand, metres (more = rounder, more shading)"))
            .add(F(f_float("topdown_cloud_opacity", 0.92f, 0.01f, 0.0f, 1.0f), "Opacity", "0 = invisible clouds (shadows only), 1 = solid"))
            .add(F(f_float("topdown_fade_start", 15.0f, 0.5f, 0.0f, 10000.0f), "Fade In From",
                   "Camera height above the layer where the clouds start to show, metres"))
            .add(F(f_float("topdown_fade_end", 60.0f, 0.5f, 0.0f, 10000.0f), "Fully Shown At",
                   "Camera height above the layer where the clouds are fully shown, metres"))
            .add(F(f_float("topdown_shadow_strength", 0.45f, 0.01f, 0.0f, 1.0f), "Shadow Strength",
                   "How much a cloud's shadow darkens the ground (0 = no shadows). Shown at every zoom"))
            .add(F(f_float("topdown_light_bands", 3.0f, 0.1f, 0.0f, 16.0f), "Light Bands", "Toon shading steps (0 = smooth shading)"))
            .add(F(f_float("topdown_outline", 0.5f, 0.01f, 0.0f, 1.0f), "Outline", "Darkening of each cloud's rim"))
            .done());

        g.push_back(GroupBuilder(general, "Anti-Aliasing", "Smooths jagged edges. One method at a time; its tuning is listed under it")
            .add(startup(F(f_enum("aa_mode", {"off", "fxaa", "smaa", "taa"}), "Method",
                           "off; fxaa: fast single-pass edge blur; smaa: sharper morphological AA; taa: temporal -- reprojects "
                           "and accumulates past frames, so a still camera converges to a clean image (needed by most "
                           "temporal effects to look their best)")))
            .sub("FXAA", "aa_mode", "fxaa")
            .add(F(f_float("fxaa_subpixel", 0.75f, 0.01f, 0.0f, 1.0f), "Subpixel Blend", "How much FXAA softens sub-pixel aliasing (thin lines, specks)"))
            .add(F(f_float("fxaa_edge_threshold", 0.166f, 0.001f, 0.0f, 1.0f), "Edge Threshold",
                   "Local contrast below which FXAA leaves a pixel alone; lower = more edges smoothed"))
            .add(F(f_float("fxaa_edge_threshold_min", 0.0312f, 0.001f, 0.0f, 1.0f), "Edge Threshold Min",
                   "Absolute contrast floor: darker edges than this are never treated"))
            .sub("SMAA", "aa_mode", "smaa")
            .add(F(f_float("smaa_threshold", 0.1f, 0.005f, 0.0f, 1.0f), "Edge Threshold", "Contrast an edge needs to be detected; lower = more edges"))
            .add(F(f_int("smaa_max_search_steps", 16, 1, 112), "Max Search Steps", "How far (texels) SMAA follows an edge to shape its blend"))
            .sub("TAA", "aa_mode", "taa")
            .add(F(f_float("taa_blending_weight", 0.99f, 0.001f, 0.0f, 1.0f), "History Weight",
                   "How much of the accumulated history is kept at rest; higher = cleaner but slower to react"))
            .add(F(f_float("taa_feedback_motion", 0.85f, 0.005f, 0.0f, 1.0f), "History Weight in Motion",
                   "The history weight floor under fast camera motion (lower = less ghosting, more aliasing)"))
            .add(F(f_float("taa_weight_scale", 30.0f, 0.5f, 0.0f, 1000.0f), "Motion Sensitivity",
                   "How quickly motion lowers the history weight: it reaches its floor at about 100 / this pixels per frame"))
            .add(F(f_float("taa_sharpness", 0.25f, 0.01f, 0.0f, 2.0f), "Sharpen", "Restores detail TAA softens while moving; 0 disables"))
            .add(F(f_float("taa_variance_gamma", 1.0f, 0.05f, 0.1f, 8.0f), "History Clip Width",
                   "How far history may differ from the current frame before it is rejected (std devs); lower = less ghosting, more flicker"))
            .done());

        // =====================================================================================
        // Render features
        // =====================================================================================
        g.push_back(GroupBuilder(features, "Shadows", "Shadow maps for the sun (cascaded) and for point and spot lights")
            .toggle(F(f_bool("shadows_enabled", true), "Shadows", "Shadow maps for every shadow-casting light"))
            .add(quality("shadow_quality", "map resolutions, filter taps, PCSS taps, contact-shadow steps and how many "
                                          "point / spot lights cast shadows"))
            .add(F(f_float("shadow_distance", 60.0f, 0.2f, 1.0f, 10000.0f), "Distance", "How far from the camera the sun's shadows reach"))
            .add(F(f_float("shadow_fade_fraction", 0.1f, 0.005f, 0.0f, 1.0f), "Fade Out",
                   "Fraction of the shadow distance over which the sun's shadows fade out at the far end"))
            .sub("Sun Cascades")
            .add(startup(F(f_int("shadow_cascades", 4, 1, 4), "Cascades",
                           "Shadow maps the sun's range is split into (1-4): near shadows sharp, far ones coarse. Each costs a pass over the casters")))
            .add(F(f_float("shadow_cascade_split_lambda", 0.75f, 0.01f, 0.0f, 1.0f), "Split Distribution",
                   "Where cascade boundaries fall: 1 = logarithmic (even detail per distance), 0 = uniform"))
            .add(startup(tier(F(f_int("shadow_map_resolution", 2048, 128, 8192), "Map Resolution",
                                "Texels per cascade tile (the atlas is up to 2x this on each axis)"), "Shadow")))
            .add(with_default(F(f_enum("shadow_fit", {"frustum", "focus"}), "Fit",
                                "frustum: slice the camera frustum from the lens outward (close-up cameras). focus: nest the cascades "
                                "around the camera's focus point (cameras looking at a world from far away)"), "frustum"))
            .add(F(f_float("shadow_focus_radius", 12.0f, 0.1f, 0.1f, 10000.0f), "Focus Radius", "Focus fit: the finest cascade's radius around the focus point"))
            .add(F(f_float("shadow_focus_distance", 0.0f, 0.1f, 0.0f, 100000.0f), "Focus Distance",
                   "Focus fit: a fixed eye-to-focus distance; 0 = the camera's focus, else what is under the screen centre"))
            .sub("Softness")
            .add(F(f_bool("soft_shadows", true), "Soft Shadows", "On: filtered, soft penumbrae. Off: a single hard depth test"))
            .add(F(f_float("shadow_softness", 0.15f, 0.005f, 0.0f, 4.0f), "Sun Softness",
                   "Sun penumbra radius in world units (with PCSS on: the maximum width)"))
            .add(tier(F(f_int("shadow_pcf_samples", 24, 1, 32), "Filter Samples", "Taps per pixel for soft sun shadows (1-32)"), "Shadow"))
            .add(F(f_float("shadow_pcf_max_texels", 12.0f, 0.1f, 0.0f, 64.0f), "Max Filter Radius", "Ceiling on the soft-shadow filter radius, in shadow-map texels"))
            .add(F(f_bool("shadow_receiver_plane_bias", false), "Receiver Plane Bias",
                   "Filter taps follow the receiving surface's plane, so wide penumbrae don't need a big normal bias (which shrinks shadows)"))
            .add(F(f_float("shadow_receiver_max_slope", 4.0f, 0.05f, 0.0f, 64.0f), "Receiver Max Slope",
                   "Receiver plane bias: the steepest surface slope (tangent) it trusts"))
            .sub("Contact Hardening (PCSS)")
            .add(F(f_bool("shadow_pcss_enabled", false), "PCSS",
                   "Shadows are sharp where an object touches the ground and widen with distance (needs Soft Shadows)"))
            .add(F(f_float("shadow_pcss_light_size", 0.02f, 0.001f, 0.0f, 1.0f), "Light Size",
                   "The sun's apparent size: how fast the penumbra grows per metre of gap. Also softens contact shadows"))
            .add(F(f_float("shadow_pcss_search_texels", 8.0f, 0.1f, 1.0f, 16.0f), "Blocker Search Radius", "Texels searched for occluders (1-16)"))
            .add(tier(F(f_int("shadow_pcss_taps", 8, 1, 16), "Blocker Search Taps", "Taps of the occluder search -- PCSS's main cost"), "Shadow"))
            .sub("Bias")
            .add(F(f_float("shadow_depth_bias_texels", 1.0f, 0.01f, 0.0f, 32.0f), "Depth Bias",
                   "Constant depth offset in shadow-map texels: raise to remove shadow acne, lower if shadows detach"))
            .add(F(f_float("shadow_slope_bias_texels", 1.0f, 0.01f, 0.0f, 32.0f), "Slope Bias",
                   "Extra offset on surfaces tilted away from the light, per unit of slope"))
            .add(F(f_float("shadow_slope_bias_max", 5.0f, 0.05f, 0.0f, 100.0f), "Slope Bias Max", "Cap on the slope the slope bias responds to (tangent)"))
            .add(F(f_float("shadow_normal_bias", 1.0f, 0.01f, 0.0f, 16.0f), "Normal Bias",
                   "Pushes the lookup out along the surface normal, in texels beyond the filter radius"))
            .add(F(f_float("shadow_bias", 0.005f, 0.0005f, 0.0f, 1.0f), "Legacy Bias", "Depth bias for the volumetric light-shaft shadow lookup; surface shadows ignore it"))
            .sub("Point & Spot Lights")
            .add(F(f_float("point_shadow_softness", 3.0f, 0.05f, 0.0f, 32.0f), "Point Softness", "Point-light penumbra radius, in cube-face texels"))
            .add(F(f_float("spot_shadow_softness", 0.15f, 0.005f, 0.0f, 4.0f), "Spot Softness", "Spot-light penumbra radius, in world units"))
            .add(tier(F(f_int("max_shadowed_point_lights", 4, 0, 64), "Max Point Lights",
                        "How many point lights cast shadows at once; the most important on screen win"), "Shadow"))
            .add(tier(F(f_int("max_shadowed_spot_lights", 4, 0, 64), "Max Spot Lights",
                        "How many spot lights cast shadows at once; the most important on screen win"), "Shadow"))
            .add(startup(tier(F(f_int("cube_shadow_resolution", 512, 32, 4096), "Point Face Resolution", "Texels per point-light cube face"), "Shadow")))
            .add(startup(tier(F(f_int("spot_shadow_resolution", 1024, 32, 8192), "Spot Resolution", "Texels per spot-light shadow"), "Shadow")))
            .add(startup(tier(F(f_int("local_shadow_atlas_resolution", 4096, 512, 16384), "Atlas Resolution",
                                "Size of the atlas all point and spot shadows share"), "Shadow")))
            .sub("Performance")
            .add(startup(F(f_bool("shadow_cache_enabled", true), "Cache Static Casters",
                           "Keep each light's shadow of non-moving objects; only moving ones are redrawn each frame")))
            .add(F(f_float("shadow_min_caster_texels", 1.0f, 0.05f, 0.0f, 64.0f), "Min Caster Size",
                   "Skip casters smaller than this many shadow texels; 0 = draw every caster"))
            .done());

        g.push_back(GroupBuilder(features, "Contact Shadows", "A short screen-space ray toward the sun that fills the small gap "
                                 "shadow maps leave where objects meet the ground (works even with Shadows off)")
            .toggle(F(f_bool("contact_shadows_enabled", false), "Contact Shadows", "Screen-space contact shadows from the sun"))
            .add(F(f_float("contact_shadow_length", 0.5f, 0.01f, 0.0f, 10.0f), "Length", "How far the ray marches, in world units"))
            .add(F(f_float("contact_shadow_strength", 1.0f, 0.01f, 0.0f, 1.0f), "Strength", "How dark a contact hit makes the shadow"))
            .add(F(f_float("contact_shadow_thickness", 0.15f, 0.005f, 0.0f, 10.0f), "Thickness",
                   "Assumed thickness of what the ray passes behind, in world units; larger = fewer gaps behind thin objects"))
            .add(tier(F(f_int("contact_shadow_steps", 8, 1, 24), "Steps", "March steps (1-24) -- the feature's whole cost"), "Shadow"))
            .add(F(f_bool("contact_shadow_temporal_enabled", true), "Temporal Accumulation", "Average the result over frames for a stable edge"))
            .add(F(f_int("contact_shadow_temporal_frames", 16, 1, 128), "Temporal Frames", "Frames averaged; fewer = crisper but noisier"))
            .done());

        g.push_back(GroupBuilder(features, "Ambient Occlusion", "Screen-space ambient occlusion (SSAO): darkens creases, corners "
                                 "and contact areas where ambient light can't reach")
            .toggle(startup(F(f_bool("ssao_enabled", true), "Ambient Occlusion", "Screen-space ambient occlusion")))
            .add(quality("ssao_quality", "slices, march steps, pixel radius and temporal depth"))
            .add(F(f_float("ssao_intensity", 1.0f, 0.01f, 0.0f, 1.0f), "Intensity", "How much of the occlusion is applied; 0 disables the darkening"))
            .add(F(f_float("ssao_radius", 0.5f, 0.01f, 0.0f, 100.0f), "Radius", "World-space distance occluders are searched within"))
            .add(F(f_float("ssao_power", 1.5f, 0.01f, 0.0f, 8.0f), "Power", "Contrast curve on the result; higher = darker, tighter occlusion"))
            .add(F(f_float("ssao_bias", 0.025f, 0.001f, 0.0f, 1.0f), "Bias",
                   "Lifts the search off the surface (world units) to avoid self-occlusion; keep well below Radius"))
            .add(F(f_float("ssao_direct_lighting_strength", 0.25f, 0.01f, 0.0f, 1.0f), "Direct Lighting Strength",
                   "How much occlusion also darkens DIRECT light; 0 = ambient light only"))
            .sub("Sampling")
            .add(tier(F(f_int("ssao_slices", 2, 1, 16), "Slices", "Search directions per pixel"), "SSAO"))
            .add(tier(F(f_int("ssao_steps", 16, 1, 64), "Steps", "March steps per direction"), "SSAO"))
            .add(tier(F(f_float("ssao_max_radius_px", 80.0f, 1.0f, 1.0f, 1024.0f), "Max Screen Radius",
                        "Caps how far (render pixels) the search reaches, whatever the world radius -- bounds cost up close"), "SSAO"))
            .add(startup(F(f_bool("ssao_half_res", true), "Half Resolution", "Compute AO at half resolution and upsample (about 4x cheaper)")))
            .sub("Denoise")
            .add(F(f_bool("ssao_blur_light", true), "Light Blur", "A smaller 4x4 blur instead of 8x8 (a quarter of the reads)"))
            .add(F(f_float("ssao_blur_plane_sigma", 0.375f, 0.005f, 0.0f, 10.0f), "Blur Edge Tolerance",
                   "How far apart (world units) two surfaces may be and still blur together; lower keeps edges crisper"))
            .add(F(f_bool("ssao_temporal_enabled", true), "Temporal Accumulation", "Average AO over frames (needed for a stable image)"))
            .add(tier(F(f_int("ssao_temporal_frames", 8, 1, 128), "Temporal Frames", "Frames averaged"), "SSAO"))
            .add(F(f_float("ssao_temporal_gamma", 1.0f, 0.05f, 0.25f, 4.0f), "History Clip Width",
                   "How far history may differ before it is rejected; lower = less ghosting behind moving objects, more noise"))
            .done());

        g.push_back(GroupBuilder(features, "Reflections", "Screen-space reflections (SSR): reflects what is on screen in glossy surfaces")
            .toggle(startup(F(f_bool("ssr_enabled", true), "Reflections", "Screen-space reflections (also required by Global Illumination)")))
            .add(quality("ssr_quality", "the trace iteration budget; Ultra traces 2 rays per pixel"))
            .sub("Tracing")
            .add(F(f_float("ssr_max_distance", 30.0f, 0.1f, 0.0f, 10000.0f), "Max Distance", "How far a reflection ray travels, in world units"))
            .add(tier(F(f_int("ssr_max_iterations", 64, 1, 512), "Max Iterations", "Trace steps per ray -- the main cost"), "SSR"))
            .add(tier(F(f_int("ssr_rays_per_pixel", 1, 1, 8), "Rays per Pixel", "Rays traced and averaged per pixel each frame"), "SSR"))
            .add(F(f_float("ssr_thickness", 0.08f, 0.005f, 0.0f, 10.0f), "Thickness", "How thick objects are assumed to be when a ray passes behind them"))
            .add(F(f_float("ssr_thickness_scale", 0.02f, 0.001f, 0.0f, 1.0f), "Thickness per Distance", "Extra thickness per unit of distance from the camera"))
            .add(F(f_float("ssr_bias_texels", 3.5f, 0.05f, 0.0f, 32.0f), "Start Bias", "Texels the ray starts off the surface, against self-hits"))
            .add(F(f_int("ssr_start_mip", 0, 0, 8), "Start Mip", "Depth-pyramid level tracing starts at (higher = faster, coarser)"))
            .add(F(f_int("ssr_min_mip0_steps", 1, 0, 64), "Min Fine Steps", "Steps taken at full depth resolution before coarser levels are used"))
            .add(F(f_bool("ssr_skip_behind", false), "Skip Behind Thin Objects", "Stride past thin occluders instead of crawling one texel at a time"))
            .add(startup(F(f_bool("ssr_half_res", true), "Half Resolution",
                           "Trace reflections (and global illumination) at half resolution (about 4x cheaper)")))
            .sub("Roughness")
            .add(F(f_float("ssr_roughness_cutoff", 1.0f, 0.01f, 0.0f, 1.0f), "Roughness Cutoff", "Surfaces rougher than this stop tracing; 1 = all trace"))
            .add(F(f_float("ssr_jitter", 1.0f, 0.01f, 0.0f, 1.0f), "Glossy Spread",
                   "How widely rough surfaces scatter their rays: 1 = physically correct, 0 = mirror rays everywhere"))
            .add(F(f_float("ssr_cone_prefilter", 0.5f, 0.01f, 0.0f, 1.0f), "Cone Prefilter",
                   "How much of a rough reflection's blur comes from pre-blurred scene colour instead of noise"))
            .add(F(f_bool("ssr_skip_negligible", true), "Skip Faint Rays", "Skip rays whose reflection can't be visible (rough, non-metal surfaces)"))
            .add(F(f_float("ssr_skip_threshold", 0.02f, 0.001f, 0.0f, 1.0f), "Faint Threshold", "Reflection weight below which Skip Faint Rays skips"))
            .sub("Denoise")
            .add(F(f_bool("ssr_temporal_enabled", true), "Temporal Accumulation", "Average reflections over frames"))
            .add(F(f_int("ssr_temporal_frames", 32, 1, 256), "Temporal Frames", "Frames averaged"))
            .add(F(f_float("ssr_temporal_blend", 0.85f, 0.005f, 0.0f, 1.0f), "Fallback Blend", "Fixed history blend used only where no frame count exists"))
            .add(F(f_float("ssr_temporal_gamma", 2.0f, 0.05f, 0.1f, 8.0f), "History Clip Width", "How far history may differ before it is rejected (std devs)"))
            .add(F(f_float("ssr_blur_radius", 0.5f, 0.01f, 0.0f, 10.0f), "Blur Radius", "World-space size of the spatial denoise"))
            .add(F(f_bool("ssr_blur_light", true), "Light Blur", "A 3x3 denoise instead of 5x5 (a third of the reads)"))
            .add(F(f_bool("ssr_blur_zero_skip", true), "Skip Empty Areas", "Skip the denoise where there is no reflection (exact, a pure saving)"))
            .done());

        g.push_back(GroupBuilder(features, "Global Illumination", "Screen-space global illumination (SSGI): light bouncing off "
                                 "nearby surfaces tints what is around them. Runs alongside Reflections").needs("ssr_enabled")
            .add(quality("ssgi_quality", "the bounce trace budget and resolution"))
            .add(F(f_float("ssgi_intensity", 0.6f, 0.01f, 0.0f, 8.0f), "Intensity", "Strength of the bounced light; 0 = reflections only"))
            .add(startup(F(f_bool("ssgi_traced", true), "Traced Bounce",
                           "Trace rays over the hemisphere for directional colour bleed; off = a cheap single lookup (a uniform lift)")))
            .add(F(f_float("ssgi_max_distance", 8.0f, 0.05f, 0.0f, 1000.0f), "Max Distance", "Traced bounce: ray length in world units"))
            .add(tier(F(f_int("ssgi_max_iterations", 32, 1, 256), "Max Iterations", "Traced bounce: trace steps per ray"), "SSGI"))
            .add(startup(tier(F(f_int("ssgi_resolution_scale", 1, 1, 4), "Resolution Divisor",
                                "Trace at the reflection resolution divided by this"), "SSGI")))
            .add(F(f_float("ssgi_distance", 0.5f, 0.01f, 0.0f, 100.0f), "Fallback Offset",
                   "Untraced bounce: how far along the normal the single lookup samples"))
            .sub("Denoise")
            .add(F(f_int("ssgi_temporal_frames", 48, 1, 256), "Temporal Frames", "Frames averaged (deeper than reflections: the bounce is noisier)"))
            .add(F(f_float("ssgi_blur_radius", 1.0f, 0.01f, 0.0f, 10.0f), "Blur Radius", "World-space size of the spatial denoise"))
            .add(F(f_bool("ssgi_blur_light", true), "Light Blur", "A 3x3 denoise instead of 5x5"))
            .done());

        g.push_back(GroupBuilder(features, "Transparency & Refraction", "The forward pass for BLEND materials (glass, water), and "
                                 "screen-space refraction through them")
            .toggle(startup(F(f_bool("transparency_enabled", false), "Transparency", "Draw BLEND (transparent) materials")))
            .add(startup(F(f_bool("refraction_enabled", true), "Refraction", "Bend what is seen through transparent meshes (needs Transparency)")))
            .sub("Refraction")
            .add(F(f_float("refraction_ior", 1.45f, 0.005f, 1.0f, 3.0f), "Default IOR", "Index of refraction for materials without their own (glass ~1.45, water ~1.33)"))
            .add(F(f_float("refraction_thickness", 0.25f, 0.005f, 0.0f, 100.0f), "Default Thickness", "World-space distance light travels through the object"))
            .add(F(f_float("refraction_strength", 1.0f, 0.01f, 0.0f, 8.0f), "Strength", "Multiplier on the screen-space offset"))
            .add(F(f_float("refraction_max_offset", 0.08f, 0.001f, 0.0f, 1.0f), "Max Offset", "Clamp on the offset (screen fraction) to stop smearing at grazing angles"))
            .add(F(f_float("refraction_chromatic", 0.0f, 0.001f, 0.0f, 1.0f), "Chromatic Split", "Splits colours by IOR (prism fringes); 0 = off"))
            .add(F(f_float("refraction_blur", 1.0f, 0.01f, 0.0f, 8.0f), "Roughness Blur", "How much roughness blurs what is behind (frosted glass)"))
            .add(F(f_float("refraction_density", 1.0f, 0.01f, 0.0f, 64.0f), "Absorption", "How strongly the tint absorbs light with thickness"))
            .add(F(listed(f_color("refraction_tint", glm::vec3(1.0f))), "Tint", "Colour light is absorbed toward through thick refraction"))
            .add(F(f_bool("refraction_fresnel", true), "Fresnel", "Dim what shows through at grazing angles"))
            .add(F(f_bool("refraction_include_reflections", true), "See Reflections", "Refraction samples the image after reflections, so reflections show through"))
            .done());

        g.push_back(GroupBuilder(features, "Fog", "Global exponential height fog over the whole scene, thinning with "
                                 "altitude. Local fog pockets are Volume components (see Volumetrics)")
            .toggle(F(f_bool("fog_enabled", true), "Fog", "Global height fog"))
            .add(F(f_int_enum("fog_mode", {"Linear", "Exponential"}, 1, {
                       "Fog ramps evenly from none at Linear Start to full at Linear End (density is ignored)",
                       "Physically based: fog thickens as exp(-density * distance), thinning with height above the base",
                   }), "Mode", "How fog builds with distance"))
            .add(F(f_float("fog_density", 0.02f, 0.001f, 0.0f, 10.0f), "Density", "Fog per metre at and below the height base"))
            .add(F(f_float("fog_linear_start", 5.0f, 0.1f, 0.0f, 100000.0f), "Linear Start", "Linear mode: distance fog begins"))
            .add(F(f_float("fog_linear_end", 60.0f, 0.1f, 0.0f, 100000.0f), "Linear End", "Linear mode: distance fog is at full strength"))
            .add(F(listed(f_color("fog_color", glm::vec3(0.55f, 0.62f, 0.72f))), "Colour", "The fog's colour"))
            .add(F(f_float("fog_sky_blend", 0.0f, 0.01f, 0.0f, 1.0f), "Sky Blend", "0 = flat fog colour, 1 = blend toward the sky gradient"))
            .add(F(f_float("fog_max_opacity", 1.0f, 0.01f, 0.0f, 1.0f), "Max Opacity", "Ceiling on how much fog can hide"))
            .sub("Distance")
            .add(F(f_float("fog_start_distance", 0.0f, 0.1f, 0.0f, 100000.0f), "Start Distance", "No fog nearer than this"))
            .add(F(f_float("fog_cutoff_distance", 0.0f, 0.5f, 0.0f, 100000.0f), "Cutoff Distance",
                   "Fog stops thickening past this distance; 0 = no cutoff"))
            .add(F(f_float("fog_sky_distance", 1000.0f, 1.0f, 1.0f, 100000.0f), "Sky Distance",
                   "How far away the sky counts as for fog; with height fog the zenith stays clear"))
            .sub("Height")
            .add(F(f_float("fog_height_base", 0.0f, 0.05f), "Height Base", "World Z where fog is at full density (Z is up); constant below"))
            .add(F(f_float("fog_height_falloff", 0.0f, 0.05f, -1000.0f, 1000.0f), "Height Falloff",
                   "Metres over which fog thins above the base; 0 or less = uniform fog"))
            .sub("Sun")
            .add(F(f_float("fog_sun_amount", 0.0f, 0.01f, 0.0f, 16.0f), "Sun Glow", "Brightening toward the sun as light scatters forward; 0 disables"))
            .add(F(f_float("fog_sun_anisotropy", 0.7f, 0.01f, -0.99f, 0.99f), "Sun Glow Tightness", "0 = even glow, near 1 = tight halo around the sun"))
            .add(F(f_float("fog_sun_start_distance", 0.0f, 0.1f, 0.0f, 100000.0f), "Sun Glow Start", "The sun glow builds only beyond this distance"))
            .done());

        g.push_back(GroupBuilder(features, "Volumetrics", "Raymarched local volumes (Volume components): fog pockets, haze, light "
                                 "shafts and light cones. The look of each volume lives on its component")
            .toggle(startup(F(f_bool("volumetrics_enabled", false), "Volumetrics", "Volume components render")))
            .add(quality("volumetrics_quality", "march steps and how many lights scatter (they multiply)"))
            .add(startup(F(f_enum("volumetrics_mode", {"froxel", "raymarch"}), "Mode",
                           "froxel: a 3D grid aligned to the view (fast, Unreal-style); raymarch: a march per pixel (sharper, costlier)")))
            .add(F(f_float("volumetrics_max_distance", 40.0f, 0.1f, 0.0f, 10000.0f), "Max Distance", "Distance the march stops at"))
            .add(F(f_float("volumetrics_max_opacity", 0.85f, 0.01f, 0.0f, 1.0f), "Max Opacity", "Ceiling on how much volumes can hide"))
            .add(tier(F(f_int("volumetrics_step_count", 48, 1, 512), "Steps", "March steps -- the main cost and what sharpens shafts"), "Volumetrics"))
            .sub("Lighting")
            .add(F(f_bool("volumetrics_shadows_enabled", true), "Sun Shadows", "Shadow the sun's light in volumes -- what makes light shafts"))
            .add(F(f_float("volumetrics_sun_anisotropy", 0.6f, 0.01f, -0.99f, 0.99f), "Sun Scatter Direction",
                   "0 = light scatters evenly, near 1 = mostly forward (bright looking toward the sun)"))
            .add(F(f_float("volumetrics_light_scatter", 1.0f, 0.01f, 0.0f, 16.0f), "Light Scatter", "Point / spot light glow in volumes; 0 skips lights entirely"))
            .add(tier(F(f_int("volumetrics_max_scatter_lights", 4, 0, 4), "Max Lights", "How many nearby point / spot lights scatter (0-4)"), "Volumetrics"))
            .sub("Froxel Grid", "volumetrics_mode", "froxel")
            .add(startup(F(f_int("volumetrics_froxel_tile", 8, 1, 64), "Tile Size", "Render pixels per grid cell on each axis")))
            .add(startup(F(f_int("volumetrics_froxel_slices", 64, 4, 256), "Depth Slices", "Grid slices along the view direction")))
            .add(F(f_float("volumetrics_froxel_history", 0.9f, 0.01f, 0.0f, 1.0f), "History Weight", "Temporal reuse of last frame's grid; 0 = off"))
            .add(F(f_int("volumetrics_froxel_miss_samples", 4, 1, 32), "Disocclusion Samples", "Extra samples where history was rejected; 1 = off"))
            .add(F(f_float("volumetrics_froxel_lookup_jitter", 0.0f, 0.01f, 0.0f, 4.0f), "Lookup Jitter",
                   "Per-pixel jitter of the grid lookup for TAA to smooth; 0 = off, about 1 = Unreal-style"))
            .sub("Raymarch", "volumetrics_mode", "raymarch")
            .add(startup(F(f_int("volumetrics_resolution_scale", 2, 1, 8), "Resolution Divisor", "March at 1/N of the render resolution (1 = full)")))
            .done());

        g.push_back(GroupBuilder(features, "SDF Raymarching", "Signed-distance-field shapes (SdfRenderer / SdfShape components), "
                                 "raymarched into the scene")
            .toggle(F(f_bool("sdf_enabled", true), "SDF Raymarching", "Draw SDF shapes"))
            .add(quality("sdf_quality", "the march and shadow-march step budgets"))
            .add(F(f_bool("sdf_shadows_enabled", true), "Cast Shadows", "SDF shapes cast shadows (works even with drawing off)"))
            .add(tier(F(f_int("sdf_max_steps", 64, 1, 1024), "Max Steps", "Ceiling on any SdfRenderer's march steps"), "SDF"))
            .add(tier(F(f_int("sdf_shadow_max_steps", 32, 1, 1024), "Max Shadow Steps", "Steps for the cheaper shadow march"), "SDF"))
            .add(startup(F(f_int("sdf_max_renderers", 64, 1, 4096), "Max Renderers", "Capacity for SDF renderers in the scene")))
            .add(startup(F(f_int("sdf_max_shapes", 512, 1, 65536), "Max Shapes", "Capacity for SDF shapes in the scene")))
            .done());

        g.push_back(GroupBuilder(features, "Water", "WaterBody surfaces and the underwater look")
            .add(quality("water_quality", "buoyancy and ripple range, water mesh density and LOD, and shader detail"))
            .add(startup(F(f_bool("underwater_enabled", true), "Underwater Effect",
                           "Fog, absorption and caustics when the camera is below a water surface")))
            .done());

        g.push_back(GroupBuilder(features, "Bloom", "A glow around bright parts of the image")
            .toggle(startup(F(f_bool("bloom_enabled", false), "Bloom", "Glow from bright regions")))
            .add(F(f_float("bloom_intensity", 1.0f, 0.05f, 0.0f, 100.0f), "Intensity", "Strength of the glow; 0 disables it"))
            .add(F(f_float("bloom_threshold", 1.0f, 0.01f, 0.0f, 64.0f), "Threshold", "Brightness below which nothing glows"))
            .add(F(f_float("bloom_soft_knee", 0.5f, 0.01f, 0.0f, 1.0f), "Soft Knee", "Eases the glow in around the threshold instead of a hard cut"))
            .add(F(f_float("bloom_scatter", 0.7f, 0.01f, 0.0f, 1.0f), "Scatter", "How far the glow spreads; higher = a wider halo"))
            .add(F(f_float("bloom_radius", 1.0f, 0.01f, 0.0f, 8.0f), "Radius", "Width of the blur filter"))
            .add(F(f_float("bloom_clamp", 20.0f, 0.1f, 0.0f, 1000.0f), "Clamp", "Brightness ceiling before glowing, so single hot pixels don't flare"))
            .done());

        g.push_back(GroupBuilder(features, "Auto Exposure", "Eye adaptation: meters the frame and adjusts exposure over time")
            .toggle(startup(F(f_bool("auto_exposure_enabled", false), "Auto Exposure", "Adapt exposure to the scene's brightness")))
            .add(F(f_float("auto_exposure_compensation", 1.0f, 0.01f, 0.0f, 16.0f), "Compensation", "Multiplier on the metered exposure (above 1 = brighter)"))
            .add(F(f_float("auto_exposure_speed_up", 3.0f, 0.05f, 0.0f, 100.0f), "Adapt to Bright", "How fast (per second) it adapts when the scene gets brighter"))
            .add(F(f_float("auto_exposure_speed_down", 1.0f, 0.05f, 0.0f, 100.0f), "Adapt to Dark", "How fast it adapts when the scene gets darker"))
            .add(F(f_float("auto_exposure_min", 0.05f, 0.005f, 0.0f, 64.0f), "Min Exposure", "Lowest exposure multiplier it may reach"))
            .add(F(f_float("auto_exposure_max", 8.0f, 0.05f, 0.0f, 256.0f), "Max Exposure", "Highest exposure multiplier it may reach"))
            .done());

        g.push_back(GroupBuilder(features, "Depth of Field", "Physically-based camera blur in front of and behind the focus")
            .toggle(startup(F(f_bool("dof_enabled", false), "Depth of Field", "Camera focus blur")))
            .add(quality("dof_quality", "bokeh gather taps"))
            .sub("Focus")
            .add(with_default(F(f_enum("dof_focus_mode", {"manual", "orbit_target", "object"}), "Focus Mode",
                                "manual: Focus Distance. orbit_target: the camera's orbit target. object: Focus Object. "
                                "A camera's own focus object overrides this"), "manual"))
            .add(F(f_float("dof_focus_distance", 8.0f, 0.05f, 0.0f, 100000.0f), "Focus Distance", "Manual mode: metres to the sharp plane"))
            .add(F(f_string("dof_focus_object", ""), "Focus Object", "Object mode: the object to focus on, as a ':'-separated scene path"))
            .add(F(f_float("dof_focus_range", 0.0f, 0.05f, 0.0f, 1000.0f), "Focus Range", "Extra metres kept sharp on each side of the focal plane"))
            .add(F(f_bool("dof_focus_cover_object", true), "Cover Object", "Object focus: widen the sharp band to fit the object's depth"))
            .add(F(f_float("dof_focus_smoothing", 8.0f, 0.1f, 0.0f, 100.0f), "Focus Speed", "How fast focus racks to a new distance (per second); 0 = instant"))
            .sub("Lens")
            .add(F(f_float("dof_aperture", 2.8f, 0.05f, 0.1f, 64.0f), "Aperture (f-stop)", "Lower = shallower focus, more blur"))
            .add(F(f_float("dof_focal_length", 0.0f, 0.5f, 0.0f, 2000.0f), "Focal Length (mm)", "0 = use the camera's lens"))
            .add(F(f_float("dof_sensor_width", 0.0f, 0.5f, 0.0f, 200.0f), "Sensor Width (mm)", "0 = use the camera's sensor width"))
            .add(F(f_float("dof_blur_scale", 1.0f, 0.01f, 0.0f, 16.0f), "Blur Strength", "Multiplier on the computed blur; 0 bypasses depth of field"))
            .add(F(f_float("dof_max_radius", 12.0f, 0.1f, 0.0f, 128.0f), "Max Blur Radius", "Safety ceiling on blur size, in render pixels"))
            .add(tier(F(f_int("dof_sample_count", 48, 8, 48), "Samples", "Bokeh taps per pixel (8-48)"), "DoF"))
            .add(F(f_int("dof_blade_count", 0, 0, 16), "Aperture Blades", "Below 3 = round bokeh; otherwise an N-sided iris"))
            .add(F(f_float("dof_blade_rotation", 0.0f, 0.5f, -360.0f, 360.0f), "Blade Rotation", "Rotates the polygonal iris, in degrees"))
            .done());

        g.push_back(GroupBuilder(features, "Motion Blur", "Smears moving objects and camera motion along their screen velocity")
            .toggle(F(f_bool("motion_blur", false), "Motion Blur", "Velocity-based blur of camera and object motion (the editor viewport camera never blurs)"))
            .add(F(f_float("motion_blur_intensity", 0.5f, 0.01f, 0.0f, 2.0f), "Shutter",
                   "Fraction of the frame the shutter is open (0.5 = a 180-degree shutter); 0 disables the blur"))
            .done());

        g.push_back(GroupBuilder(features, "Tilt Shift", "A miniature / diorama look: a sharp horizontal band, blurred above and below")
            .toggle(startup(F(f_bool("tilt_shift_enabled", false), "Tilt Shift", "Diorama blur, after upscaling")))
            .add(F(f_float("tilt_shift_focus_center", 0.55f, 0.005f, 0.0f, 1.0f), "Band Centre", "Screen height of the sharp band's centre (0 = top)"))
            .add(F(f_float("tilt_shift_focus_width", 0.18f, 0.005f, 0.0f, 1.0f), "Band Width", "Half-height of the fully sharp band (screen fraction)"))
            .add(F(f_float("tilt_shift_ramp_width", 0.22f, 0.005f, 0.0f, 1.0f), "Ramp Width", "Distance the blur fades in over"))
            .add(F(f_float("tilt_shift_blur_top", 1.0f, 0.01f, 0.0f, 4.0f), "Blur Above", "Blur strength above the band"))
            .add(F(f_float("tilt_shift_blur_bottom", 0.7f, 0.01f, 0.0f, 4.0f), "Blur Below", "Blur strength below the band"))
            .add(F(f_float("tilt_shift_max_radius", 6.0f, 0.1f, 0.0f, 64.0f), "Max Radius", "Largest blur radius, in window pixels"))
            .add(F(f_float("tilt_shift_angle", 0.0f, 0.5f, -180.0f, 180.0f), "Angle", "Tilts the band, in degrees off horizontal"))
            .done());

        g.push_back(GroupBuilder(features, "Color Grading", "A colour look-up table applied after tonemapping")
            .toggle(F(f_bool("grading_enabled", true), "Color Grading", "Apply the grading LUT (nothing happens until one is chosen)"))
            .add(startup(F(root_relative(f_asset("grading_lut", "textures", ".png")), "LUT",
                           "A strip LUT image (N*N x N, e.g. 1024x32); empty = no grading")))
            .done());

        g.push_back(GroupBuilder(features, "UI Canvases", "Where UI is drawn")
            .add(startup(F(f_bool("world_ui_enabled", true), "World-Space UI", "Draw world-space canvases (nameplates, signs) at render resolution")))
            .add(startup(F(f_bool("screen_ui_enabled", true), "Screen UI", "Draw screen overlay canvases (HUD, menus) at window resolution")))
            .done());

        // =====================================================================================
        // Stylize
        // =====================================================================================
        g.push_back(GroupBuilder(stylize, "Outline", "Ink lines along depth and normal edges")
            .toggle(F(f_bool("outline_enabled", true), "Outline", "Draw edge outlines"))
            .add(F(f_float("outline_thickness", 1.0f, 0.05f, 0.0f, 8.0f), "Thickness", "Line width, in render pixels"))
            .add(F(listed(f_color4("outline_color", glm::vec4(0.05f, 0.04f, 0.08f, 1.0f))), "Colour", "Line colour and opacity"))
            .add(F(f_float("depth_threshold", 0.02f, 0.001f, 0.0f, 10.0f), "Depth Threshold", "Relative depth jump that counts as an edge; lower = more lines"))
            .add(F(f_float("normal_threshold", 0.75f, 0.005f, 0.0f, 2.0f), "Normal Threshold", "Change in surface direction that counts as an edge; lower = more lines"))
            .done());

        g.push_back(GroupBuilder(stylize, "Palette", "Quantizes the image to a limited colour palette")
            .toggle(F(f_bool("palette_enabled", true), "Palette", "Snap colours to the palette"))
            .add(startup(F(root_relative(f_asset("palette", "palettes", ".png")), "Palette Image", "A strip of colours the image is snapped to")))
            .done());

        g.push_back(GroupBuilder(stylize, "Dither", "Ordered dithering before palette quantization")
            .toggle(F(f_bool("dither_enabled", true), "Dither", "Add ordered dither"))
            .add(F(f_float("dither_strength", 0.0f, 0.005f, 0.0f, 1.0f), "Strength", "How strong the dither pattern is"))
            .done());

        g.push_back(GroupBuilder(stylize, "Pixel Stability", "Keeps low-resolution renders steady under camera motion")
            .add(F(f_bool("camera_pixel_snap", true), "Camera Pixel Snap", "Snap orthographic cameras to whole pixels so edges don't shimmer"))
            .add(project_only(F(f_bool("texel_aa", true), "Texel AA",
                   "Texture texels stay hard at rest but their edges blend over one screen pixel, so magnified textures don't crawl; off = raw nearest sampling. "
                   "Baked into the texture sampler at start-up: project-wide, applies after Restart Editor Engine")))
            .done());

        // =====================================================================================
        // Debug
        // =====================================================================================
        g.push_back(GroupBuilder(debug, "Debug View", "Shows one raw render channel instead of the final image")
            .add(F(f_enum("debug_view", {"off", "albedo", "normals", "roughness", "metallic", "emissive", "material_ao",
                                         "world_pos", "depth", "direct", "indirect", "shadows", "contact_shadows", "ssao",
                                         "ssr", "ssr_confidence", "ssgi", "dof", "volumetrics", "lines", "solid", "wireframe",
                                         "material_preview", "velocity"}),
                   "View", "One intermediate channel, unprocessed (no tonemap, bloom or TAA). Effect views need their feature on"))
            .done());
        return g;
    }();
    return groups;
}

/** @brief Every render key the groups above cover (the rest are shown generically). */
inline std::vector<std::string> render_settings_keys() {
    std::vector<std::string> keys;
    for (const auto& g : render_settings_groups()) for (const auto& f : g.all_fields()) keys.push_back(f.key);
    return keys;
}

inline const std::vector<SettingsGroup>& project_settings_groups() {
    static const std::vector<SettingsGroup> groups = {
        {"Window", {f_string("title", "toyengine"), f_int("width", 1920, 64, 16384), f_int("height", 1080, 64, 16384),
                    f_bool("vsync", true)}},
        {"Physics", {f_vec3("gravity", glm::vec3(0.0f, 0.0f, -9.81f), 0.05f), f_float("fixed_timestep", 1.0f / 60.0f, 0.0005f, 0.0001f, 1.0f)}},
        {"Jobs", {f_int("worker_threads", 0, 0, 256), f_int("parallel_threshold", 4, 1, 1 << 20)}},
        {"Output", {f_bool("save_on_exit", true), f_string("filepath", "./output/frame.png"), f_bool("save_low_res", true)}},
        {"Debug", {f_enum("overlay", {"off", "fps", "full"})}},
    };
    return groups;
}

/** @brief The config.yaml section each project settings group lives in. */
inline std::string project_section_key(const std::string& group_title) {
    if (group_title == "Window") return "window";
    if (group_title == "Physics") return "physics";
    if (group_title == "Jobs") return "jobs";
    if (group_title == "Debug") return "debug";
    return "output";
}

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_SETTINGS_SCHEMA_H
