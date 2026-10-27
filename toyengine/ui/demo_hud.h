/**
 * @file demo_hud.h
 * @brief A screen-space game HUD: vitals, a quest line, a message log, a hotbar and key hints.
 *
 * The shape a game's own HUD takes, and the one to copy when writing one. Every panel comes
 * from the composite vocabulary (`ctx.stat_bar`, `ctx.hotbar`, `ctx.message_log`,
 * `ctx.prompt_bar`) and is positioned through `tune()`, so where each sits is a sidecar
 * `assets/ui/DemoHud.tune.yaml` a designer can drag rather than a number recompiled.
 *
 * It owns its own numbers, because it is a demo. A real HUD reads the player's model instead:
 * replace the fields below with pointers to it and delete nothing else.
 *
 * @code{.yaml}
 * - type: DemoHud
 *   theme_ref: ui/themes/game_dark
 * @endcode
 */

#ifndef TOYENGINE_UI_DEMO_HUD_H
#define TOYENGINE_UI_DEMO_HUD_H

#include <string>
#include <vector>

#include <toyengine/ui/imm_screen.h>

namespace toy::ui {

/**
 * @class DemoHud
 * @brief The stock HUD: three vitals, a quest line, a log, ten hotbar slots, key prompts.
 */
class DemoHud : public ImmScreen {
public:
    std::string type_name() const override { return "DemoHud"; }

    float health = 100.0f,  health_max = 100.0f;
    float stamina = 100.0f, stamina_max = 100.0f;
    float mana = 60.0f,     mana_max = 60.0f;

    /** @brief The quest line, top-right. Empty title draws neither line. */
    std::string quest_title = "The Lost Lantern";
    std::string quest_step  = "Search the old mill for clues";

    /** @brief Hotbar slots. The selected one is outlined in the theme's accent. */
    int hotbar_slots = 10;
    int hotbar_selected = 0;

    /** @brief Lines in the message log, oldest first. */
    std::vector<std::string> messages{
        "You enter the clearing.",
        "The lantern flickers.",
        "Wind rises from the east.",
    };

    /** @brief Key hints along the bottom, as "key -- action" pairs. */
    std::vector<std::pair<std::string, std::string>> prompts{
        {"E", "Interact"}, {"M", "Map"}, {"Esc", "Menu"},
    };

    /** @brief Drain the vitals over time so a capture shows partly-empty bars. */
    bool animate = true;

    void on_update(float delta_time) override;
    void draw(coopa::ui::imm::Context& ctx) override;

private:
    /// Animation clock. Per instance, so two HUDs do not advance one shared clock twice as
    /// fast and a reloaded scene starts from zero rather than resuming mid-sine.
    double clock_ = 0.0;
};

}  // namespace toy::ui

#endif  // TOYENGINE_UI_DEMO_HUD_H
