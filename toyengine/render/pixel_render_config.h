/**
 * @file pixel_render_config.h
 * @brief Tunable parameters for the pixel-art render pipeline.
 */

#ifndef TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H
#define TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H

#include <cstdint>
#include <iostream>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/pipeline/shader_library.h>
#include <gfxcoopa/pipeline/surface_shader.h>
#include <gfxcoopa/engine/render_features.h>

namespace toy {
namespace render {

/**
 * @brief One quality tier for a render feature's preset block.
 *
 * Selected per feature via PixelRenderConfig's `*_quality` fields and expanded into
 * concrete parameter values by PixelRenderConfig::apply_quality_presets(). `High` is
 * defined as the engine's shipped defaults, so a config that never mentions quality
 * renders identically to one that sets every feature to `high`.
 */
enum class RenderQuality {
    Low,     /**< Cheapest tier: lowest resolutions and sample counts. */
    Medium,  /**< Balanced tier below the shipped defaults. */
    High,    /**< The shipped defaults. */
    Ultra    /**< Highest tier: maximum resolutions and sample counts. */
};

/**
 * @brief Which intermediate render buffer `debug_view` replaces the image with.
 *
 * Explicit int values: `debug_view.frag`'s `DBG_*` push-constant constants MUST match
 * this enum member-for-member (see PixelRenderPipeline's debug_view_pass_). Everything
 * from Albedo through Ssgi is a "channel" (debug_view_is_channel() below) drawn by that
 * one fullscreen pass, raw -- no tonemap, no bloom, no TAA history blend -- so what's on
 * screen is exactly the number the renderer computed. Dof/Volumetrics instead flow
 * through their own already-existing pass with the post chain's stylize step reduced to
 * a no-op (see record_post_chain_()); Lines is an overlay on the normal image, not a
 * replacement, and Off is the normal image.
 */
enum class DebugView : int {
    Off = 0,
    // --- G-buffer / material ---
    Albedo,
    Normals,
    Roughness,
    Metallic,
    Emissive,
    MaterialAo,
    WorldPos,
    Depth,
    // --- lighting terms ---
    Direct,
    Indirect,
    Shadows,
    /// The resolved contact-shadow buffer (ContactShadowPass), unscaled by strength or per-light
    /// darkness. Black unless `contact_shadows_enabled` -- the march returns 0 on its first line
    /// when its strength is 0, the same way `ssgi` needs `ssgi_traced` and `dof` needs
    /// `dof_enabled`. Shows what lighting actually samples, i.e. post temporal accumulation,
    /// rather than a raw single-frame march.
    ContactShadows,
    Ssao,
    // --- screen-space ---
    Ssr,
    SsrConfidence,
    Ssgi,
    // --- in-chain ---
    Dof,
    Volumetrics,
    // --- overlay ---
    Lines,
    // --- editor viewport shading (debug_view_pass_ channels) ---
    /// Blender-style "solid" shading: a headlight + fixed key term over the G-buffer normals,
    /// lightly tinted by albedo, on a neutral background. No scene lights, shadows or post.
    Solid,
    /// A flat backdrop with surfaces only faintly filled -- the base the editor's wireframe
    /// edges (debug lines) draw over.
    Wireframe,
    /// Blender-style "material preview": the G-buffer material (albedo / metallic / roughness /
    /// emissive) lit by a fixed studio rig on a neutral backdrop, independent of scene lights.
    MaterialPreview,
    // --- G-buffer, continued (appended so the editor's shading-mode values above stay put) ---
    /// The G4 velocity attachment: per-object screen motion since last frame, shown as a
    /// colour offset from mid-grey (red = +x, green = +y, scaled 8x), blue where the surface
    /// was behind the eye last frame. Static geometry under a still camera is flat grey.
    Velocity,
};

/**
 * @brief Parses `debug_view`'s YAML string into a DebugView, defaulting to Off.
 *
 * Unlike an unrecognized YAML *key* (silently ignored, per this file's own parsing
 * convention), a typo'd *value* here must not silently look like "the debug view is
 * broken" -- so an unknown name is reported to stderr, naming the valid set, rather than
 * failing quietly the way a bad key does.
 */
inline DebugView parse_debug_view(const std::string& value) {
    if (value == "off")             return DebugView::Off;
    if (value == "albedo")          return DebugView::Albedo;
    if (value == "normals")         return DebugView::Normals;
    if (value == "roughness")       return DebugView::Roughness;
    if (value == "metallic")        return DebugView::Metallic;
    if (value == "emissive")        return DebugView::Emissive;
    if (value == "material_ao")     return DebugView::MaterialAo;
    if (value == "world_pos")       return DebugView::WorldPos;
    if (value == "depth")           return DebugView::Depth;
    if (value == "direct")          return DebugView::Direct;
    if (value == "indirect")        return DebugView::Indirect;
    if (value == "shadows")         return DebugView::Shadows;
    if (value == "contact_shadows") return DebugView::ContactShadows;
    if (value == "ssao")            return DebugView::Ssao;
    if (value == "ssr")             return DebugView::Ssr;
    if (value == "ssr_confidence")  return DebugView::SsrConfidence;
    if (value == "ssgi")            return DebugView::Ssgi;
    if (value == "dof")             return DebugView::Dof;
    if (value == "volumetrics")     return DebugView::Volumetrics;
    if (value == "lines")           return DebugView::Lines;
    if (value == "solid")           return DebugView::Solid;
    if (value == "wireframe")       return DebugView::Wireframe;
    if (value == "material_preview") return DebugView::MaterialPreview;
    if (value == "velocity")        return DebugView::Velocity;
    std::cerr << "[toy::render] Unknown debug_view '" << value << "', expected one of: "
                 "off | albedo | normals | roughness | metallic | emissive | material_ao | "
                 "world_pos | depth | velocity | direct | indirect | shadows | contact_shadows | ssao | "
                 "ssr | ssr_confidence | ssgi | dof | volumetrics | lines | solid | wireframe | material_preview. Using 'off'.\n";
    return DebugView::Off;
}

/**
 * @brief True for every DebugView drawn by PixelRenderPipeline's debug_view_pass_ --
 *        i.e. every value except Off, Dof, Volumetrics and Lines (see that enum's doc).
 */
inline bool debug_view_is_channel(DebugView view) {
    switch (view) {
        case DebugView::Off:
        case DebugView::Dof:
        case DebugView::Volumetrics:
        case DebugView::Lines:
            return false;
        default:
            return true;
    }
}

/**
 * @brief The editor's viewport shading modes (Solid / Wireframe / MaterialPreview): drawn by
 *        the channel pass, but meant to be looked at, not measured -- so unlike the diagnostic
 *        channels they keep TAA's normal history blend (otherwise the sub-pixel jitter shows
 *        as shaking edges).
 */
inline bool debug_view_is_editor_shading(DebugView view) {
    return view == DebugView::Solid || view == DebugView::Wireframe || view == DebugView::MaterialPreview;
}

/**
 * @struct PixelRenderConfig
 * @brief Configuration for PixelRenderPipeline: internal resolution, upscaling,
 *        banded lighting, hard shadows, and the pixel-art post-process stack
 *        (outline, palette quantization, ordered dithering), plus the
 *        independently-toggleable SSAO / SSR add-ons.
 */
struct PixelRenderConfig {
    // --- Feature toggles ---
    bool outline_enabled   = true;
    bool palette_enabled   = true;  /**< Independent of palette_path, so toggling off keeps the configured path. */
    bool dither_enabled    = true;  /**< Independent of dither_strength, so toggling off keeps the configured strength. */
    bool camera_pixel_snap = true;  /**< Orthographic cameras only. */
    bool soft_lighting     = false; /**< true = smooth Cook-Torrance direct lighting; false = this engine's default banded/ramped cel-shaded look. */
    bool ssao_enabled      = true;
    bool ssr_enabled       = true;  /**< Also gates the SSGI diffuse-bounce term (ssgi_intensity). */
    bool transparency_enabled = false;  /**< Forward BLEND-material pass, drawn after SSR compositing. */
    /**
     * Editor viewport shading (debug_view solid / material_preview): darken with SSAO.
     * Runtime-switchable; needs ssao_enabled (startup-fixed) for there to be any AO to show.
     */
    bool editor_ssao = true;
    /**
     * Editor viewport shading (solid / material preview): Blender's X-Ray -- surfaces drawn at
     * this opacity over the backdrop (1 = opaque, off). Runtime-switchable, editor-only.
     */
    float editor_xray_alpha = 1.0f;

    /**
     * Screen-space refraction for BLEND MESH objects only -- MeshRenderer, not SdfRenderer;
     * SDF glass is deliberately excluded. Bends the background
     * sample by the surface's IOR/thickness, applies Beer-Lambert tint absorption and
     * roughness-driven blur, and optionally chromatic aberration and a Fresnel falloff --
     * see assets/shaders/refraction.glsl. Requires transparency_enabled; meaningless
     * without it. Per-object ior/refraction_thickness/refraction_tint/refraction override
     * these defaults via PBRMaterial (see mesh_renderer.h).
     */
    bool      refraction_enabled       = true;
    float     refraction_ior           = 1.45f; /**< Default IOR; glass ~1.45, water ~1.33. */
    float     refraction_thickness     = 0.25f; /**< Default world-space distance the ray travels through the object. */
    float     refraction_strength      = 1.0f;  /**< Global multiplier on the screen-space UV offset. */
    float     refraction_max_offset    = 0.08f; /**< Clamp in UV units; stops smearing at grazing angles. */
    float     refraction_chromatic     = 0.0f;  /**< RGB IOR split (chromatic aberration); 0 = single tap. */
    float     refraction_blur          = 1.0f;  /**< Roughness -> scene-colour mip scale; frosted glass. */
    float     refraction_density       = 1.0f;  /**< Beer-Lambert absorption strength through refraction_tint. */
    bool      refraction_fresnel       = true;  /**< Dim transmission at grazing angles (energy already in the SSR/sky term). */
    glm::vec3 refraction_tint          = glm::vec3(1.0f, 1.0f, 1.0f); /**< Default transmission tint. */
    /**
     * When true (and refraction_enabled), refraction_scene_color_mip_pass_ -- the dedicated
     * scene-colour chain the MESH forward pass's u_scene_color reads whenever refraction is
     * active (see that member's own doc in pixel_render_pipeline.h) -- is built from the SSR
     * composite output each frame, so refraction sees a background that includes SSR
     * reflections. When false, it's built from the same pre-SSR image ssr_pass_'s own trace
     * uses instead. Either way this is a SEPARATE, independent SceneColorMipPass instance
     * from scene_color_mip_pass_ -- not a second execute() on that shared one, which would
     * corrupt its own internal per-mip descriptor sets (see that member's doc for the
     * validation-layer error this was caught by). MESH-only: BLEND SdfRenderers are unaffected
     * either way, since SDF glass is excluded from refraction entirely.
     */
    bool      refraction_include_reflections = true;

