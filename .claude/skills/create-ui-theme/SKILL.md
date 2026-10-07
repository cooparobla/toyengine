---
name: create-ui-theme
description: Create or edit a toyengine game UI theme -- assets/ui/themes/<name>.yaml with the colours, fonts, sizes, corner radii and metrics every themed UI widget reads -- to restyle HUDs and menus (e.g. a fantasy, sci-fi, light or high-contrast skin). Use when asked to make a UI theme/skin/style, change UI colours or fonts globally, or match a UI to a game's look.
---

# Create a UI theme

A theme is the style sheet the themed composites (Window, MenuList, StatBar...) read. Parser:
`libs/uicoopa/uicoopa/builder/ui_theme_yaml.h` (`parse_theme`). Loading starts from the built-in
dark theme, so **a theme file only needs the keys it changes**; unknown keys are ignored (so a
misspelt key is a silent no-op).

References: `assets/ui/themes/default.yaml` (most sections, commented), `blender.yaml`,
`unity.yaml` (also `collapsible` and `menu`). These three are **generated** by
`tools/gen_ui_templates.py` (`THEME`, `variant()`, `BLENDER_THEME`, `UNITY_THEME`) and are
overwritten when it runs: either name a new theme differently, or add it to that script's
`THEMES` list. Editor chrome themes in `editor/themes/` are a different thing.

## Sections

Colours are `{ r, g, b, a }` floats 0..1.

| Section | Keys |
|---|---|
| `panel` | background, panel, panel_alt, header_bar, border |
| `text` | primary, secondary, muted, accent, success, warning, info, selection; size_title, size_heading, size_body, size_label, size_small; `font_path`; `fonts: { title, heading, body, label, caption, numeric: { path, size } }` |
| `button`, `button_primary`, `button_success` | normal, hover, press, disabled |
| `slider` | track, fill, handle, handle_hover, handle_press, handle_disabled, height, handle_width, show_value_field, field_width, field_gap |
| `toggle` | bg, bg_hover, bg_press, bg_disabled, check, size |
| `spinbox`, `combobox` | bg, height, btn_width / bg, popup_bg, height |
| `slot` | bg, border, hover, selected, tooltip_bg, tooltip_text |
| `hud` | bar_bg, bar_border, bar_text, health_fill, health_ghost, stamina_fill, stamina_ghost, hotbar_key, hotbar_selected, log_bg, log_text, console_*; corner_margin, bar_width, bar_height, bar_spacing, ghost_delay, ghost_speed, log_line_height, log_hold_seconds, log_fade_seconds |
| `metrics` | row_height, row_spacing, label_width, header_height, card_header_height, scrollbar_thickness, button_padding_x, button_min_width, section_spacing, dialog_padding, tab_height, tab_spacing, tab_indicator_height, scrim_alpha |
| `shape` | panel_radius, button_radius, control_radius, bar_radius, slot_radius, border_width, shadow_size, shadow_color |
| `tab`, `tooltip`, `collapsible`, `menu`, `focus`, `cursor`, `icons` | see the parser |

Font paths resolve **relative to the theme file** (default.yaml uses
`../../fonts/inter_regular.ttf` -> `assets/fonts/`). Shipped fonts: Inter regular and semibold.

## Procedure

1. Start from the closest shipped theme; copy only the sections you change into
   `assets/ui/themes/<name>.yaml` with a header comment describing the look.
2. Keep contrast readable: primary text vs panel at least ~4.5:1; accent distinct from success /
   warning; hover/press states visibly different from normal.
3. Point a UI at it: `Theme { source: ui/themes/<name>.yaml }` on the UI's root (or preview it
   on a copy of a shipped screen). Mention it in `assets/README.md` ("UI themes").

## Verify

Render a scene showing several widget kinds with the theme -- e.g. a scratch copy of
`scenes/ui_showcase` whose UI roots point at the new theme:

```sh
.claude/skills/create-scene/scripts/render_scene.sh <scene>
./build/toyengine_tests caml_roundtrips_every_asset
```

Look at buttons, bars, sliders, text sizes and panels in the frame. The editor's Themes tab
previews a theme live on any UI asset.
