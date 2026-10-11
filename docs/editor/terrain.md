[Editor manual](README.md) > Terrain

A **Heightmap Terrain** is a large landscape you shape and paint with brushes: hills, valleys,
cliffs and riverbeds, painted with grass, rock, sand or snow. It is made for open worlds: a
terrain can be kilometres across, and the engine draws it in chunks with more detail near the
camera.

(The other terrain type, **Tile Terrain**, generates a stylised tile world on its own and is not
sculpted by hand -- see [Tile Terrain](#tile-terrain) at the end of this page.)

## Add a terrain

1. Choose **Add > Heightmap Terrain** in the viewport header.
2. In **Properties > Components**, set its **Size** (width and depth in metres), **Height** (how
   tall a fully white heightmap is), **Chunk Size** and **Resolution** (height samples per side).

A new terrain starts as gentle procedural hills. Its shape is stored in a heightmap image beside
the scene, and its paint in two more images; both are written when you save the scene
(**Ctrl+S**).

## Enter and leave Terrain Mode

1. Select the terrain.
2. Click **Edit Terrain** in **Properties > Tool**, or pick **Terrain Mode** in the mode dropdown
   at the left of the viewport header.

Press **Tab** or **Esc** to go back to Object Mode. As in the other brush modes, the view still
moves as usual, and **Alt**+drag moves the view instead of painting.

## Tools

The toolbar on the left, and the **Tools** panel, list the tools. Number keys **1** to **7** pick
them.

| Tool | What it does | With Ctrl |
|---|---|---|
| **Raise / Lower** | Builds the ground up. Holding still keeps raising | Digs down |
| **Smooth** | Softens bumps and ridges. Hold **Shift** with any tool to smooth | |
| **Flatten** | Levels the ground to the height where the stroke started | |
| **Set Height** | Pulls the ground to **Target Height** | Click to pick the target from the terrain |
| **Noise** | Adds natural, bumpy variation | Carves it in |
| **Erode** | Wears steep slopes into gentler ones | |
| **Paint** | Paints the chosen layer | Erases it |

## Brush

The brush is the white circle draped over the ground. The inner ring is the part that works at
full strength; outside it the effect fades to the edge.

| To | Do this |
|---|---|
| Resize the brush | Press **F**, move the mouse, then click. Or **[** and **]**, or drag **Radius** in the header |
| Change the strength | Press **Shift+F**, move the mouse, then click. Or drag **Strength** in the header |

The radius is in metres on the ground, so the brush stays the same size on the terrain however
far away you look from. **Falloff** softens the edge; **Spacing** sets how close together the
dabs of a stroke are.

## Layers

Each terrain has up to eight paint layers. In the **Layers** panel, open a layer to set its
**Albedo** and **Normal Map** textures, a **Tint**, its **Tiling** (metres one texture repeat
covers), **Roughness** and **Metallic**. Turn on **Triplanar** for rock and cliff layers: on
steep slopes the texture is projected from the sides, so cliffs are not smeared. Click **Paint
This Layer** to start painting it, **Add Layer** to make a new one.

## Terrain Size

Change **Width**, **Depth**, **Height**, **Chunk Size** or the resolutions, then click
**Apply**. The current shape is kept: it is stretched to the new size, scaled to the new height,
or resampled to the new resolution. Undo restores the previous shape exactly.

### Performance

The **Size & Performance** panel also holds the settings that decide what a terrain costs:

- **Automatic View Distance** (on by default) draws every part of the terrain the camera can reach;
  parts off screen are skipped automatically. Turn it off and set **View Distance** to load only
  the ground near the camera, a hard limit for games with a close or top-down camera, or for very
  large terrains.
- **Shadow Distance** stops far parts of the terrain from casting shadows (0 uses the view
  distance). **Cast Shadows** turns them off entirely.

Distant ground automatically uses less detail. For grass and other details, keep the **Draw
Distance** modest, use **Thin Out From** so they fade out instead of stopping at a hard edge, and
turn **Cast Shadows** off for grass (Properties > Components on the detail object).

## Heightmap

- **Import Heightmap** replaces the shape with a greyscale PNG from any tool (16-bit gives smooth
  slopes; 8-bit works). White is the terrain's full **Height**.
- **Export Heightmap** saves the current shape as a 16-bit PNG.
- **Generate** replaces the shape with procedural hills; **Seed**, **Feature Size**, **Ridges**
  and **Island** shape it.
- **Fill to Target Height** flattens everything to the brush's Target Height, a blank start.

All of these can be undone.

## Details: grass, trees and rocks

The **Details** panel lists the objects scattered over the terrain and adds new ones: **Add
Grass**, **Add Trees**, **Add Rocks**. Each is a scatter particle system: select it to choose
which layer it grows on, how dense it is, the slopes and heights it allows, its mesh and how far
away it still draws. Painting and sculpting re-scatter the plants as you go.

## Undo and saving

Every stroke is one step of the scene's undo history (**Ctrl+Z**). The terrain's shape and paint
are saved with the scene. Opening the scene again without saving discards unsaved strokes.

## Tile Terrain

A **Tile Terrain** is a terraced world made of tiles. It starts as a generated island, continent
or archipelago with rivers and biomes, and from then on you edit it tile by tile. Add one with
**Add > Tile Terrain**. Its panel in **Properties > Components** has these sections:

- **Tiles**: the **Tile Set**, the object every tile is built from. Its pieces shape every top,
  edge, corner and wall, so it decides the terrain's whole form: **tileset_round** gives soft,
  rounded cliffs and **tileset_voxel** gives square blocks (**tileset_soft** and
  **tileset_rock** are softer and chunkier versions of round). **Open** shows the set's pieces.
  **Duplicate as New Tile Set** copies the set into your project, with its own piece meshes to
  reshape, and switches the terrain to the copy. **Per-Surface Tile Sets** builds chosen surfaces
  from another set, for example stone from tileset_rock or water from tileset_voxel.
- **Material**: the colours. The atlas holds one colour per surface (grass, sand, stone...), and
  the terrain shader adds the detail.
- **Generator**: the settings the world was generated from: **Seed** (or **New Random Seed**),
  **Shape**, sea level, rivers, climate, size and height range. The world is generated once and
  stored as tiles, so changing a setting here does nothing until you press **Regenerate World**.
  Regenerating replaces every tile, your edits included, and can be undone.
- **Size & Terraces**: how big each tile and each step is in the world.
- **Streaming**: how many chunks stay loaded around the point the camera looks at. This is the
  main cost control.
- **Collision**: on by default. Every chunk collides with exactly the surface it draws.

The tiles are saved in their own file (`terrain/<name>.tiles` beside the scene) when you save the
scene (**Ctrl+S**).

### Tile Mode

Press **Edit Tiles** in the panel, or choose **Tile Mode** from the mode menu, to edit the tiles.
Everything snaps to whole tiles and whole height steps:

| Tool | Key | What it does |
|---|---|---|
| Select | 1 | Drag a rectangle of tiles. **Shift** adds to the selection, **Ctrl** removes from it. Double-click selects the connected tiles of the same height (or the same surface, set in the panel). **A** selects everything, **Alt+A** or **Esc** clears. |
| Raise / Lower | 2 | Each tile under the brush rises by **Steps per Tile**, once per stroke. **Ctrl** lowers. **Shift** smooths. |
| Flatten | 3 | Levels the brush area to the height of the tile you start on. |
| Set Height | 4 | Sets tiles to the **Target Height**. **Ctrl**-click a tile to pick its height. |
| Paint | 5 | Paints a surface (grass, sand, snow...) onto tile tops, and optionally the soil below. |
| Smooth | 6 | Moves each tile one step toward its neighbours' height. |
| Water | 7 | Floods tiles below the **Water Level**. **Ctrl** drains water back down to its bed. |

The brush is a square of **Size** tiles (**[** and **]** change it). With tiles selected, the
**Selected Tiles** section of **Properties > Tool** edits all of them at once:
- height (set it, or **+1** / **−1 Step**);
- the top and side surfaces;
- the **Biome**;
- **Temperature** and **Moisture** (0 to 1, from the generator);
- **Water**;
- the region and country ids.

Where the selected tiles differ, the field shows "mixed". **Tile Under Cursor** shows everything
stored about the tile you hover. Every stroke and edit is one undo step (**Ctrl+Z**). **Tab**
leaves Tile Mode.