    /**
     * GLOBAL exponential height fog (Unreal's Exponential Height Fog / HDRP's fog model),
     * analytic and config-driven -- see the fog_* fields below and gfx/fog.glsl. There is no fog
     * component and no local fog volume: anything bounded is a VolumeComponent on the
     * volumetrics pass instead. Applied per medium: gfxcoopa's FogPass fogs the opaque scene and
     * sky before translucency, and every forward shader (BLEND meshes, water, particles, SDF
     * glass) fogs its own fragment at its own distance. With the camera under water only the
     * part of each ray above the surface is fogged. Runtime: switches on the next frame.
     */
    bool fog_enabled = true;

    /**
     * The underwater look (toyengine/render/passes/underwater_pass.h): fog, absorption and
     * caustics when the camera is below a water surface, driven per frame by set_water_state().
     * Startup-fixed -- it sits in the post chain's source path -- and costs one
     * full-resolution HDR copy per frame while the camera is above water.
     */
    bool underwater_enabled = true;

    /**
     * Raymarched LOCAL volumes -- scene-placed VolumeComponents, each `kind: fog` (a
     * static pocket), `wind` (advected ribbons) or `haze` (drifting billows). Runs in
     * linear HDR after fog, translucency and the underwater look, and before DOF/bloom, so volumes defocus and
     * sun-lit ones glow (see gfxcoopa's VolumetricsPass / gfx/volumetrics.glsl).
     *
     * A separate pass from fog rather than more fog parameters, because fog is an
     * analytic medium that is everywhere, while these are bounded and their density
     * varies within them -- which has no closed form, so they must march. The two
     * toggle independently. The march clips to the union of the volumes' bounds, so a
     * view with no volume in it skips the march entirely.
     *
     * Startup-fixed, same policy as bloom_enabled/dof_enabled: the
     * pass's source image and DOF's source image are both chosen once at construction
     * from this flag's value.
     */
    bool volumetrics_enabled = false;

    /**
     * Signed-distance-field raymarching system (see gfxcoopa's SdfRenderer/SdfShape
     * components and toyengine's sdf_gbuffer.frag/sdf_forward.frag/sdf_shadow*.frag). sdf_enabled is the single per-frame gate: when false, the SDF
     * gather in PixelRenderPipeline::render() produces an empty draw list and every
     * downstream recording site (G-buffer/shadow/forward) is naturally a no-op --
     * the five SDF passes themselves are always constructed (same always-on-but-gated
     * policy as SSR/fog/bloom above).
     */
    bool sdf_enabled = true;
    /** Independent of sdf_enabled/shadows_enabled -- an SDF object can be visible but never
     *  cast a shadow (cheaper), same as shadows_enabled gates mesh shadow casting globally. */
    bool sdf_shadows_enabled = true;
    /** Global ceiling on SdfRenderer::max_steps; a per-renderer value above this is clamped down. */
    uint32_t sdf_max_steps = 64;
    /** Cheaper step budget for the depth-only shadow march (see SdfShadowPass). */
    uint32_t sdf_shadow_max_steps = 32;
    /** SdfData's renderer/shape SSBO capacities -- startup-fixed (SSBO sizing), not a runtime toggle. */
    uint32_t sdf_max_renderers = 64;
    uint32_t sdf_max_shapes    = 512;

    // --- Quality presets ---
    // One RenderQuality tier per feature with a meaningful cost dial. Expanded into the
    // concrete fields by apply_quality_presets(); AppConfig::load() applies the presets
    // BEFORE parsing the per-key values, so an explicitly-written key always overrides
    // its preset. All default to High (= the shipped field defaults below).
    RenderQuality shadow_quality      = RenderQuality::High;  /**< Sets shadow_map/cube/spot_shadow_resolution, shadow_pcf_samples, shadow_pcss_taps and contact_shadow_steps. */
    RenderQuality ssao_quality        = RenderQuality::High;  /**< Sets ssao_slices/ssao_steps/ssao_max_radius_px/ssao_temporal_frames. */
    RenderQuality ssr_quality         = RenderQuality::High;  /**< Sets ssr_max_iterations. */
    RenderQuality ssgi_quality        = RenderQuality::High;  /**< Sets ssgi_max_iterations. Separate from ssr_quality: the traced bounce is its own trace/resolve/denoise chain with its own budget (see ssgi_max_iterations). */
    RenderQuality dof_quality         = RenderQuality::High;  /**< Sets dof_sample_count (Ultra equals High: the gather shader clamps taps to 48). */
    RenderQuality volumetrics_quality = RenderQuality::High;  /**< Sets volumetrics_step_count and volumetrics_max_scatter_lights. */
    RenderQuality sdf_quality         = RenderQuality::High;  /**< Sets sdf_max_steps and sdf_shadow_max_steps. */
    /** Water: simulation range, ripples, mesh density/LOD and shader detail. Not expanded here --
     *  Engine maps it to toy::water::WaterSettings (toyengine/water/water_settings.h) every
     *  frame, so it is live-switchable. */
    RenderQuality water_quality       = RenderQuality::High;
    /** Physical sky: the sky-view table's integration steps and the cloud march's view / shadow
     *  steps. Not expanded here -- read by the pipeline every frame, so it switches live. */
    RenderQuality sky_quality         = RenderQuality::High;

    /**
     * @brief Overwrites every preset-covered field from the `*_quality` tiers above.
     *
     * Called by AppConfig::load() after the quality keys are parsed and before the
     * per-key values are, so explicit keys win over their preset. The High row of every
     * table equals the fields' own defaults.
     */
    void apply_quality_presets() {
        // shadow_pcss_taps/contact_shadow_steps only cost anything when their own feature
        // toggle is on, so scaling them here is free for a config that leaves both off.
        switch (shadow_quality) {
            // shadow_map_resolution is PER CASCADE (see its doc): the directional atlas is up
            // to 2x this on each axis, so the VRAM column at the default 4 cascades is
            // 4/16/64/144 MB. Medium matches the VRAM of one 2048 map, and high spends 4x that
            // so every cascade beats a single 2048 map's world-per-texel at every distance.
            case RenderQuality::Low:    shadow_map_resolution = 512;  cube_shadow_resolution = 256;  spot_shadow_resolution = 512;  shadow_pcf_samples = 8;  shadow_pcss_taps = 4;  contact_shadow_steps = 4;  break;
            case RenderQuality::Medium: shadow_map_resolution = 1024; cube_shadow_resolution = 512;  spot_shadow_resolution = 1024; shadow_pcf_samples = 16; shadow_pcss_taps = 6;  contact_shadow_steps = 6;  break;
            case RenderQuality::High:   shadow_map_resolution = 2048; cube_shadow_resolution = 512;  spot_shadow_resolution = 1024; shadow_pcf_samples = 24; shadow_pcss_taps = 8;  contact_shadow_steps = 8;  break;
            case RenderQuality::Ultra:  shadow_map_resolution = 3072; cube_shadow_resolution = 1024; spot_shadow_resolution = 2048; shadow_pcf_samples = 32; shadow_pcss_taps = 16; contact_shadow_steps = 16; break;
        }
        // The local-light atlas and budgets per tier, sized so the budget packs: a point light is
        // a 3x2 block of cube tiles, a spot one spot tile (see pack_shadow_atlas()).
        switch (shadow_quality) {
            case RenderQuality::Low:    local_shadow_atlas_resolution = 1024; max_shadowed_point_lights = 1; max_shadowed_spot_lights = 1; break;
            case RenderQuality::Medium: local_shadow_atlas_resolution = 2048; max_shadowed_point_lights = 1; max_shadowed_spot_lights = 2; break;
            case RenderQuality::High:   local_shadow_atlas_resolution = 4096; max_shadowed_point_lights = 4; max_shadowed_spot_lights = 4; break;
            case RenderQuality::Ultra:  local_shadow_atlas_resolution = 6144; max_shadowed_point_lights = 4; max_shadowed_spot_lights = 3; break;
        }
        switch (ssao_quality) {
            case RenderQuality::Low:    ssao_slices = 1; ssao_steps = 6;  ssao_max_radius_px = 32.0f; ssao_temporal_frames = 4;  break;
            case RenderQuality::Medium: ssao_slices = 2; ssao_steps = 8;  ssao_max_radius_px = 40.0f; ssao_temporal_frames = 8;  break;
            case RenderQuality::High:   ssao_slices = 2; ssao_steps = 16; ssao_max_radius_px = 80.0f; ssao_temporal_frames = 8;  break;
            case RenderQuality::Ultra:  ssao_slices = 3; ssao_steps = 24; ssao_max_radius_px = 96.0f; ssao_temporal_frames = 12; break;
        }
        switch (ssr_quality) {
            case RenderQuality::Low:    ssr_max_iterations = 24;  ssr_rays_per_pixel = 1; break;
            case RenderQuality::Medium: ssr_max_iterations = 48;  ssr_rays_per_pixel = 1; break;
            case RenderQuality::High:   ssr_max_iterations = 64;  ssr_rays_per_pixel = 1; break;
            case RenderQuality::Ultra:  ssr_max_iterations = 128; ssr_rays_per_pixel = 2; break;
        }
        // Lower across the board than ssr_quality's rows, and deliberately: the bounce ray is
        // short and lands in a coarse cone mip, so it converges in far fewer steps. This is
        // the single biggest cost dial the traced bounce has -- it is three full-resolution
        // passes (trace, resolve, denoise) when ssgi_traced is on.
        switch (ssgi_quality) {
            case RenderQuality::Low:    ssgi_max_iterations = 12; ssgi_resolution_scale = 2; break;
            case RenderQuality::Medium: ssgi_max_iterations = 20; ssgi_resolution_scale = 2; break;
            case RenderQuality::High:   ssgi_max_iterations = 32; ssgi_resolution_scale = 1; break;
            case RenderQuality::Ultra:  ssgi_max_iterations = 48; ssgi_resolution_scale = 1; break;
        }
        switch (dof_quality) {
            case RenderQuality::Low:    dof_sample_count = 16; break;
            case RenderQuality::Medium: dof_sample_count = 32; break;
            case RenderQuality::High:   dof_sample_count = 48; break;
            case RenderQuality::Ultra:  dof_sample_count = 48; break; // dof.frag clamps taps to [8, 48]
        }
        // The two multiply: every scatter light is evaluated at every step. Low drops the
        // light loop entirely and keeps only the sun-shaft term, which is one shadow tap.
        switch (volumetrics_quality) {
            case RenderQuality::Low:    volumetrics_step_count = 24; volumetrics_max_scatter_lights = 0; break;
            case RenderQuality::Medium: volumetrics_step_count = 32; volumetrics_max_scatter_lights = 2; break;
            case RenderQuality::High:   volumetrics_step_count = 48; volumetrics_max_scatter_lights = 4; break;
            case RenderQuality::Ultra:  volumetrics_step_count = 96; volumetrics_max_scatter_lights = 4; break; // 4 == MAX_SCATTER_LIGHTS
        }
        switch (sdf_quality) {
            case RenderQuality::Low:    sdf_max_steps = 32;  sdf_shadow_max_steps = 16; break;
            case RenderQuality::Medium: sdf_max_steps = 48;  sdf_shadow_max_steps = 24; break;
            case RenderQuality::High:   sdf_max_steps = 64;  sdf_shadow_max_steps = 32; break;
            case RenderQuality::Ultra:  sdf_max_steps = 128; sdf_shadow_max_steps = 64; break;
        }
    }

