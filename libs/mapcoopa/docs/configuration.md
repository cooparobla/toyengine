# Configuration

Every setting the generator has lives in [`assets/config.yaml`](../assets/config.yaml). Each
key carries a comment with its units and what it does, so that file is the full key
reference. This page covers how settings are resolved, the command-line flags, the world
scale, and the settings that shape the land: landmass shape, climate, terrain and the
elevation surface.

## How settings are resolved

Settings are applied in four layers. Each overrides the one before it.

| | Source | Where |
|---|---|---|
| 1 | `MapConfig`'s in-struct defaults | [`coopa/maps/map_config.h`](../coopa/maps/map_config.h) |
| 2 | the `mapcoopa` tool's own defaults | [`examples/map_generator.cpp`](../examples/map_generator.cpp) |
| 3 | `assets/config.yaml`, or the file `--config=PATH` names | |
| 4 | command-line flags | |

Layer 2 sets an 80-cell grid, 55 rivers, 28 towns and the landmark caps. It exists so that
the tool still produces the full-size world if the config file is missing. The library's
own defaults (layer 1) are smaller: a bare `MapConfig` has a 50-cell grid, 25 rivers and
12 towns.

Every key is optional. A missing key falls back to the layer beneath it, so deleting a line
is always safe. Unknown keys are ignored, so an older binary still reads a newer file.

**A missing config file is a warning; a malformed one is fatal.** With no file, the tool
says so on stderr and continues with layer 2. A file that exists but does not parse stops
the run with the parser's message and exit code 1.

A few keys behave differently:

- **`seed`.** `--seed` wins over it. With neither, the tool draws a seed from system entropy
  and prints it, so the run can be reproduced.
- **Noise seeds.** The tool always derives `noise_island.seed`, `noise_temperature.seed`,
  `noise_blend.seed` and `noise_cave.seed` from the master seed (`seed`, `seed + 1`,
  `seed + 2`, `seed + 3`), whatever the file says. `noise_relief.seed` and
  `noise_terrain.seed` are not derived and can be set in the file. The library does
  **not** derive any of them: a C++ caller that wants one seed to reproduce the whole map
  should set them the same way.
- **`noise_island.frequency`.** A reference value for `grid_size` 40. The tool scales it by
  `40 / grid_size`, so `--grid-size` controls detail rather than the size of the
  landmasses.

From C++, `coopa::maps::load_config()` in [`map_yaml.h`](../coopa/maps/map_yaml.h) applies a
file on top of an existing `MapConfig`. It returns a `ConfigLoadResult` with a `status`
(`Ok`, `NotFound`, `Malformed`), a message, and `has_seed`.

## Command-line flags

`./build/mapcoopa --help` prints the same list.

