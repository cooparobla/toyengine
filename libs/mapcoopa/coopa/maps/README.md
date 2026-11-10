# Maps Module (`coopa::maps`)

The `maps` module generates whole world maps from a seed: Voronoi cells carrying an
elevation, a climate and a biome, threaded with rivers that run downhill to the sea and a
road network routed between the places worth travelling between, graded by the traffic
each stretch carries; nations divided into provinces whose borders settle on
ridgelines and coasts; settlements placed where people would actually live, each with a
population counted from its buildings and a name in its region's own invented language;
and landmarks read off the terrain itself. It is a port of Amit Patel's *Polygonal Map Generation*
([Red Blob Games](https://www.redblobgames.com/maps/mapgen2/)), reworked to fit this
library.

The module is its own repository rather than part of a graphics package because none of
it depends on Vulkan, GLFW or any sibling repository beyond libcoopa — it produces
**data**. The two PNG renderers are a debugging convenience, deliberately software-only;
the deliverable is a `MapGraph`, and `map_yaml.h` writes one to disk so a game can load a
world it did not generate itself.

Everything is header-only and reproducible: a `MapConfig` plus its `seed` fully
determines the output, byte for byte, including the settlements and the wobble on every
coastline.

---

## Maps Module Architecture

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                          MAPS MODULE DATA FLOW                                │
└──────────────────────────────────────────────────────────────────────────────┘

   ┌──────────────┐
   │  MapConfig   │  grid size, seed, noise field, per-pass knobs, town rules
   └──────┬───────┘
          │
          ▼
   ┌──────────────────────────────────────────────┐
   │              MapGenerator                     │
   │                                               │
   │  jittered grid ──► Delaunator ──► Voronoi dual │
   └──────┬───────────────────────────────────────┘
          │ builds, then annotates in place
          ▼
   ┌───────────────────────────────────────────────┐        ┌───────────────────┐
   │                 MapGraph                      │        │      passes/      │
   │                                               │◄───────│                   │
   │  centers[]  cells (biome, elevation, water)   │        │  1 water          │
   │  corners[]  Voronoi verts  (rivers, downslope)│        │  2 coast          │
   │  edges[]    Delaunay + Voronoi edge, shared   │        │  3 elevation      │
   │  roads[]    routed runs, graded by traffic    │        │  4 temperature    │
   │  rivers[]   smoothed watercourses             │        │  5 rivers         │
   │  towns[]    settlements + packed buildings    │        │  6 valleys        │
   │  regions[]  provinces, countries[] nations    │        │  7 moisture       │
   │  landmarks[] notable places                   │        │  8 biomes         │
   │  caves[]    systems, floor and roof per node  │        │  9 roads          │
   │                                               │        │ 10 regions        │
   │  all adjacency by index, never by pointer     │        │ 11 towns          │
   └──────┬───────────────────────────┬────────────┘        │ 12 landmarks      │
          │                           │                     │ 13 caves          │
          │                           │                     │ 14 noisy edges    │
          ▼                           ▼                     └───────────────────┘
   ┌─────────────────┐        ┌─────────────────┐
   │  map_renderer.h │        │   map_yaml.h    │
   │  MapLayers      │        │  save_map()     │
   │  9 image layers │        │  load_map()     │
   └────────┬────────┘        └─────────────────┘
            ▼
   ┌─────────────────┐
   │ image_writer.h  │  ──►  9 .png layers
   └─────────────────┘
```

Cells, corners and edges refer to each other by `CenterId` / `CornerId` / `EdgeId`
indices into `MapGraph`'s three arrays rather than `shared_ptr`s, which in both directions
would form reference cycles and leak the whole graph on every generation. The cost is one invariant every pass must respect: a record's slot is
its own index, so a pass that needs ranked order sorts an index array, never the storage.

---

## File Breakdown

### [`map_config.h`](./map_config.h)
`MapConfig` (grid density, seed, thresholds, fourteen pass toggles), `NoiseConfig` (typed
against FastNoiseLite's own enums rather than raw `int`s cast at the point of use),
the per-pass configs (`RoadConfig`, `RegionConfig`, `TownConfig`, `LandmarkConfig`,
`CaveConfig`), the landmass `MapShape` / `ShapeConfig` / `ShapeField`, and `BiomePalette`. The palette is a flat array indexed by
`Biome`, so the renderer looks a colour up per cell rather than running a string comparison
chain.

### [`biome.h`](./biome.h)
The `Biome` enum (33 entries), its stable `snake_case` serialisation names, and
`classify_biome()` — a Whittaker table in three dimensions, branching on temperature
first, then elevation, then moisture. Temperature comes first because latitude is the
strongest control on what grows: the same height and rainfall give taiga at the pole and
rain forest at the equator. Names are append-only; they are the on-disk identity of every
saved map.

### [`name_generator.h`](./name_generator.h)
`Language`, a synthetic phonology, and `generate_name()`. Each country draws its own
inventory of onsets, vowels and codas, so its settlements share a sound and its neighbours
do not; regions get a dialect of their country's. `language_for()` and `dialect_for()`
rebuild a language from an id, so any pass can name a place in the right voice without a
language being passed around or serialised.

### [`cave.h`](./cave.h)
`CaveZone` (`Vadose` / `Phreatic`) and `CaveFeature` (`Passage`, `Chamber`, `Shaft`, `Sump`),
with their stable `snake_case` names, exactly as `building.h` carries the building roles. The
zone is *recorded* rather than inferred from the floor height, because which regime cut a
passage depends on where that system's mouth opened and on nothing about the passage in hand.

### [`building.h`](./building.h)
`BuildingRole` (`Dwelling` plus the civic roles) and its stable `snake_case` names, append-only
like the biome names.

### [`landmark.h`](./landmark.h)
`LandmarkKind`, its names, the descriptive noun each takes, and `landmark_suits_biome()` —
the gate that stops a volcano appearing on ice or an oasis outside a desert.

### [`region.h`](./region.h)
Convenience include for `MapRegion` and `MapCountry`, which are declared in `map_data.h`
alongside the graph that owns them.

### [`map_data.h`](./map_data.h)
`MapPoint`, `MapCenter`, `MapCorner`, `MapEdge`, `MapTown`, `MapBuilding`, `MapRegion`,
`MapCountry`, `MapLandmark`, `MapCave` and the `MapGraph` that owns them, plus the geometry a
consumer needs to place them:
`building_corners()` gives a footprint's four rotated corners — the authoritative shape,
since containment and non-overlap are guaranteed against those rather than an
axis-aligned box — with `point_in_polygon()` and a separating-axis `buildings_overlap()`
alongside. Also `MapGraph::cell_outline()`, which assembles a cell's
polygon in winding order by walking its corners and chaining the edge paths between them
— necessary once edges are subdivided, because a wobbled outline is not convex and
cannot have its winding recovered by sorting points about their centroid.

### [`noise.h`](./noise.h)
`Noise`, a FastNoiseLite instance configured once at construction and shared across a
pass, so no sample pays for rebuilding the generator.

### [`map_generator.h`](./map_generator.h)
`MapGenerator` — lays the jittered point lattice and its boundary ring, triangulates with
`delaunator`, reads the Voronoi dual off the half-edges, flags the border band, then runs
the enabled passes in order. Exposes the finished graph through `graph()`.

### [`passes/`](./passes/)
The fourteen annotation stages, one header each. See [`passes/README.md`](./passes/README.md).

### [`image.h`](./image.h) and [`image_writer.h`](./image_writer.h)
`Image` (an 8-bit interleaved buffer, not a texture — mapcoopa carries no graphics
dependency) plus half-space triangle fill, convex polygon fan fill and distance-tested line
strokes (`draw_line()`); `image_writer.h` adds `write_png()`.

### [`map_renderer.h`](./map_renderer.h)
`MapLayers` renders nine views of a map — elevation, water surface, biomes, roads,
structures, landmarks, regions, a lit composite of all of them, and a readable cave overview.
Cave *geometry* is deliberately not among them and is not rastered at all: a branching network
at several depths does not fit a stack of heightmaps without being both flattened and
quantised, and the saved map already carries every station and passage at full precision, so
the picture would be a lossy and much larger copy of the document. See
[docs/caves.md](../../docs/caves.md#storage) for the measurements.

What the composite does carry is a ring at each cave mouth, drawn by `draw_markers_()`
alongside the town and landmark markers so the composite and the landmarks overlay cannot
disagree about it. A passage is underground; a mouth is a hole in a hillside. The three overlay layers are RGBA on transparency so they stack; the rest
are RGB. Every layer is drawn at the same
scale, so at the default one pixel per metre a width measured off a render is a
measurement of the ground. See [docs/output.md](../../docs/output.md#layers) for the file
list and the [repository README](../../README.md#legend) for the colour legend.

The composite lights its biome colours by `MapConfig::composite_shading`. `Elevation`, the
default, is a function of height read straight from `MapGraph::elevation_at()` — high
ground pale, and the same height the same brightness anywhere on the map. `Hillshade` is a
function of *slope* instead: more sculptural, but it cannot tell a slope at sea level from
the same slope on a summit.

Hillshading needs the *rasterised* elevation layer, blurred by a fraction of a cell first.
Elevation inside a cell is interpolated from that cell's own corners, so the surface is
continuous across a shared edge but its slope is not — and shading straight off
`elevation_at()` draws every cell as its own little dome. Smoothing first leaves the relief
of the landscape. Elevation shading needs none of that machinery, because the cusps that
ruin a *gradient* taken from that function are harmless to its *value*.

All of it is a debugging aid: a consumer wanting a smooth heightfield should sample
`MapGraph::elevation_at()` rather than re-derive it from a lossy 8-bit image.

`elevation_at()` interpolates barycentrically over the Delaunay triangle containing the
sample, blending the three cell-site heights at its vertices — the natural piecewise-linear
surface through samples taken at the sites. Two alternatives are worse: inverse-distance
weighting over a cell's corners reads as a plateau, and barycentric over the cell's own
corner fan creases six times per cell, puts a tent pole at every site, and leaves some 1.8%
of drawn pixels outside every triangle because the fan covers the straight corner polygon
while the renderer draws the subdivided one. An overload
takes a `TerrainDetail` — a borrowed `Noise` plus an amplitude, built by
`make_terrain_detail()` — and displaces the result, tapered by the local height so a
coastline stays at sea level. A further overload takes a `RiverChannels` from
`make_river_channels()` and cuts the watercourses into the surface, at the width
`river_width()` gives them, subtracted last so the roughness cannot fill the bed back in.

Both deliberately live in the sampler and not in the graph: cells and corners stay the
smooth control field that biomes, rivers and roads are classified from. For the channel
that is not merely tidy, it is the only place it *can* live — sites are 60 m apart and a
river is 5 to 20 m wide, so carved into the mesh the sharpest achievable river is a 500 m
depression with no edge. Measured contrast half a cell from a centreline: **+2.3 grey
levels** carved into the mesh, **+12.3** cut at sample time.

`RiverChannels` stores its segments flat, grouped by cell, each group with a bounding box
the sampler tests before it measures any distance. A segment is filed under every cell its
corner *touches*, which is what keeps the surface continuous across a boundary a river runs
along: three cells share a corner and two share an edge, so both sides see the same segments
and compute the same cut.

`MapCenter::water_level` is the *surface* of whatever water covers a cell, flat per body,
as against `elevation` which is the height of the ground underneath. That distinction is
the whole reason the water layer can draw a sheet: drawing `elevation` would draw the sea bed.
A lake stands at its **spill level** -- just under the lowest cell of its shore -- over a
**basin**: at least `lake_depth_m` under the surface anywhere, and deeper by `lake_bed_grade`
(rise over run, in real metres) for every metre in from the shoreline, to a floor of
`lake_depth_max_m` (`passes/water_levels.h`; settled again after the valleys pass, which can cut
a shore). Every shore cell therefore stands above its lake. The distance in from the shoreline is
a Dijkstra walk over site-to-site distances inside the body, seeded half a step out from each dry
neighbour (`shoreline_distances()`) -- real distance, so an elongated lake gets an elongated
trough, and seeded at the *boundary* rather than at the shore cells, or a small lake, whose every
cell is a shore cell, would read zero everywhere and come out flat however wide it was. One
depth for the whole body makes every lake a flat pan instead -- a bed spread of exactly zero
metres, on every lake of every seed.

`RiverChannels::banked` is each river's *physical* surface (`RiverFit::Banked`): decided from the
uncut ground before the channel is cut -- under both banks, only ever falling, backed up to the
body it feeds near its mouth -- and the cut is then made deep enough to hold it. Each river with a
body to feed gets one more channel segment, its **mouth**, from its last point to the nearest cell
of that body whose bed is really under water, held at the body's level: a coastal water cell's bed
can stand above its surface, and without the mouth a strip of dry shore would dam the river.
`make_river_surfaces()` still returns the 2D water layer's fit (`RiverFit::Sheet`) by default,
which stands a ribbon on the highest ground under its stroke so the image hides the bed.

`MapConfig::sea_level` is a real height partway up the field, not zero, so the sea bed sits
below it and land above — which is what makes "under water" a comparison rather than a
tautology, and what makes the sea's surface visible on the same greyscale as the ground.
Land therefore spans `[sea_level, 1]`, and any threshold describing land goes through
`land_height()`. `elevation_range_m` gives the field a vertical scale in metres, so depths
and overhangs can be stated physically like every horizontal size already could.

### Terrain models
`MapConfig::terrain_model` picks the ground `elevation_at(center, x, y, detail)` samples (build the
detail with `TerrainSurface(graph, config)`). **Classic**: flat triangles between the cell sites, plus
`terrain_roughness`. **Natural** (`NaturalTerrainConfig`, noise fields `noise_hills`, `noise_ridge`,
`noise_plateau`, `noise_warp`): the sites' heights joined smoothly (eased barycentric weights, so it
still passes through every site and stays within its triangle's range), then shaped by landform
through a warped field -- ridged mountain ranges on high ground, rolling hills, flat-topped
plateaus. Near seas and lakes (blended cell rings from the water) the shaping may only raise the
ground, so every water guarantee holds on both (`water_field_test` runs both). An elevation render of
a natural map samples the surface directly whatever `elevation_surface` says.

Every landform above is gated on height *above* the waterline, so all of them are exactly zero
below it. **Under the water the bed has its own two** (`seabed`, `seabed_ridges`, field
`noise_seabed`): rolling relief faded in over the first `seabed_shelf` of depth, and ridges and
gullies faded in past it, so a shore shelves smoothly and only the open floor is broken up. They
are *added* to the land shaping rather than replacing it, which is what keeps the surface
continuous across a lake's shoreline -- a lake stands above `sea_level` and so reads as land
height, where a branch would leave a step there. A rise is capped at half the water's remaining
depth, so a shoal can never break the surface and strand dry ground in open water; a fall is
capped at half the height left under it, so the deep floor rises into seamounts rather than being
gouged through the bottom of the field. Without them a sea floor moves 0.1 m per 3.75 m of ground
where the land beside it moves 1.9 (`seabed_test`).

Both shapings are tested before they are sampled, which is what makes the second set free: the
land shaping is *exactly* zero wherever the blended cell ring is (both its gates are
`smoothstep(., ., 0)`), and a ring of zero means every cell of the triangle is a water cell --
the open sea, and most of a map. So a submerged sample reads the bed's two fields and skips the
land's four. Measured over the standard map's whole grid, 1.44 M samples: **500 ms** for the land
fields everywhere and no bed, **456 ms** for the bed shaped and the land fields skipped where they
cannot matter.

### [`water_field.h`](./water_field.h)
`WaterField::at(cell, x, y)` -- the counterpart of `elevation_at()` for water, for a consumer
meshing the map in 3D: whether a point is under water, which body (`Sea`, `Lake`, `River`), how
high its surface is, and a river's downstream direction. A sea or lake reaches every point whose
Delaunay triangle has one of its cells as a vertex, a river the points in its channel; the point
is wet where the ground lies below the surface. With every shore above its lake and every river
under its banks, the water never stands above the dry ground beside it (`water_field_test`).

### [`map_presets.h`](./map_presets.h)
`standard_config()` -- the standard map, exactly what a bare `mapcoopa` run generates (the CLI and
the viewer start from it; `presets_test` holds it to `assets/config.yaml`) -- and
`derived_config()`, the values every host derives from a config before generating: per-pass
noise seeds from the master seed, the island field scaled to the grid, and `image_size`. Skip it
and the seed barely moves the coastline.

### [`map_task.h`](./map_task.h)
`MapTask` — the token an asynchronous generation or export is observed through:
`done()`, `progress()`, `cancel()`, `wait()`. Move-only, and its destructor cancels and
waits, because the work writes into storage the caller owns. With no `JobEngine` injected
the operation has already run inline by the time a task exists, so there is one API and one
call pattern either way.

### [`map_export.h`](./map_export.h)
`MapExporter` — renders all nine layers and writes them, in parallel. Its own header rather
than more of `map_renderer.h` because exporting needs `image_writer.h`, which carries the
stb *implementation*; a consumer rendering a layer into a texture should not have to link an
encoder.

Two levels of parallelism, both by disjoint output so the result is bit-identical to a
serial run: across layers (which is the only way to overlap PNG encodes, stb's deflate
being one opaque call per image) and across row bands within a layer. Nesting them is safe —
`JobEngine::wait_for()` has a waiting worker participate rather than idle.

### [`portable_random.h`](./portable_random.h) and [`portable_sort.h`](./portable_sort.h)
`UniformIntDistribution`, `UniformRealDistribution`, `shuffle()` and `sort()`: reimplementations
of libstdc++'s algorithms, so a seed produces the same world with libc++ (macOS) as with
libstdc++ (Linux). Every pass uses these instead of the `std::` versions.

### [`map_yaml.h`](./map_yaml.h)
`save_map()` / `load_map()`, and the `map_to_node()` / `map_from_node()` pair beneath
them. Writes the entire graph — every cell, corner, edge, settlement and adjacency list —
so the document is a save of generator state rather than a derived export. Built on
`coopa::collections::YAMLMap`, with the bulk arrays assembled as `fkyaml::node` sequences
directly. Float fields are lossy to six significant digits; see the header comment.

Also `load_config()`, which reads a standalone settings file such as
[`assets/config.yaml`](../../assets/config.yaml). It shares `apply_config_node()` with the
`config:` block embedded in a saved map, so the two can never disagree about what a key
means. It *applies* onto an existing `MapConfig` rather than returning a fresh one — an
absent key leaves the caller's value alone, which is what lets a tool set its own defaults
first and let the file override only what it mentions — and it returns a `ConfigLoadResult`
rather than printing: the caller decides whether a missing or malformed file is fatal, and
`has_seed` tells it whether the document named a seed or merely inherited the default.

---

## Usage Example

```cpp
#include <coopa/debug/logger.h>
#include <coopa/maps/image_writer.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_yaml.h>

coopa::maps::MapConfig config;
config.grid_size = 80;
config.seed = 251;
config.noise_island.seed = config.seed;  // one seed reproduces the whole map
config.towns.town_count = 28;
config.regions.country_count = 5;

coopa::debug::Logger logger("mapgen");
coopa::maps::MapGenerator generator(config, logger);
generator.generate();

const coopa::maps::MapGraph& map = generator.graph();

// Read the map as data.
for (const coopa::maps::MapCenter& cell : map.centers) {
    if (cell.ocean || cell.water) {
        continue;
    }
    spawn_terrain(cell.point.x, cell.point.y,
                  coopa::maps::biome_name(cell.biome),
                  cell.elevation);
}
for (const coopa::maps::MapTown& town : map.towns) {
    spawn_settlement(town.name, town.point.x, town.point.y, town.tier,
                     town.population, town.buildings);
}
for (const coopa::maps::MapCountry& country : map.countries) {
    register_nation(country.name, country.population, country.regions);
}
for (const coopa::maps::MapLandmark& landmark : map.landmarks) {
    spawn_landmark(landmark.name, landmark.kind, landmark.point.x, landmark.point.y);
}

// Persist it, and render a preview.
coopa::maps::save_map(map, config, "world.yaml");
coopa::maps::write_png("world_composite.png",
                       coopa::maps::MapLayers::composite(map, config));

// Load it back in a later session.
coopa::maps::MapGraph loaded;
coopa::maps::MapConfig loaded_config;
coopa::maps::load_map("world.yaml", loaded, loaded_config);
```

A runnable version of the above ships as [`examples/map_generator.cpp`](../../examples/map_generator.cpp),
built as the `mapcoopa` target — the repo's bare project name, so `cplay` forwards its
arguments straight through to it:

```sh
cbuild                              # configure + build
cplay                               # random seed, writes ./map_out.*
cplay --seed=251 --out=/tmp/m       # reproducible
cplay --countries=8 --towns=40      # a busier world
cplay --help
```
