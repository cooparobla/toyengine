/**
 * @file pixel_render_config.h
 * @brief Tunable parameters for the pixel-art render pipeline.
 */

#ifndef TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H
#define TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include <gfxcoopa/pipeline/shader_library.h>
#include <gfxcoopa/engine/render_features.h>

namespace toy {
namespace render {

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
     * Opaque surfaces (e.g. the floor) also reflect transparent geometry, via a second forward
     * capture of BLEND objects (depth/normal/position/shaded-color) and a second Hi-Z pyramid
     * ssr.frag's raymarch tries alongside its primary opaque source -- see
     * PixelRenderPipeline::record_transparent_capture_(). Meaningless without ssr_enabled AND
     * transparency_enabled also true. Opt-in (default false), not implied by those two: this
     * roughly doubles the opaque raymarch's per-pixel cost and adds an extra forward draw +
     * Hi-Z build + scene-colour-mip build every frame it's on.
     */
    bool ssr_reflect_transparent = false;

    // --- Internal resolution ---
    std::string resolution_mode = "fixed";   /**< "fixed" or "divisor". */
    uint32_t    render_width    = 480;       /**< Used when resolution_mode == "fixed". */
    uint32_t    render_height   = 270;       /**< Used when resolution_mode == "fixed". */
    uint32_t    scale_divisor   = 4;         /**< Used when resolution_mode == "divisor". */
    std::string upscale_mode    = "integer"; /**< "integer" (letterboxed) or "stretch". */

    // --- Lighting ---
    float exposure          = 1.0f;
    float light_bands       = 4.0f;   /**< Discrete shading steps per light; <= 1 disables banding. */
    float spec_threshold    = 0.55f;  /**< Hard specular highlight cutoff. */
    float rim_strength      = 0.0f;   /**< 0 disables the rim term. */
    // ambient_intensity/sky_intensity/ssgi_intensity/ssgi_distance live in `indirect` below
    // (shared with SsrPass::Params so the two can never disagree -- see render_features.h).

    // --- Shadows ---
    bool     shadows_enabled        = true;
    uint32_t shadow_map_resolution  = 2048;
    uint32_t cube_shadow_resolution = 512;
    float    shadow_bias            = 0.005f;
    float    shadow_max_extent      = 0.0f;   /**< Ceiling on the fitted ortho box's half-extent, in world units; <= 0 disables. */

    // --- Outline ---
    float     outline_thickness = 1.0f;     /**< In low-resolution texels. */
    glm::vec4 outline_color     = glm::vec4(0.05f, 0.04f, 0.08f, 1.0f);
    float     depth_threshold   = 0.02f;    /**< Relative depth step (fraction of the fragment's own view-space distance) that counts as an edge; scale-invariant, so one value works at any camera distance. */
    float     normal_threshold  = 0.75f;

    // --- Palette ---
    std::string palette_path;                 /**< Empty disables palette quantization. */

    // --- Dither ---
    float dither_strength = 0.0f;             /**< 0 disables ordered dithering. */

    // --- SSAO ---
    float ssao_radius           = 0.5f;
    float ssao_bias             = 0.025f;
    float ssao_power            = 1.5f;
    int   ssao_kernel_size      = 24;
    bool  ssao_temporal_enabled = true;
    float ssao_temporal_blend   = 0.85f;

    // --- SSR + SSGI ---
    float ssr_max_distance     = 15.0f;
    int   ssr_max_iterations   = 64;
    float ssr_thickness        = 0.05f;
    float ssr_thickness_scale  = 0.01f;
    float ssr_bias_texels      = 3.5f;
    float ssr_roughness_cutoff = 1.0f;  /**< 1.0 so rough surfaces still trace and feed the SSGI bounce. */
    int   ssr_start_mip        = 0;
    int   ssr_min_mip0_steps   = 1;
    bool  ssr_temporal_enabled = true;
    float ssr_temporal_blend   = 0.85f;

    // Indirect-lighting terms shared with SsrPass::Params (gfxcoopa/engine/render_features.h) --
    // fed to both the lighting pass and the SSR composite from this single instance so the two
    // can never disagree. ssgi_intensity defaults to 0.6 here (nonzero -- SSGI on by default),
    // overriding IndirectParams' own 0.0 "no consumer has this concept" default.
    coopa::gfx::engine::IndirectParams indirect{1.0f, 1.0f, 0.6f, 0.5f};

    std::string shader_dir;                   /**< Absolute path to assets/shaders. */
    // Ordered search path resolving a logical shader name (e.g. "gbuffer.vert") to a compiled
    // .spv path -- this app's own directory first, then gfxcoopa's shared base library. Set
    // alongside shader_dir (see engine.h's make_render_config_); shader_dir is kept for
    // logging/debugging, `shaders` is what every pass construction actually resolves through.
    coopa::gfx::pipeline::ShaderLibrary shaders;
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H
