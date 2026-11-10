# Gameplay HUD Builder Helpers and libcoopa Model Binding

## Context

The existing `InventoryGrid`/`InventorySlot` widgets *were* the inventory — items lived in the
widget's own `items_` vector, with no backing model and no way to author them from YAML. This
work introduces two new libcoopa modules (`coopa::item`, `coopa::stat` — see libcoopa's own
`plans/2026-09-08_item-inventory-and-stat-model.md`) and makes uicoopa's inventory UI a
**visualization** of that model instead of its owner, then builds the rest of a typical
gameplay HUD (health/stamina bars, a hotbar, a pickup/kill feed, a backtick dev console) as
one-line `UIBuilder` calls, demonstrated end-to-end in a new `test_hud_builder.cpp`.

## Architecture decisions worth remembering

- **The binding seam is a single interception point.** `InventoryGrid` gained an optional
  `std::function<bool(int,int)> transfer_override`, checked at the top of
  `transfer_or_swap_items()` — not at each call site — because both `InventorySlot::on_drop()`
  and `InventoryGrid::pick_place_confirm()` already funnel through that one function. Null
  (every pre-existing caller) is byte-identical to before.
- **No new widget stores a `const UITheme*`.** Every demo's teardown runs
  `ThemeLibrary::clear()` before the scene is destroyed; all HUD widgets resolve theme colors
  into plain members at build time instead (the `InventoryGrid::tooltip_bg` pattern).
- **`ProgressBar` is not a disabled `Slider`.** `Slider::cursor_role()` reports
  `CursorRole::Disabled` whenever `!interactable`, which would flip the software cursor on
  hovering an ordinary HUD bar. The fill math itself (`fill_direction.h`'s
  `apply_fill_rect()`) is shared, not duplicated, and locked by a test asserting a `Slider`
  and a `ProgressBar` driven identically produce identical anchors.
- **`Console` polls `Input` directly, in `late_update()`, from an always-active node** —
  keyboard events are only ever dispatched to `FocusContext::focused()`, so a *closed* console
  could never otherwise learn its toggle key was pressed; `late_update()` (not `update()`) so
  the same frame's keystroke has already passed through `EventSystem::process()`.
- **`InventoryBinding`'s destructor does not touch `grid`.** `SceneObject::components_` is a
  `vector<unique_ptr<Component>>`, destroyed element-by-element in insertion order; since
  `bind_inventory()` always adds the binding *after* the grid already exists on the same
  object, `grid` is dangling by the time `~InventoryBinding()` runs during ordinary teardown.
  The destructor only disconnects its own subscriptions into the (separately-lived) model;
  the explicit `detach()` method is the one that also clears `grid->transfer_override`, for
  the "model dies first, grid is still alive" case. `Console::~Console()` has the mirror-image
  situation — it removes `panel` (a *child* SceneObject, destroyed before the parent's
  components) from `ModalContext`'s stack, which is safe only because `ModalContext::remove()`
  compares the pointer's *value*, never dereferences it. Both are covered by dedicated tests.

## New libcoopa-facing widgets (`uicoopa/widgets/`)

- **`fill_direction.h`** — `SliderDirection` + `apply_fill_rect()`, extracted from `Slider` so
  it and `ProgressBar` share one copy.
- **`progress_bar.h`** — `ProgressBar`: fill + optional delayed "chip damage" ghost trail
  (holds `ghost_delay` seconds, then drains at `ghost_speed` bar-length-pixels/second) +
  formatted label; `bind()`s to a `coopa::stat::Resource`.
- **`message_log.h`** — `MessageLog`: a capped ring of timed, fading lines backed by a **pool**
  of reused `Text` children (not rebuilt per `push()`) — `LayoutGroupBase::layout_children()`
  skips inactive children, so an expired line's node is deactivated rather than blanked, and
  the vertical layout collapses around it for free. `hold_seconds <= 0` never expires — the
  console-scrollback configuration.
- **`inventory_binding.h`** — `InventoryBinding` + free `to_ui_item()`, the seam described
  above.
