/**
 * @file canvas_scaler.h
 * @brief Screen-size-independent UI scaling, analogous to Unity's CanvasScaler.
 */

#ifndef UICOOPA_LAYOUT_CANVAS_SCALER_H
#define UICOOPA_LAYOUT_CANVAS_SCALER_H

#include <glm/glm.hpp>
#include <cmath>

namespace coopa {
namespace ui {

/**
 * @enum ScaleMode
 * @brief How CanvasScaler derives the canvas's scale factor from the screen size.
 */
enum class ScaleMode {
    ConstantPixelSize,   ///< scale_factor is used directly, regardless of screen size.
    ScaleWithScreenSize, ///< Scale factor derived from screen size vs. reference_resolution.
    ConstantPhysicalSize, ///< Scale factor derived from DPI, so UI occupies a fixed physical size.
};

/**
 * @class CanvasScaler
 * @brief Computes a canvas's design-to-pixel scale factor for one of three modes.
 *
 * Usage:
 * @code
 * CanvasScaler scaler;
 * scaler.mode = ScaleMode::ScaleWithScreenSize;
 * scaler.reference_resolution = {1920.0f, 1080.0f};
 * scaler.match_width_or_height = 0.5f;
 * scaler.user_scale = 1.25f;   // "Interface scale: 125%"
 * float scale = scaler.compute_scale_factor(screen_w, screen_h);
 * @endcode
 */
class CanvasScaler {
public:
    /** @brief DPI at which compute_scale_factor() returns 1.0 in ConstantPhysicalSize mode. */
    static constexpr float kReferenceDpi = 96.0f;

    ScaleMode mode = ScaleMode::ConstantPixelSize;

    float     scale_factor = 1.0f;                     /**< Used directly in ConstantPixelSize mode. */
    glm::vec2 reference_resolution{1920.0f, 1080.0f};   /**< Design resolution for ScaleWithScreenSize mode. */
    float     match_width_or_height = 0.0f;             /**< 0 = match width, 1 = match height, in between = log-blend. */
    float     fallback_dpi = 96.0f;                     /**< Used in ConstantPhysicalSize mode when actual_dpi <= 0. */

    /**
     * @brief A flat multiplier on whichever mode's result, for "make the interface bigger".
     *
     * It multiplies the scale factor rather than dividing reference_resolution, so it reads the
     * same in all three modes and composes with a player-facing scale the host drives
     * separately (CanvasComponent::set_ui_scale()). The canvas spans screen / scale_factor
     * canvas pixels, so 1.25 here means a canvas 20% smaller in authored units -- every widget
     * keeps its authored size but covers 25% more of the screen, and anchored edges stay on
     * their edges. <= 0 is ignored.
     */
    float     user_scale = 1.0f;

    /**
     * @brief Computes the canvas scale factor for the given screen size.
     *
     * @param screen_w Framebuffer width in pixels.
     * @param screen_h Framebuffer height in pixels.
     * @param actual_dpi Measured monitor DPI, if known; falls back to fallback_dpi otherwise.
     *                    Only used in ConstantPhysicalSize mode.
     * @return The scale factor: canvas pixel space = screen pixels / scale factor.
     */
    float compute_scale_factor(uint32_t screen_w, uint32_t screen_h, float actual_dpi = 0.0f) const {
        return mode_scale_factor_(screen_w, screen_h, actual_dpi) * (user_scale > 0.0f ? user_scale : 1.0f);
    }

private:
    /** @brief The scale factor `mode` alone implies, before user_scale. */
    float mode_scale_factor_(uint32_t screen_w, uint32_t screen_h, float actual_dpi) const {
        switch (mode) {
            case ScaleMode::ConstantPixelSize:
                return scale_factor;

            case ScaleMode::ScaleWithScreenSize: {
                if (screen_w == 0 || screen_h == 0 ||
                    reference_resolution.x <= 0.0f || reference_resolution.y <= 0.0f) {
                    return 1.0f;
                }
                float log_width  = std::log2(static_cast<float>(screen_w) / reference_resolution.x);
                float log_height = std::log2(static_cast<float>(screen_h) / reference_resolution.y);
                float log_avg = log_width + (log_height - log_width) * match_width_or_height;
                return std::pow(2.0f, log_avg);
            }

            case ScaleMode::ConstantPhysicalSize: {
                float dpi = actual_dpi > 0.0f ? actual_dpi : fallback_dpi;
                return dpi > 0.0f ? (dpi / kReferenceDpi) : 1.0f;
            }
        }
        return 1.0f;
    }
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_LAYOUT_CANVAS_SCALER_H
