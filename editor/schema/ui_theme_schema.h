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
inline const std::vector<std::string>& ui_theme_font_roles() {
    static const std::vector<std::string> r = {"title", "heading", "body", "label", "caption", "numeric"};
    return r;
}

inline const std::vector<ThemeSection>& ui_theme_sections() {
    static const std::vector<ThemeSection> sections = [] {
        const coopa::ui::UITheme d = coopa::ui::UITheme::builtin_dark();
        auto c = [](const char* k, const glm::vec4& v) { return f_color4(k, v); };
        auto px = [](const char* k, float v, float hi = 2000.0f) { return f_float(k, v, 0.5f, 0.0f, hi); };
        auto sec = [](const char* k, float v) { return f_float(k, v, 0.01f, 0.0f, 60.0f); };
        auto button = [&](const char* key, const char* title, const coopa::ui::ButtonStyle& b) {
            return ThemeSection{key, title, {c("normal", b.normal), c("hover", b.hover), c("press", b.press), c("disabled", b.disabled)}};
        };
        std::vector<ThemeSection> s;
        s.push_back({"panel", "Panels", {c("background", d.panel.background), c("panel", d.panel.panel),
                                         c("panel_alt", d.panel.panel_alt), c("header_bar", d.panel.header_bar),
                                         c("border", d.panel.border)}});
        s.push_back({"text", "Text", {c("primary", d.text.primary), c("secondary", d.text.secondary), c("muted", d.text.muted),
                                      c("accent", d.text.accent), c("success", d.text.success), c("warning", d.text.warning),
                                      c("info", d.text.info), c("selection", d.text.selection),
                                      px("size_title", d.text.size_title, 400.0f), px("size_heading", d.text.size_heading, 400.0f),
                                      px("size_body", d.text.size_body, 400.0f), px("size_label", d.text.size_label, 400.0f),
                                      px("size_small", d.text.size_small, 400.0f)}});
        s.push_back(button("button", "Buttons", d.button));
        s.push_back(button("button_primary", "Primary Buttons", d.button_primary));
        s.push_back(button("button_success", "Success Buttons", d.button_success));
        s.push_back({"slider", "Sliders", {c("track", d.slider.track), c("fill", d.slider.fill), c("handle", d.slider.handle),
                                           c("handle_hover", d.slider.handle_hover), c("handle_press", d.slider.handle_press),
                                           c("handle_disabled", d.slider.handle_disabled), px("height", d.slider.height),
                                           px("handle_width", d.slider.handle_width), f_bool("show_value_field", d.slider.show_value_field),
                                           px("field_width", d.slider.field_width), px("field_gap", d.slider.field_gap)}});
        s.push_back({"toggle", "Toggles", {c("bg", d.toggle.bg), c("bg_hover", d.toggle.bg_hover), c("bg_press", d.toggle.bg_press),
                                           c("bg_disabled", d.toggle.bg_disabled), c("check", d.toggle.check), px("size", d.toggle.size)}});
        s.push_back({"spinbox", "Spin Boxes", {c("bg", d.spinbox.bg), px("height", d.spinbox.height), px("btn_width", d.spinbox.btn_width)}});
        s.push_back({"combobox", "Combo Boxes", {c("bg", d.combobox.bg), c("popup_bg", d.combobox.popup_bg), px("height", d.combobox.height)}});
        s.push_back({"tab", "Tabs", {c("normal", d.tab.normal), c("hover", d.tab.hover), c("press", d.tab.press),
                                     c("selected", d.tab.selected), c("selected_hover", d.tab.selected_hover), c("indicator", d.tab.indicator)}});
        s.push_back({"collapsible", "Collapsibles", {c("header", d.collapsible.header), c("hover", d.collapsible.hover),
                                                     c("press", d.collapsible.press), px("rail_width", d.collapsible.rail_width),
                                                     px("indent", d.collapsible.indent)}});
        s.push_back({"menu", "Menus", {c("bar", d.menu.bar), c("title_normal", d.menu.title_normal), c("title_hover", d.menu.title_hover),
                                       c("title_press", d.menu.title_press), c("popup_bg", d.menu.popup_bg), c("item_normal", d.menu.item_normal),
                                       c("item_hover", d.menu.item_hover), c("item_press", d.menu.item_press), c("separator", d.menu.separator),
                                       px("bar_height", d.menu.bar_height), px("item_height", d.menu.item_height),
                                       px("title_padding_x", d.menu.title_padding_x)}});
        s.push_back({"slot", "Item Slots", {c("bg", d.slot.bg), c("border", d.slot.border), c("hover", d.slot.hover),
                                            c("selected", d.slot.selected), c("tooltip_bg", d.slot.tooltip_bg), c("tooltip_text", d.slot.tooltip_text)}});
        s.push_back({"tooltip", "Tooltips", {c("bg", d.tooltip.bg), c("text", d.tooltip.text), px("padding_x", d.tooltip.padding_x),
                                             px("padding_y", d.tooltip.padding_y), px("max_width", d.tooltip.max_width),
                                             sec("delay", d.tooltip.delay), px("cursor_offset", d.tooltip.cursor_offset)}});
        s.push_back({"focus", "Gamepad Focus", {c("color", d.focus.color), c("fill", d.focus.fill), px("thickness", d.focus.thickness, 64.0f),
                                                px("padding", d.focus.padding, 64.0f), sec("move_duration", d.focus.move_duration),
                                                sec("fade_duration", d.focus.fade_duration)}});
        s.push_back({"shape", "Shape", {px("panel_radius", d.shape.panel_radius, 200.0f), px("button_radius", d.shape.button_radius, 200.0f),
                                        px("control_radius", d.shape.control_radius, 200.0f), px("bar_radius", d.shape.bar_radius, 200.0f),
                                        px("slot_radius", d.shape.slot_radius, 200.0f), px("border_width", d.shape.border_width, 16.0f),
                                        px("shadow_size", d.shape.shadow_size, 200.0f), c("shadow_color", d.shape.shadow_color)}});
        s.push_back({"metrics", "Metrics", {px("row_height", d.metrics.row_height), px("row_spacing", d.metrics.row_spacing),
                                            px("label_width", d.metrics.label_width), px("header_height", d.metrics.header_height),
                                            px("card_header_height", d.metrics.card_header_height),
                                            px("scrollbar_thickness", d.metrics.scrollbar_thickness),
                                            px("scroll_frame_padding", d.metrics.scroll_frame_padding),
                                            px("button_padding_x", d.metrics.button_padding_x), px("button_min_width", d.metrics.button_min_width),
                                            px("section_spacing", d.metrics.section_spacing), px("dialog_padding", d.metrics.dialog_padding),
                                            px("tab_height", d.metrics.tab_height), px("tab_spacing", d.metrics.tab_spacing),
                                            px("tab_indicator_height", d.metrics.tab_indicator_height),
                                            f_float("scrim_alpha", d.metrics.scrim_alpha, 0.005f, 0.0f, 1.0f)}});
        s.push_back({"hud", "HUD", {c("bar_bg", d.hud.bar_bg), c("bar_border", d.hud.bar_border), c("bar_text", d.hud.bar_text),
                                    c("health_fill", d.hud.health_fill), c("health_ghost", d.hud.health_ghost),
                                    c("stamina_fill", d.hud.stamina_fill), c("stamina_ghost", d.hud.stamina_ghost),
                                    c("hotbar_key", d.hud.hotbar_key), c("hotbar_selected", d.hud.hotbar_selected),
                                    c("log_bg", d.hud.log_bg), c("log_text", d.hud.log_text), c("console_bg", d.hud.console_bg),
                                    c("console_input_bg", d.hud.console_input_bg), c("console_text", d.hud.console_text),
                                    c("console_prompt", d.hud.console_prompt), c("console_echo", d.hud.console_echo),
                                    c("console_error", d.hud.console_error), px("corner_margin", d.hud.corner_margin),
                                    px("bar_width", d.hud.bar_width), px("bar_height", d.hud.bar_height), px("bar_spacing", d.hud.bar_spacing),
                                    sec("ghost_delay", d.hud.ghost_delay), f_float("ghost_speed", d.hud.ghost_speed, 0.5f, 0.0f, 1000.0f),
                                    px("log_line_height", d.hud.log_line_height), sec("log_hold_seconds", d.hud.log_hold_seconds),
                                    sec("log_fade_seconds", d.hud.log_fade_seconds), px("console_height", d.hud.console_height),
                                    px("console_input_height", d.hud.console_input_height)}});
        return s;
    }();
    return sections;
}

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_UI_THEME_SCHEMA_H
