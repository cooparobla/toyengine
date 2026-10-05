# assets/

The repo's own project: the toyengine editor opens this folder by default, and every demo scene
loads from it. A scene resolves each path (mesh, material, texture, clip) against **its own
directory first, then `assets/`** -- so shared content lives here once, and a scene keeps a local
copy only for something that really is its own.

## Naming

Every file, folder, object, scene name and clip is **lowercase snake_case**: `glass_sphere`,
`robot_arm/wave.yaml`, `scene_name: pixel_demo`. No Blender-style `.000` suffixes, no PascalCase.
Component types (`MeshRenderer`) and enum values (`Perspective`, `BLEND`) are engine identifiers,
not names, and keep their own spelling.

## Layout

| Folder | Holds |
| --- | --- |
| `config.yaml` | window / render / scene settings, a comment on every key |
| `scenes/<name>/scene.yaml` | one folder per scene; scene-specific meshes in `scenes/<name>/meshes/` |
| `meshes/` | shared meshes (plus `<mesh>.lod.yaml` LOD sidecars) |
| `materials/` | shared PBR materials -- see below |
| `textures/` | shared material maps |
| `objects/` | object assets (prefabs): the animation test rigs |
| `animations/<rig>/` | clips, one folder per rig |
| `physics_materials/` | friction / restitution sets for colliders |
| `shaders/` | GLSL; surface shaders are named by a material's `shader:` |
| `ui/` | UI assets -- HUD, menus, screens (open them in the editor's UI tab) |
| `ui/themes/` | UI themes: colours, fonts, sizes of the themed widgets |
| `palettes/`, `fonts/` | colour palettes, UI fonts |

### Shared meshes

| Mesh | Shape | Origin |
| --- | --- | --- |
| `cube` | unit cube, [-0.5, 0.5]^3 | centre |
| `cube_corner` | unit cube, [0, 1]^3 | corner -- older scenes place boxes by their corner |
| `plane` | 2x2 quad in XY, normal +Z | centre |
| `sphere` | radius-1 UV sphere, with an LOD sidecar | centre |
| `sphere_low` | radius-1 low-poly lathe sphere (water props) | centre |
| `ball` | radius-0.5 smooth sphere (animation rigs) | centre |
| `barrel` | capsule, radius 0.35, height 1.3, along Z | centre |
| `water_grid` | 2x2 plane subdivided 12x12, for wave displacement | centre |
| `tentacle` | skinned tube, vertex groups `seg_0..seg_3` | base |
| `tile_side_flat`, `tile_side_bevel` | terrain tile sides (see toyengine/world) | -- |

Generators in `tools/` rebuild the generated ones (`gen_water_test_meshes.py`,
`gen_water_grid_mesh.py`, `gen_animation_test_scene.py`, `gen_tile_side_meshes.py`).

## Materials

A renderer's `material:` takes any of three forms:

```yaml
material: materials/brick                                   # a shared material
material: { base: materials/brick, albedo: { r: 0.8, g: 0.35, b: 0.25 } }   # shared + overrides
material: { albedo: { r: 1.0, g: 0.0, b: 0.0 }, roughness: 0.4 }           # inline
```

| Material | What it is |
| --- | --- |
| `default` | neutral light-grey dielectric |
| `plastic` | glossy off-white; tint with an `albedo` override |
| `rubber` | dark red, fully matte |
| `concrete`, `stone`, `sand` | rough greys / lake-bed sand |
| `wood`, `wood_crate` | unfinished mid-brown / light crate pine |
| `brick` | albedo + normal + metallic-roughness maps (`textures/brick_*.png`) |
| `steel`, `chrome`, `gold`, `copper` | metals: brushed, mirror, polished, satin |
| `ice` | glossy opaque blue-white |
| `glass`, `frosted_glass` | BLEND + refraction, clear / rough (needs `render.transparency_enabled`) |
| `emissive` | warm self-lit glow |
| `foliage` | CUTOUT leaf card on the `foliage` surface shader (wind sway) |
| `water` | translucent refractive surface for a `WaterBody` |
| `prototype_grid` | 1 m blockout grid, `triplanar` in world space |
| `triplanar_test` | the brick maps on the `triplanar` surface shader |

Physics materials (`physics_materials/`) share names where both exist (`concrete`, `wood`,
`rubber`, `ice`), so a collider and its renderer can be matched by name.

## Triplanar surface shader

`shader: triplanar` ([shaders/triplanar.frag](shaders/triplanar.frag)) projects every material
map along X, Y and Z and blends them by the surface normal instead of reading mesh UVs -- even
texel density on stretched or UV-less meshes, and seamless joins between neighbouring objects.

```yaml
shader: triplanar
shader_params: [tiling, sharpness, space, normal_strength]
```

- `tiling` -- texture repeats per unit (default 1).
- `sharpness` -- blend exponent; higher narrows the seams between projections (default 4).
- `space` -- `0` world: the texture stays put in the world and keeps its real size at any object
  scale. `1` object: it rides with the object, following its position, rotation and scale.
- `normal_strength` -- scales the normal map's tilt (default 1).

It is for OPAQUE materials: shadow passes use the stock shaders, so a CUTOUT triplanar material
would cast its mesh-UV silhouette. `scenes/pixel_demo` shows both spaces side by side
(`triplanar_world`, `triplanar_local`).

## Scene settings

A scene can override config.yaml's `render` and `physics` keys for itself:

```yaml
scene:
  scene_name: night_market
  settings:
    render: { sky_intensity: 0.3, fog_density: 0.06 }
    physics: { gravity: { x: 0.0, y: 0.0, z: -4.0 } }
```

Whichever scene is active runs with config.yaml plus its overrides (render keys apply live,
physics when the scene starts); a scene without them runs with the project's settings. The
layering is done on the documents, so quality presets resolve as if the keys were in
config.yaml. Startup-only render keys (resolution, shadow-map sizes...) can't vary per scene.
In the editor, editing a Render / World / Physics setting while a scene is open writes such an
override -- the row is tinted -- and right-click reverts it or applies it to config.yaml.

## UI assets

`ui/<name>.yaml` is an object asset whose root carries a `Canvas` (authored at 1920 x 1080,
`ScaleWithScreenSize`) and a `Theme { source: ui/themes/default.yaml }`. Place one in a scene as
`prefab: ui/hud`, or open it from game code with `toy::ui::open(scene, "ui/pause_menu")`
(`toyengine/ui/ui_assets.h`). Widgets are mostly uicoopa composites -- `Window`, `MenuList`,
`StatBar`, `Hotbar`, `SettingRow`... (see `libs/uicoopa/uicoopa/builder/ui_composites_yaml.h`) --
so a file stays short and restyles with its theme.

Game code reaches widgets **by name** through `coopa::ui::UiHandle` (`on_click("Resume", ...)`,
`get<float>("MasterVolume")`, `bind_bar("Health", &hp)`); the editor's Bindings tab lists every
name in a file.

| UI | What it is | Names to bind |
|---|---|---|
| `hud` | vitals, quest tracker, message log, hotbar, prompts | `Health`, `Stamina`, `Mana`, `QuestTitle`, `QuestStep`, `MessageLog`, `Hotbar` |
| `main_menu` | title screen | `NewGame`, `Continue`, `Settings`, `Quit` |
| `pause_menu` | pause over a dimmer | `Resume`, `Settings`, `SaveGame`, `QuitToMenu` |
| `dialog_box` | NPC dialogue | `Portrait`, `Speaker`, `Line`, `Accept`, `AskMore`, `Leave` |
| `inventory` | bag and equipment grids, details | `Bag`, `Equipment`, `ItemName`, `ItemDescription`, `Use`, `Gold`, `Close` |
| `settings` | Audio / Video / Gameplay tabs | `MasterVolume`, `MusicVolume`, `EffectsVolume`, `Fullscreen`, `VSync`, `Quality`, `Resolution`, `Difficulty`, `Subtitles`, `CameraShake`, `Back`, `Apply` |

### UI themes

`ui/themes/` holds the game UI themes -- the colours, fonts and sizes of every themed widget.
Three ship: `default` (full-resolution, dark slate with a gold accent), `blender` (Blender's
dark UI: neutral greys, `#4772b3` blue, compact rows) and `unity` (Unity's dark Pro skin,
`#3a79bb` blue). A UI picks one with its root `Theme { source: ui/themes/<name>.yaml }`.
Edit them in the editor's **Themes** tab: a theme opens in the Properties editor's Theme tab,
previewed live on a UI asset (with its unsaved edits, in place of that UI's own theme) until
Save Theme. These are game assets; the editor's own chrome themes live in `editor/themes/`.

`scenes/ui_showcase` places the HUD over a small scene with a world-space nameplate; its
`HealthDriver` drives the HUD's `Health` bar by name. `tools/gen_ui_templates.py` regenerates
these files (and the editor's New UI templates, `editor/templates/ui/`).
