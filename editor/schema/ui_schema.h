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
const std::vector<std::string>& ui_signal_names();

/** @brief The component types that make an object a UI element (outliner icons, palette). */
bool is_ui_component_type(const std::string& t);

void add_ui_schemas(const std::function<void(ComponentSchema)>& add);

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_UI_SCHEMA_H
