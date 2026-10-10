/**
 * @file ui_palette.h
 * @brief The UI designer's widget palette: every entry is a ready-made object (or component)
 *        as document YAML -- themed composites, wired-up primitives, layout containers.
 *
 * Primitive widgets that need children come pre-wired (a Slider with its Track / Fill /
 * Handle, a ProgressBar with Fill / Ghost / Label) so they work the moment they land. The
 * composites are one component each and take their look from the canvas's theme.
 */

#ifndef TOYEDITOR_UI_UI_PALETTE_H
#define TOYEDITOR_UI_UI_PALETTE_H

#include "../core/yaml_util.h"

#include <uicoopa/immediate/imm.h>

#include <functional>
#include <string>
#include <vector>

namespace toy::editor::ui {

namespace imm = coopa::ui::imm;

/** @brief One palette entry. */
struct PaletteEntry {
    std::string id;            ///< Stable key (drag payload, tests).
    std::string label;
    std::string category;      ///< Basics, Layout, HUD, Windows & Menus, Reactors
    imm::Icon icon = imm::Icon::UiWidget;
    std::string tip;
    bool component = false;    ///< Adds `make()`'s single component to the selection instead of an object.
    std::function<Node()> make;
};

namespace palette_detail {

Node v2(float x, float y);
Node rgba(float r, float g, float b, float a = 1.0f);
/** @brief A RectTransform in the editor's canonical form. */
Node rect(glm::vec2 amin, glm::vec2 amax, glm::vec2 pivot, glm::vec2 pos, glm::vec2 size);
inline Node centered(float w, float h) { return rect({0.5f, 0.5f}, {0.5f, 0.5f}, {0.5f, 0.5f}, {0, 0}, {w, h}); }
inline Node stretch(float inset = 0.0f) { return rect({0, 0}, {1, 1}, {0.5f, 0.5f}, {0, 0}, {-2 * inset, -2 * inset}); }
Node comp(const std::string& type);
Node object(const std::string& name, std::vector<Node> comps, std::vector<Node> children = {});
Node items(std::vector<std::pair<std::string, std::string>> list, const char* first_role = nullptr);

}  // namespace palette_detail

/** @brief Every palette entry, grouped by category in display order. */
const std::vector<PaletteEntry>& palette();

const PaletteEntry* find_palette_entry(const std::string& id);

/** @brief The categories in display order. */
const std::vector<std::string>& palette_categories();

} // namespace toy::editor::ui

#endif // TOYEDITOR_UI_UI_PALETTE_H