- **`console.h`** — `ConsoleInput` (a `TextField` subclass: Enter submits-and-stays-editing,
  Escape/Up/Down repurposed for cancel/history, backtick never typed) and `Console` (toggle/
  focus/`ModalContext` lifecycle, scrollback, a small named-command registry).

`InventoryGrid` itself gained: `icon_path` resolution through `IconLibrary` (previously
declared, never read), an exclusive `set_selected_slot()`/`selected_slot()` pair using the
previously-unused `theme.slot.selected`, and `transfer_override`.

## Builder additions (`uicoopa/builder/`)

`ui_theme.h` gained `HudStyle` (bar/hotbar/log/console colors and metrics), parsed by
`ui_theme_yaml.h` and present in both `dark.yaml`/`light.yaml`. New `builder/detail/hud.h`
provides the factories behind `UIBuilder::hud_layer()`, `hud_corner()` (docks a fixed-size
region to one of eight `HudAnchor` points), `add_progress_bar()`, `add_stat_bar()`,
`add_message_log()`, `add_hotbar()` (returns `HotbarHandle`), and `add_console()` (returns
`ConsoleHandle`) — the latter two follow the same "declared in-class, defined out-of-line
after the handle type is complete" pattern `dialog()`/`tab_view()` already established.

Hotbar selection is deliberately **one-way, model → view**: every input path (number keys,
scroll wheel, a slot click) calls `coopa::item::Hotbar::select()`/`next()`/`prev()`, and
`on_selection_changed` drives the grid's highlight back around.

## The demo (`test_hud_builder.cpp`)

Cloned structurally from `test_gamepad_builder.cpp`. `coopa::item::Inventory`/`Hotbar`,
`coopa::stat::StatBlock`, and the loaded `coopa::item::ItemDatabase` (from
`assets/items/items.yaml`, via `ItemDatabaseLoader`) are declared **before** `Scene scene` in
`main()`, so the model outlives the view during teardown. Layout: status line (top-left),
health/stamina `StatBar`s (top-right), a pickup/kill `MessageLog` (bottom-left), a 9-slot
hotbar with key labels (bottom-center), and a backtick console with `give`/`take`/`damage`/
`heal`/`stamina`/`kill`/`say`/`items`/`clearlog` commands, each mutating the model directly —
the HUD follows via signals, never the reverse.

Env hooks beyond the standard `MAX_FRAMES`/`ONESHOT`/`SCREENSHOT_NAME`/`THEME`/`UI_AUDIO`:
`HUD_HP`, `HUD_STAMINA`, `HUD_HOTBAR`, `HUD_CONSOLE`, `HUD_SCRIPT` (replays comma-separated
console commands pre-loop), `HUD_LOG_HOLD` (so a scripted pickup line can't fade before a
screenshot).

## Testing

Headless coverage added to `test.cpp`: `InventoryGrid` upgrades (icon resolution, selection
exclusivity, override interception on both drop and pick-place paths, a locked "null override
matches the pre-existing built-in rule" regression), the `HudStyle` theme parse and
`hud_corner()`'s rect math for all eight anchors, `ProgressBar` (including the
Slider-parity lock, clamping/label formatting, the ghost trail's hold-then-drain timing, and
live `Resource` binding), `MessageLog` (pool reuse/capping, expiry/deactivation, ordering,
never-expire), `InventoryBinding`/`HotbarHandle` (model push-through, a real drag-drop driven
through `InventorySlot`'s actual pointer handlers proving the *model* moved, the
`ItemDatabase`-vs-mirrored-`max_stack` divergence, detach, selection follow-through, slot-click
routing, and the `first_slot` offset), and `Console` (toggle/focus/modal, the backtick guard,
submit-and-stay-editing, unknown-command/help, history recall, a full `EventSystem::process()`
integration proving the modal blocks HUD input beneath it, and the destructor's dangling-but-
safe `ModalContext::remove()`). All pass (`./build/uicoopa`, `ctest`).
