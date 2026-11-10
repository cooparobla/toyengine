# Output: layers, shading and the map file

A run of the `mapcoopa` tool writes `PREFIX.yaml` and nine PNG layers, `PREFIX_<layer>.png`.
The YAML is the real output. The PNGs are a software-rendered debugging aid.

## The map file

`coopa::maps::save_map()` in [`map_yaml.h`](../coopa/maps/map_yaml.h) writes the whole
`MapGraph`: every cell, corner, edge, river, road, settlement, building, region, landmark
and cave, with all adjacency lists, plus the `MapConfig` that produced it under a `config:`
block. `load_map()` reads both back. The document is a save of generator state, so a game
can load a world it did not generate.

Float fields are written to six significant digits. At the default 80-cell grid the file is
about 14 MB.

Biomes, building roles, landmark kinds and cave zones are written as stable `snake_case`
names (for example `temperate_deciduous_forest`). Key on those names, not on enum
positions. The names are append-only.

## Layers

Each layer is a separate PNG, so a consumer can take the height field without roads drawn
over it, or the roads without terrain under them. All layers register pixel for pixel.

| File | Format | Contents |
|---|---|---|
| `_elevation.png` | RGB | Ground height, sea bed included: black at the deepest water, white at the summit. Nothing is painted on it, but river valleys and channels show because the ground there really is lower. |
| `_water.png` | RGB | Water-surface height on the same scale as `_elevation.png`. The sea sits at `sea_level`, each lake at one height, rivers at ground height plus a depth. Dry land is black. |
| `_biomes.png` | RGB | Flat biome colour, no overlays. |
| `_roads.png` | RGBA | The road network by class, transparent elsewhere. |
| `_structures.png` | RGBA | Settlements: market squares, streets, and building footprints as rotated quads with civic buildings picked out. Transparent elsewhere. |
| `_landmarks.png` | RGBA | Settlement markers and cave-mouth rings, plus landmark marks when `draw_landmark_marks` is on. Transparent elsewhere. |
| `_regions.png` | RGB | Provinces in flat colour. Every pixel is exactly one region's colour or the background, so a consumer can recover which region covers a pixel. |
| `_composite.png` | RGB | Biome colour lit from the elevation field, then water, roads, buildings and markers. |
| `_caves.png` | RGB | Every cave system over dimmed terrain, coloured by depth, with chambers at their real size and a ring at each mouth. |

The three RGBA layers stack over the terrain in any image editor and reproduce the
composite's arrangement. The colours are listed in the [legend](../README.md#legend).

`draw_landmark_marks` defaults to false: landmarks are labels rather than terrain, and the
GUI viewer shows them as hoverable pins instead. It is render-only and is not
`enable_landmarks`, which decides whether a map has landmarks at all.

From C++, `MapLayers` in [`map_renderer.h`](../coopa/maps/map_renderer.h) renders any one
layer to an `Image` (`MapLayers::render(MapLayer::Biomes, graph, config)` or the named
helpers such as `MapLayers::composite(graph, config)`). `MapExporter` in
[`map_export.h`](../coopa/maps/map_export.h) renders and writes all nine. It is a separate
header because it pulls in the stb_image_write implementation.

A colour read off `_biomes.png` or `_composite.png` is not the raw palette colour. While
`show_regions` is on (the default), every claimed land cell is mixed `region_tint` (0.13)
toward its region's colour. The composite also lights every land pixel. Use `_biomes.png`
with `show_regions: false` (or `--no-regions`) for exact palette matches.

## Shading

The composite lights its biome colours one of two ways, chosen by `composite_shading` or
`--shading=MODE`. Neither changes the height data.

| Mode | Brightness follows | Shows |
|---|---|---|
| `elevation` (default) | height: high ground is pale | Where the high ground is. The same height reads the same everywhere. |
| `hillshade` | slope, lit from the north-west | The shape of the land: ridgelines and valley walls. A slope at sea level and the same slope on a summit look the same. |

Hillshade needs a second full-resolution pass and a blur, so it is slower. The blur is
required: elevation is continuous across a cell edge but its slope is not, and shading the
raw field draws every cell as a small dome.

## Meshing terrain and water together

Build a terrain mesh from the elevation data and a water mesh from the water surface at the
same vertical scale, and this holds:

- **Rivers: the water is above the terrain everywhere.** `make_river_surfaces()` in
  [`map_data.h`](../coopa/maps/map_data.h) measures the ground with the same
  `elevation_at()` call the elevation layer draws with, and takes the highest point over
  the stroke's footprint before adding `river_depth_m`.
- **Lakes and sea: guaranteed in the interior, not within one cell of a shore.** A cell's
  height is the mean of its corners, and a cell the sea reaches into has corners on land.
  About one water cell in ten has a bed above its own surface for that reason. Where a cell
  **and every neighbour** have a bed at or below their surface, the drawn ground is under
  the water. Elsewhere, clip the water to where the terrain is below it.
- `water_edge_overlap_m` (default 1) extends every water surface past its own edge, so it
  clips into the terrain instead of meeting it at a seam.

For a smooth heightfield, sample `MapGraph::elevation_at()` rather than the 8-bit PNG.

## Render cost

At the default 4.8 km world and 1 m/px, each layer is 4800 x 4800 pixels, about 23
megapixels and some 90 MB in memory while it is written. See
[performance.md](performance.md) for timings.
