/**
 * @file logo.h
 * @brief The toyengine logo (toyengine/core/branding.h) drawn as immediate-mode triangles --
 *        shared by the editor's top bar / About box and the project hub.
 */

#ifndef TOYEDITOR_UI_LOGO_H
#define TOYEDITOR_UI_LOGO_H

#include <toyengine/core/branding.h>

#include <uicoopa/immediate/imm.h>

#include <algorithm>
#include <cmath>

namespace toy::editor {

namespace imm = coopa::ui::imm;

/** @brief Draws the toyengine logo into `box` (kept square, centred). */
void draw_logo(imm::Context& ctx, const imm::Box& box);

} // namespace toy::editor

#endif // TOYEDITOR_UI_LOGO_H
