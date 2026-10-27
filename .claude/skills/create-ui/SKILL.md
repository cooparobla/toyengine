---
name: create-ui
description: Create a toyengine UI -- a screen authored as YAML (assets/ui/<name>.yaml -- tabs, labelled rows, buttons, bound to settings or game values) drawn by a PanelScreen component, or a C++ ImmScreen for shapes a row list cannot express (HUDs, hotbars, world nameplates). Use when asked to make, lay out or fix a UI, HUD, menu, screen, panel, settings screen, button, health bar, or in-world label.
---

# Creating a UI

Two ways, and the choice is about **shape**, not size.

| The screen is... | Make it | Compile? |
|---|---|---|
| a stack of labelled rows -- settings, a pause menu, a debug panel | a **panel asset** (YAML) + `PanelScreen` | no |
| anything else -- a HUD, a hotbar, a transport deck, a world nameplate | a **C++ `ImmScreen`** | yes |

Most screens are the first. Reach for C++ only when a row list genuinely cannot say it.

## 1. A panel asset (no C++)

`assets/ui/settings.yaml`:

```yaml
title: Settings            # omit for a frameless panel
anchor: Center             # TopLeft .. BottomRight (toy::ui::anchor_names())
size: [720, 520]
offset: [0, 0]             # moves it away from the anchored edge
closable: true             # corner X, reports the "close" action
tabs:
  - name: Audio
    rows:
      - { kind: slider, label: Master volume, bind: audio.master, min: 0, max: 1,
          tooltip: "Everything, after the per-bus levels" }
      - { kind: toggle, label: Subtitles, bind: ui.subtitles }
      - { kind: separator }
      - { kind: combo,  label: Quality, bind: render.quality, options: [Low, High] }
  - name: Controls
    rows:
      - { kind: int, label: Hotbar slots, bind: ui.slots, min: 1, max: 10 }
buttons:
  - { label: Close, action: close }
```

Row kinds: `slider`, `toggle`, `int`, `combo`, `button`, `label`, `separator`, `spacer`.
A panel with no `tabs:` takes `rows:` at the top level.

Every value row also takes **`default:`** -- what it shows before anything has written its key.
Give one: without it a slider starts at `min` (so an unwritten `audio.master` reads 0.00 against
a real volume of 1.0, and the first nudge persists silence) and a toggle can only default to
off. `step:` quantizes the value. A `combo` stores the chosen option **by name**, so inserting
an option does not silently change what an existing setting means.

Put it in a scene:

```yaml
- name: settings
  components:
    - type: PanelScreen
      panel: ui/settings
      theme_ref: ui/themes/game_dark
      hidden: true              # for a screen something else opens
```

### `bind:` is a key, not a pointer

That indirection is what lets a file hold a screen at all. By default a bound row reads and
writes `toy::core::UserSettings`, which already persists -- so a settings screen works before
any game code exists. To drive something else, register a provider where you have the object:

```cpp
auto* s = object->get_component<toy::ui::PanelScreen>();
s->bindings().provide("player.fov", [&]{ return cam.fov; }, [&](float v){ cam.fov = v; });
s->bindings().on_action("resume", [&]{ engine.set_paused(false); s->hidden = true; });
```

`close` needs no handler -- `PanelScreen` hides itself when nothing else claims it. A host
that would rather poll than register calls `take_action()`, which reports the action once and
clears it.

**A `bind` key with no consumer does nothing, and looks identical to one that works** -- the row
still reads and writes `UserSettings`, and the key joins the player's settings file forever. The
editor's Panel tab lists any such key; `toy::ui::unbound_keys()` is the same check in code, and
`toy::core::engine_setting_keys()` is what the engine itself consumes.

Handlers run from inside `draw()`, mid-scene-walk, so anything structural (load a scene, destroy
an object) must be queued on the scene's command buffer rather than done there.

## 2. A C++ screen

Derive from `toy::ui::ImmScreen` (`toyengine/ui/imm_screen.h`) and override `draw()`. It is
called once per frame, so it reads live state directly -- nothing to bind, nothing to keep in
step:

```cpp
class CombatHud : public toy::ui::ImmScreen {
public:
    std::string type_name() const override { return "CombatHud"; }
    void draw(coopa::ui::imm::Context& ctx) override {
        using coopa::ui::imm::Anchor;
        const auto b = tune().box(ctx, "health", Anchor::TopLeft, {24, 24}, {300, 28});
        ctx.stat_bar("hp", b, "Health", player_->health, player_->max_health,
                     bar_style(theme(), theme().bars.health));
    }
};
```

Then register it like any component (parser in `register.cpp` or a game's `TOY_MODULE()`, plus
a `ComponentSchema`) -- see `CLAUDE.md`'s component chain. Worked examples:
`toyengine/ui/demo_hud.h`, `toyengine/audio/audio_demo_panel.h`.

**Ask for boxes through `tune()`**, not inline: it costs the same, and it is what lets
`assets/ui/<Screen>.tune.yaml` move or resize the panel without a rebuild. It also declares the
box by name, which is what the editor's **Placement** tab lists -- open that file and each box
is an anchor, a position and a size beside a live preview. The code default is the real layout;
the file holds only what someone changed.

### The composite vocabulary

On `imm::Context`: `stat_bar` (with a damage trail), `slot_grid`, `hotbar`, `cooldown`,
`message_log`, `prompt_bar`, `panel_frame`, `menu_list`, `drag_handle`, `floating_text`,
`nameplate`, `value_bar`, plus the tool widgets (`button`, `slider_float`, `checkbox`, `combo`,
`tab_bar`, `input_text`, `tree_node`...). Place with `ctx.anchor(...)` / `ctx.stretch()`.

## World-space plates

Set `space = ImmSpace::World` (or use `WorldLabel` / `HealthDriver` as examples). The plate is a
quad in the scene, parented to what it labels:

```yaml
- type: WorldLabel
  title: Aldric
  bar_value: 72
  bar_max: 100
  world_size: { x: 260, y: 60 }     # CANVAS PIXELS, not metres
  pixels_per_unit: 160              # world_size / this = its extent in metres
  world_occlude: false              # a nameplate stays readable through a hill
  billboard: Transform              # or omit: face the camera
```

Keep world plates to bars and labels. A dropdown on a small plate is clamped to the plate and
clipped rather than relocated -- there is no escape to screen space.

## Checks

- `ctest --test-dir build -L unit -R "ui_panel|imm_game|game_theme|ui_tunables" -j2`
- `HEADLESS=1 MAX_FRAMES=90 ./build/toyengine ui_demo` -- three kinds of screen in one scene
- Rebuild `toyengine_editor` before looking in the editor; a stale binary drops new components.
