# The GUI map viewer

`mapcoopa_viewer` is the same generator behind a window. It has a collapsible panel of the
settings that most change a map, a **Refresh** button, a picker for all nine layers, and a
File menu that imports and exports `.yaml` and `.png`.

It is optional. It is built only when the sibling `../uicoopa` checkout exists (and,
through it, gfxcoopa and a Vulkan driver). Without it, CMake prints a status message and
skips the viewer. `-DMAPCOOPA_WITH_VIEWER=OFF` skips it explicitly. The `coopa::maps`
library target never depends on it, and a consumer using `add_subdirectory()` never builds
it.

```bash
./build/mapcoopa_viewer
```

It starts from the same settings as the command-line tool (the tool defaults plus
`assets/config.yaml`), so the same seed gives the same map in both. Edits take effect when
you press **Refresh**. Switching layers re-renders from the map already generated. Hovering
a setting shows its documentation, taken from
[`coopa/maps/map_config.h`](../coopa/maps/map_config.h).

The **Shape** and **Surface** dropdowns show only the settings the current choice reads.
For example, the blur controls appear only under the `blended` surface.

## Controls

| Input | Effect |
|---|---|
| Wheel | Zoom around the cursor, up to 16x |
| Middle-drag | Pan |
| Ctrl + 0 | Reset to fit |
| Hover a pin | Its name and what it is |
| Hover the map | What the current layer shows at that point |

Pins and cursor readouts follow the layer:

| Layer | Pins | Cursor readout |
|---|---|---|
| composite | towns, landmarks, caves | biome and height |
| structures | towns | |
| landmarks | landmarks | |
| caves | caves | |
| biomes | | biome |
| elevation | | height above sea level, or depth |
| water | | ocean, lake or land, with depth |
| regions | | region and country |
| roads | | |

Town pins show at any zoom. Landmark and cave pins appear past 2x, because a default map has
around a hundred landmarks.

Zoom is instant: it samples the texture already on screen. When you stop, the preview
re-renders at up to 4x resolution, capped at 4096 px. The preview renders at a smaller size
than `image_size`; exports always use the full configured size.

## Scripted runs

The viewer reads environment variables so it can be driven without a person at the window.
`MAX_FRAMES` and `ONESHOT` come from gfxcoopa's app context, and the rest from the viewer
itself:

```bash
SEED=251 MAX_FRAMES=400 SCREENSHOT_NAME=viewer ./build/mapcoopa_viewer   # writes output/viewer.png
```

| Variable | Effect |
|---|---|
| `SEED=N` | Start on a given seed |
| `THEME=name` | A uicoopa theme (`dark`, `light`) |
| `MAX_FRAMES=N` | Exit after N frames |
| `ONESHOT=1` | Render a single frame and exit |
| `SCREENSHOT_NAME=x` | Write `output/x.png` on exit |
| `LAYER=name` | Start on a layer (`composite`, `biomes`, `elevation`, ...) |
| `ZOOM=n` | Start zoomed in n times |
| `VIEW=cx,cy` | Centre the view at an image-space position |
| `HOVER_ROW=label` | Park the pointer on a settings row, to capture its tooltip |
| `HOVER_MAP=x,y` | Park the pointer at a grid-space map position |
| `SHAPE=name` | Select a shape (`rectangle`, `circle`, `triangle`, `continent`, `archipelago`) |
| `SURFACE=name` | Select a surface (`interpolated`, `flat`, `blended`) |
| `EXPAND_ALL=1` | Open every settings section |
| `COLLAPSE=1` | Start with the sidebar folded |
| `OPEN_MENU=1` | Start with the File menu open |
| `OPEN_PICKER=1` | Start with the file browser open |

Note that these open a window. For headless use, prefer the `mapcoopa` command-line tool.