    // --- Internal resolution ---
    /**
     * "fixed", "divisor", or "fill": render_height rows, width following the display region's
     * aspect so the image fills it (the Engine rebuilds the pipeline when that aspect changes).
     */
    std::string resolution_mode = "fixed";
    uint32_t    render_width    = 480;       /**< Used when resolution_mode == "fixed". */
    uint32_t    render_height   = 270;       /**< Used when resolution_mode == "fixed" or "fill". */
    /// fill mode: the display region's aspect (width / height). Set by the Engine, not YAML;
    /// 0 = use the swapchain's.
    float       fill_aspect     = 0.0f;
    uint32_t    scale_divisor   = 4;         /**< Used when resolution_mode == "divisor". */
    std::string upscale_mode    = "fit";     /**< "fit" (aspect-preserving best fit, default,
                                                   letterboxed only on the mismatched axis) or
                                                   "integer" (whole-number scale, more letterboxing
                                                   but every texel is an exact NxN block). */

    // --- Lighting ---
    float exposure          = 1.0f;
    /**
     * Auto-exposure (eye adaptation): a 1x1 metering pass measures the geometric-mean
     * luminance of the final pre-tonemap frame each frame and adapts an exposure
     * multiplier toward it, which `exposure` above is then scaled by -- so the config
     * value keeps its meaning as the scene's baseline stop (see ExposurePass /
     * exposure.frag). STARTUP-FIXED: it constructs a pass and binds the stylize pass's
     * exposure descriptor. Every tunable below it is runtime.
     */
    bool  auto_exposure_enabled      = false;
    float auto_exposure_compensation = 1.0f;  /**< Multiplier on the metered exposure. */
    float auto_exposure_speed_up     = 3.0f;  /**< Adaptation rate (1/sec) when the scene gets BRIGHTER. */
    float auto_exposure_speed_down   = 1.0f;  /**< Adaptation rate (1/sec) when the scene gets DARKER;
                                                lower than speed_up on purpose -- the eye darkens fast
                                                and brightens slowly, and one symmetric rate reads wrong
                                                in both directions. */
    float auto_exposure_min          = 0.05f; /**< Clamps on the adapted multiplier. Keep the range
                                                narrow enough that a dark corner can't blow the whole
                                                frame out on approach. */
    float auto_exposure_max          = 8.0f;

    /**
     * Colour grading through a strip LUT (N*N by N, e.g. 1024x32 -- the format an image
     * editor or grading tool exports), applied after the tonemap and before
     * outline/dither/palette. Empty path loads a 1x1 dummy and disables the lookup, so
     * this costs nothing when unused. STARTUP-FIXED (the image is loaded and bound once),
     * unlike grading_enabled, which only zeroes a push constant.
     */
    std::string grading_lut_path;
    bool        grading_enabled = true; /**< Independent of grading_lut_path, so toggling off keeps the configured path. */
    float light_bands       = 4.0f;   /**< Discrete shading steps per light; <= 1 disables banding. */
    float spec_threshold    = 0.55f;  /**< Hard specular highlight cutoff. */
    float rim_strength      = 0.0f;   /**< 0 disables the rim term. */
    // ambient_intensity/sky_intensity/ssgi_intensity/ssgi_distance/sky_zenith/sky_horizon/
    // sky_ground live in `indirect` below (shared with SsrPass::Params so the two can never
    // disagree -- see render_features.h).