| Flag | Shipped value | Effect |
|---|---|---|
| `--seed=N` | 42 (from config) | Master seed; reproduces a map exactly |
| `--grid-size=N` | 80 | Cells per axis |
| `--image-size=N` | derived (4800) | Render size in pixels, square. Back-computes `meters_per_pixel` |
| `--rivers=N` | 55 | River sources to attempt |
| `--towns=N` | 28 | Settlements to place |
| `--road-hubs=N` | 32 | Places the road network is routed between |
| `--countries=N` | 5 | Nations to carve out |
| `--regions=N` | 3 | Provinces per nation |
| `--caves=N` | 18 | Cave systems to open, on the steepest slopes |
| `--cave-depth=M` | 260 | How far below its mouth a system may reach, in metres |
| `--cave-grade=F` | 0.25 | Steepness (rise over run) a slope needs to bear a cave |
| `--no-regions`, `--no-landmarks`, `--no-caves`, `--no-roads`, `--no-valleys` | | Skip that feature |
| `--no-subdivide` | | Straight cell boundaries instead of wobbled ones |
| `--shape=S` | `continent` | `rect`, `circle`, `triangle`, `continent` or `archipelago` |
| `--shape-size=M` | 0 (fill) | Width, diameter, edge length or mean continent size, in metres |
| `--shape-height=M` | | Rectangle height; defaults to `--shape-size` |
| `--shape-rot=DEG` | | Shape rotation in degrees (only visible on the triangle) |
| `--shape-count=N` | 4 | Landmasses to attempt (archipelago) |
| `--shape-wobble=F` | 0.35 | How far a coast wanders from a circle, 0 to 0.6 |
| `--relief=F` | 0.65 | Fractal reshaping of the height field, 0 to 1 |
| `--roughness=F` | 0 | Fine terrain detail, 0 to 1 |
| `--incision=M` | 60 | Valley depth rivers cut, in metres |
| `--channel=M` | 18 | River channel depth, in metres |
| `--surface=MODE` | `blended` | `interpolated`, `flat` or `blended` |
| `--blend=F` | 0.1 | `blended`'s blur radius, as a fraction of a cell |
| `--blend-variation=F` | 0.5 | How much that radius varies per cell, 0 to 1 |
| `--temperature=F` | 0 | Shift the whole world warmer or colder, -1 to 1 |
| `--polar=F`, `--polar-north=F`, `--polar-south=F` | 0.065 | Frozen fraction at each pole, 0 to 0.5 |
| `--shading=MODE` | `elevation` | Composite lighting: `elevation` or `hillshade` |
| `--out=PATH` | `map_out` | Output prefix, relative to the working directory |
| `--config=PATH` | `assets/config.yaml` | Settings file to read before the flags |
| `--threads=N` | 0 (all cores) | Worker threads; `1` runs everything serially |
| `--png-level=N` | 8 | PNG deflate effort, 1 to 9; lower is faster and larger |

`--incision` and `--channel` also scale the matching `_per_volume_m` keys, so one flag
deepens the whole river network.

## World scale

Two numbers give the world its size:

```
world extent = grid_size x meters_per_grid_unit = 80 x 60 m = 4.8 km square
render size  = world extent / meters_per_pixel  = 4800 / 1  = 4800 px square
```

`meters_per_grid_unit` (60) is how much ground a cell covers. `elevation_range_m` (600) is
the vertical counterpart: how many metres the normalised `[0, 1]` height field spans.

Every physical size in the config is in metres and has an `_m` suffix, for example
`road_width_m: 6`, `building_size_max_m: 14`, `river_width_base_m: 5`. Counts, costs, scores
and ratios have no suffix.

### Render resolution

`meters_per_pixel` and `image_size` are one setting with two ends. Set either one:

| Set | In | Effect |
|---|---|---|
| `meters_per_pixel: 1` | `config.yaml` | The default. 4.8 km at 1 m/px is **4800 px** |
| `image_size: 2048` | `config.yaml` | `meters_per_pixel` becomes 4800 / 2048 = 2.34 |
| `--image-size=2048` | CLI | The same, and overrides the file |

Setting one computes the other, so they cannot disagree. `image_size` wins if both are set.
The invariant is `image_size * meters_per_pixel == grid_size * meters_per_grid_unit`;
`derive_image_size()` computes it.

At the default 1 m/px, a PNG is a literal one-pixel-per-metre map: a 6 m road is 6 px wide.
Below roughly 4 m/px, thin features stop being reliable. A 7 m river is narrower than a
pixel at 8 m/px, and its stroke fragments.

## Landmass shape

Land is confined to a shape centred on the canvas. Everything outside it is sea. The canvas
is always `grid_size` square; a smaller shape is a smaller world on the same size of map.

| Shape | Dimensions (`shape:` block) | Flag |
|---|---|---|
| `rectangle` | `width_m`, `height_m` | `--shape=rect --shape-size=M --shape-height=M` |
| `circle` | `diameter_m` | `--shape=circle --shape-size=M` |
| `triangle` | `edge_length_m`, equilateral, apex up | `--shape=triangle --shape-size=M --shape-rot=DEG` |
| `continent` | `continent_size_m`, one irregular landmass | `--shape=continent --shape-size=M --shape-wobble=F` |
| `archipelago` | `continent_size_m` x `continent_count`, scattered | `--shape=archipelago --shape-size=M --shape-count=N` |

