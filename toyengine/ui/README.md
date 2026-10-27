# toyengine/ui

Game interfaces, declared per frame on uicoopa's immediate layer. A screen is a component: it
draws itself from whatever the game's state happens to be right now, so there is nothing to
bind and nothing that can fall out of step with the model.

| File | Purpose |
|---|---|
| [`imm_screen.h`](imm_screen.h) | `ImmScreen` — the base every screen derives from. Builds its canvas (screen-space or a world plate), resolves a theme and font, wires UI sound, and calls `draw(imm::Context&)` once a frame. |
| [`ui_panel.h`](ui_panel.h) | A screen authored as YAML: tabs, labelled rows, buttons. `PanelBindings` resolves a row's `bind` key — to `UserSettings` by default, or to a game value a provider registers. |
| [`panel_screen.h`](panel_screen.h) | `PanelScreen` — the component that draws a panel asset. One implementation, any number of authored screens. |
| [`ui_tunables.h`](ui_tunables.h) | Where each panel of a screen sits, as a sidecar `*.tune.yaml`. The code supplies the default; the file holds only what someone moved. |
| [`game_theme.h`](game_theme.h) | A game's own colour roles (bars, slots, feedback, world plates) as sections of an `imm::Theme`, plus `bar_style()` / `slot_style()`. |
| [`demo_hud.h`](demo_hud.h) | The stock HUD, and the worked example to copy: vitals, quest line, message log, hotbar, key prompts. |
| [`world_label.h`](world_label.h) | A plate in the 3D world — a nameplate, a sign, a label on a machine. |
| [`screen_hint.h`](screen_hint.h) | A line or two pinned to a corner: controls, a reminder. |

## Which to use

A screen that is **a stack of labelled rows** — settings, a pause menu, a debug panel — is a
panel asset and needs no C++. Anything else — a HUD, a hotbar, a transport deck — is an
`ImmScreen` subclass. See the `create-ui` skill.

## Conventions

- **Ask for boxes through `tune()`.** `tune().box(ctx, "health", Anchor::TopLeft, {24,24}, {300,28})`
  costs the same as computing one inline, and it is what lets a `ui/<Screen>.tune.yaml` move or
  resize the panel without a rebuild — the file holds only what someone changed. It also
  declares the box by name, which is what the editor's Placement tab lists.
- **Colours come from `theme()`**, not literals. A role earns a place in `GameTheme` when more
  than one screen must agree on it; one button's colour is not a theme decision.
- **Never restructure the scene from `draw()`** — it runs mid-walk. Queue it on the scene's
  command buffer, as `ImmScreen` itself does to build its canvas. This applies to a panel's
  `on_action` handler too: `draw_panel()` invokes it from inside the draw, so "load a scene" or
  "destroy this object" has to be queued rather than done there. A host that would rather not
  register a handler can poll `PanelScreen::take_action()`, which reports the action once and
  clears it.
- **A world plate is bars and labels.** Popups clamp to the plate and clip rather than
  relocating, so a dropdown on a nameplate is a mistake, not a layout bug.

## What a host provides

`Engine` drives every canvas each frame (`drive_ui_canvases_`): viewport sizing, HiDPI cursor
scaling, the player's interface scale, editor placement, and — for world plates — the pointer
ray. `Engine::set_ui_preview(false)` keeps an authored interface out of a scene view until the
game actually runs, which is what the editor does everywhere except its UI workspace.