    // --- Shadows ---
    bool     shadows_enabled        = true;
    /**
     * @brief Edge length, in texels, of ONE directional cascade's tile -- not of the whole
     *        directional shadow image.
     *
     * The directional map is an atlas of `shadow_cascades` tiles this size (1x1, 2x1 or 2x2
     * -- see ShadowMapTarget), so the image is up to 2x this on each axis and up to 4x this
     * many texels. At the default 4 cascades: low 512 -> 1024^2 (4 MB), medium 1024 ->
     * 2048^2 (16 MB), high 2048 -> 4096^2 (64 MB), ultra 3072 -> 6144^2 (144 MB).
     */
    uint32_t shadow_map_resolution  = 2048;
    /**
     * @brief How many cascades the directional shadow splits into, 1..4.
     *
     * Each cascade is an independent ortho fit to one slice of the camera's depth range, so
     * the near cascade's texels land on a box a few metres across instead of one spanning
     * the whole `shadow_distance` -- that is what makes close-up shadows sharp while distant
     * ones stay coarse. Every cascade re-draws the shadow casters, so this multiplies the
     * (depth-only) directional shadow pass cost directly.
     *
     * 1 is the single-map behaviour: one box over the whole range, no atlas, no per-pixel
     * cascade selection. Deliberately NOT covered by `shadow_quality` -- the tier moves
     * resolution only, so changing tiers can never silently change how many cascades exist.
     */
    uint32_t shadow_cascades        = 4;
    /**
     * @brief Log-vs-uniform blend for where the cascade boundaries fall, 0..1.
     *
     * 1 is fully logarithmic (equalizes world-per-texel across cascades, but crushes the
     * first cascade to centimetres around the near plane), 0 fully uniform (first cascade so
     * large it barely improves on one map). See toy::render::compute_cascade_splits().
     * Ignored when `shadow_cascades` is 1.
     */
    float    shadow_cascade_split_lambda = 0.75f;
    /**
     * @brief How the directional cascades are placed: `"frustum"` or `"focus"`.
     *
     * `"frustum"` (default) slices the camera's own view frustum from the near plane out to
     * `shadow_distance` (see `shadow_cascade_split_lambda`) -- right for a camera close to its
     * subject, as on assets/scenes/pixel_demo.
     *
     * `"focus"` instead centres every cascade on the camera's FOCUS POINT, as nested boxes
     * whose radii grow geometrically from `shadow_focus_radius` out to `shadow_distance`
     * (compute_focus_cascade_radii()). The focus point lies along the view direction, at the
     * first of:
     *   1. `shadow_focus_distance`, when > 0;
     *   2. a focus the scene states -- the camera's `focus_distance` or `focus_object`, or an
     *      orbit camera's target under `dof_focus_mode: orbit_target`;
     *   3. otherwise, what the camera is looking at: the G-buffer surface under the screen
     *      centre, read back a frame later (PixelRenderPipeline::record_focus_probe_), so no
     *      scene setup is needed at all. For a camera that orbits or follows a subject from
     * tens of metres away, frustum slicing spends its finest cascades on the empty air in
     * front of the lens; focus cascades put them where the player is looking, so the shadows
     * there get the texel density a close-up camera gets. The box sizes are constant, so the
     * shadows do not swim as the camera turns; only the texel-snapped centre moves.
     */
    std::string shadow_fit = "frustum";
    /**
     * @brief `"focus"` fit only: radius, in world units, of the finest cascade around the
     *        focus point. Its texels are `2 * (radius + 1) / shadow_map_resolution` across.
     */
    float    shadow_focus_radius = 12.0f;
    /**
     * @brief `"focus"` fit only: distance from the eye to the focus point along the view
     *        direction, in world units; 0 follows the scene's focus, else what the camera is
     *        looking at (see `shadow_fit`).
     */
    float    shadow_focus_distance = 0.0f;
    /**
     * @brief Ceiling on the directional PCF radius, in shadow-map texels.
     *
     * `shadow_softness` is converted per cascade into texels and clamped to this. A fine
     * cascade otherwise asks for more texels than the Vogel disk is tuned for. The clamp also
     * sizes the cascade-selection inset, which keeps the widest kernel inside its own atlas
     * tile. Raise it, with `shadow_quality` taps to match, when a fine cascade's penumbra is
     * visibly narrower than `shadow_softness` asks for.
     */
    float    shadow_pcf_max_texels = 12.0f;
    /**
     * @brief Receiver-plane depth bias for the directional PCF.
     *
     * Off, every PCF tap compares against the shading point's own depth. A receiver that does
     * not face the light then shadows itself across a wide kernel, so the normal offset has
     * to clear the whole kernel (compute_shadow_normal_bias() adds the PCF radius). That shrinks
     * every shadow by `shadow_softness` -- harmless at the default 0.15, visible on small
     * casters once a large scene wants a wide penumbra.
     *
     * On, each tap's compare depth follows the receiver's own plane
     * (gfx_shadow_dir_pcf_vogel_rpdb), and the normal offset drops to `shadow_normal_bias`
     * texels alone. So `shadow_softness` widens the penumbra without eroding the shadow.
     * Not applied on the PCSS path.
     */
    bool     shadow_receiver_plane_bias = false;
    /** @brief Receiver-plane bias only: the steepest receiver slope it trusts, as a tangent. */
    float    shadow_receiver_max_slope  = 4.0f;
    uint32_t cube_shadow_resolution = 512;   ///< One point-light cube FACE tile, texels (local atlas).
    uint32_t spot_shadow_resolution = 1024;  ///< One spot-light tile, texels (local atlas).
    /**
     * @brief Edge of the local-light (point/spot) shadow atlas, texels. Every shadowed point
     *        light takes a 3x2 block of `cube_shadow_resolution` tiles and every shadowed spot one
     *        `spot_shadow_resolution` tile; lights that do not fit are left unshadowed (lowest
     *        importance first). Startup-fixed (sizes the images); driven by `shadow_quality`.
     */
    uint32_t local_shadow_atlas_resolution = 4096;
    /**
     * @brief How many point / spot lights may cast shadows at once. The `cast_shadows` lights
     *        with the highest screen importance (influence radius over distance from the camera,
     *        times brightness, with a little hysteresis) win. RUNTIME; driven by `shadow_quality`.
     */
    uint32_t max_shadowed_point_lights = 4;
    uint32_t max_shadowed_spot_lights  = 4;
    /**
     * @brief Cache static casters' depth per local light (Unreal's static shadow caching for
     *        stationary/movable lights): a light's static geometry is rendered once into a cache
     *        atlas and copied in each frame, and only casters that moved recently draw live.
     *        Re-rendered when the light, its tile, or the static caster set changes. Costs a
     *        second atlas image. Startup-fixed.
     */
    bool     shadow_cache_enabled = true;
    /**
     * @brief A constant depth bias in the directional map's [0,1] light depth, written to
     *        LightUBO::dir_shadow_params.x.
     *
     * Surface shading ignores it (every shading path biases by `shadow_depth_bias_texels` +
     * `shadow_slope_bias_texels`, which scale with each map's own texel size); its one reader is
     * the volumetrics sun-shadow lookup, via VolumetricsUBO::shadow_params.z.
     */
    float    shadow_bias            = 0.005f;
    /**
     * @brief Constant shadow-map depth bias, in shadow-map TEXELS (Unreal's "Shadow Bias").
     *
     * Converted per cascade (and per local-light map, per pixel) into that map's own depth units,
     * so it means the same thing at every resolution and distance. RUNTIME.
     */
    float    shadow_depth_bias_texels = 1.0f;
    /**
     * @brief Slope-scaled depth bias, in texels per unit tan(angle between surface normal and
     *        light) -- Unreal's "Shadow Slope Bias". A receiver tilted away from the light
     *        climbs this much depth across one texel; capped at `shadow_slope_bias_max` tan.
     *        Multiplied by (1 + PCF radius in texels), since a soft kernel's outer taps compare
     *        that far across the receiver's slope. RUNTIME.
     */
    float    shadow_slope_bias_texels = 1.0f;
    float    shadow_slope_bias_max    = 5.0f;
    /**
     * @brief Fraction of `shadow_distance` over which directional shadows fade out at their far
     *        end, plus the width (tile uv) of the last cascade's outer band that fades to
     *        unshadowed -- so shadows end in a gradient rather than a line. 0 = hard cut. RUNTIME.
     */
    float    shadow_fade_fraction     = 0.1f;
    /**
     * @brief Normal-offset shadow bias, in shadow-map TEXELS BEYOND the PCF disk's own reach.
     *
     * Converted to world units every frame against the ortho box actually in effect, exactly as
     * `shadow_softness` below is -- see toy::render::compute_shadow_normal_bias(), which also
     * explains why the PCF radius is added in rather than left to the caller.
     *
     * Trades acne against peter-panning in the usual way: too small and grazing faces speckle
     * and crawl as the camera moves, too large and a shadow visibly detaches from its caster at
     * the contact point. At 1.0 the offset clears the PCF disk by a texel, which on
     * assets/scenes/pixel_demo costs about 2% of the shadowed area -- a measurable but
     * visually negligible recession.
     */
    float    shadow_normal_bias     = 1.0f;
    /**
     * @brief How far from the camera, in world units, the directional shadow is computed at
     *        all -- Unity's own "Shadow Distance" quality setting. update_dir_shadow_matrix_()
     *        fits the shadow frustum to a bounding sphere of the camera's OWN view frustum out
     *        to this distance (clipped to the camera's far clip plane), and separately uses it
     *        to size the near-side margin behind that sphere (so an off-frustum caster between
     *        the light and the visible sphere still shadows into frame). This is the only knob
     *        that affects the fit -- unlike the AABB-over-scene-content fit this replaced, no
     *        renderer/SDF/physics-body position is ever read, so nothing in the scene (however
     *        far off, however it got there) can perturb or blow out the shadow frustum.
     */
    float    shadow_distance        = 60.0f;
    bool     soft_shadows           = true;  /**< false = single hard depth compare (the pre-existing look). */
    /**
     * @brief Directional PCF penumbra radius in WORLD units, not texels. Converted to a
     *        texel count every frame against the ortho box actually in effect (see
     *        update_dir_shadow_matrix_()), so the penumbra stays visually constant in world
     *        units even as that box refits to the camera -- a fixed texel-count radius would
     *        otherwise shrink to imperceptible on a scene-spanning box and grow huge on a
     *        tight one. Ignored when soft_shadows is false.
     */
    float    shadow_softness        = 0.15f;
    /**
     * @brief Point-light PCF penumbra radius, in cube-face TEXELS. Clamped to 8; also sizes
     *        each face's guard band (radius + 2 texels widened field of view), so the kernel
     *        never reads across into the next face of the local shadow atlas. Ignored when
     *        soft_shadows is false.
     */
    float    point_shadow_softness  = 3.0f;
    /**
     * @brief Spot-light PCF penumbra radius in WORLD units, same convention as
     *        shadow_softness above. The spot map is a perspective projection, so its
     *        world-per-texel scale varies with each receiver's distance from the light;
     *        the conversion to a texel radius therefore happens PER PIXEL in
     *        gfx_local_shadow() (gfx/local_shadow.glsl) against the texel size at that
     *        receiver's depth. Clamped to shadow_pcf_max_texels, matching the directional
     *        radius. Ignored when soft_shadows is false.
     */
    float    spot_shadow_softness   = 0.15f;
    uint32_t shadow_pcf_samples     = 24;    /**< Vogel disk taps for directional soft shadows; clamped to 1..32, the kernel's own hard limit. */
    /**
     * PCSS contact hardening for the directional shadow (Unity HDRP's "High Quality"
     * shadow filtering): a blocker search drives the Vogel-PCF radius per pixel, so a
     * shadow is sharp where it meets its caster and widens with occluder distance,
     * instead of one constant `shadow_softness` width everywhere. Requires
     * soft_shadows; `shadow_softness` becomes the penumbra's MAXIMUM. Runtime toggle
     * (UBO fields only). Costs 8 extra depth reads per shadowed pixel, minus the
     * fully-lit early-out on open ground.
     */
    bool     shadow_pcss_enabled    = false;
    /**
     * Sun angular size for PCSS, as tan(angular radius): the penumbra grows by this
     * many world units per world unit of blocker-to-receiver gap. The real sun is
     * ~0.005; stylized scenes usually want it larger. Ignored unless
     * shadow_pcss_enabled.
     */
    float    shadow_pcss_light_size = 0.02f;
    /** PCSS blocker-search radius, in shadow-map texels; clamped to 1..16. */
    float    shadow_pcss_search_texels = 8.0f;
    /**
     * PCSS blocker-search tap count; clamped to 1..16. Set by `shadow_quality`. This is the
     * dial that actually costs, and it is separate from shadow_pcf_samples: the search runs
     * on every shadowed pixel (including the fully-lit ones it then early-outs on), while the
     * penumbra filter only runs where blockers were found.
     */
    uint32_t shadow_pcss_taps       = 8;
    /**
     * Screen-space contact shadows (Unity HDRP's feature of the same name): a short
     * per-pixel march through the G-buffer toward the sun, max()-combined with the
     * shadow map's result. Catches the few-centimetre contact occlusion the
     * normal-offset bias necessarily recedes from -- the gap at every object's base.
     * Deferred (directional light) only. Independent of shadows_enabled: with the
     * shadow maps off the march is the only directional occlusion term, which gives
     * a contact-only view of the effect. All four fields are runtime.
     *
     * Follows soft_shadows: when on, the march averages several rays across the sun's
     * angular cone (sized from shadow_pcss_light_size) for a PCSS-style penumbra that
     * is sharp at the contact point and softens with blocker distance; when off, a
     * single ray gives a hard edge.
     */
    bool     contact_shadows_enabled  = false;
    float    contact_shadow_length    = 0.5f;  /**< March length in world units. */
    float    contact_shadow_strength  = 1.0f;  /**< Occlusion strength of a contact hit, 0..1. */
    float    contact_shadow_thickness = 0.15f; /**< Depth tolerance in world units -- how thick a
                                                 screen-space occluder is assumed to be behind
                                                 its visible surface. */
    int      contact_shadow_steps     = 8;     /**< March steps; clamped to 1..24. Set by `shadow_quality` --
                                                 this is a per-pixel screen-space march, so the step count is
                                                 the feature's whole cost. */
    /**
     * Accumulate the contact-shadow march across frames, the way `ssao_temporal_enabled` and
     * `ssr_temporal_enabled` do for theirs. Off makes ContactShadowPass's resolve a passthrough
     * of the current frame, which is what an A/B capture needs: with it on, the march's input
     * moves with the TAA sub-pixel jitter, so the running average is not periodic in the jitter
     * cycle and two captures a cycle apart are not the same frame.
     */
    bool     contact_shadow_temporal_enabled = true;
    /**
     * Accumulation depth of the contact-shadow temporal resolve, on the same converging
     * `1/(N+1)` schedule (and against the same shared per-pixel count) as
     * `ssao_temporal_frames` and `ssr_temporal_frames`. Shallower than either: a contact
     * shadow is a thin, high-contrast, geometrically local feature, so deep accumulation
     * trades its crispness for stability faster than the other two do.
     */
    int      contact_shadow_temporal_frames = 16;

