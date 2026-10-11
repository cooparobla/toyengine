---
name: create-heightmap-terrain
description: Add a sculptable, paintable heightmap terrain to a toyengine scene -- the HeightmapTerrain component (size / height / chunk size / resolution in world units, a 16-bit heightmap PNG, eight splat-painted layers with per-layer triplanar, GPU-displaced tessellated chunks, a heightfield collider, snow / rain support) -- plus foliage, trees and rocks scattered over it with ParticleSystem `shape: terrain`. Use when asked for a landscape, open-world terrain, mountains / valleys / hills, an imported heightmap, terrain painting or sculpting, or grass / forests over terrain.
---

# Add a heightmap terrain

Copy from `assets/scenes/terrain/heightmap_terrain_demo/scene.yaml` (a 1 km valley: seven layers,
grass / pines / bushes / boulders, a lake, a player). Component and keys:
`toyengine/world/heightmap_terrain.h`; how it draws, collides and streams:
`toyengine/world/heightmap_terrain_system.h`, `assets/shaders/heightmap_terrain_surface.glsl`.
The other terrain kind, `TileTerrain` (mapcoopa tile worlds), is the create-terrain skill.

## Minimal recipe

```yaml
- name: terrain
  components:
    - type: Transform
      position: { x: -256, y: -256, z: 0 }     # the grid's (0, 0) corner; rotation / scale ignored
    - type: HeightmapTerrain
      size: { x: 512, y: 512 }    # metres
      height: 80                  # metres at a white heightmap
      chunk_size: 32              # metres per chunk
      resolution: 513             # height samples per side (2^n + 1)
      heightmap: terrain/height.png   # scene-relative; generated from `generate` when missing
      generate: { seed: 3, feature_size: 160, ridges: 0.4 }
      layers:
        - { name: grass, albedo: textures/terrain/layers/grass_albedo.png, normal: textures/terrain/layers/grass_normal.png, tiling: 4 }
        - { name: rock, albedo: textures/terrain/layers/rock_albedo.png, tiling: 9, triplanar: true }
      deep_snow: true             # snow piles (and is carved) on flat ground when it snows
    - type: WeatherSurface        # rain splashes / settled flakes land on it
```

## Keys

| Key | Meaning |
|---|---|
| `size`, `height`, `chunk_size`, `resolution`, `splat_resolution` | Geometry; changing one reshapes the live data (heights resampled / rescaled) |
| `heightmap`, `splat0`, `splat1` | PNG files (default `terrain/<object>_height.png` ...); splat0 RGBA = layers 0-3, splat1 = 4-7 |
| `generate {seed, octaves, feature_size, ridges, base, amplitude, island}` | Procedural start when the heightmap file is missing |
| `layers [{name, albedo, normal, tint, tiling, roughness, metallic, normal_strength, triplanar, triplanar_sharpness}]` | Up to 8 paint layers; `tiling` is metres per repeat; `triplanar` for cliffs |
| `tessellation {enabled, edge_pixels, max_factor, max_distance}`, `patch_resolution` | Detail by screen size: `patch_resolution` 0 = auto (4 quads, tessellation adds near detail); `max_distance` 0 = the view distance |
| `view_distance`, `shadow_distance` | Chunks exist within `view_distance` m (0 / `auto` = the camera's far clip); beyond `shadow_distance` they cast no shadow |
| `layer_texture_size` | Every layer texture is resized to this (mipmapped array) |
| `collision`, `physics_material` | Heightfield collider (static) from the same grid |
| `deep_snow`, `puddles`, `cast_shadows` | Weather and shadows |

Heights and paint are NOT in the scene file: they live in the PNGs, which the editor writes when the
scene is saved. A 16-bit greyscale heightmap from any tool loads (white = `height`); rows run
north (+y) at the top of the image.

## Foliage, trees, rocks

A child object with a scatter ParticleSystem over the terrain (see create-particle-effect):

```yaml
- type: ParticleSystem
  mode: scatter
  shape: terrain
  terrain: { layer: grass, density: 1.2, min_weight: 0.35, slope_max: 30, sink: 0.05 }
  seed: 11
  align_to_normal: true
  start_size: [0.6, 1.1]
  max_draw_distance: 70        # chunks stream in / out around the camera
  render_mode: mesh
  render_mesh: nature/grass_tuft
  material: { ... }
```

Placement is deterministic per chunk (seed, chunk, cell): two systems with the same `terrain` block
and seed line up (the demo's pine trunks and canopies). Painting or sculpting re-scatters the
touched chunks live. Stock meshes: `meshes/nature/{grass_tuft, pine_trunk, pine_canopy, bush, boulder}`. Give grass, bushes and canopies `shader: foliage` (see create-material for its params) so they bend in the World tab's wind.

## Performance

- **Terrain surface:** leave `view_distance` on auto. Every chunk shares one mesh, height texture and
  material, frustum culling skips off-screen chunks, and screen-space tessellation makes far ground a
  few triangles. Set a number only as a hard cap: a close / top-down camera that never sees far, or
  a terrain so large that per-chunk bookkeeping shows up. `shadow_distance` (e.g. 600) keeps far
  chunks out of the shadow cascades.
- **Details are the real cost.** Each scatter chunk lives in a persistent GPU buffer (uploaded once
  per rebuild, never re-copied per view); keep `max_draw_distance` modest for small things (or 0 =
  automatic from mesh size), `fade_start` thins them out before the edge, `cast_shadows: false` for
  grass, and give meshes LOD sidecars (`<mesh>.lod.yaml`, see `meshes/nature/`) -- each chunk
  picks a level for one instance at its nearest point.

## Editing

In the editor, select the terrain and pick **Terrain Mode** (mode menu) or **Edit Terrain**
(Tool tab): Raise / Lower, Smooth, Flatten, Set Height, Noise, Erode and Paint brushes sized in
metres; Layers, Terrain Size (apply), Heightmap (import / export / generate) and Details (add
grass / trees / rocks) panels. Strokes undo in the scene's timeline; Ctrl S writes the PNGs.

## Regenerating the demo

`tools/gen_heightmap_terrain_demo.py` (numpy + Pillow, use a scratch venv) writes the demo's
heightmap, splats, layer textures and nature meshes.

## Verify

```sh
FRAMES=120 .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/t.png"
FRAMES=120 .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/t_snow.png" snow_cover_override=1.0
./build/tests/toyengine_tests --suite heightmap_terrain_render
```
