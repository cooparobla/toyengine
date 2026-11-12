# Qt/PySide-Style Standard Widgets, UIBuilder Shorthand & Drag-and-Drop Inventory

## Introduction
This plan outlines the architecture, design, and step-by-step roadmap to make `uicoopa` provide a high-level developer experience inspired by **Qt / PyQt / PySide**. It introduces standard composite widgets (`Slider`, `ComboBox`/Dropdown, `SpinBox`, `Toggle`), a fluent parent-based builder API (`UIBuilder`) with shorthand for controls and direct value querying, a generalized Drag-and-Drop framework, and an interactive grid-based inventory system.

## Key Design Principles
1. **Zero Breaking Changes:** Built directly on existing `uicoopa` primitives (`SceneObject`, `RectTransform`, `CanvasComponent`, `LayoutGroup`, `IPointerHandler`, and `coopa::event::Signal`).
2. **Sensible Defaults & Theming (`UITheme`):** Standard controls work out of the box with one line of code while allowing granular style customization.
3. **Ergonomic Parent-Driven Creation:** Add controls directly through the parent container (e.g. `settings.add_slider(...)`).
4. **Direct Value Access:** Query or mutate values by pointer (`slider->value()`) or by name from the parent container (`settings.get_value<float>("Volume")`).

---

## Implementation Phases

### Phase 1: Core Standard Composite Widgets (`uicoopa/widgets/`)
- **`Slider` (`uicoopa/widgets/slider.h`)**:
  - Implements `UIComponent` and `IPointerHandler`.
  - Manages track, fill rect, and draggable handle/thumb.
  - Supports min, max, step, direction (horizontal/vertical).
  - Emits `coopa::event::Signal<float> on_value_changed`.
  - Provides `float value() const` and `void set_value(float, bool notify = true)`.
- **`Toggle` (`uicoopa/widgets/toggle.h`)**:
  - Checkbox / toggle switch with check indicator.
  - Emits `coopa::event::Signal<bool> on_value_changed`.
  - Provides `bool is_on() const` and `void set_is_on(bool, bool notify = true)`.
- **`SpinBox` (`uicoopa/widgets/spinbox.h`)**:
  - Numeric stepper with `[-]`, text display, `[+]` buttons, min/max/step clamping.
  - Emits `coopa::event::Signal<double> on_value_changed`.
  - Provides `double value() const` and `void set_value(double, bool notify = true)`.
- **`ComboBox` (`uicoopa/widgets/combobox.h`)**:
  - Dropdown button with popup options panel (`VerticalLayoutGroup`) and dismiss scrim.
  - Emits `coopa::event::Signal<int, const std::string&> on_selection_changed`.
  - Provides `int current_index() const`, `std::string current_text() const`, and item mutation helpers.

### Phase 2: Fluent Shorthand & Form Builder API (`uicoopa/builder/`)
- **`UITheme` (`uicoopa/builder/ui_theme.h`)**:
  - Curated color palettes, padding, font sizes, and widget dimensions for default controls.
- **`UIBuilder` (`uicoopa/builder/ui_builder.h`)**:
  - Wraps a `SceneObject*` or `CanvasComponent*`.
  - Layout constructors: `vertical_layout()`, `horizontal_layout()`, `grid_layout()`, `panel()`.
  - Control shorthand: `add_slider()`, `add_toggle()`, `add_spinbox()`, `add_dropdown()`, `add_button()`, `add_label()`.
  - Form row helpers: `add_slider_row()`, `add_dropdown_row()`, `add_spinbox_row()`, `add_toggle_row()`.
  - Direct value inspection from container:
    - `get_value<float>(name)`
    - `get_value<bool>(name)`
    - `get_value<double>(name)`
    - `get_value<std::string>(name)`
    - `set_value(name, val)`

### Phase 3: Drag & Drop Framework & Inventory Grid (`uicoopa/input/` & `uicoopa/widgets/`)
- **Drag & Drop Architecture (`uicoopa/input/drag_drop.h`)**:
  - `DragPayload` payload carrier.
  - `IDragSource` (declares payload, drag start/end callbacks).
  - `IDropTarget` (declares `can_accept_drop()`, `on_drop()`, hover states).
  - `DragDropManager` driving cursor-following visual preview and release dispatching.
- **Inventory Grid (`uicoopa/widgets/inventory_grid.h`)**:
  - `InventoryItem` data model.
  - `InventorySlot` acting as `IDragSource` + `IDropTarget`.
  - $R \times C$ slot grid using `GridLayoutGroup`.
  - Automatic item move, item swap, and stack merging.
  - Signals: `on_items_swapped`, `on_slot_changed`, `on_slot_clicked`.

### Phase 4: YAML Serialization & Headless/Visual Demos
- **YAML Registration (`uicoopa/ui_yaml.h`)**:
  - Parsers for `!Slider`, `!Toggle`, `!SpinBox`, `!ComboBox`, and `!InventoryGrid`.
- **Headless Unit Tests (`test.cpp`)**:
  - Value calculation, stepping, snapping, clamping, signal emission, builder creation, parent value querying, drag & drop, and inventory grid operations.
- **Interactive Visual Demo (`test_window.cpp`)**:
  - Settings window with live sliders, dropdowns, spinboxes, and toggles.
  - Interactive inventory grid with drag-and-drop item swapping.

---

## Testing and Validation

### Automated Headless Tests
Run test suite without a GPU display:
```bash
cmake -B build && cmake --build build
./build/uicoopa
```
Tests will assert:
1. Slider normalized drag mapping, stepping, and signal emission.
2. Toggle click toggling and state synchronization.
3. SpinBox stepping, boundaries, and formatting.
4. ComboBox item addition, selection change, and popup toggling.
5. UIBuilder scene graph construction and parent value getters.
6. Drag-and-drop payload delivery and InventoryGrid item swaps/stacks.

### Manual / Visual Verification
Run interactive demo window with Vulkan:
```bash
./build/uicoopa_test_window
```
And scripted headless screenshot validation:
```bash
MAX_FRAMES=60 ./build/uicoopa_test_window
```
Check `output/test_window.png` to confirm correct rendering of settings controls and inventory grid.
