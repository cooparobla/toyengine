/**
 * @file ui_theme_schema.h
 * @brief The fields of a game UI theme (ui/themes/*.yaml), for the editor's Theme editor.
 *
 * Generated from the two field lists the format itself is defined by -- imm::visit_style()
 * for the widget metrics and colours, and toy::ui::visit_game_theme() for a game's own roles
 * (bars, slots, feedback, world). Driving the editor from the same lists the loader walks is
 * what stops a theme gaining a key the editor cannot show, or showing one the format dropped.
 *
 * Every key is optional in a file; an absent one keeps the built-in value, which is what each
 * field's default is taken from here. The theme's single `font:` is a path relative to the
 * file and gets its own editor, so it is not listed as a field.
 */

#ifndef TOYEDITOR_SCHEMA_UI_THEME_SCHEMA_H
#define TOYEDITOR_SCHEMA_UI_THEME_SCHEMA_H

#include "component_schema.h"


#include <string>
#include <vector>

namespace toy::editor {

/** @brief One top-level section of a theme file: its key, a title, and its fields. */
struct ThemeSection {
    std::string key;
    std::string title;
    std::vector<FieldDesc> fields;
};

/** @brief The font roles a theme's `text.fonts` may set, in display order. */
const std::vector<std::string>& ui_theme_font_roles();

const std::vector<ThemeSection>& ui_theme_sections();

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_UI_THEME_SCHEMA_H
