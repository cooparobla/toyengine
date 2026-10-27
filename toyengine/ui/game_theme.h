/**
 * @file game_theme.h
 * @brief A game's own interface colours, as a named section of an immediate-mode theme.
 *
 * `imm::Theme` (uicoopa's `immediate/imm_theme.h`) carries the widget Style every panel reads
 * -- panel backgrounds, text, accents, field colours -- plus any number of free-form
 * application sections. The editor gives its sections names and types in
 * editor/app/editor_theme.h (`chrome`, `viewport`, `outliner`); this file does the same for a
 * game's: the roles only a game knows the meaning of, like which colour health is.
 *
 * @code{.yaml}
 * # assets/ui/themes/game_dark.yaml
 * name: Default
 * inherits: base_dark          # a theme can be a handful of overrides
 * metrics:
 *   font_size: 20
 *   rounding: 6
 * colors:                      # the widget Style: panels, text, fields, accents
 *   panel_bg: "#0d1014eb"
 *   accent:   "#f5c766"
 * bars:                        # the sections below -- one per GameTheme group
 *   health:  "#e04b4b"
 *   stamina: "#6ad06a"
 * slots:
 *   empty:   "#1a1d25e6"
 * @endcode
 *
 * `bars`, `slots`, `feedback` and `world` sit at the TOP level beside `colors:`, exactly as
 * the editor's `chrome`/`viewport`/`outliner` do -- any top-level mapping a theme file does
 * not reserve becomes an application section.
 *
 * A theme that leaves a role out keeps the value below, so a project can state two colours and
 * inherit the rest. One field list (visit_game_theme) is walked by the loader and by any editor
 * that offers these as rows, so the two can never disagree about which roles exist.
 */

#ifndef TOYENGINE_UI_GAME_THEME_H
#define TOYENGINE_UI_GAME_THEME_H

#include <glm/glm.hpp>

#include <uicoopa/immediate/imm.h>

// imm_theme.h is what pulls in fkYAML, and only the loader needs it -- a screen that just
// reads colours should not pay for a YAML parser. Declared here, included in the .cpp.
namespace coopa::ui::imm { struct Theme; }

namespace toy::ui {

/**
 * @struct GameTheme
 * @brief The interface roles a game states, beyond the widget Style every panel already reads.
 *
 * Deliberately small. A role earns a place here only when more than one screen needs to agree
 * on it -- health being red in both the HUD and a nameplate is a theme decision; the colour of
 * one particular button is not.
 */
struct GameTheme {
    /** @brief Resource bars. The fill; the track and trail come from the Style. */
    struct Bars {
        glm::vec4 health {0.878f, 0.294f, 0.294f, 1.0f};
        glm::vec4 mana   {0.329f, 0.510f, 0.878f, 1.0f};
        glm::vec4 stamina{0.416f, 0.816f, 0.416f, 1.0f};
        glm::vec4 shield {0.600f, 0.690f, 0.780f, 1.0f};
        glm::vec4 experience{0.839f, 0.706f, 0.310f, 1.0f};
        /** @brief What a bar lost, held briefly where the value used to be. */
        glm::vec4 damage_trail{0.878f, 0.294f, 0.294f, 0.55f};
    } bars;

    /** @brief Slotted interfaces: inventories, hotbars, worn equipment. */
    struct Slots {
        glm::vec4 empty   {0.102f, 0.114f, 0.145f, 0.90f};
        glm::vec4 filled  {0.141f, 0.157f, 0.196f, 0.95f};
        glm::vec4 hover   {0.196f, 0.216f, 0.267f, 1.0f};
        glm::vec4 selected{0.961f, 0.780f, 0.400f, 1.0f};
        /** @brief The "1".."9" label in a hotbar slot's corner. */
        glm::vec4 key_label{0.702f, 0.722f, 0.784f, 0.90f};
        /** @brief The wipe over a slot whose ability is still recharging. */
        glm::vec4 cooldown{0.0f, 0.0f, 0.0f, 0.55f};
    } slots;

    /** @brief Floating combat text and the message log's severities. */
    struct Feedback {
        glm::vec4 damage  {0.937f, 0.361f, 0.361f, 1.0f};
        glm::vec4 healing {0.439f, 0.820f, 0.498f, 1.0f};
        glm::vec4 critical{0.980f, 0.702f, 0.231f, 1.0f};
        glm::vec4 miss    {0.667f, 0.682f, 0.722f, 1.0f};
        glm::vec4 notice  {0.549f, 0.733f, 0.961f, 1.0f};
    } feedback;

    /** @brief In-world plates: nameplates, signs, floating labels. */
    struct World {
        glm::vec4 plate_bg {0.043f, 0.051f, 0.071f, 0.78f};
        glm::vec4 plate_text{0.941f, 0.941f, 0.929f, 1.0f};
        /** @brief A hostile's plate, where a game distinguishes them. */
        glm::vec4 hostile  {0.878f, 0.333f, 0.294f, 1.0f};
        glm::vec4 friendly {0.439f, 0.780f, 0.478f, 1.0f};
        glm::vec4 neutral  {0.839f, 0.780f, 0.459f, 1.0f};
    } world;
};

/**
 * @brief Calls `v(section, role, colour&)` for every game role -- the single list of them.
 *
 * Loading and any editor that offers these as rows both walk this, so neither can drift from
 * the other about which roles exist. Section names are the YAML mapping keys.
 */
template <class T, class V>
void visit_game_theme(T& t, V&& v) {
    v("bars", "health", t.bars.health);
    v("bars", "mana", t.bars.mana);
    v("bars", "stamina", t.bars.stamina);
    v("bars", "shield", t.bars.shield);
    v("bars", "experience", t.bars.experience);
    v("bars", "damage_trail", t.bars.damage_trail);
    v("slots", "empty", t.slots.empty);
    v("slots", "filled", t.slots.filled);
    v("slots", "hover", t.slots.hover);
    v("slots", "selected", t.slots.selected);
    v("slots", "key_label", t.slots.key_label);
    v("slots", "cooldown", t.slots.cooldown);
    v("feedback", "damage", t.feedback.damage);
    v("feedback", "healing", t.feedback.healing);
    v("feedback", "critical", t.feedback.critical);
    v("feedback", "miss", t.feedback.miss);
    v("feedback", "notice", t.feedback.notice);
    v("world", "plate_bg", t.world.plate_bg);
    v("world", "plate_text", t.world.plate_text);
    v("world", "hostile", t.world.hostile);
    v("world", "friendly", t.world.friendly);
    v("world", "neutral", t.world.neutral);
}

/** @brief The game roles `theme` defines; any it omits keeps GameTheme's own value. */
GameTheme game_theme_from(const coopa::ui::imm::Theme& theme);

/**
 * @brief A BarStyle for one of the resource roles, so a screen does not restate the pattern.
 *
 * The fill is the role's colour and the trail is the theme's; the track stays the Style's
 * field colour, which is what keeps a game's bars sitting correctly against its panels.
 */
coopa::ui::imm::BarStyle bar_style(const GameTheme& t, const glm::vec4& fill);

/** @brief A SlotStyle from the theme's slot roles. */
coopa::ui::imm::SlotStyle slot_style(const GameTheme& t);

}  // namespace toy::ui

#endif  // TOYENGINE_UI_GAME_THEME_H