    // --- Outline ---
    float     outline_thickness = 1.0f;     /**< In low-resolution texels. */
    glm::vec4 outline_color     = glm::vec4(0.05f, 0.04f, 0.08f, 1.0f);
    float     depth_threshold   = 0.02f;    /**< Relative depth step (fraction of the fragment's own view-space distance) that counts as an edge; scale-invariant, so one value works at any camera distance. */
    float     normal_threshold  = 0.75f;

    // --- Palette ---
    std::string palette_path;                 /**< Empty disables palette quantization. */

    // --- Dither ---
    float dither_strength = 0.0f;             /**< 0 disables ordered dithering. */

    /**
     * Texel-AA sampling for the pixel-art material textures (STARTUP-FIXED: chooses the
     * TextureLoader's sampler at construction). True binds them LINEAR
     * (SamplerDesc::pixel_art_smooth()) and gbuffer.frag sharpens UVs with
     * gfx_texel_aa_uv(), which keeps texel interiors as flat and hard-edged as point
     * sampling while blending each texel boundary over exactly one screen pixel -- the
     * fix for magnified texels crawling/shimmering whenever the camera moves sub-pixel.
     * False restores raw NEAREST point sampling.
     */
    bool texel_aa = true;

    // --- SSAO ---
    float ssao_radius           = 0.5f;
    float ssao_bias             = 0.025f;
    float ssao_power            = 1.5f;
    int   ssao_slices           = 2;   ///< Horizon slices per pixel while the camera moves (ssao.frag).
    int   ssao_steps            = 8;   ///< March steps per slice direction while the camera moves.
    /**
     * Upper clamp on the horizon march's screen-space extent, in render-target pixels
     * (Unity HDRP's "Maximum Radius in Pixels": 40 medium, 80 high). Caps the cost and
     * the screen-space reach of near-camera geometry regardless of ssao_radius.
     */
    float ssao_max_radius_px    = 80.0f;
    /**
     * How much occlusion darkens direct lighting (Unity HDRP's Direct Lighting
     * Strength, pixel_lighting.frag): 0 = occlusion affects indirect only.
     */
    float ssao_direct_lighting_strength = 0.25f;
    /**
     * World-space plane-distance tolerance of the blur's bilateral weight, per dilation
     * unit (ssao_blur.frag). Tracks the geometry's step scale (what depth gap separates
     * two surfaces), independent of ssao_radius: coupling it to the gather radius made a
     * wider radius silently blend AO across terrace faces the kernel should reject.
     */
    float ssao_blur_plane_sigma = 0.375f;
    /**
     * Run the AO estimate, temporal resolve and blur at half resolution per axis (a quarter of
     * the pixels), then upsample to full resolution with the depth/normal-aware filter the SSR
     * composite uses -- Unity HDRP's and Unreal's default AO setup. The march keeps its
     * full-resolution footprint; AO comes out very slightly softer. Startup-fixed (sizes targets).
     */
    bool  ssao_half_res = true;
    /**
     * Lighter bilateral blur: 4x4 taps over the 8x8 kernel's footprint (a quarter of the
     * fetches), and the motion dilation capped at half the default so a pan cannot spread the
     * taps across a cache-hostile 40 px square. false = the original 8x8 kernel. RUNTIME.
     */
    bool  ssao_blur_light = true;
    bool  ssao_temporal_enabled = true;
    /**
     * Accumulation depth of the temporal resolve: each pixel averages this many frames of the
     * continuously-jittered estimate (blending frame N at 1/(N+1)) before switching to a
     * fixed-rate running average. 8 is Unreal's GTAO temporal filter (a ~0.1 history blend):
     * with the G-buffer's per-object motion vectors reprojecting history exactly, a short
     * window is enough for stability, and it keeps AO responding to moving objects within a
     * handful of frames instead of trailing behind them.
     */
    int   ssao_temporal_frames  = 8;
    /**
     * Variance-clip half-width of the AO temporal resolve, in standard deviations of the
     * current 3x3 raw neighbourhood (same idea as ssr_temporal_gamma / taa_variance_gamma):
     * reprojected history outside mean +- gamma*sigma is clamped to that band, which is what
     * stops occlusion an object cast before it moved from lingering. Lower = less ghosting,
     * more noise under motion. RUNTIME.
     */
    float ssao_temporal_gamma   = 1.0f;
    /**
     * Unreal's AO "Intensity": the lighting composite reads mix(1, ao, intensity), so 0
     * disables the darkening entirely and 1 applies the resolved AO as is. RUNTIME.
     */
    float ssao_intensity        = 1.0f;

    // --- SSR + SSGI ---
    float ssr_max_distance     = 30.0f;
    int   ssr_max_iterations   = 64;
    float ssr_thickness        = 0.08f;
    float ssr_thickness_scale  = 0.02f;
    float ssr_bias_texels      = 3.5f;
    /**
     * Roughness above which a surface stops tracing. 1.0 = everything traces, so rough surfaces
     * still feed the SSGI bounce.
     *
     * Worth knowing before tuning it: the composite does not *add* the reflection, it
     * **replaces** the env/sky specular with it --
     * `scene_color + (ssr_specular - confidence * ind.value) * ao_spec` in
     * `gfx/ssr_composite_body.glsl`. Roughness past the cutoff fades confidence to 0, which skips
     * that subtraction and leaves the analytic sky term standing, and this engine has no
     * reflection probes to hand those surfaces to (`indirect_hooks.glsl`'s
     * `hook_env_specular()` returns the sky gradient unmodified). Measured on terrain_test,
     * though, 1.0 vs 0.6 moves whole-frame saturation by 0.002 -- the handover is not the visible
     * lever it looks like on paper.
     */
    float ssr_roughness_cutoff = 1.0f;
    int   ssr_start_mip        = 0;
    int   ssr_min_mip0_steps   = 1;
    bool  ssr_temporal_enabled = true;
    /**
     * Accumulation depth of the SSR temporal resolve: each pixel averages this many frames of
     * the stochastically-jittered trace (blending frame N at 1/(N+1), against the shared
     * per-pixel count TemporalHistoryPass publishes) before switching to a fixed-rate running
     * average. Deeper = quieter reflections in motion, slower response to genuine change.
     * Same knob and same schedule as `ssao_temporal_frames`.
     */
    int   ssr_temporal_frames  = 32;
    /**
     * Accumulation depth of the traced-SSGI bounce's temporal resolve. Deeper than
     * `ssr_temporal_frames` on purpose: one cosine-hemisphere ray per pixel has far higher
     * variance than a near-mirror reflection ray, and a diffuse bounce is low-frequency enough
     * that the extra lag is invisible.
     */
    int   ssgi_temporal_frames = 48;
    /**
     * Fallback exponential-blend weight on history, used only where the shared accumulation
     * count is unavailable (a consumer bound to TemporalHistoryPass's 1x1 neutral texture, i.e.
     * a frame where that pass never ran). The converging `*_temporal_frames` average above is
     * what the shipped path uses.
     */
    float ssr_temporal_blend   = 0.85f;
    float ssr_blur_radius      = 0.5f;  /**< World-space sigma for the spatial SSR denoise (ssr_blur.frag). */
    bool  ssr_blur_light       = true;  /**< 3x3 SSR denoise footprint instead of 5x5 (a third of the reads). */
    bool  ssr_blur_zero_skip   = true;  /**< Skip the SSR denoise kernel where its whole footprint is zero (exact). */
    /**
     * Scale on the GGX lobe the reflection rays are importance-sampled from (visible-normal
     * sampling, Heitz 2018): 1 = the physical lobe for each pixel's roughness, smaller values
     * narrow it toward the mirror direction. 0 traces the single deterministic mirror ray. RUNTIME.
     */
    float ssr_jitter           = 1.0f;
    /**
     * GGX-sampled rays traced per SSR pixel and averaged before the temporal resolve (Unreal's
     * per-quality ray count). Each costs a full trace, so it scales ssr.trace linearly; the
     * resolve's accumulation already averages one ray a frame, so 1 is the right default.
     * Set by ssr_quality (Ultra 2). Clamped to [1, 8]. RUNTIME.
     */
    int   ssr_rays_per_pixel   = 1;
    /**
     * How much of the GGX cone the hit-colour lookup prefilters through the scene-colour mips,
     * on top of the ray footprint. With importance-sampled rays the lobe is already resolved by
     * the rays themselves and the temporal average, so the full cone would blur twice; 0.5 keeps
     * just enough prefiltering to hide the per-frame noise. 1 = the full-cone lookup. RUNTIME.
     */
    float ssr_cone_prefilter   = 0.5f;
    /**
     * Let a ray that has passed behind thin geometry at the finest Hi-Z level take growing
     * strides (2, 4, ... 16 texels) instead of crawling one texel per iteration until its
     * budget runs out. RUNTIME.
     */
    bool  ssr_skip_behind      = false;
    /**
     * Trace SSR and SSGI at half the render resolution per axis (a quarter of the rays); the
     * composite upsamples with a depth/normal-aware filter (gfx/ssr_composite_body.glsl), so
     * geometry edges stay sharp and only the reflections themselves soften slightly. Cuts the
     * trace, resolve and blur passes of both chains ~4x. Startup-fixed: it sizes targets.
     */
    bool  ssr_half_res         = true;
    /**
     * The traced-SSGI chain runs at the SSR trace resolution divided by this per axis (1 = the
     * same resolution, 2 = a quarter of its pixels). The bounce is low-frequency, so the lower
     * tiers trade a little edge definition in the bounce for a large cut in its trace, resolve
     * and denoise. Set by ssgi_quality (Low/Medium 2, High/Ultra 1). Startup-fixed: it sizes
     * targets.
     */
    int   ssgi_resolution_scale = 1;
    /**
     * Skip the SSR trace for pixels whose reflection could add almost nothing: the specular
     * weight the composite would apply (split-sum GGX, Fresnel) times the trace's own
     * roughness and direction fades, all known before marching, below ssr_skip_threshold.
     * On rough dielectrics (most terrain) that is nearly every pixel. SSGI is unaffected --
     * it has its own trace. false = trace every pixel the roughness cutoff admits. RUNTIME.
     */
    bool  ssr_skip_negligible  = true;
    float ssr_skip_threshold   = 0.02f; /**< Weight below which ssr_skip_negligible skips the trace. */
    float ssr_temporal_gamma   = 2.0f;  /**< Variance-clipping width for the SSR temporal resolve,
                                              in std deviations of the 3x3 neighbourhood. */
    /**
     * Traced SSGI: the composite's diffuse-bounce term reads a real cosine-hemisphere
     * Hi-Z trace (ssgi.frag, one ray per pixel per frame, temporally resolved through
     * the same variance-clipped resolve as SSR) instead of its single normal-offset
     * mip tap -- directional colour bleed rather than a uniform lift. STARTUP-FIXED
     * (builds a pipeline and binds the composite's u_ssgi_map descriptor); the
     * intensity dial stays `indirect.ssgi_intensity` either way, so 0 still disables
     * the whole term at runtime. Meaningless without ssr_enabled.
     */
    bool  ssgi_traced          = true;
    float ssgi_max_distance    = 8.0f;  /**< World-space ray length for the traced-SSGI march;
                                              the specular trace keeps its own ssr_max_distance. */
    /**
     * Hi-Z iteration budget for the traced-SSGI march, set by `ssgi_quality`. Its own knob
     * rather than a share of ssr_max_iterations, because the two marches want very different
     * budgets: an SSGI ray is short (ssgi_max_distance, ~8 m) and its hit feeds a wide
     * cone-mip lookup, so it converges in far fewer steps than a mirror reflection running
     * out to ssr_max_distance.
     */
    int   ssgi_max_iterations  = 32;
    /**
     * World-space sigma for the traced-SSGI spatial denoise. Wider than ssr_blur_radius
     * on purpose: one hemisphere ray per pixel is far noisier than a near-mirror ray, and
     * a diffuse bounce is low-frequency enough that a wide kernel costs it no real detail.
     * Too low and the image keeps visibly settling for several frames after the camera stops.
     */
    float ssgi_blur_radius     = 1.0f;
    bool  ssgi_blur_light      = true;  /**< 3x3 SSGI denoise footprint instead of 5x5 (a third of the reads). */