The library default is `rectangle`; the shipped config uses `continent`. Every dimension
defaults to 0, meaning "fill the canvas". `rotation` in the `shape:` block is in radians (the
`--shape-rot` flag takes degrees and converts).

```bash
./build/mapcoopa --shape=circle      --shape-size=3600
./build/mapcoopa --shape=triangle    --shape-size=4000 --shape-rot=30
./build/mapcoopa --shape=rect        --shape-size=3000 --shape-height=1800
./build/mapcoopa --shape=archipelago --shape-count=5
```

The whole feature is one predicate. `ShapeField::inset()` in
[`map_config.h`](../coopa/maps/map_config.h) returns how far inside the shape a point lies,
and the generator flags anything within `border_length` of the edge. The water pass floods
those cells and the elevation pass measures height outward from them, so coastlines,
mountains, regions and roads all follow the shape without knowing about it.

### Organic shapes

`continent` and `archipelago` give the coast an irregular outline. A landmass is a radius
that varies with angle: a sum of four sine harmonics at frequencies 2, 3, 5 and 7. Whole
numbers close the outline with no seam; coprime numbers keep it from coming out symmetric.
`irregularity` scales how far it wanders, clamped to 0.6, because past that a landmass
pinches in two.

That outline alone is star-shaped (every ray from the centre crosses the coast once). The
boundary is then displaced by `noise_shape`, at `coast_detail` times the mean radius. That
adds inlets, peninsulas that fold back, and the occasional offshore island.

`archipelago` scatters `continent_count` landmasses, each a different size
(`size_variance`), and takes their union. Two that land close together fuse into one larger
continent. The count is an upper bound. Positions come from best-candidate sampling, biased
away from the centre.

Both shapes keep open sea all the way around the canvas. A landmass touching the frame would
be cut off by the border band, and could wall the ocean flood fill out of a bay.

`continent_size_m` is the mean diameter of one landmass, not the size of the world. At 0 it
sizes itself to the canvas.

## Climate

Temperature is a latitude band, minus an altitude lapse rate, plus a noise field, plus a
global offset. It is the first axis `classify_biome()` branches on, so it changes what a map
looks like more than anything except the coastline.

| Key | Shipped | Effect |
|---|---|---|
| `temperature_offset` | 0 | Shifts the whole world warmer or colder. `--temperature=F` |
| `polar_extent_north` | 0.065 | Frozen fraction of the map at the `y = 0` edge, 0 to 0.5. `--polar-north=F` |
| `polar_extent_south` | 0.065 | The same at the far edge. `--polar=F` sets both |
| `temperature_falloff` | 1.7 | Shapes the curve between the polar cap and the equator |
| `temperature_lapse_rate` | 0.40 | How much a full unit of height cools the air |

A polar extent of 0 means no ice cap on that side. The two poles are independent. With an
extent of 0, latitude alone never produces ice, glacier or cold desert; altitude still can.

```bash
./build/mapcoopa --polar=0                         # no ice caps
./build/mapcoopa --polar-north=0.3 --polar-south=0 # ice at the top only
./build/mapcoopa --temperature=-0.25               # ice age
./build/mapcoopa --temperature=0.25                # hothouse
```

The lapse rate works on the normalised height field, so `elevation_range_m` does not change
it. `sea_level` does: a higher waterline pushes land to higher normalised elevations.

There are no moisture controls. Moisture is derived from lakes, rivers and distance to the
coast, then normalised to span the full range on every map. Only its arrangement changes,
through the river count, sea level and lake threshold.

## Terrain

Height starts as breadth-first distance from the coast. That keeps coastlines at sea level
and puts mountains inland. On its own it puts every summit on a thin ridge halfway between
the bays on either side, which looks artificial. Three settings work at three scales:

