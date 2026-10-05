/**
 * @file ui_schema.h
 * @brief Inspector schemas for uicoopa's UI components and composites -- what the UI
 *        designer's Properties and "Add Component" show for a canvas's objects.
 *
 * Included by component_schema.h (after the field builders, before schemas()); include that
 * header rather than this one. Keys mirror uicoopa/ui_yaml.h's parsers and the composites in
 * uicoopa/builder/ui_composites_yaml.h. Categories group the Add menu:
 *   UI            Canvas, RectTransform, Theme, Image, Text, Mask
 *   UI Layout     the layout groups, LayoutElement, ContentSizeFitter, ScrollRect
 *   UI Widgets    Button, Slider, ProgressBar, Toggle, NumberField, SpinBox, ComboBox, ...
 *   UI Composites themed building blocks (Window, MenuList, StatBar, ...)
 *   UI Reactors   no-code wiring: show / recolour / retext on a signal
 */

#ifndef TOYEDITOR_SCHEMA_UI_SCHEMA_H
#define TOYEDITOR_SCHEMA_UI_SCHEMA_H

#include <functional>
#include <string>
#include <vector>

namespace toy::editor {

/** @brief Signals uicoopa widgets publish by object name (EventBus), for dropdowns. */
inline const std::vector<std::string>& ui_signal_names() {
    static const std::vector<std::string> s = {"click", "value_changed", "selection_changed", "tab_changed", "opened",
                                               "closed", "slot_clicked", "hover_enter", "hover_exit", "press", "release"};
    return s;
}

/** @brief The component types that make an object a UI element (outliner icons, palette). */
inline bool is_ui_component_type(const std::string& t) {
    static const std::vector<std::string> types = {
        "RectTransform", "Canvas", "Theme", "Image", "Text", "Mask", "Button", "HorizontalLayoutGroup",
        "VerticalLayoutGroup", "GridLayoutGroup", "ContentSizeFitter", "LayoutElement", "ScrollRect", "Scrollbar",
        "Slider", "ProgressBar", "Toggle", "NumberField", "SpinBox", "ComboBox", "InventoryGrid",
        "SetActiveOnSignal", "ColorOnSignal", "TextOnSignal", "LogOnSignal"};
    for (const auto& x : types) if (x == t) return true;
    return false;
}

inline void add_ui_schemas(const std::function<void(ComponentSchema)>& add) {
    const std::vector<std::string> halign = {"Left", "Center", "Right"};
    const std::vector<std::string> valign = {"Top", "Middle", "Bottom"};
    const std::vector<std::string> child_align = {"UpperLeft", "UpperCenter", "UpperRight", "MiddleLeft", "MiddleCenter",
                                                  "MiddleRight", "LowerLeft", "LowerCenter", "LowerRight"};
    const std::vector<std::string> directions = {"LeftToRight", "RightToLeft", "BottomToTop", "TopToBottom"};
    const std::vector<std::string> font_roles = {"Body", "Title", "Heading", "Label", "Caption", "Numeric"};
    const std::vector<std::string> text_roles = {"Primary", "Secondary", "Muted", "Accent", "Success", "Warning", "Info"};
    const std::vector<std::string> roles = {"Neutral", "Primary", "Success"};
    const glm::vec4 white{1.0f, 1.0f, 1.0f, 1.0f};

    auto layout_common = [&]() {
        return std::vector<FieldDesc>{
            f_float("spacing", 8.0f, 0.5f, 0.0f, 1000.0f, true),
            f_padding("padding"),
            f_enum("child_alignment", child_align),
            f_bool("child_force_expand_width", true),
            f_bool("child_force_expand_height", true),
            f_bool("child_control_width", true),
            f_bool("child_control_height", true),
        };
    };
    auto reactor = [&](std::vector<FieldDesc> extra) {
        std::vector<FieldDesc> f = {
            with_tip(f_child("listen_object", "", true), "The object whose signal triggers this (by name)"),
            f_enum("listen_signal", ui_signal_names(), true),
            f_bool("once", false),
            with_tip(f_child("target"), "The object acted on (empty: this one)"),
        };
        for (auto& e : extra) f.push_back(std::move(e));
        return f;
    };
    // MenuList / ActionBar / Dialog button specs.
    auto button_items = [&](std::vector<std::pair<std::string, std::string>> def) {
        Node seq = Node::sequence();
        for (auto& [name, label] : def) {
            Node i = Node::mapping();
            i["name"] = Node(name);
            i["label"] = Node(label);
            seq.as_seq().push_back(i);
        }
        return seq;
    };
    const std::vector<FieldDesc> button_item_fields = {f_string("name"), f_string("label"), f_enum("role", roles)};

    // --- UI core ---
    add({"RectTransform", "UI", {
        f_vec2("anchor_min", {0.5f, 0.5f}, 0.01f, true), f_vec2("anchor_max", {0.5f, 0.5f}, 0.01f, true),
        f_vec2("pivot", {0.5f, 0.5f}, 0.01f, true), f_vec2("anchored_position", {0.0f, 0.0f}, 1.0f, true),
        f_vec2("size_delta", {160.0f, 40.0f}, 1.0f, true), f_float("rotation", 0.0f, 0.5f),
        f_vec2("scale", {1.0f, 1.0f}, 0.01f), f_bool("hittable", true), f_int("z_order", 0),
    }});
    add({"Canvas", "UI", {
        f_enum("mode", {"ScaleWithScreenSize", "ConstantPixelSize", "ConstantPhysicalSize"}, true),
        f_vec2("reference_resolution", {1920.0f, 1080.0f}, 1.0f, true),
        with_tip(f_float("match_width_or_height", 0.5f, 0.01f, 0.0f, 1.0f, true), "0 scales with the width, 1 with the height"),
        f_int("sort_order", 0), f_float("text_supersample", 1.0f, 0.05f, 0.25f, 8.0f),
        f_enum("render_mode", {"ScreenSpaceOverlay", "WorldSpace"}), f_enum("billboard", {"CameraFacing", "Transform"}),
        f_vec2("world_size", {200.0f, 50.0f}), f_float("pixels_per_unit", 100.0f, 0.5f, 0.01f, 100000.0f), f_bool("occlude", false),
    }});
    add({"Theme", "UI", {with_tip(f_asset("source", "ui/themes", ".yaml", false, false, "", true),
                                  "The theme every composite below this object is styled with")}});
    add({"Image", "UI", {
        f_asset("sprite", "textures", ".png"), f_color4("color", white, true), f_enum("type", {"Simple", "Sliced"}),
        f_padding("border"), f_bool("raycast_target", true),
        with_tip(f_float("corner_radius", 0.0f, 0.25f, 0.0f, 1000.0f), "Rounded corners (canvas px); half the height makes a pill"),
        f_enum("corners", {"all", "top", "bottom", "left", "right"}),
        f_float("border_width", 0.0f, 0.1f, 0.0f, 100.0f), f_color4("border_color", glm::vec4(1.0f, 1.0f, 1.0f, 0.15f)),
        f_float("shadow_size", 0.0f, 0.25f, 0.0f, 200.0f), f_color4("shadow_color", glm::vec4(0.0f, 0.0f, 0.0f, 0.45f)),
    }});
    add({"Text", "UI", {
        f_string("text", "Text", true), f_asset("font", "fonts", ".ttf"), f_int("font_size", 18, 1, 512, true),
        f_color4("color", white, true), f_enum("horizontal_align", halign, true), f_enum("vertical_align", valign, true),
        f_enum("overflow", {"Overflow", "Wrap", "Truncate"}), f_float("line_spacing", 1.0f, 0.01f, 0.1f, 10.0f),
        f_bool("raycast_target", true),
    }});
    add({"Mask", "UI", {}});

    // --- layout ---
    add({"HorizontalLayoutGroup", "UI Layout", layout_common()});
    add({"VerticalLayoutGroup", "UI Layout", layout_common()});
    {
        auto f = layout_common();
        f.push_back(f_vec2("cell_size", {64.0f, 64.0f}, 1.0f, true));
        f.push_back(f_vec2("cell_spacing", {0.0f, 0.0f}));
        f.push_back(f_enum("start_corner", {"UpperLeft", "UpperRight", "LowerLeft", "LowerRight"}));
        f.push_back(f_enum("start_axis", {"Horizontal", "Vertical"}));
        f.push_back(f_enum("constraint", {"Flexible", "FixedColumnCount", "FixedRowCount"}));
        f.push_back(f_int("constraint_count", 1, 1, 1000));
        add({"GridLayoutGroup", "UI Layout", f});
    }
    add({"ContentSizeFitter", "UI Layout", {f_enum("horizontal_fit", {"Unconstrained", "MinSize", "PreferredSize"}),
                                            f_enum("vertical_fit", {"PreferredSize", "Unconstrained", "MinSize"}, true)}});
    add({"LayoutElement", "UI Layout", {
        f_vec2("min_size", {-1.0f, -1.0f}), f_vec2("preferred_size", {-1.0f, 40.0f}, 1.0f, true), with_tip(f_vec2("flexible_size", {-1.0f, -1.0f}, 0.05f), "-1: automatic"),
        f_bool("ignore_layout", false),
    }});
    add({"ScrollRect", "UI Layout", {
        f_child("content", "", true), f_bool("horizontal", false), f_bool("vertical", true),
        f_enum("movement_type", {"Elastic", "Clamped", "Unrestricted"}), f_float("elasticity", 0.1f, 0.005f, 0.0f, 10.0f),
        f_bool("inertia", true), f_float("deceleration_rate", 0.135f, 0.005f, 0.0f, 1.0f), f_float("scroll_sensitivity", 20.0f, 0.5f),
        f_child("vertical_scrollbar"), f_child("horizontal_scrollbar"), f_bool("auto_scrollbars", true),
        f_float("scrollbar_thickness", 10.0f, 0.5f, 0.0f, 100.0f), f_bool("hide_scrollbar_when_unneeded", true),
    }});

    // --- widgets ---
    add({"Button", "UI Widgets", {f_bool("interactable", true), with_tip(f_child("target_graphic"), "The Image that tints on hover/press")}});
    add({"Scrollbar", "UI Widgets", {f_bool("interactable", true), f_child("handle", "Handle", true), f_enum("direction", {"Vertical", "Horizontal"}),
                                     f_float("size", 1.0f, 0.01f, 0.0f, 1.0f), f_float("value", 0.0f, 0.01f, 0.0f, 1.0f)}});
    add({"Slider", "UI Widgets", {
        f_bool("interactable", true), f_float("min", 0.0f, 0.1f, -1e30f, 1e30f, true), f_float("max", 1.0f, 0.1f, -1e30f, 1e30f, true),
        f_float("value", 0.5f, 0.01f, -1e30f, 1e30f, true), f_float("step", 0.0f, 0.01f, 0.0f), f_int("decimals", -1, -1, 6),
        f_child("fill", "Fill", true), f_child("handle", "Handle", true), f_child("track"), f_child("field"),
        f_enum("direction", directions),
    }});
    add({"ProgressBar", "UI Widgets", {
        f_float("min", 0.0f, 0.1f, -1e30f, 1e30f, true), f_float("max", 100.0f, 0.1f, -1e30f, 1e30f, true),
        f_float("value", 100.0f, 0.1f, -1e30f, 1e30f, true), f_child("fill", "Fill", true), f_child("ghost"), f_child("label"),
        with_tip(f_string("label_format", "{cur} / {max}"), "{cur} and {max} are replaced by the value and maximum"), f_float("ghost_delay", 0.35f, 0.01f, 0.0f, 10.0f), f_float("ghost_speed", 45.0f, 0.5f, 0.0f, 1000.0f),
        f_enum("direction", directions),
    }});
    add({"Toggle", "UI Widgets", {f_bool("interactable", true), f_child("checkmark", "Checkmark", true), f_bool("hide_when_off", true),
                                  f_bool("is_on", false, true)}});
    auto number = [&](bool spin) {
        std::vector<FieldDesc> f = {
            f_bool("interactable", true), f_float("min", 0.0f, 0.1f, -1e30f, 1e30f, true), f_float("max", 100.0f, 0.1f, -1e30f, 1e30f, true),
            f_float("step", 1.0f, 0.01f, 0.0f), f_int("decimals", 0, 0, 6), f_string("prefix"), f_string("suffix"),
            f_child("label"), f_float("value", 0.0f, 0.1f, -1e30f, 1e30f, true),
        };
        if (spin) { f.push_back(f_child("dec_button")); f.push_back(f_child("inc_button")); }
        return f;
    };
    add({"NumberField", "UI Widgets", number(false)});
    add({"SpinBox", "UI Widgets", number(true)});
    add({"ComboBox", "UI Widgets", {f_bool("interactable", true), f_child("button"), f_child("label"), f_child("popup"),
                                    f_strings("items", {"Option A", "Option B"}, true), f_int("selected_index", 0, 0, 10000)}});
    add({"InventoryGrid", "UI Widgets", {f_int("rows", 4, 1, 64, true), f_int("cols", 6, 1, 64, true)}});

    // --- reactors ---
    add({"SetActiveOnSignal", "UI Reactors", reactor({f_bool("active_value", true, true)}), false});
    add({"ColorOnSignal", "UI Reactors", reactor({f_color4("color", white, true), f_float("fade_duration", 0.1f, 0.01f, 0.0f, 10.0f),
                                                  f_string("target_component")}), false});
    add({"TextOnSignal", "UI Reactors", reactor({f_string("text", "", true)}), false});
    add({"LogOnSignal", "UI Reactors", reactor({f_string("message", "signal fired", true)}), false});

    // --- composites (uicoopa/builder/ui_composites_yaml.h) ---
    add({"ThemedPanel", "UI Composites", {f_enum("style", {"panel", "panel_alt", "background", "header", "border"}, true),
                                          with_tip(f_color4("color", white), "Unset: the theme's colour for the style"), f_bool("blocks_clicks", false),
                                          with_tip(f_bool("shadow", false), "A soft shadow (the theme's shape.shadow_size)"),
                                          with_tip(f_bool("border", true), "The theme's outline (shape.border_width)")}});
    add({"ThemedText", "UI Composites", {f_string("text", "Text", true), f_enum("font_role", font_roles, true), f_enum("text_role", text_roles),
                                         f_enum("align", halign), f_enum("valign", {"Middle", "Top", "Bottom"}), f_bool("wrap", false),
                                         with_tip(f_float("size", 0.0f, 0.5f, 0.0f, 512.0f), "0: the font role's size"),
                                         with_tip(f_color4("color", white), "Unset: the theme's colour for the text role")}});
    add({"ThemedButton", "UI Composites", {f_string("label", "Button", true), f_enum("role", roles, true), f_enum("font_role", {"Label", "Body", "Heading", "Title"}),
                                           f_bool("interactable", true)}});
    auto window = [&](bool dialog) {
        std::vector<FieldDesc> f = {
            f_string("title", dialog ? "Dialog" : "Window", true), f_bool("close_button", false), f_bool("show_header", true),
            with_tip(f_float("padding", 16.0f, 0.5f, 0.0f, 500.0f), "Unset: the theme's metrics.dialog_padding"),
            f_enum("layout", {"vertical", "horizontal", "none"}),
            with_tip(f_float("spacing", 8.0f, 0.5f, 0.0f, 500.0f), "Unset: the theme's metrics.row_spacing"), f_enum("align", child_align), f_enum("style", {"panel", "panel_alt", "background", "none"}),
            f_enum("title_align", halign), f_enum("title_role", {"Heading", "Title", "Label"}),
            with_tip(f_bool("shadow", true), "A soft shadow under the window (the theme's shape.shadow_size)"),
        };
        if (dialog) {
            f.push_back(f_items("buttons", button_item_fields, button_items({{"Cancel", "Cancel"}, {"Confirm", "Confirm"}}), true));
            f.push_back(with_tip(f_bool("close_on_button", true), "Any footer button also closes the dialog"));
            f.push_back(f_bool("starts_open", true));
            f.push_back(with_tip(f_bool("modal", false), "While open, nothing outside the dialog takes input"));
        }
        return f;
    };
    add({"Window", "UI Composites", window(false)});
    add({"Dialog", "UI Composites", window(true)});
    add({"ScrollView", "UI Composites", {f_float("spacing", 8.0f, 0.5f, 0.0f, 500.0f, true), with_tip(f_float("padding", 6.0f, 0.5f, 0.0f, 500.0f), "Unset: the theme's metrics.scroll_frame_padding"),
                                         f_bool("background", true), f_bool("horizontal", false), f_float("scroll_sensitivity", 30.0f, 0.5f)}});
    add({"TabView", "UI Composites", {with_tip(f_strings("tabs", {"General", "Advanced"}, true), "One tab per entry; the object's children fill them in order"),
                                      with_tip(f_float("tab_height", 32.0f, 0.5f, 8.0f, 200.0f), "Unset: the theme's value"),
                                      with_tip(f_float("tab_spacing", 4.0f, 0.5f, 0.0f, 100.0f), "Unset: the theme's value")}});
    add({"Collapsible", "UI Composites", {f_string("title", "Section", true), f_bool("start_expanded", true), f_bool("boxed", true)}});
    auto menu = [&](const char* dir, const char* align) {
        return std::vector<FieldDesc>{
            f_items("items", button_item_fields, button_items({{"Play", "Play"}, {"Options", "Options"}, {"Quit", "Quit"}}), true),
            f_enum("direction", dir == std::string("vertical") ? std::vector<std::string>{"vertical", "horizontal"}
                                                               : std::vector<std::string>{"horizontal", "vertical"}),
            with_tip(f_float("spacing", 8.0f, 0.5f, 0.0f, 500.0f), "Unset: the theme's value"),
            with_tip(f_float("button_height", 38.0f, 0.5f, 8.0f, 500.0f), "Unset: 1.35 x the theme's metrics.row_height"),
            f_float("button_width", 0.0f, 0.5f, 0.0f, 2000.0f), with_default(f_enum("align", child_align), align),
        };
    };
    add({"MenuList", "UI Composites", menu("vertical", "UpperCenter")});
    add({"ActionBar", "UI Composites", menu("horizontal", "MiddleRight")});
    add({"SettingRow", "UI Composites", {
        f_enum("kind", {"slider", "toggle", "dropdown", "spinbox", "text", "value"}, true), f_string("label", "Setting", true),
        with_tip(f_float("label_width", 175.0f, 0.5f, 0.0f, 2000.0f), "Unset: the theme's metrics.label_width"), f_float("min", 0.0f, 0.1f), f_float("max", 1.0f, 0.1f),
        f_float("value", 0.5f, 0.01f), f_float("step", 0.0f, 0.01f, 0.0f), f_int("decimals", -1, -1, 6),
        f_strings("items"), f_int("selected_index", 0, 0, 10000), f_bool("is_on", false), f_string("text"),
    }});
    add({"HudCorner", "UI Composites", {f_enum("flow", {"vertical", "horizontal"}, true), with_tip(f_float("spacing", 6.0f, 0.5f, 0.0f, 500.0f), "Unset: the theme's hud.bar_spacing"),
                                        with_tip(f_enum("align", [&] { auto a = child_align; a.insert(a.begin(), ""); return a; }()),
                                                 "Empty: pack toward the corner the object is anchored to")}});
    add({"StatBar", "UI Composites", {
        f_enum("role", {"health", "stamina", "mana", "neutral"}, true), f_string("icon"), f_float("min", 0.0f, 0.1f),
        f_float("max", 100.0f, 0.1f, -1e30f, 1e30f, true), f_float("value", 100.0f, 0.1f, -1e30f, 1e30f, true),
        f_bool("show_label", true), with_tip(f_string("label_format"), "e.g. {cur} / {max}"),
        with_tip(f_float("bar_height", 14.0f, 0.5f, 1.0f, 200.0f), "Unset: the theme's hud.bar_height"),
        with_tip(f_color4("fill_color", white), "Unset: the theme's colour for the role"),
    }});
    add({"Hotbar", "UI Composites", {f_int("count", 10, 1, 32, true), f_float("slot_size", 52.0f, 0.5f, 8.0f, 512.0f),
                                     f_float("spacing", 6.0f, 0.5f, 0.0f, 100.0f), f_bool("key_labels", true)}});
    add({"ItemGrid", "UI Composites", {f_int("rows", 4, 1, 64, true), f_int("cols", 6, 1, 64, true), f_float("slot_size", 56.0f, 0.5f, 8.0f, 512.0f),
                                       f_float("spacing", 6.0f, 0.5f, 0.0f, 100.0f), f_enum("align", {"center", "top", "topleft"})}});
    add({"MessageLog", "UI Composites", {f_int("max_lines", 6, 1, 200, true), f_bool("boxed", false), with_tip(f_float("hold_seconds", 6.0f, 0.1f, 0.0f, 600.0f), "Unset: the theme's value")}});
    {
        Node def = Node::sequence();
        for (auto [a, l] : {std::pair<const char*, const char*>{"Confirm", "Select"}, {"Back", "Back"}}) {
            Node i = Node::mapping();
            i["action"] = Node(std::string(a));
            i["label"] = Node(std::string(l));
            def.as_seq().push_back(i);
        }
        add({"PromptBar", "UI Composites", {
            f_items("prompts", {f_enum("action", {"Confirm", "Back", "Alt", "Menu", "PrevTab", "NextTab", "Advance", "Up", "Down", "Left", "Right"}),
                                f_string("label")}, def, true),
            f_enum("align", {"MiddleRight", "MiddleLeft", "MiddleCenter"}),
        }});
    }
}

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_UI_SCHEMA_H
