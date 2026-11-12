#include <toyengine/ui/game_theme.h>

#include <uicoopa/immediate/imm_theme.h>

namespace toy::ui {

GameTheme game_theme_from(const coopa::ui::imm::Theme& theme) {
    GameTheme t;
    visit_game_theme(t, [&](const char* section, const char* role, glm::vec4& c) {
        c = theme.color(section, role, c);
    });
    return t;
}

coopa::ui::imm::BarStyle bar_style(const GameTheme& t, const glm::vec4& fill) {
    coopa::ui::imm::BarStyle s;
    s.fill = fill;
    s.trail = t.bars.damage_trail;
    // track and label stay unset, so they resolve from the widget Style -- a game's bars
    // should sit against its panels, not carry their own idea of what a background is.
    return s;
}

coopa::ui::imm::SlotStyle slot_style(const GameTheme& t) {
    coopa::ui::imm::SlotStyle s;
    s.cell = t.slots.empty;
    s.cell_hover = t.slots.hover;
    return s;
}

}  // namespace toy::ui
