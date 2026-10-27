/**
 * @file world_label.h
 * @brief A plate in the 3D world: a line or two of text, optionally over a bar.
 *
 * The general shape of in-world UI -- a nameplate over a creature, a sign on a wall, a label
 * on a machine. One component, because the only things that differ between those are the text,
 * whether it turns to face you, and whether the world can hide it.
 *
 * @code{.yaml}
 * - type: WorldLabel
 *   title: Aldric
 *   bar_value: 100
 *   bar_max: 100
 *   world_size: {x: 260, y: 60}
 * @endcode
 *
 * Code that wants the bar to mean something sets `bar_value` each frame, or subclasses and
 * overrides draw(). A game with real creatures usually does the latter, so the plate reads the
 * creature's own model instead of a mirror -- see toyengine/scene/health_driver.h.
 */

#ifndef TOYENGINE_UI_WORLD_LABEL_H
#define TOYENGINE_UI_WORLD_LABEL_H

#include <string>

#include <toyengine/ui/imm_screen.h>

namespace toy::ui {

/**
 * @class WorldLabel
 * @brief Text, and optionally a bar, on a world-space plate above its object.
 */
class WorldLabel : public ImmScreen {
public:
    std::string type_name() const override { return "WorldLabel"; }

    /** @brief The main line, drawn largest. */
    std::string title;

    /** @brief A smaller line under the title. Empty draws none. */
    std::string subtitle;

    /** @brief Draw a bar along the bottom when `bar_max` is above zero. */
    float bar_value = 0.0f;
    float bar_max = 0.0f;

    /**
     * @brief Which themed role the bar takes: "health", "mana", "stamina", "shield",
     *        "experience". Anything else uses the accent colour.
     */
    std::string bar_role = "health";

    /** @brief Paint the plate's background. Off leaves text floating on the world. */
    bool backdrop = true;

    void start() override;
    void draw(coopa::ui::imm::Context& ctx) override;

protected:
    /** @brief The bar's fill colour for `bar_role`; `ctx` supplies the accent fallback. */
    glm::vec4 bar_color(const coopa::ui::imm::Context& ctx) const;
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_WORLD_LABEL_H
