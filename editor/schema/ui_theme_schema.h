/**
 * @file ui_theme_schema.h
 * @brief The fields of a game UI theme (ui/themes/*.yaml), for the editor's Theme editor.
 *
 * One section per top-level key of uicoopa's builder theme (see
 * libs/uicoopa/uicoopa/builder/ui_theme_yaml.h's parse_theme() for the format). Every key is
 * optional in a file; an absent one shows -- and the game uses -- uicoopa's built-in dark value,
 * which is what each field's default is taken from here. Fonts (`text.font_path`,
 * `text.fonts.<role>`) are paths relative to the theme file and get their own editor, so they
 * are not listed as fields.
 */

#ifndef TOYEDITOR_SCHEMA_UI_THEME_SCHEMA_H
#define TOYEDITOR_SCHEMA_UI_THEME_SCHEMA_H

#include "component_schema.h"

#include <uicoopa/builder/ui_theme.h>

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