    /**
     * Additive glow from a dedicated BloomPass pyramid (bright-pass threshold -> multi-tap
     * downsample -> tent-filter upsample+combine -- see gfxcoopa's bloom_pass.h), sourced
     * from the FINAL pre-tonemap HDR image (after fog, translucency, underwater and
     * volumetrics) -- the same image pixel_stylize_pass_ itself reads, so SSR
     * reflections, transparent geometry and fog all bloom too.
     *
     * An independent pyramid with its own construction-fixed descriptors, deliberately not a
     * reuse of SSR's mip chain: sharing that would tie bloom's availability to SSR being on,
     * and its hard 2x2 `texelFetch` box downsample has no sub-texel interpolation, so the
     * sampled brightness pops discretely as the camera moves. Every kernel here is a smooth
     * multi-tap `texture()` read. Needing no per-frame rebind, it also costs no device wait.
     *
     * bloom_enabled is startup-fixed (the descriptor binding it controls is decided once at
     * construction); bloom_intensity <= 0 is a genuine per-frame no-op either way.
     */
    bool  bloom_enabled   = false;
    float bloom_threshold = 1.0f;   /**< Brightness (max3 of RGB) below which nothing glows. 1.0 = "brighter than white". */
    float bloom_soft_knee = 0.5f;   /**< 0..1 fraction of bloom_threshold the quadratic knee spans; 0 = hard cutoff (pops). */
    float bloom_intensity = 1.0f;   /**< Final multiplier on the composited glow; <= 0 disables. */
    float bloom_scatter   = 0.7f;   /**< Per-level blend toward the coarser tent; Unity URP's Bloom "Scatter". Higher = wider, softer halo. */
    float bloom_radius    = 1.0f;   /**< Tent-filter width multiplier on the upsample; > 1 widens further at a small cost in sharpness. */
    float bloom_clamp     = 20.0f;  /**< Per-tap HDR ceiling applied before thresholding; suppresses single-texel fireflies. */

    // --- Fog (gfx/fog.glsl; gfxcoopa's FogPass for the opaque scene) ---
    // Exponential height fog: `fog_density` extinction per metre at and below fog_height_base,
    // thinning as exp(-(z - base) / fog_height_falloff) above it (falloff <= 0: uniform fog).
    // Transmittance over a view ray is exp(-optical depth), integrated in closed form.
    int       fog_mode             = 1;      /**< 1 Exponential (height fog), 0 Linear (legacy distance ramp). A legacy
                                                  2 (Exp2) loads as 1. */
    float     fog_density          = 0.02f;  /**< Extinction per metre at/below the height base. */
    float     fog_linear_start     = 5.0f;   /**< Linear mode only: distance fog starts. */
    float     fog_linear_end       = 60.0f;  /**< Linear mode only: distance fog reaches max_opacity. */
    glm::vec3 fog_color            = glm::vec3(0.55f, 0.62f, 0.72f); /**< In-scatter colour. */
    float     fog_height_base      = 0.0f;   /**< World Z (engine is Z-up) of full density; constant below it. */
    float     fog_height_falloff   = 0.0f;   /**< Metres over which density drops by e above the base; <= 0 = uniform. */
    float     fog_sky_blend        = 0.0f;   /**< 0 = flat fog_color, 1 = fully blended toward the sky gradient. */
    float     fog_sun_amount       = 0.0f;   /**< Directional (Henyey-Greenstein) sun in-scatter strength; 0 disables. */
    float     fog_sun_anisotropy   = 0.7f;   /**< HG g; 0 isotropic, close to 1 = tight forward scatter toward the sun. */
    float     fog_sun_start_distance = 0.0f; /**< Distance the directional sun in-scatter starts at. */
    float     fog_max_opacity      = 1.0f;   /**< Ceiling on how much fog can occlude the scene (1 = fully opaque). */
    float     fog_start_distance   = 0.0f;   /**< No fog nearer than this. */
    float     fog_cutoff_distance  = 0.0f;   /**< Fog stops accumulating past this distance; 0 = no cutoff. */
    float     fog_sky_distance     = 1000.0f;/**< Distance sky pixels integrate to. With height fog the horizon fogs
                                                  and the zenith stays clear on its own. */

    /**
     * Lying snow (0..1) every opaque surface shows on its open, up-facing parts -- see
     * gfx/surface/snow.glsl. Negative (the default): the scene's weather decides
     * (WeatherState::snow_cover, none without weather). 0..1 forces it, for scenes without
     * weather and for tests. Runtime.
     */
    float     snow_cover_override = -1.0f;