- **`terrain_relief`** (0 to 1, shipped 0.65) blends the distance field toward fractal
  noise, so the interior becomes massifs and valleys. A coastal mask keeps the shore the
  lowest land. 0 restores the pure distance field of the original algorithm. `--relief=F`.
- **`elevation_smoothing_iterations`** relaxes the stored height field. It controls how
  smooth the landform is, not how smooth the render looks. Use `elevation_surface` for
  that.
- **`terrain_roughness`** (0 to 1, default 0) displaces the sampled surface with a finer
  detail field. `--roughness=F`.

Roughness and river channels live in the sampler (`MapGraph::elevation_at()`), not in the
graph. Cell and corner heights stay as the passes computed them, so biomes, rivers and roads
are classified on the smooth control field. The roughness is scaled by local height, so the
coastline stays at sea level.

`elevation_at()` interpolates barycentrically over the Delaunay triangle that contains the
sample, blending the three cell-site heights. This is the piecewise-linear surface through
the sites, which is what a terrain mesh built from the data would be.

## Elevation surface

`elevation_surface` decides how the height field is drawn between the cells it is stored at.

| Mode | Effect |
|---|---|
| `interpolated` (library default) | Barycentric over the Delaunay triangle. Continuous. |
| `flat` | One height per cell, with a hard edge at every boundary. |
| `blended` (shipped config) | `flat`, then the finished raster is box-blurred, then river channels are cut back in. |

```bash
./build/mapcoopa --surface=flat
./build/mapcoopa --surface=blended --blend=0.5
./build/mapcoopa --surface=blended --blend=0.5 --blend-variation=0.8
```

`elevation_blend` is the blur radius as a fraction of a cell (library default 0.5, shipped
0.1). 0 is byte-identical to `flat`. 1 is the ceiling; past it the blur starts erasing
landforms. The channels are cut after the blur because blurring a raster that already has
them costs a river nearly half its contrast.

`flat` drops `terrain_roughness` on purpose, because roughness is surface texture. Rivers
still show in `flat`: the channel cut depends on position, not on interpolation.

These settings only change how the surface is drawn. They are not
`elevation_smoothing_iterations`, which changes the stored heights.

### Varying the blur per cell

`elevation_blend_variation` (library default 0, shipped 0.5) gives every cell its own blur
radius, drawn from `noise_blend` at the cell's site. The factor is `1 + variation * n` for a
field value `n` in `[-1, 1]`, so the average radius stays at `elevation_blend`. At 0 the
field is never read.

`noise_blend.frequency` sets the patch size: the field's wavelength `1 / frequency` is
measured in cells. Measured as the correlation between the factors of two cells that share
an edge:

| `frequency` | wavelength | neighbour correlation | reads as |
|---|---|---|---|
| `0.5` | 2 cells | -0.01 | every cell independent |
| `0.33` | 3 cells | 0.14 | mostly independent |
| `0.25` | 4 cells | 0.35 | loose clumps |
| `0.2` | 5 cells | 0.51 | clear patches |
| `0.125` | 8 cells | 0.78 | broad regions |
| `0.045` | 22 cells | 0.97 | no visible variation |

The useful range is 0.5 down to 0.125. The field uses one octave, because octaves above the
first are finer than a cell and add only noise when sampled once per cell. It is not
rescaled by `grid_size`, unlike `noise_island`.

The variation is built as a three-level pyramid: the sharp raster, a blur at the base
radius, and a blur at twice it. A per-cell factor, feathered by the blur radius (capped at a
quarter cell), chooses where between them each pixel lands. Three globally consistent blurs
cannot seam, where a box blur with a true per-pixel radius would.

The variation scales `elevation_blend`, so a small blend leaves little to vary. At
`elevation_blend = 0.1` the radius is 3 to 4 px on a 60 px cell.

It affects the elevation layer and the composite's shading, including hillshade.
