---
name: create-ui
description: Create a toyengine UI asset -- a screen-space HUD, menu, dialog, inventory or settings screen in assets/ui/<name>.yaml, or a world-space canvas (nameplate, sign, health bar over an object) -- built from themed composites (Window, MenuList, StatBar, Hotbar, SettingRow...) with named widgets for game-code bindings. Use when asked to make, lay out or fix a UI, HUD, menu, screen, panel, button, health bar, dialog or in-world label.
---

# Create a UI

A UI asset is an object asset (`format: toyengine-object`) whose root carries a `Canvas` and a
`Theme`; its children are widgets. Parsers: `libs/uicoopa/uicoopa/ui_yaml.h` (core components)
and `libs/uicoopa/uicoopa/builder/ui_composites_yaml.h` (themed composites, list near the
bottom). Read `assets/README.md` "UI assets" and the shipped screens in `assets/ui/` (hud,
main_menu, pause_menu, dialog_box, inventory, settings) before writing a new one.

**Those shipped files are generated** by `tools/gen_ui_templates.py`, which overwrites them (and
`assets/ui/themes/*`, `editor/templates/ui/*`, `scenes/ui_showcase`). Give a new UI a new name,
or add it to that script's `SCREENS` list if it should be a template.

## Skeleton (screen space)

```yaml
# <name> -- <what the screen is>. Bindings: <the widget names game code uses>.
format: toyengine-object
object:
  name: <name>
  components:
    - type: RectTransform
      anchor_min: { x: 0.0, y: 0.0 }
      anchor_max: { x: 1.0, y: 1.0 }
      pivot: { x: 0.5, y: 0.5 }
      anchored_position: { x: 0.0, y: 0.0 }
      size_delta: { x: 0.0, y: 0.0 }
    - type: Canvas
      mode: ScaleWithScreenSize
      reference_resolution: { x: 1920, y: 1080 }   # author every size at 1920x1080
      match_width_or_height: 0.5
      sort_order: 15            # stock: hud 0, main_menu 10, dialog 20, inventory 30, settings 40, pause 50
    - type: Theme
      source: ui/themes/default.yaml               # or blender / unity, or your own (create-ui-theme)
  children:
    - name: panel
      components:
        - type: RectTransform
          anchor_preset: MiddleCenter
          size_delta: { x: 640, y: 420 }
        - type: Window
          title: Options
          close_button: true
          layout: vertical
          spacing: 12
      children:
        - name: menu
          components:
            - type: RectTransform
              anchor_preset: StretchAll
            - type: MenuList
              items:
                - { name: Resume, label: Resume, role: Primary }
                - { name: Quit, label: Quit }
```

Layout essentials: `anchor_preset` (TopLeft ... BottomRight, MiddleCenter, StretchAll,
StretchTop/Bottom/Left/Right/Horizontal/Vertical) is applied first, then raw `anchor_min/max`,
`pivot` override it. **Y is up**: a top-anchored element uses a negative y `anchored_position`.

## Building blocks

Prefer **composites** -- they read the theme, so the file stays short and restyles with it:
ThemedPanel, ThemedText (`font_role` Title/Heading/Body/Label/Caption/Numeric, `text_role`),
ThemedButton (`label`, `role` Neutral/Primary/Success), Window (`title`, `close_button`,
`layout`, `padding`, `spacing`), Dialog (+ `buttons`, `modal`, `starts_open`), ScrollView,
TabView (`tabs`, child i = page i), Collapsible, MenuList / ActionBar (`items`), SettingRow
(`kind` slider/toggle/dropdown/spinbox/text/value + its value keys), HudCorner, StatBar (`role`
health/stamina/mana, `min`, `max`, `value`), Hotbar, ItemGrid, MessageLog, PromptBar. Composite
enum values are case-insensitive; unknown keys fall back silently.

Core components when a composite doesn't fit: Image (`sprite`, `color {r,g,b,a}`,
`corner_radius`, `border_width`...), Text (`text`, `font_size`, `horizontal_align`
Left/Center/Right, `vertical_align`, `overflow` -- case-sensitive enums), Button, layout groups
(Horizontal/Vertical/GridLayoutGroup), LayoutElement, ContentSizeFitter, ScrollRect, Slider,
ProgressBar, Toggle, ComboBox, SpinBox, and the signal reactors (SetActiveOnSignal...).
Note `type: Image` makes the Image's own `type: Sliced` key unreachable in the `type:` form.

## Bindings: names are the contract

The YAML holds no callbacks. Game code reaches widgets by **name** with `coopa::ui::UiHandle`
(`on_click("Resume", fn)`, `get<float>("MusicVolume")`, `bind_bar("Health", &hp)`, `set_text`).
MenuList/ActionBar/Dialog buttons are named by each item's `name`; SettingRow, StatBar, Hotbar,
ItemGrid and MessageLog widgets take the composite object's name. Use clear PascalCase action
names for those (as the shipped screens do) and list them in the file header and in
`assets/README.md`'s UI table. Open from code with `toy::ui::open(scene, "ui/<name>")`
(`toyengine/ui/ui_assets.h`) or place in a scene with `prefab: ui/<name>`.

## World-space canvas

On an object (or a child of the object it labels), Z-up:

```yaml
- type: Transform
  position: { x: 0.0, y: 0.0, z: 2.2 }
  rotation: { x: 0.0, y: 0.0, z: 0.0 }
  scale: { x: 1.0, y: 1.0, z: 1.0 }
- type: RectTransform
  anchor_preset: StretchAll
- type: Canvas
  render_mode: WorldSpace
  billboard: CameraFacing      # or Transform (a fixed sign)
  world_size: { x: 260, y: 60 }  # canvas pixels
  pixels_per_unit: 160          # 260 px / 160 = 1.6 m wide
  text_supersample: 2
  occlude: false
- type: Theme
  source: ui/themes/default.yaml
```

Examples: `assets/scenes/world_canvas_test` (header explains `text_supersample`) and the
nameplate in `scenes/ui_showcase`. World UI needs `render.world_ui_enabled`.

## Verify

Place it in a scene (`prefab: ui/<name>` on a root object) and render the display-resolution
frame (screen UI is not in the low-res buffer):

```sh
.claude/skills/create-scene/scripts/render_scene.sh <scene>          # save_low_res stays false
./build/toyengine_tests caml_roundtrips_every_asset
./build/toyengine_tests --group render_ui
```

Look at the image: nothing clipped or overlapping at 1920x1080, text readable, the theme
applied. The editor's UI tab (Bindings tab) lists every bindable name -- a quick check that names
came out as intended.