    // --- Volumetrics (raymarched LOCAL volumes; see gfxcoopa's VolumetricsPass) ---
    // Only genuinely SHARED march settings live here. Everything about how a volume
    // looks -- density, noise, advection, colour -- is per-volume and lives on
    // VolumeComponent in the scene, because volumes are local by definition.
    int   volumetrics_step_count     = 48;    /**< Raymarch steps. The primary perf knob, and what resolves
                                                thin ribbons; the start offset is dithered per pixel and per
                                                frame, so TAA recovers much of what a low count costs. */
    float volumetrics_max_distance   = 40.0f; /**< Distance the march stops at. */
    /**
     * Divisor on render resolution for the march (1 = full, 2 = half, 4 = quarter per
     * axis). The march is the expensive half of VolumetricsPass and a low-frequency medium
     * loses little at reduced resolution: the full-resolution composite upsamples it with
     * a depth-aware filter, so geometry silhouettes stay sharp through the haze. Each step
     * of the divisor cuts march cost ~4x. Startup-fixed: it sizes a target.
     */
    uint32_t volumetrics_resolution_scale = 2;
    /**
     * How the local volumes are resolved. Startup-fixed (it selects passes and targets):
     *   "froxel"   -- the Unreal/HDRP technique: density and lighting evaluated once per froxel
     *                 of a camera-aligned grid, temporally reprojected, integrated per column,
     *                 one lookup per pixel (gfxcoopa's FroxelVolumetricsPass). Cost follows the
     *                 grid, not the screen or a step count; thin features soften to the grid.
     *   "raymarch" -- per-pixel march at 1/volumetrics_resolution_scale resolution with
     *                 volumetrics_step_count steps (VolumetricsPass). Sharper, much costlier.
     */
    std::string volumetrics_mode = "froxel";
    uint32_t volumetrics_froxel_tile   = 8;    /**< Render pixels per froxel, each axis. Startup-fixed. */
    uint32_t volumetrics_froxel_slices = 64;   /**< Depth slices along the view ray. Startup-fixed. */
    float    volumetrics_froxel_history = 0.9f; /**< Temporal reprojection weight (0 = none). RUNTIME. */
    /** Samples taken by a froxel whose history was rejected (newly revealed by camera motion),
     *  so it does not start as one raw sample. 1 = no supersampling. RUNTIME. */
    uint32_t volumetrics_froxel_miss_samples = 4;
    /** Per-pixel jitter of the composite's grid lookup, in froxels / slices (0 = off). Breaks
     *  up the froxel cell pattern for TAA to resolve; ignored unless aa_mode == "taa". Off by
     *  default: with centre-reprojected history the cells do not show in pixel_demo, and
     *  the dither TAA leaves behind measured as slightly MORE flicker. RUNTIME. */
    float    volumetrics_froxel_lookup_jitter = 0.0f;
    float volumetrics_max_opacity    = 0.85f; /**< Ceiling on how much volumetrics can occlude the scene. */
    float volumetrics_sun_anisotropy = 0.6f;  /**< HG g; 0 isotropic, close to 1 = tight forward scatter.
                                                Shared, not per-volume: it is a property of the light's
                                                phase function, not of which medium a sample sits in. */
    /**
     * Shadow the march's sun in-scatter term with the directional shadow map (one
     * hardware-PCF tap per step), producing visible light shafts where geometry
     * occludes a volume. A RUNTIME toggle -- it only gates a UBO flag; the shadow
     * maps themselves are always bound (VolumetricsPass::set_shadow_images). The
     * GLOBAL fog term stays analytic and unshadowed by design: it has no march to
     * sample along, so god rays are a volumetrics feature, not a fog one.
     */
    bool  volumetrics_shadows_enabled = true;
    /**
     * Strength of point/spot light in-scatter into local volumes (the closest
     * MAX_SCATTER_LIGHTS lights are fed to the march each frame); 0 skips the
     * per-step light loop entirely. Each volume's own `sun_amount` scales its
     * response to these lights exactly as it scales its response to the sun.
     */
    float volumetrics_light_scatter  = 1.0f;
    /**
     * How many point/spot lights may in-scatter into the march, set by
     * `volumetrics_quality` and capped at MAX_SCATTER_LIGHTS (4). The nearest to the camera
     * win. This multiplies against volumetrics_step_count -- every light is evaluated at
     * every step of every pixel -- so it is the second real cost dial of the pass, and 0
     * leaves the (much cheaper) sun-shaft term running on its own.
     */
    int   volumetrics_max_scatter_lights = 4;

    // --- Mesh visibility (frustum culling, batching and LOD; see toyengine/render/visibility.h) ---
    /**
     * Global multiplier on every renderer's projected screen size before LOD selection:
     * > 1 keeps detail longer, < 1 switches to coarser levels sooner. Meshes without a
     * `lods` block are unaffected (they have only LOD 0). RUNTIME.
     */
    float mesh_lod_bias = 1.0f;
    /**
     * A shadow caster whose bounds would cover fewer than this many texels of a shadow view
     * (a cascade tile, a cube face, the spot map) is skipped in that view -- tiny props in a
     * far cascade contribute nothing a filter could resolve. 0 disables. RUNTIME.
     */
    float shadow_min_caster_texels = 1.0f;

    /**
     * Diorama-style tilt-shift blur (Zelda: Link's Awakening [Switch] reference) -- see
     * gfxcoopa's TiltShiftPass. Unlike every other post effect here, it runs at DISPLAY
     * resolution, after the pixel-art upscale: it's a lens effect layered on the final
     * image, not a pixel-grid effect, so running it at the low internal resolution would
     * quantize the blur kernel and the focus ramp to a handful of steps (see
     * TiltShiftPass's own file doc). The circle of confusion is a function of screen
     * position only (no depth sampling), so it stays exact under a separable
     * horizontal-then-vertical Gaussian with no silhouette bleeding.
     *
     * tilt_shift_enabled is a startup-fixed toggle (same policy as bloom_enabled/
     * fog_enabled): its descriptor binding -- whether upscale_pass_ reads
     * tilt_shift_pass_'s result or post_target_ directly -- is decided once at
     * construction from this flag's startup value.
     */
    bool  tilt_shift_enabled      = false;
    float tilt_shift_focus_center = 0.55f; /**< 0..1 screen position of the sharp band's centre (0 = top, at angle 0). */
    float tilt_shift_focus_width  = 0.18f; /**< 0..1 half-height of the fully-sharp band. */
    float tilt_shift_ramp_width   = 0.22f; /**< 0..1 distance the blur ramps in over, smoothstepped. */
    float tilt_shift_blur_top     = 1.0f;  /**< Strength multiplier on the "far" side of the band. */
    float tilt_shift_blur_bottom  = 0.7f;  /**< Strength multiplier on the "near" side of the band. */
    float tilt_shift_max_radius   = 6.0f;  /**< Blur radius in DISPLAY pixels at full strength. */
    float tilt_shift_angle        = 0.0f;  /**< Degrees; rotates the focus band off horizontal. */

    /**
     * Physically-based depth of field -- see gfxcoopa's DofPass. Unlike
     * tilt_shift above, this runs at RENDER resolution, on linear HDR, before
     * bloom_enabled's pyramid: the circle of confusion is a function of scene
     * DEPTH (a thin-lens formula from focal length/aperture/focus distance), and
     * defocused HDR highlights should bloom into real bokeh rather than DOF
     * blurring an already-tonemapped image. See DofPass's own file doc for why a
     * depth-driven CoC needs a single-pass gather rather than tilt_shift's
     * separable blur.
     *
     * dof_focal_length/dof_sensor_width <= 0 fall back to the active camera's
     * `lens`/`sensor_width` (CameraComponent) -- and a camera's own `aperture`/
     * `focus_distance` (also <= 0 = inherit) override dof_aperture/
     * dof_focus_distance below, so a scene can DOF one camera differently from
     * another without touching this global config.
     *
     * dof_focus_mode == "object" focuses on a named scene object instead of a fixed
     * distance: dof_focus_object gives its ':'-separated scene path (e.g.
     * "sdf_blob:sdf_blob_sphere", resolved via Scene::find_object_by_path()), and the
     * view-space depth of its Transform each frame becomes the focal plane. A camera's
     * own CameraComponent::focus_object (see that class's doc) overrides this field AND
     * self-activates object-focus mode for that camera even when dof_focus_mode here is
     * "manual" -- see PixelRenderPipeline::resolve_dof_focus_() for the exact precedence.
     * dof_focus_smoothing eases the focal plane toward a moving/retargeted object
     * (1/sec, like CameraController::follow_smoothing; <= 0 snaps instead) -- it applies
     * ONLY to object-focus mode, not orbit_target, which is already smoothed twice over
     * by CameraController's own follow_smoothing/movement_smoothing.
     *
     * dof_enabled is a startup-fixed toggle (same policy as bloom_enabled/
     * fog_enabled/tilt_shift_enabled above): its descriptor binding -- whether
     * bloom_pass_/pixel_stylize_pass_ read dof_pass_'s result or the pre-DOF
     * image directly -- is decided once at construction from this flag's
     * startup value. dof_focus_mode/object/smoothing (and debug_view, see that
     * enum's doc) are RUNTIME fields: they only change push constants (or which
     * scene object CPU code reads), not a descriptor binding, so they're safe to
     * change every frame.
     */
    bool        dof_enabled         = false;
    std::string dof_focus_mode      = "manual";  /**< manual | orbit_target | object. */
    std::string dof_focus_object    = "";        /**< ':'-separated scene path; used when dof_focus_mode == "object". Overridden by CameraComponent::focus_object. */
    float       dof_focus_smoothing = 8.0f;      /**< Focus-rack rate (1/sec) for object-focus mode; <= 0 snaps. */
    float       dof_focus_distance  = 8.0f;      /**< Metres; used when dof_focus_mode == "manual". */
    /** Extra forced-sharp half-depth in metres, added around the focal plane in EVERY focus
     *  mode. Deliberately non-physical: the thin-lens sharp band goes as F^2, so it collapses
     *  when the camera closes in and no aperture compensates -- see DofPass::Params::focus_range. */
    float       dof_focus_range     = 0.0f;
    /** Object focus also fits the forced-sharp band to the subject's own depth extent, so the
     *  whole subject stays sharp at any distance. See resolve_dof_focus_(). */
    bool        dof_focus_cover_object = true;
    /** Plain |CoC| multiplier (blur strength), applied before the dof_max_radius clamp.
     *  Separate from dof_focus_range (band WIDTH) and dof_max_radius (a safety ceiling). */
    float       dof_blur_scale      = 1.0f;
    float       dof_aperture        = 2.8f;      /**< f-stop; lower = shallower depth of field. */
    float       dof_focal_length    = 0.0f;      /**< mm; <= 0 takes the active camera's `lens`. */
    float       dof_sensor_width    = 0.0f;      /**< mm; <= 0 takes the active camera's `sensor_width`. */
    float       dof_max_radius      = 12.0f;     /**< |CoC| ceiling, in full-res pixels. */
    int         dof_sample_count    = 32;        /**< Spiral gather taps; clamped to [8, 48] in-shader. */
    int         dof_blade_count     = 0;         /**< < 3 = perfect disc bokeh; else an N-sided polygonal iris. */
    float       dof_blade_rotation  = 0.0f;      /**< Iris rotation, degrees. */

    /**
     * Velocity-buffer motion blur -- see toyengine/render/passes/motion_blur_pass.h. Runs on the
     * linear HDR image after fog/volumetrics and before DoF and bloom, so blurred highlights still
     * bloom; camera and object motion both come from the G-buffer velocity (G4), the sky's from the
     * camera alone. A camera opts out with CameraComponent::motion_blur = false (the editor's
     * viewport camera does).
     *
     * RUNTIME, every field: the pass is always built and blurs in place into the image downstream
     * passes already read, so `motion_blur` only decides whether it is recorded. Off records
     * nothing -- zero cost and a byte-identical frame.
     */
    bool  motion_blur           = false;
    float motion_blur_intensity = 0.5f;   /**< Shutter fraction of the frame interval (0.5 = a 180-degree shutter); 0 disables. */

