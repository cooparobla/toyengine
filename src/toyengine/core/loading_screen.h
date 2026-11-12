/**
 * @file loading_screen.h
 * @brief The built-in loading mark: toyengine's block, drawn as flat triangles, its three
 *        faces cycling brightness in rotation so it reads as the block turning.
 */

#ifndef TOYENGINE_CORE_LOADING_SCREEN_H
#define TOYENGINE_CORE_LOADING_SCREEN_H

#include <glm/glm.hpp>

#include <uicoopa/render/draw_list.h>

#include <toyengine/core/loading_config.h>

namespace toy {
namespace core {

/**
 * @brief Emits the loading mark's geometry into `dl`. Draws nothing if `style.spinner` is
 *        false -- a project that wants to own the whole screen still gets the background fill
 *        a transition already draws, just not this.
 *
 * Pure: no device, no text, no state of its own -- the same discipline as emit_boot_splash().
 * Canvas space is uicoopa's (origin bottom-left, +Y up); `dl` must already be begun by the
 * caller, since a transition's loading screen shares a DrawList already open for its own fade
 * fill. The mark scales with canvas height, so it reads the same size at 720p and at 4K.
 *
 * @param dl     Target draw list, already begun, with the texture the caller wants flat
 *               triangles to sample (the default white texture leaves them flat-coloured).
 * @param canvas Target size in pixels.
 * @param style  Whether to draw at all, the tint, and which corner.
 * @param time   Seconds since the screen appeared, driving the face-cycle animation.
 */
void emit_loading_screen(coopa::ui::DrawList& dl, glm::vec2 canvas, const LoadingConfig& style,
                         float time);

}  // namespace core
}  // namespace toy

#endif  // TOYENGINE_CORE_LOADING_SCREEN_H
