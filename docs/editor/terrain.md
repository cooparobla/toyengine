[Editor manual](README.md) > Terrain

A **Heightmap Terrain** is a large landscape you shape and paint with brushes: hills, valleys,
cliffs and riverbeds, painted with grass, rock, sand or snow. It is made for open worlds: a
terrain can be kilometres across, and the engine draws it in chunks with more detail near the
camera.

(The other terrain type, **Tile Terrain**, generates a stylised tile world on its own and is not
sculpted by hand.)

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
