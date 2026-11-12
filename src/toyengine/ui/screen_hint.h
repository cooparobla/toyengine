/**
 * @file screen_hint.h
 * @brief A line or two of text pinned to an edge of the screen: controls, a reminder, a label.
 *
 * The smallest useful screen, and the one most demos want -- "Left-click a chair to set it
 * alight. R resets." It exists so a scene does not need a UI asset to say one sentence.
 *
 * @code{.yaml}
 * - type: ScreenHint
 *   anchor: BottomLeft
 *   lines: ["Left-click a chair to set it alight.", "R resets."]
 * @endcode
 */

#ifndef TOYENGINE_UI_SCREEN_HINT_H
#define TOYENGINE_UI_SCREEN_HINT_H

#include <string>
#include <vector>

#include <toyengine/ui/imm_screen.h>

namespace toy::ui {

/**
 * @class ScreenHint
 * @brief Lines of text against one corner of the screen, over a soft backdrop.
 */
class ScreenHint : public ImmScreen {
public:
    std::string type_name() const override { return "ScreenHint"; }

    /** @brief Optional heading above the lines. */
    std::string title;

    /** @brief The lines, in order. */
    std::vector<std::string> lines;

    /** @brief Which corner it sits in. See coopa::ui::imm::Anchor. */
    coopa::ui::imm::Anchor anchor = coopa::ui::imm::Anchor::BottomLeft;

    /** @brief Inset from the anchored corner, in canvas pixels. */
    glm::vec2 offset{24.0f, -24.0f};

    /** @brief Paint a panel behind the text. Off leaves it floating on the scene. */
    bool backdrop = true;

    void draw(coopa::ui::imm::Context& ctx) override;
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_SCREEN_HINT_H
