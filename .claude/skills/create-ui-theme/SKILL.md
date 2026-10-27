---
name: create-ui-theme
description: Create or edit a toyengine game UI theme -- assets/ui/themes/<name>.yaml holding the widget metrics and colours every screen reads, plus a game's own roles (health/mana bars, inventory slots, combat feedback, world plates). Use when asked to make a UI theme or skin, change UI colours or fonts globally, or match a UI to a game's look.
---

# Creating a UI theme

A theme is one file. It styles **every** screen that names it -- authored panels and C++ screens
alike -- because both draw through the same `imm::Context`.

```yaml
# assets/ui/themes/my_theme.yaml
name: My Theme
author: you
inherits: game_dark              # optional: start from another theme in this folder
font: ../../fonts/inter_regular.ttf   # relative to this file

metrics:                         # sizes, in canvas pixels
  font_size: 17
  row_height: 30
  padding: 10
  spacing: 6
  rounding: 6

colors:                          # the widget palette every panel reads
  panel_bg:  "#141821ee"
  header:    "#1e2531"
  text:      "#e8e9ec"
  text_dim:  "#9aa1ad"
  accent:    "#f0b35c"
  button:    "#232b38"
  field:     "#0f131a"           # bar tracks, input backgrounds

# --- a game's own roles ---
bars:
  health:       "#d1504b"
  mana:         "#4f7fd4"
  damage_trail: "#d1504b8c"      # held where a bar's value just dropped from
slots:
  empty:    "#0f131aee"
  selected: "#f0b35c"
feedback:
  damage:  "#ef5b5b"
  healing: "#70d17f"
world:
  plate_bg:   "#0a0d12c7"
  plate_text: "#f0f0ed"
```

Colours take `"#rrggbb"`, `"#rrggbbaa"`, `[r, g, b, a]` floats, or `{r, g, b, a}`.
**Every key is optional** -- an absent one keeps what it inherits, so a variant can be five
lines. Point a screen at it with `theme_ref: ui/themes/my_theme`.

## The two halves

| Section | Defined by | Read by |
|---|---|---|
| `metrics` + `colors` | `imm::visit_style()` (`immediate/imm_theme.h`) | every widget |
| `bars` / `slots` / `feedback` / `world` | `toy::ui::visit_game_theme()` (`toyengine/ui/game_theme.h`) | the game composites |

Both are **single lists**: the loader, the serializer and the editor's Theme tab all walk them,
so a role cannot exist in one and not the others. Adding a role means adding it to the struct
*and* its `visit_*` list -- nothing else.

In code, pull a role out with the helpers rather than restating the mapping:

```cpp
ctx.stat_bar("hp", box, "Health", hp, max, bar_style(theme(), theme().bars.health));
ctx.slot_grid("bag", box, 5, 4, draw_slot, slot_style(theme()));
```

## Sizing for a game, not a tool

The built-in `imm::Style` is the editor's own look: 12 px type, 21 px rows, read from a desk
chair. A game's interface is read at a glance from further away -- `game_dark.yaml` uses 17 px
type and 30 px rows, and is the one to copy.

## Editor chrome is a different thing

`editor/themes/*.yaml` style the editor itself (`chrome`, `viewport`, `outliner` sections) and
are not edited here. They share the same format, which is why the two can inherit from each
other if you want the game to match the tool.

## Checks

- `ctest --test-dir build -L unit -R game_theme -j2` -- every role round-trips through a file
- `HEADLESS=1 MAX_FRAMES=90 ./build/toyengine ui_demo` after pointing a screen at the theme
