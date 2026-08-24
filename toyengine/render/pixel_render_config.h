/**
 * @file pixel_render_config.h
 * @brief Tunable parameters for the pixel-art render pipeline.
 */

#ifndef TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H
#define TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H

#include <cstdint>
#include <string>

#include <glm/glm.hpp>

namespace toy {
namespace render {

/**
 * @struct PixelRenderConfig
 * @brief Configuration for PixelRenderPipeline: internal resolution, upscaling,
 *        banded lighting, shadows, and the pixel-art post-process stack
 *        (outline, palette quantization, ordered dithering), plus the
 *        independently-toggleable soft shadows / SSAO / SSR add-ons.
 */
struct PixelRenderConfig {
    // --- Feature toggles ---
    bool outline_enabled   = true;
    bool palette_enabled   = true;  /**< Independent of palette_path, so toggling off keeps the configured path. */
    bool dither_enabled    = true;  /**< Independent of dither_strength, so toggling off keeps the configured strength. */
    bool camera_pixel_snap = true;  /**< Orthographic cameras only. */
    bool soft_shadows      = true;  /**< Rotated-Poisson PCF instead of a single hard compare. */
    bool ssao_enabled      = true;
    bool ssr_enabled       = true;  /**< Also gates the SSGI diffuse-bounce term (ssgi_intensity). */

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
    float ambient_intensity = 1.0f;   /**< Scales sky_gradient(N) indirect diffuse. */
    float sky_intensity     = 1.0f;   /**< Scales the sky-gradient indirect specular base. */

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
    float ssgi_intensity       = 0.6f;  /**< Diffuse colour-bleed strength; 0 disables the bounce term. */
    float ssgi_distance        = 0.5f;  /**< World-space offset along N for the bounce sample point. */

    std::string shader_dir;                   /**< Absolute path to assets/shaders. */
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H
