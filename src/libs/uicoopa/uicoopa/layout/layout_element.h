/**
 * @file layout_element.h
 * @brief Explicit size hints for layout groups, analogous to Unity's LayoutElement.
 */

#ifndef UICOOPA_LAYOUT_LAYOUT_ELEMENT_H
#define UICOOPA_LAYOUT_LAYOUT_ELEMENT_H

#include <uicoopa/ui_component.h>
#include <glm/glm.hpp>
#include <string>

namespace coopa {
namespace ui {

/**
 * @class LayoutElement
 * @brief Overrides an object's automatic layout sizing with explicit values.
 *
 * Any field left at its default (-1) is ignored, letting a layout group fall
 * back to that axis's automatic sizing (e.g. from a Text component's measured
 * content size). Matches Unity's LayoutElement semantics: a negative value means
 * "don't override this axis."
 */
class LayoutElement : public UIComponent {
public:
    LayoutElement() = default;

    std::string type_name() const override { return "LayoutElement"; }

    glm::vec2 min_size{-1.0f};       /**< Overrides min size per-axis; -1 = use automatic. */
    glm::vec2 preferred_size{-1.0f}; /**< Overrides preferred size per-axis; -1 = use automatic. */
    glm::vec2 flexible_size{-1.0f};  /**< Overrides flexible size per-axis; -1 = use automatic. */
    bool      ignore_layout = false; /**< If true, layout groups skip this object entirely. */

    SizeConstraints measure() const override {
        SizeConstraints c;
        c.min       = glm::max(min_size, glm::vec2(0.0f));
        c.preferred = glm::max(preferred_size, glm::vec2(0.0f));
        c.flexible  = glm::max(flexible_size, glm::vec2(0.0f));
        return c;
    }
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_LAYOUT_LAYOUT_ELEMENT_H
