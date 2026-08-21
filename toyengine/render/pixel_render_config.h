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
 *        (outline, palette quantization, ordered dithering).
 */
struct PixelRenderConfig {
    // --- Internal resolution ---
    std::string resolution_mode = "fixed";   /**< "fixed" or "divisor". */
    uint32_t    render_width    = 480;       /**< Used when resolution_mode == "fixed". */
    uint32_t    render_height   = 270;       /**< Used when resolution_mode == "fixed". */
    uint32_t    scale_divisor   = 4;         /**< Used when resolution_mode == "divisor". */
    std::string upscale_mode    = "integer"; /**< "integer" (letterboxed) or "stretch". */

    // --- Lighting ---
    float    exposure               = 1.0f;
    float    light_bands            = 4.0f;   /**< Discrete shading steps per light. */
    float    spec_threshold         = 0.55f;  /**< Hard specular highlight cutoff. */
    float    rim_strength           = 0.0f;   /**< 0 disables the rim term. */
    bool     shadows_enabled        = true;
    uint32_t shadow_map_resolution  = 2048;
    uint32_t cube_shadow_resolution = 512;
    float    shadow_bias            = 0.005f;
    float    shadow_max_extent      = 0.0f;   /**< Ceiling on the fitted ortho box's half-extent, in world units; <= 0 disables. */

    // --- Pixel post ---
    bool        outline_enabled   = true;
    float       outline_thickness = 1.0f;     /**< In low-resolution texels. */
    glm::vec4   outline_color     = glm::vec4(0.05f, 0.04f, 0.08f, 1.0f);
    float       depth_threshold   = 0.02f;
    float       normal_threshold  = 0.75f;
    std::string palette_path;                 /**< Empty disables palette quantization. */
    bool        palette_enabled   = true;     /**< Independent of palette_path, so toggling off keeps the configured path. */
    float       dither_strength   = 0.0f;     /**< 0 disables ordered dithering. */
    bool        dither_enabled    = true;     /**< Independent of dither_strength, so toggling off keeps the configured strength. */
    bool        camera_pixel_snap = true;     /**< Orthographic cameras only. */

    std::string shader_dir;                   /**< Absolute path to assets/shaders. */
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PIXEL_RENDER_CONFIG_H