    /**
     * Anti-aliasing. Three modes; MSAA is deliberately absent, since every target here is
     * SampleCount::X1.
     *
     * aa_mode is STARTUP-FIXED: going from "off" to any AA mode (or back) changes which
     * descriptor the composite and tilt shift are bound to, and that is decided once at
     * pipeline construction. Switching AMONG "fxaa"/"smaa"/"taa" at runtime IS safe -- all
     * three passes are constructed together whenever aa_mode != "off" and all three write the
     * same target, so render() just branches per frame on which draw() to call.
     *
     * "taa" jitters the camera projection every frame, which is in inherent tension with
     * camera_pixel_snap's whole-texel snapping above -- not a bug to fix. The TAA resolve
     * reprojects its accumulation history through the camera's frame-to-frame motion (from
     * the depth buffer and the previous unjittered view-projection; camera-only, there are
     * no per-object motion vectors) and variance-clips it in YCoCg against the current 3x3
     * neighbourhood, so dynamic objects shed ghosts through the clip rather than through
     * true velocities. Accumulation is age-weighted: a still camera converges as a true
     * running average of the jitter cycle rather than orbiting an exponential blend.
     */
    std::string aa_mode = "off"; /**< "off" | "fxaa" | "smaa" | "taa". */
    float fxaa_subpixel           = 0.75f;   /**< Blend weight of FXAA's subpixel-aliasing term. */
    float fxaa_edge_threshold     = 0.166f;  /**< Local contrast (fraction of lumaMax) below which FXAA does nothing. */
    float fxaa_edge_threshold_min = 0.0312f; /**< Absolute contrast floor -- avoids AA-ing near-black noise. */
    float smaa_threshold          = 0.1f;    /**< SMAA edge-detection local contrast threshold. */
    int   smaa_max_search_steps   = 16;      /**< SMAA blend-weight pass's max horizontal/vertical search distance, in texels. */
    float taa_blending_weight     = 0.99f;   /**< History weight TAA's accumulation converges to at rest. */
    /** Velocity response: TAA's history weight falls from taa_blending_weight toward
     *  taa_feedback_motion, reaching the floor at ~100/taa_weight_scale pixels of per-frame
     *  screen velocity. */
    float taa_weight_scale        = 30.0f;
    float taa_feedback_motion     = 0.85f;   /**< History weight floor under fast camera motion. */
    float taa_sharpness           = 0.25f;   /**< Motion-gated high-frequency restore in the resolve; 0 disables. */
    float taa_variance_gamma      = 1.0f;    /**< History clip box half-width, in standard deviations of the 3x3 YCoCg neighbourhood. */

    /**
     * @brief Replaces the image with one intermediate render buffer, or (debug_view ==
     *        "lines") overlays physics collider/contact wireframes (DebugLinePass, gathered
     *        each frame from PhysicsWorld::debug_draw() -- see
     *        toyengine/render/passes/debug_line_pass.h) on top of the normal image.
     *
     * See the DebugView enum's own doc for the full option list and what each one shows.
     * RUNTIME, like ssr_enabled: PixelRenderPipeline's debug_view_pass_ (and every pass
     * this reduces to a no-op) is always constructed, so re-reading this fresh every frame
     * changes nothing about which descriptors exist -- only which fullscreen draw runs and
     * which push-constant fields are zeroed.
     */
    std::string debug_view = "off";

    /**
     * @brief Where SkinnedMeshRenderer skins: "gpu" (a compute pre-pass, passes/skinning_pass.h)
     *        or "cpu" (the per-vertex CPU loop + upload). "gpu" falls back to "cpu" on a device
     *        without compute. STARTUP-FIXED.
     */
    std::string skinning = "gpu";

    /**
     * @brief Draws every WorldSpace uicoopa CanvasComponent in the scene (UiWorldPass) as a
     *        guest inside post_target_'s bracket -- see uicoopa/render/ui_world_pass.h and
     *        CanvasRenderMode.
     *
     * STARTUP-FIXED, unlike debug_view above: turning it on builds the UI pipelines
     * and binds the G-buffer depth into a descriptor set, neither of which can happen once
     * the frame loop is running (DescriptorSet::bind_image() updates descriptors
     * immediately). Off leaves the pass unconstructed and the descriptor unallocated, so it
     * costs literally nothing. See PixelRenderPipeline::apply_live_config().
     *
     * Screen-space canvases are unaffected by this flag -- they are drawn by a separate pass
     * with its own toggle, screen_ui_enabled below.
     */
    bool  world_ui_enabled        = true;

    /**
     * @brief Draws ScreenSpaceOverlay canvases (uicoopa's UiPass), at WINDOW resolution.
     *
     * The companion to world_ui_enabled, and STARTUP-FIXED for the same reason: it decides
     * whether the pass and its descriptor pool are built at all.
     *
     * Unlike the world-space layer, this one is not part of the pixel-art image: it draws
     * last, straight into the swapchain-sized overlay target, at full window resolution, so
     * an HUD stays crisp instead of being quantised to the internal render grid. Both UI
     * layers land after every post effect -- see PixelRenderPipeline's ui_composite_pass_.
     */
    bool  screen_ui_enabled       = true;

    // Indirect-lighting terms shared with SsrPass::Params (gfxcoopa/engine/render_features.h) --
    // fed to both the lighting pass and the SSR composite from this single instance so the two
    // can never disagree. ssgi_intensity defaults to 0.6 here (nonzero -- SSGI on by default),
    // overriding IndirectParams' own 0.0 "no consumer has this concept" default.
    coopa::gfx::engine::IndirectParams indirect{1.0f, 1.0f, 0.6f, 0.5f};

    // --- Sky model ---
    /**
     * "gradient" (default): the three-colour sky gradient above (indirect.sky_*), drawn as is.
     * "physical": a physically based atmosphere (Hillaire 2020 -- transmittance, multiple
     * scattering and sky-view tables, see passes/sky_atmosphere_pass.h) with a sun disc, a moon,
     * stars at night and, with `clouds`, a raymarched cloud layer. The sky follows the scene's
     * directional light (or the weather's sun), and the engine derives sky_zenith / horizon /
     * ground and the sun's colour from the same atmosphere, so ambient light, reflections, fog
     * and water agree with the sky drawn. Runtime: switches live.
     */
    std::string sky_model = "gradient";
    float atmosphere_density = 1.0f;   /**< Haze (aerosol amount): 1 = a clear day, higher = hazier, whiter, redder sunsets. */
    float ozone = 1.0f;                /**< Ozone layer amount: deepens the blue of twilight skies. */
    float sun_disc_size = 0.53f;       /**< The sun disc's angular diameter, degrees (0 hides it). */
    float moon_disc_size = 0.6f;       /**< The moon disc's angular diameter, degrees (0 hides it). */
    bool  sky_stars = true;            /**< Stars fade in as the sky darkens (physical sky only). */
    /** A raymarched cloud layer over the physical sky (needs sky_model: physical). Runtime. */
    bool  clouds = false;
    float cloud_coverage = 0.4f;       /**< 0 = clear .. 1 = overcast. The weather drives it while it is on. */
    float cloud_altitude = 1500.0f;    /**< Height of the layer's base, metres. */
    float cloud_thickness = 1500.0f;   /**< The layer's depth, metres. */
    float cloud_density = 1.0f;        /**< Extinction multiplier: higher = darker, more solid clouds. */
    float cloud_wind_speed = 8.0f;     /**< m/s the clouds drift (along the weather's wind when it is on, else along +X). */

    // --- Topdown mode ---
    /**
     * A toon cloud layer for topdown games: flat-shaded puffy clouds floating at a fixed world
     * height between a zoomed-out camera and the ground, fading in as the camera rises, with
     * their shadows drifting over the ground. Works with either sky model; the weather drives
     * its coverage (cloud_coverage) and drift (cloud_wind_speed). Runtime.
     */
    bool  topdown_mode = false;
    float topdown_cloud_height = 60.0f;       /**< World height (z) of the layer, metres. */
    float topdown_cloud_size = 30.0f;         /**< Typical diameter of one puff, metres. */
    float topdown_cloud_thickness = 8.0f;     /**< How tall the puffs stand, metres. */
    float topdown_cloud_opacity = 0.92f;      /**< 0..1. */
    float topdown_fade_start = 15.0f;         /**< Camera height above the layer where the clouds start to show, metres. */
    float topdown_fade_end = 60.0f;           /**< Camera height above the layer where they are fully shown, metres. */
    float topdown_shadow_strength = 0.45f;    /**< How much a cloud's shadow darkens the ground (0 = no shadows). */
    float topdown_light_bands = 3.0f;         /**< Toon shading steps (0 = smooth shading). */
    float topdown_outline = 0.5f;             /**< Darkening of each cloud's rim, 0..1. */

    std::string shader_dir;                   /**< Absolute path to assets/shaders. */
    // Ordered search path resolving a logical shader name (e.g. "gbuffer.vert") to a compiled
    // .spv path -- the project's, the engine's, gfxcoopa's then uicoopa's shader directories
    // (see RuntimeLayout::shader_roots). Set
    // alongside shader_dir (see engine.h's make_render_config_); shader_dir is kept for
    // logging/debugging, `shaders` is what every pass construction actually resolves through.
    coopa::gfx::pipeline::ShaderLibrary shaders;

    // Derived surface shaders a scene's materials may select by name (PBRMaterial::shader) --
    // see gfxcoopa/pipeline/surface_shader.h. Populated alongside
    // `shaders` in engine.h's make_render_config_(); empty by default, so a scene that never
    // references a custom shader behaves exactly as if this field didn't exist. Every entry's
    // logical shader names are resolved through `shaders` above at pass-construction time (see
    // PixelRenderPipeline's ctor), the same search every stock entry point goes through.
    coopa::gfx::pipeline::SurfaceShaderRegistry surface_shaders;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H
