# toyengine/world

World-scale ground. **One terrain component, two modes** -- `toy::world::TerrainComponent`, with
`mode:` choosing which model it carries its shape in:

| `mode:` | What it is | Drawn as |
|---|---|---|
| `tiles` (default) | a streamed 3D tile world of integer-height columns | a welded mesh of a tile set's pieces, per chunk. Most of this file |
| `heightmap` | a sculptable, paintable height grid for open worlds | one shared patch per chunk, GPU-displaced and tessellated. See [Heightmap terrain](#heightmap-terrain) |

Three names load, all the same component: **`Terrain`** is the unified spelling, and
**`TileTerrain`** / **`HeightmapTerrain`** are what scenes say and what the editor shows for each
mode. A block with no `mode:` is read from its keys (`tile_set` / `tiles` / `generator` mean
tiles; `heightmap` / `resolution` / `size` mean heightmap), and silence means tiles -- which is
what a bare `Terrain` has always meant. So no scene ever had to be migrated.

### What the two modes share

Both modes are the SAME storage now, not two stores behind a forking facade. One
[`TerraData`](terra_data.h) ([`terra_geometry.h`](terra_geometry.h) for its shape,
[`terra_codec.h`](terra_codec.h) for its one `.terra` file, [`terra_store.h`](terra_store.h) for
the process-wide instance keyed by file) holds the tile grid, an optional float height plane, the
two splat planes, and every tile's water/biome/climate fields together -- whichever mode draws it.
`TerrainMode` only decides which **meshing style** reads that data: Heightmap is "draw the float
plane, GPU-displaced, tessellated"; Tiles is "draw the grid's integer `steps`, welded from a tile
set's pieces". Nothing about biome, water, or a painted layer is mode-specific storage any more --
switching mode is switching which system streams the SAME field, not swapping to a different file.

- **The data** -- [`terrain_field.h`](terrain_field.h)'s `TerrainField`: a thin, non-owning
  wrapper over one `TerraData` plus the local<->world geometry a brush, GroundProbe or gameplay
  needs. The **sub-resolution** (`height_sub`) is what determines detail: `1` means no height
  plane exists at all and a tile's own `steps` IS its height (the terraced read); `>1` is a real
  float plane, continuous, bilinear. One API answers `ground_at`, `normal_at`, `layer_weight_at`,
  `record_at` and `water_at` whichever way a given terrain is shaped, which is why GroundProbe,
  the foliage scatter and gameplay no longer care which they are standing on.
- **The layers** -- one authored `layers:` list (up to `k_terrain_layer_count`), named the same
  way in both modes. A heightmap tile blends them by painted splat weights; a tiles-mode tile
  names one per face through its 4-bit surface nibble. Left empty, a tile world uses the built-in
  set (`builtin_tile_layers()` in [`tile_types.h`](tile_types.h)) and looks exactly as it always
  has.
- **The water** -- a kind (None / Ocean / Lake / River), a surface **level** (absolute metres,
  exact in both modes) and a river's flow, per tile (`TileCell`/`WaterCell`). Depth is never
  stored; it is `level - ground`. See [Water](#water).

### What differs at runtime

A shared field makes the two modes *virtually* interchangeable, not identical -- the two
streaming systems (`TerrainSystem`, `HeightmapTerrainSystem`) and their meshers stay distinct, so
a handful of runtime gaps are accepted rather than papered over. See
`assets/scenes/terrain/unified_terrain_demo` for all of them against one shared map, and the
non-parity list in its scene header:

- The water plane sits 0-1 m lower on tiles than on the heightmap -- the terraced mesher always
  reads the tile's real integer `steps`, never a fresh rounding of a height plane
  (`tile_column()`'s `floor(water_level / height_step)` in `terra_data.h`), and subtracts an
  inset from that quantised step; the heightmap mesher uses the stored continuous level raw. This
  is now a meshing-time quantisation, not a second, separately-authoritative water field --
  both modes read the same `TileCell::water_level`.
- The shallow shelf's shape differs even though which tiles are wet does not: tiles get a flat
  terrace `water_inset` deep; the heightmap's bed is continuously graded (`grade_water_shore()`,
  below) and falls away at whatever rate the generator's own lake/shore grading gave it.
- Tiles are solid only within `collision_radius` chunks of the camera; the heightfield is one
  whole-grid collider, solid everywhere.
- Switching back to a heightmap that evicted (see **Eviction** under Heightmap terrain below)
  re-decodes every layer PNG synchronously -- a brief hitch tiles never have.
- A terrain's first activation bakes once, lazily, per session -- one frame of stale history
  (TAA ghosting) plus the bake time, on tiles and heightmap alike.

### What generates a world is not here

Map generation lives in [`toyengine/mapgen`](../mapgen/README.md). A `MapGenerator` component
holds a [mapcoopa](../../libs/mapcoopa) world and **applies** it to a terrain -- in either mode --
which is the same gesture as importing a heightmap PNG. A terrain keeps the applied map in
`applied_map` so gameplay can still read the biomes, regions and rivers behind the ground.

## TileTerrain

A streamed 3D tile world, built from a [mapcoopa](../../libs/mapcoopa) `MapGraph`.

The map generator produces a 2D world — elevation, biomes, rivers, roads, towns. This module
**bakes** it once into a stored grid of tiles (integer-height columns plus their climate — see
[Stored tiles](#stored-tiles)), and from then on the tiles *are* the terrain: edited in the
editor's Tile Mode or by gameplay, saved in a `.terra` file. Every column is assembled from a
**tile set**'s authored pieces, chosen by its neighbourhood — a top always, walls only where a
neighbour is lower. Every piece in a chunk merges into one GPU mesh (and one collision mesh)
built on the job system.

**The tile set is the template.** A tile set is an object asset
(`assets/objects/terrain/tileset_*.yaml`) with one child per piece: three top quadrants, wall
caps and walls for straight runs, outer and inner corners. The terrain's whole form comes from
it, so changing the form is one key:

```yaml
- type: TileTerrain
  tile_set: objects/terrain/tileset_round     # soft rounded lips and corners (Animal Crossing)
# tile_set: objects/terrain/tileset_voxel     # square blocks
  kind_tile_sets:                             # optional: some surfaces from another set
    stone: objects/terrain/tileset_rock
  generator: { seed: 7, sea_level: 0.3 }      # mapcoopa's own keys; the rest is its standard map
  water: { clarity: 2.0 }                     # WaterBody keys for the sea, lakes and rivers
  collision: true                             # each chunk collides with what it draws
  material:                                   # separate from the tile set: the palette atlas
    texture_albedo: textures/terrain_atlas.png
```

The stock sets are `round`, `soft`, `rock` and `voxel`. There is no mode switch: the voxel
set is the same eleven pieces, square, and goes through the same mesher as the round one. A
terrain without `tile_set` uses `tileset_voxel`. In the editor, the TileTerrain panel picks the
tile set from a list, opens or duplicates it, and sets the per-surface overrides. See
[Tile sets](#tile-sets-the-template-as-one-object).

Older scenes still load: `styles:` / `kind_styles:` map onto the same thing, and the retired
voxel mesher's `side_mesh:` / `sides:` fall back to `tileset_voxel` with a warning. The panel's
*Convert to Tile Set* button rewrites them.

Demo: `./build/toyengine terrain_demo` ([assets/scenes/terrain/terrain_demo](../../assets/scenes/terrain/terrain_demo)),
rounded, with dynamic bodies dropped onto it. Its header says how to switch it to the voxel set.

| File | Purpose |
|---|---|
| [`tile_types.h`](tile_types.h) | The vocabulary: `TileFace` (the six sides), `TileKind` (the twelve atlas surfaces), `TileColumn`, `ChunkCoord`, and `variant_transform()` — the eight D4 orientations a piece is baked into. |
| [`tile_topology.h`](tile_topology.h) | The tile classifier: `TilePiece` (the eleven pieces a tile set is made of), `WallEnd`, `classify_quadrant()`, `classify_wall_end()`, `ColumnPad` (a chunk's columns plus a one-tile neighbour skirt), and the surface blend codes. Pure functions from a column's neighbourhood to a piece and one of eight orientations. |
| [`tile_mesh_library.h`](tile_mesh_library.h) | `TileMeshLibrary` — bakes each tile set's pieces into all eight orientations once, **derives** their section caps, and `append_styled()`s one into a chunk's buffers with a scale, a translate and an index rebase. Geometry is **copied** out of its asset handles, so a baked library is immutable and safe for concurrent readers. |
| [`terra_geometry.h`](terra_geometry.h) | `TerraGeometry` -- the authored shape of a `TerraData`: `tiles_x`/`tiles_y` (independent -- no forced-square rule), `tile_size`, `height_sub`/`layer_sub`, `height_step`, `height`, `chunk_tiles`. The ONE authority every plane's extent derives from. |
| [`terra_data.h`](terra_data.h) | `TerraData` -- the one CPU store: the tile plane (`TileCell`, 24 bytes), an optional dense float height plane, two RGBA8 layer planes, three change journals and the bake handshake. Pure CPU, unit-testable with nothing else. |
| [`terra_codec.h`](terra_codec.h) | `save_terra()` / `load_terra()` -- the one `.terra` file format: a plane table (additive, not a version bump) over 32 x 32-tile storage chunks, SoA-transposed then zlib'd, with atomic load-into-scratch. |
| [`terra_store.h`](terra_store.h) | `TerraStore` -- the process-wide owner of every terrain's `TerraData`, keyed by its `.terra` file, surviving the editor's re-parse-on-every-change. Replaces `TileMapStore` + `HeightmapStore`. |
| [`terrain_field.h`](terrain_field.h) | `TerrainField` -- a thin, non-owning wrapper over one `TerraData` plus the local<->world geometry every consumer wants: `ground_at`, `normal_at`, `layer_weight_at`, `record_at`, `water_at`, the change cursor, and `TerrainFieldView` for world-space queries. |
| [`terrain_types.h`](terrain_types.h) | The vocabulary both modes share: `IRect`, `TerrainMode`, `WaterKind`, and `TerrainLayer` -- one authored surface, named the same way in either mode. |
| [`terrain_params.h`](terrain_params.h) | `TerrainParams` -- how a tile world is cut into tiles and chunks. Passed by value, so a meshing job holds its own copy. |
| [`terrain_water.h`](terrain_water.h) | The water operations over a field: flood, drain, settle, and the resumable `WaterSettle` a frame's budget is spent on. Shared by both modes, the editor's brush and gameplay. |
| [`terrain_water_mesh.h`](terrain_water_mesh.h) | The water surface over a **smooth** chunk: flat at the water's level, one tile past the waterline so the ground ends it, with every corner averaged by global tile coordinate so two chunks cannot crack their shared edge. |
| [`terrain_views.h`](terrain_views.h) | Every terrain in a scene as one world-space query surface, whichever mode each is in -- what GroundProbe walks. |
| [`terrain_sampler.h`](../mapgen/terrain_sampler.h) | `TerrainParams` (the whole tiling: tile size, height step, chunk size, view radius) and `TerrainSampler` — owns the generated `MapGraph` and answers `sample(tile_x, tile_y) -> TileColumn`. Adds the point-to-cell lookup mapcoopa has no need for: a nearest-site query over a uniform bucket index. Const after `build()`, hence concurrently readable. |
| [`terrain_chunk.h`](terrain_chunk.h) | The mesher. `sample_chunk_columns()` reads a chunk plus a one-tile neighbour skirt; `mesh_chunk_styled()` assembles its columns into a `ChunkMeshData`; `build_chunk_collision()` turns that into a static collision mesh. Pure — no Vulkan, no Scene, no AssetManager — which is what lets it run on a worker and be tested with no device. `ChunkBuildJob` bundles one job's inputs and outputs into a single shared object. |
| [`terrain_component.h`](terrain_component.h) | `TerrainComponent` — the scene-authored `TileTerrain` component (old name `Terrain`) (tile set, stored tiles, generator keys, tiling, collision, material) plus the long-lived state: the bake, the baked library and the chunk table, and gameplay queries (`tile_at`, `record_at`, `height_at`). Carries no per-frame logic. |
| [`tile_edit.h`](tile_edit.h) | The pure edit operations over a `TerrainField&`: `tile_raise()`, `tile_set_height()`, `tile_paint()`, `tile_smooth()`, `tile_water()` -- shared by the editor's brushes and by gameplay. (`tile_water.h` is a compatibility alias for `terrain_water.h`, below -- both spellings of the same operations.) |
| [`terrain_generator.h`](../mapgen/terrain_generator.h) | The `generator:` block: mapcoopa's own config keys over its standard map (`coopa::maps::standard_config()`), only the differences written; older scenes' flat keys still load. |
| [`snow_field.h`](snow_field.h) | `SnowField` — the snow trench field: a toroidal 512² grid of trench depths (unorm16, 10 cm cells) following the camera, with `stamp()` (max-combine), `refill()` and `trench_at()`. Also the CPU mirrors of the shaders' open-sky test and deep-snow height (`snow_open_sky()`, `deep_snow_depth()`). Not part of the tile world; it lives here as world-space ground state. |
| [`snow_system.h`](snow_system.h) | The `SnowDeformer` component and `SnowSystem` (order **370**): stamps every deformer (and every Rigidbody with the weather's `snow_auto_deformers`) that stands in deep snow, refills trenches while it snows, and answers `depth_at()`. See [toyengine/weather](../weather/README.md#snow). |
| [`terrain_system.h`](terrain_system.h) | `TerrainSystem` + `install_terrain_system()` — the streaming policy, at order **60**. Decides which chunks exist, submits their meshing jobs, uploads finished ones (with their colliders), and releases the ones the camera has left behind. |

## How a frame moves through it

```
TerrainSystem::execute()            (order 60, owner thread)
├─ begin_generation()  ──────────►  MapGenerator::generate_async()   [job engine; only to bake]
├─ poll_generation()   ──────────►  TerrainSampler::build() + bake   [bake on the job engine]
├─ poll_tile_sets()    ──────────►  TileMeshLibrary::bake_*()        [owner thread, once]
├─ mark_edited_chunks_()            chunks whose tiles changed go stale
├─ want / release chunks around where the main camera looks
├─ submit_queued_chunks()  ──────►  ChunkBuildJob::run()             [job engine, per chunk]
│    sample_chunk_columns(tiles)      └─ mesh_chunk_styled() + weld + build_chunk_collision()
│    [owner thread: a plain copy]
├─ upload_finished_chunks()  ────►  Mesh::from_arrays() + assets.create() + add_child()
│                                     + MeshCollider                 [owner thread]
└─ PhysicsSystem::refresh()   (once, if a chunk collider came or went)
```

The split is the design. Sampling, meshing and the collision mesh's weld/adjacency/BVH are nearly
all the cost, are pure, and are independent per chunk — so they run on workers. Creating GPU
buffers, publishing assets and touching the scene tree are all single-owner operations, so they
stay on the owner thread.

## Four things worth knowing before changing anything here

1. **The tile set is data, and that is load-bearing.** Every piece is authored once in one
   canonical frame of the unit cell `[0,1]³` and baked into its eight orientations. A new look —
   square, chamfered, rounded, something else — is a new tile-set object, never a code change.
   The voxel set ships beside the rounded ones to keep that claim honest.

2. **The one-tile pad is what prevents seams.** Exposure at a chunk's edge needs the neighbouring
   chunk's columns, so sampling covers `(chunk_size + 2)²` and meshing walks only the interior.
   Without it every chunk assumes air past its border and walls itself in, and the world becomes a
   grid of visible boxes. `neighbouring_chunks_agree_on_their_shared_border` in `terrain_sampler`
   holds this.

3. **The ways to let go of a chunk are different hazards.** A still-running meshing job is
   handled by shared ownership of its `ChunkBuildJob` plus parking the chunk in `Retiring` until
   its `JobHandle` can be closed (every handle must be closed exactly once). A still-in-flight GPU
   read is handled by the AssetManager's own payload grace period. A collision shape the physics
   world (or an async navmesh bake) still points at is parked on the component for a while
   (`retire_collision()`). See `terrain_system.h`'s file doc.

4. **The classifier is what keeps a rounded tile set from quilting.** A rounded lip belongs on an
   exposed edge only. Stamp a rounded top on every column and a plateau becomes a quilt of buns.
   Each quadrant and wall half therefore takes a piece chosen from the neighbourhood
   (`tile_topology.h`), and a tile with no exposed edge is one flat quad.
   `plateau_interior_merges_flat` in `styled_tiles` is the tripwire.

## Stored tiles

A terrain's tile plane lives in a `TerraData` ([`terra_data.h`](terra_data.h)), one 24-byte
`TileCell` per tile -- the same plane whichever meshing style draws it (see
[What the two modes share](#what-the-two-modes-share) above):

| Field | Meaning |
|---|---|
| `steps` | Ground height in whole steps -- under water, the bed. Authoritative with no height plane; a maintained derived cache of it otherwise (`TerraData`'s private `refresh_steps_()`) |
| `kinds` | Top and side surface kinds (4 bits each) |
| `flags` | Coast, River (a channel cuts it), Road, Town |
| `biome` | mapcoopa biome |
| `temperature`, `moisture` | mapcoopa's 0..1 values (temperature is a static yearly mean: 0 polar, 1 equatorial), as bytes |
| `water_kind` | `WaterKind` (None/Ocean/Lake/River), as a value -- not flag bits |
| `water_level` | The water SURFACE, absolute metres (not a depth-in-steps); unused when dry |
| `flow` | A river's downstream direction, an octant 1..8 |
| `flow_authored` | Set when a human (or mapcoopa) wrote `flow` directly, as opposed to some future auto-derivation leaving it alone |
| `region`, `country` | mapcoopa ids (`0xFFFF` = none) |

Depth is never stored -- it is always `water_level - ground`, which is the one rule that makes
the same cell correct whether a terraced mesher reads a quantised step or a smooth one reads a
continuous height (see [Water](#water)).

**Baking.** With no `.terra` file (or on a Regenerate) the terrain generates the mapcoopa map from
its `generator:` settings -- mapcoopa's standard map (`coopa::maps::standard_config()`, what a bare
`mapcoopa` run makes) in mapcoopa's **natural terrain model** (`default_generator()`: smooth ground
shaped into ridged mountain ranges, rolling hills and plateaus, instead of the classic model's flat
facets between cells, which turn into rows of straight terraces on a large world) with whatever the
block changes (`terrain_model: classic` brings the facets back), through `derived_config()` as the CLI does,
so the seed moves the coastline -- cut into `world_tiles` tiles per side (0: `grid_size *
tiles_per_grid_unit`; the world spans `world_tiles * tile_size` units) at `height_scale`, and
`TerrainSampler::bake()` fills every record. After that the generator only matters to the next
Regenerate; `tile_size`, `height_step`, `water_inset`, the tile sets, chunking and collision
apply live. The editor's **World Generator** window edits the block with a live preview.

### Water

Water is a **surface over the ground**, never geometry in it. Each tile carries a `WaterCell`
(`terrain_field.h`): a kind, the height its surface stands at, and a river's downstream octant.
Depth is derived, `level - ground`, which is the one rule that makes the same field work over
terraced steps and over a smooth slope.

[`terrain_water.h`](terrain_water.h) is that as operations -- `flood_water`, `drain_water`,
`settle_water` and the resumable `WaterSettle` -- shared by both modes, by the editor's water
brush and by gameplay. Both modes build their surfaces per chunk and hand them to the water
system as still (sea, lake) and flowing (river) `WaterBody` children, so waves, depth colour,
shore foam, refraction, caustics and buoyancy are the water system's either way:

| | tiles | heightmap |
|---|---|---|
| mesh | one quad per flooded tile at its terrace, `water_inset` under the top | a grid at `level`, corners averaged per tile, with a buried apron `water_skirt_tiles` wide ([`terrain_water_mesh.h`](terrain_water_mesh.h)) |
| shoreline | where the surface meets the bank's wall -- the tile set's shape | where the surface meets the smooth ground: the terrain's own slope is the beach, kept that way by grading the bed (`grade_water_shore()`, below) |

**Both kinds of body over a chunk keep the same wave field.** A river and the lake it runs into
share their seam's vertices, and the vertex stage displaces each from its own body's wave
params -- so two different wave fields pull a shared vertex apart and the bed shows through.
A river's own character comes from its per-vertex flow and turbulence, which agrees across a
seam by construction.

**Where the map's water comes from.** mapcoopa decides where water is and how high (`coopa/maps/water_field.h`): the sea at
sea level; each lake at its spill level, just under the lowest point of its shore, over a basin
(`lake_depth_m` under the surface at the shoreline, deepening by `lake_bed_grade` the further in
it reaches); each river at its banked surface, under both banks and only ever
falling, with its channel cut to hold it and carried on past its mouth to where the body's water
really starts, so every river runs into the sea or lake it feeds. The bake samples that per tile and rounds it to steps
the ground's way, never the water's: a dry tile beside water rounds up to that water's terrace (a
bank), a flooded tile's bed down to at least a step under it -- **so water never stands above dry
ground beside it.** It is never drawn as blocks: every chunk builds a smooth surface over its
flooded tiles (`build_chunk_water()`), `water_inset` under the terrace (`river_inset` for rivers,
nearer the bank tops), its corners averaged so a
river stepping down a terrace slopes, with a skirt tucked under the banks. Still water (sea, lakes)
and flowing water (rivers, `u` downstream) become two `WaterBody` children of the chunk, so waves,
depth colour, shore foam, refraction, buoyancy and caustics are the water system's. The shoreline
is where that surface meets the bank -- the tile set's shape, rounded or square -- and the beds
are ordinary ground, so they collide and the water does not. `water:` takes WaterBody keys.

**The heightmap's shore.** A terraced world only has to choose which way a fractional shoreline
*rounds* (`settle_water_rounding()`, above); a smooth one has to make sure the bed genuinely falls
away from the waterline, because its shoreline is simply wherever the ground crosses the level and
the water shader's edge fade and foam band are measured in metres of depth there.
[`grade_water_shore()`](terrain_water.h) is that: it lowers each wet tile's four corners just far
enough that their MEAN -- the same bilinear value `TerrainField::set_water()`'s own dryness test
reads -- clears the level by `shore_clearance` (a few millimetres, enough to beat the half-tile
offset between corner-sampled height and tile-centred water), and optionally ramps a further
`shore_grade_depth` in over `shore_grade_band` tiles for an otherwise flat bed. It only ever
lowers a sample, so it can never flood ground no tile called wet, and running it twice changes
nothing -- which is what lets the mapgen bake, the heightmap system's own apply, and the editor's
water brush (`terrain_water_dab()`) all call it over whatever they just flooded instead of
tracking whether someone already has. `build_smooth_chunk_water()`'s own apron -- `water_skirt_tiles`
wide, held flat at the level everywhere its own ground stays underwater and tucked a few
centimetres under it only once that ground has risen to meet the surface -- is the surface's half
of the same idea: without it the mesh would stop at a tile boundary rather than at the waterline,
showing a straight edge of water hanging over dry ground.

**The floor under it.** The bed is terrain, and gets the whole treatment: mapcoopa shapes it with
landforms of its own (`NaturalTerrainConfig::seabed`, `seabed_ridges`) that fade in with depth
from the shoreline, so a sea floor has swells, banks and gullies rather than being the bare
interpolation of the map's cell heights -- which moves a tenth of a metre per tile where the land
moves two, and bakes a whole ocean to one terrace. The bed's *surface* is graded by depth instead
of taking the shore's sand all the way down (`TerrainSampler::bed_kind_for()`): sand in the surf,
silt below the wave base, scoured stone on the open floor, and mud from the margin in on a lake.
`terrain_sampler_test` holds the baked floor to having relief and more than one surface.

**Files.** `tiles:` (Tiles mode) or `data:` (Heightmap mode) names the file -- both spellings of
the same key -- default `terrain/<object>.terra` beside the scene. A header, a geometry block, a
plane table (additive: a new plane or a grown `TileCell` is never a version break), then each
plane's own chunk index and deflated blobs, one per 32 x 32-tile storage chunk, SoA-transposed so
the terrain's coherence compresses. Saving recompresses only chunks edited since.
[`TerraStore`](terra_store.h) owns every terrain's data keyed by file, so re-parsing the
component (the editor does on every change) keeps the live — possibly unsaved — data; the editor
writes it with the scene (Ctrl S). A hand-sculpted terrain and a mapcoopa bake are both, in the
end, just a `TerraData` committed through the same store -- the codec cannot tell them apart.

**Editing at runtime.** The same calls the editor's brushes use:

```cpp
auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
glm::ivec2 t;
if (terrain->tile_at(world_pos, t)) {
    const toy::world::IRect cell{t.x, t.y, t.x + 1, t.y + 1};
    terrain->field.data()->at(t.x, t.y).steps -= 1;              // dig (on dry ground)
    terrain->field.mark_tiles_dirty(cell);
    toy::world::settle_water(terrain->field, cell);              // water beside it runs in
}
toy::world::flood_water(terrain->field, {t}, 6);                 // fill a basin to terrace 6
const toy::world::TileCell* r = terrain->record_at(world_pos);    // biome, temperature...
```

Or through `tile_edit.h`'s pure operations (`tile_raise()`, `tile_paint()`, ...), which both the
editor's brushes and `TerrainComponent::raise_at()`/`raise_tiles()` forward to.

`mark_tiles_dirty()` journals the rect; TerrainSystem re-meshes exactly the chunks it touches (and the
neighbours whose skirt it is), each keeping its old mesh until the new one swaps in, and the chunk
collider follows. Chunk jobs mesh a copy of their columns taken at submit, so edits never race a
running job. Owner thread only.

**Editing in the editor.** Tile Mode (`editor/app/ui/tile_mode.cpp`, tools in
`editor/terrain/tile_brush.h`): square brushes, tile selection and selection edits, undo in the
scene's timeline. See the editor manual's Terrain page.

## Collision

With `collision: true` (the default) a chunk within `collision_radius` chunks of the camera
carries a static `MeshCollider` built from the very triangles it draws: rounded lips collide
round, voxel blocks collide square. The mesh is welded and its adjacency and BVH built on the
chunk's worker (`build_chunk_collision()`), then handed to the collider by `shared_ptr`
(`MeshCollider::set_mesh()`'s runtime overload), with no asset involved. `physics_material:`
names a `physics_materials/<name>.yaml` for all of them.

`collision_radius` (4 chunks by default, `0` for every loaded chunk) is there because a collider
is the most expensive thing a chunk carries: a welded mesh with adjacency and a BVH, a megabyte
or two of it, and a `PhysicsSystem::refresh()` whose re-gather is linear in every collider in
the scene. A chunk crossing inward is re-meshed with one (its old mesh keeps drawing meanwhile);
crossing outward it simply drops the component and retires the shape. **Ground outside the
radius is not solid** -- raise it past wherever physics actually happens, and remember that
bodies resting on terrain the camera streams away entirely lose their ground with it, so
`view_radius` has to cover that too.

## Editing it at runtime

The tiles are the terrain (see [Stored tiles](#stored-tiles)), so changing the world is changing
them: write cells, `mark_tiles_dirty()`, and the chunks they touch re-mesh with their colliders
and water. `tile_edit.h` is that as operations rather than raw writes -- `tile_raise()`,
`tile_set_height()`, `tile_paint()`, `tile_smooth()`, `tile_water()` -- pure functions over a
`TerrainField&`, shared by the editor's brushes (`editor/terrain/tile_brush.h` forwards to them)
and by gameplay.

Gameplay goes through the component, which knows where the tiles are in the world and owns the
water that follows:

```cpp
terrain->raise_at(world_position, /*radius_tiles=*/1, /*delta=*/-2);   // dig a 3x3 pit
terrain->raycast_tiles(ray_origin, ray_direction, 1e4f, hit);          // what is under the cursor
```

**The water follows by itself.** Ground that rose through its surface loses it, and ground that
dropped beside water is flooded by it, to that body's level and carrying what it is -- sea,
lake, or a river with its flow direction (`terrain_water.h`'s `settle_water()`). What gameplay must
not do is pay for it all at once: a hole dug at the shore floods as far as the basin runs,
which is thousands of tiles and a rebuild for every chunk they cross. So an edit through the
component **queues** the settle (`TerrainComponent::water_settles`) and `TerrainSystem` spends
`water_settle_budget` tiles a frame on it -- the water is seen to run in, and the chunk rebuilds
ride the streamer's existing pacing. `settle_water()` still runs the whole thing in one call for
the editor, and `terrain_water_test` asserts the two end up identical.

On a planet every one of these steps its neighbours through `PlanetLayout::neighbour()`, because
one column along in storage is a column of the next face, nowhere near the water.

`TerrainDigger` (toyengine/scene/terrain_digger.h) is a small component that does all of the
above from the mouse: it outlines the tiles under the cursor every frame (the cue Engine draws
into the debug-line overlay), a drag digs them, and `raise_key` held raises instead.
`assets/scenes/terrain/dig_demo` is the scene for it -- a small coast with lakes and a river,
close enough to aim at individual tiles. It is the only demo that carries one: it is gameplay,
so **it runs in Play, not in the editor's edit mode**, where the editor's own Tile Mode brushes
are the way to move ground (they go through the same tile_edit.h operations, water settling
included).

## Cost model

This section (through Planets, below) is Tile mode's cost model: its chunks are meshed on the
CPU, and `lods:` names simplification levels of that mesh. A heightmap chunk is a different
drawing scheme entirely (see [Heightmap terrain](#heightmap-terrain)) — its own cost model lives
there.

The pipeline frustum-culls every chunk per view (see `ToyRenderPipeline::gather_meshes_()`): the
camera draws only the chunks it can see, and each shadow cascade/face draws only the chunks inside
its own frustum. `view_radius` still sets the resident set — `(2 * view_radius + 1)²` chunks meshed
and held in memory — and chunks behind the camera can still cast shadows into view, until
`shadow_distance` (metres, `0` for no limit) drops the far ones out of the cascades. Prefer
raising `chunk_size` over `view_radius` to see further — it buys coverage at fewer, larger draws,
at the cost of a coarser streaming granularity.

**Levels of detail.** Every chunk mesh is built with coarser levels behind its full one
(`lods:`, `TerrainParams::chunk_lods`), so a chunk small on screen draws a fraction of its
triangles. `lods:` is Tile-mode-only: a heightmap chunk draws one shared flat patch with
`lods_enabled = false` on its `MeshRenderer`, so a `lods:` key on a `mode: heightmap` terrain is
rejected with a warning, and `select_lod()`, `mesh_lod_bias` and a heightmap renderer's own
`lod_bias` are all inert for it — its detail comes from tessellation instead.

```yaml
- type: TileTerrain
  lods:                                  # coarsest last; [] turns the levels off
    - { ratio: 0.40, screen_size: 0.14 } # 40% of the triangles, under 14% of screen height
    - { ratio: 0.15, screen_size: 0.07 }
    - { ratio: 0.07, screen_size: 0.03 }
```

The levels are simplified on the chunk's own worker (`build_chunk_lods()` →
`Mesh::simplify_levels()`) and share LOD 0's vertices, so they cost index ranges, not geometry —
and **every vertex on the chunk's seam is pinned through every level** (`chunk_seam_locks()`),
which is what lets neighbours sit at different levels without a crack between them. meshopt's
own `LockBorder` is not that guarantee and was measured failing it: a chunk's edge wall carries
the surface over the seam, so the simplifier sees no boundary there. A level that loses a
pinned vertex anyway (its last triangle collapsing out from under it) is dropped, so the
coarsest tier may not exist on every chunk — measured on a busy 32-tile chunk: 40%, 15%, and
the 7% level rejected. The renderer picks one per chunk per frame from
its projected height, with hysteresis, and the shadow views reuse that choice
(`toy::render::select_lod()`); the collider is always LOD 0. The defaults coarsen only what is
genuinely small, so a ground-level camera on a streamed terrain sees LOD 0 throughout; they earn
their keep on a planet held whole in frame and on terrains with a large `view_radius`. Pin them
off per renderer with `lods_enabled: false`, or globally with the render config's
`mesh_lod_bias`.

A level can name `tile_set:` instead of `ratio:`, re-meshing the whole chunk in another style
(`TerrainParams::ChunkLod::style`, a library index resolved the same way a scene's own `tile_set:`
/ `kind_tile_sets:` are) rather than simplifying LOD 0:

```yaml
lods:
  - { ratio: 0.40, screen_size: 0.14 }
  - { ratio: 0.15, screen_size: 0.07 }
  - { tile_set: objects/terrain/tileset_soft_lod, screen_size: 0.03 } # style tier, coarsest last
```

A style level does not share LOD 0's vertices -- it is meshed and welded independently and
appended behind every ratio level, with its own `vertex_offset` for `vkCmdDrawIndexed` to rebase
at draw time -- so it carries none of a `ratio` level's rejection risk (a pinned seam vertex can't
be lost to simplification if nothing is simplified) at the cost of its own vertex buffer rather
than shared index ranges. It costs an unconditional extra stamp-and-weld on the chunk's worker,
whether or not the tier is ever reached on screen, and stays shading-identical at the seam only if
the override style shares its neighbours' `round_kind_corners` and corner radii -- `soft_lod`
(below) is `soft`'s own radii at coarser segment counts for exactly this reason. `native_border`
keeps that many tiles of the chunk's rim in the un-overridden style, for a tier that wants its
interior cheap but its edge geometry matching a full-detail neighbour exactly. Planets don't
support this tier: the planet warp runs before a style level would need its own copy of it.

**The `field` tier: a chunk as a plain heightfield.** A level can instead name `field:`, the
number of tiles per cell of a quantised grid (`mesh_chunk_field()`): one flat quad per cell at
that cell's *tallest* column, vertical walls wherever two cells differ, and no tile pieces, lip or
corner rounding at all. `native_border` keeps that many tiles of the rim at one cell per tile, so
the coarse surface matches the true heights where it meets a neighbour.

```yaml
lods:
  - { ratio: 0.40, screen_size: 0.33 }
  - { field: 1,  screen_size: 0.20, native_border: 1 }   # one quad per tile -- no lips
  - { field: 16, screen_size: 0.085 }                    # one quad per 16x16 tiles
```

It is the cheapest tier by a wide margin and the only re-meshed one a **planet** carries. Measured
on a busy 32-tile `soft` chunk (`terrain_chunk_cost`), against LOD 0's 19.9k triangles:

| tier | triangles | of LOD 0 | build |
|---|---|---|---|
| the three default `ratio` levels | 7k + 2k (+1 rejected) | 40% / 15% | **18 ms** |
| `field: 1, native_border: 1` | 2,392 | 12.0% | 0.09 ms |
| `field: 4, native_border: 1` | 336 | 1.7% | 0.02 ms |
| `field: 8, native_border: 1` | 146 | 0.7% | 0.01 ms |
| `field: 8, native_border: 0` | 82 | 0.41% | 0.01 ms |

A `field` level is flat quads meeting vertical walls, so every edge between them is a hard shading
crease. `smooth: true` on the level averages its normals across the faces meeting at each position
(`smooth_field_normals()`), which reads as a rounded slope instead of a staircase. Geometry,
triangle count and the tallest-column seam rule are all untouched -- only the normals move -- so it
is free and cannot crack a seam. Best left off on the first coarse level, which is seen closest.

Three properties `ratio` lacks. Its cost is a function of `chunk_size / field` and nothing else,
not of how busy the terrain is. It cannot be rejected, so the tier exists on *every* chunk. And
being built in face space from the same columns LOD 0 used, `warp_chunk_to_planet()` bends it onto
a globe like any other geometry — which is what makes it work on a planet where a style tier
cannot. Note the 18 ms: the `ratio` tiers cost **5.6x the 3.2 ms of meshing and welding the chunk
in the first place**, so a scene that replaces them with field tiers gets its load time back as
well as its triangles.

**Why a field tier cannot crack against its neighbours.** Not by preserving vertices, as a `ratio`
level does, but by construction. A cell takes the height of its *tallest* column, so a coarse
surface only ever sits at or above the ground it stands in for; and a chunk's boundary walls
descend to the neighbouring chunk's **own** column height, read from the shared `ColumnPad` — the
same value that neighbour sampled. Two chunks therefore always overlap along their shared edge
rather than opening a gap, whatever levels each happened to pick. `terrain_chunk_cost`'s
`two_field_chunks_overlap_on_their_shared_seam` builds neighbours at *different* strides and checks
all 32 seam rows for this.

**What a chunk costs the frame it lands in.** Meshing, welding, the collider's BVH *and the
chunk's GPU buffers* are all built on its worker (see `terrain_chunk.h` for why the Vulkan half
is safe there), so the owner thread only publishes the result: a scene insert, an asset, the
water children's first bake and — inside `collision_radius` — a physics re-gather.
`upload_budget_ms` (1.5 ms) bounds that, nearest chunk first, with at least one chunk landing
per frame whatever the budget. How much is outstanding is bounded by `max_chunks_in_flight`
(default: twice the JobEngine's worker count) rather than by a per-frame count, and each job is
submitted at a priority that falls off with distance, so the ground under the camera is meshed
before the horizon. `dormant_chunks` (32) keeps that many chunks whole after they leave view, so
turning round at a chunk boundary re-attaches ground that is already built instead of rebuilding
it; the streaming centre is latched (`latch_stream_centre()`) so that merely *looking* around
cannot move the loaded region at all.

**Deactivation releases everything, immediately.** `scene.get_components<TerrainComponent>()`
only sees a terrain that is active in hierarchy, so deactivating one (a preset switch, a script,
an inspector toggle, or an ancestor going inactive) drops it out of `TerrainSystem::execute()`'s
gather. The system diffs this frame's gather against last frame's to notice the drop, then
`release_terrain_()` tears down every chunk the same frame: Live ones are destroyed outright,
a Meshing one is cancelled and parked `Retiring` and drained over the following frames by the
same path `release_distant_chunks_()` already uses. Reactivating starts the resident set over
from nothing — `dormant_chunks` does not survive a deactivation, only a chunk briefly leaving
view. This is deliberate: a scene that cycles between several terrain flavours (see
`unified_terrain_demo`) would otherwise carry every flavour's chunks, meshes and colliders at
once for as long as it runs.

`tiles_per_grid_unit` is quadratic in tiles and does not add detail the map does not have; past
the point where a map cell is a handful of blocks wide, all it resolves is
`MapConfig::terrain_roughness`'s fractal displacement.

## Tile sets and pieces

### Tile sets: the template as one object

A tile set is a **tile-set object**, for example `assets/objects/terrain/tileset_round.yaml`. It is
an object asset with one child per piece, each child named by its piece suffix and carrying a
MeshRenderer for that piece. `tools/gen_tile_styles.py` writes the stock ones, marked
`tile_set: true`. A TileTerrain's `tile_set:` (or a `kind_tile_sets:` entry) names the object, and
the terrain finds each piece by its child's name. A piece the set leaves out falls back to its
nearest relative (`TileMeshLibrary::finish_styles()`).

One optional key on the object steers the shader rather than the geometry:
`round_kind_corners: false` keeps the boundaries between surface kinds square on top (the voxel
set sets it); by default `terrain_styled.frag` rounds them (see below).

Inside the engine each distinct tile set a terrain uses is a **style** (`TileMeshLibrary`'s
term), and each column is shaped by its top kind's style.

In the editor, the tile sets appear in the Objects tab, under toyengine's read-only section in a
game project, and in the TileTerrain panel's Tile Set list:

- **Open one** to see every piece laid out by tier, then Tab into a piece to reshape its mesh.
- **Make a new look:** use *Duplicate as New Tile Set* on a built-in set, or *Duplicate* on a
  project one. The copy gets its own copy of every piece mesh (`meshes/<copy>_<piece>.yaml`), so
  reshaping it never touches the original. Then pick the copy as the terrain's Tile Set (the
  panel's own *Duplicate as New Tile Set* does both at once).

### Pieces

A style is eleven meshes, each authored once in one canonical frame of the unit cell `[0,1]^3`
(Z up) and baked into all eight D4 orientations (`variant_transform()`: mirror across
`x = 0.5`, then quarter turns about +Z):

| Piece | Canonical frame | Placed where |
|---|---|---|
| `top_inner`, `top_edge`, `top_outer` | the +X+Y quadrant of the top, `z = 1`; `top_edge` exposed on +Y | each quadrant of a top with an exposed edge, by whether its two cardinals are lower — three shapes cover all 47 blob cases |
| `wall_cap_{continue,convex,concave}` | the right half (`x ∈ [0.5,1]`) of the +Y wall, with the lip's round-over | the topmost cell of a wall |
| `wall_{continue,convex,concave}` | the same half, a vertical extrusion of the plan profile | every cell below |
| `wall_cap_concave_taper` | the concave half: plain on its own face, lipped along the fillet arc | the taller wall's half of an inner corner, at the cell where the lower wall's plateau ends |
| `wall_cap_continue_taper` | the straight cap half, its lip easing from full at `x = 0.5` to nothing at `x = 1`, with a top fill over the strip it uncovers | the lower wall's cap half where its straight run ends against a taller neighbour on the same face |

A wall half's shape is its **end state** at that cell (`WallEnd`): the cliff continues straight
onto the next column, wraps a convex corner, or fillets a concave one. A concave fillet is
material added into the *lower* cell's corner, so each of the two walls meeting there owns a 45°
half. Where the two walls have different heights, the lower one's half caps out with its lip at
its plateau. The taller one's half at that same cell is the **taper** piece: its own face stays
plain, because that cliff keeps rising. Its lip eases in along the fillet arc, from nothing at the
face to full at the corner's midline, where it meets the lower cap. The inner corner stays one
smooth curve, and no lip ever carves into the taller column.

The same meeting along a straight face -- a lower plateau's cliff running on into a taller
neighbour's, flush -- is the **continue taper**: the lower cap's last half eases its lip out to
nothing, so it meets the taller column's plain wall square. A full lip there would leave its
cut-out standing against the flat face as a hard wedge.

**One style per column.** The column's top kind picks the style for its top and its whole cliff,
lip to base. An ACNH cliff is one smooth shape all the way down. Its soil band and stone below are
the atlas kinds the triangles carry (up-facing triangles of the lip take the top kind, so grass
rounds over its own edge), not different geometry.

### Section caps (how different tile sets meet)

Two neighbouring pieces can have different profiles: the next wall along, at a straight or
concave end, where the neighbouring column has another style or ends its wall at another height;
or the cell below, where a wall's end state changes with depth. Something has to close the step.
Styles are never paired for this. Instead, at bake time each piece's boundary edges on its own
planes (`z = 0`, `z = 1`, its end plane) are chained and fanned into a **section cap**: the
piece's own solid cross-section. The mesher emits a piece's cap facing the neighbour wherever the
neighbour differs. Whatever part lies inside the neighbour's solid is hidden by the neighbour's
own surface, and the part that is not is exactly the step. **Convex feet** cover the corner of the floor
a rounded wall carves out of its own footprint.

Because caps are derived, a hand-authored piece gets them for free, as long as it keeps the
**authoring contract**:

- Vertices on a boundary plane lie exactly on it, and rim lines agree between pieces
  (`top_edge`'s inset edge at `y = 1 − LIP_H`, the rim arcs). Only interior vertices may wander.
- A straight piece is an extrusion along x: the mesher stretches runs of them (below).
- A concave lip stays inside its corner: `LIP_H ≤ (√2 − 1) · R_IN`.
- Proportions assume a cubic cell. On a taller step the cap tier keeps everything above its
  `z = 0.5` at tile proportions, pinned to the cell top, and stretches only the band below, so
  `LIP_V ≤ 0.5`.

`surface_has_no_holes_from_above` (suite `styled_tiles`) ray-casts busy random pads with all four
shipped tile sets from above, and `surface_has_no_holes_at_grazing_angles` from nearly side-on — the view a player on
the ground has, where gaps in walls and corners show. Both fail on any see-through hole or
exposed back face. Run them with `STYLED_RAY_STRESS=1` (about 230k rays) after changing a piece,
the classifier or the cap rules.

### Surface: `terrain_styled.frag`

Chunks carry no texture coordinates. Every vertex's uv.x is a packed **blend code**
(`encode_surface_blend()` in `tile_topology.h`): the kinds the surface may show and how to choose.
Boundaries between kinds stay straight, where the mesh puts them. What the shader removes is
their right-angle **corners**:

- **Top** quadrants at a convex corner of their own kind (both edge neighbours, at the same
  height, are other kinds) carry those neighbours' kinds -- unless the column's tile set says
  `round_kind_corners: false`, when every top quadrant is one kind and the boundary stays square. Pixels outside a quarter circle take
  the neighbours' kind. The other kind's inner corner is the same curve seen from the other side.
- **Checkerboards** round only the higher-indexed kind, so the other connects through a smooth
  waist.
- **Three-kind junctions** give the cut-off corner to the kind that also holds the diagonal,
  forming a smooth T.
- **Lips** (the cap tier) show turf where the surface faces up past a threshold, so the grass
  line follows the round-over's curve.
- **Everything else** is one kind, with no extra geometry: only corner quadrants stay unmerged,
  and only they carry neighbour kinds, so everything else merges and welds.

The shader then reads the chosen kind's colour and roughness from the centre of its atlas cell,
and draws the detail procedurally. It uses world-space value noise at a few frequencies, plus specks, strata on
stone and a slight bump from the noise's analytic gradient, with each octave faded out as it
nears the pixel's footprint. In practice that is an analytic mip chain: sharp up close, calm at a
distance, seamless across lips, corners and chunks, and independent of the engine's (absent)
texture mipmapping. Per-kind character is a table in the shader, in `TileKind` order. The atlas
(`terrain_atlas.png`) is therefore just a palette.

### Cost

On `terrain_demo` a rounded 32×32 chunk averages about 11k triangles and 9k vertices; the voxel set
is far lighter, its pieces being single quads. The `terrain_chunk_cost` suite measures a worker's
share on a deliberately busy rounded chunk (~20k triangles, about twice the demo's average): about
1 ms meshing and 3 ms weld-and-optimize, plus about 9 ms for the collision mesh's weld, adjacency
and BVH where the chunk is inside `collision_radius` -- the collider dominates a chunk's build.
Re-run that suite for current numbers on any machine; the ratios are the stable part. Two merges
keep the meshing there:
- Straight halves, most of any cliff and most of a lip's rings, are collected and stamped as one
  stretched strip per run.
- Top **quadrants** with no exposed edge of their own merge into flat rectangles of equal height
  and kind (`TileMeshLibrary::top_merges()`), at half-tile granularity -- a tile with one convex
  same-kind corner still merges its other three quadrants, rather than the whole tile falling out
  of the merge the way a tile-granularity pass would.

The levers beyond that are the generator's `ARC_SEG` / `LIP_SEG` and `view_radius`.

**Per-piece triangle counts**, read straight off the generated mesh YAMLs (`gen_tile_styles.py`'s
`profile()` × `levels()` grid), for `soft` against the far-LOD `tile_set:` style `soft_lod` --
same `R_OUT`/`R_IN`/`LIP_H`/`LIP_V`/`round_kind_corners`, coarser `ARC_SEG`/`LIP_SEG` only:

| piece | soft | soft_lod |
|---|---|---|
| `top_inner` | 2 | 2 |
| `top_edge` | 2 | 2 |
| `top_outer` | 8 | 4 |
| `wall_continue` | 2 | 2 |
| `wall_convex` | 8 | 4 |
| `wall_concave` | 8 | 4 |
| `wall_cap_continue` | 10 | 4 |
| `wall_cap_convex` | 40 | 8 |
| `wall_cap_concave` | 44 | 10 |
| `wall_cap_concave_taper` | 31 | 6 |
| `wall_cap_continue_taper` | 43 | 22 |

The win concentrates in corner-cap pieces, exactly where `soft`'s triangle count diverges from
voxel's in the first place. There is no before/after chunk total to report here: Stage 1's
quadrant merge and the `soft_lod` tier both land on top of the demo scene in the same pass, with
no pre-merge baseline captured on this machine -- `terrain_chunk_cost`'s own printed numbers are
the up-to-date record; re-run that suite for current ones.

One trap in this code is easy to step into. `append_styled()` runs thousands of times per chunk,
so it must never `reserve(size + n)`: an exact-size reserve defeats the vector's geometric growth
and once made a chunk take 200 ms to mesh.

## Where the tile assets live

The stock tile sets are **shared**, not scene-local:

```
assets/objects/terrain/tileset_<set>.yaml              (round, soft, rock, voxel)
assets/meshes/terrain/<set>/tile_<set>_<piece>.yaml
assets/textures/terrain/terrain_atlas{,_mr}.png        (the palette)
```

The folders under each type folder are tags (see [assets/README.md](../../assets/README.md));
a reference may name the asset alone (`objects/tileset_round` still finds
`objects/terrain/tileset_round.yaml`).

A tile set and a surface atlas are primitives of this system, not demo content the way
the kitchen_sink test scene's `pillar.yaml` is — every terrain scene wants the same ones, and keeping them
inside one scene would mean the second terrain scene copies them.

Nothing selects between shared and local. `coopa::asset::AssetSource::resolve()` tries the
loading scene's own directory **first** and the registered search roots (`assets/`) second, so a
scene that wants its own piece or palette just drops a file of the same name into its own
`meshes/` or `textures/` and wins by precedence. `assets/scenes/terrain/terrain_demo/` therefore
holds nothing but its `scene.yaml`.

One consequence worth knowing: `assets/textures/` is a fallback namespace for *every* scene's
`textures/…` references. Nothing collides today — the test fixtures (`tests/fixtures/scenes/kitchen_sink`, `material_maps`) carry
their own `crate_*.png` / `noise_mask.png`, which win locally — but a scene that typos a local
texture name can resolve to a shared file instead of failing outright.

## Generating the assets

Both are standalone tools, not part of the build:

```bash
python3 tools/gen_tile_styles.py        # assets/meshes/terrain/<set>/tile_<set>_<piece>.yaml + objects/terrain/tileset_<set>.yaml
python3 tools/gen_terrain_atlas.py      # assets/textures/terrain/terrain_atlas{,_mr}.png (numpy, Pillow)
```

The tile-set generator uses `tools/mesh_yaml.py`'s writer. All are deterministic: re-running
one reproduces its files' contents. The PNGs' bytes can still differ across Pillow versions even
when every pixel matches. Each style in `gen_tile_styles.py` is a handful of constants: corner
radii, lip size, segment counts, optional facet jitter of interior wall vertices, and whether
the set rounds kind corners. A new set is a new entry there; point a terrain's `tile_set:` at
the object it writes.

`KINDS` in the atlas generator must stay in `TileKind` order — the shader reads a kind's cell from
the enum value alone, so a kind is appended, never inserted.

## Planets

`planet: true` turns a TileTerrain into a small round world: the same tiles, tile sets, water and
editing, wrapped around a globe, with gravity pulling toward its centre. It is a mode beside the
flat terrain, off by default — a terrain without the key is built exactly as before.

```yaml
- type: TileTerrain
  planet: true
  face_tiles: 96        # tiles along each cube face's edge; the circumference is 4 x that
  chunk_size: 16
  height_scale: 12.0
  gravity: 9.81         # m/s^2 at the surface (0: none)
  generator: { grid_size: 40 }
```

See `assets/scenes/terrain/planet_demo` -- a globe held whole in frame, spun by the mouse
(`Turntable` on the terrain object) under a camera that only zooms.

- **Map.** mapcoopa generates the world **on a sphere** (`topology: sphere` — cells over the whole
  globe, no edges, climate by latitude); see its README. The terrain sets that itself.
- **Layout** (`planet.h`, `PlanetLayout`). Six faces of a cube, each `face_tiles` square, bent
  onto the sphere with equal-angle faces (tiles stay near-square everywhere). The radius is
  `2 · face_tiles · tile_size / π`, so a tile in the middle of a face is `tile_size` across.
  The six faces are stored side by side in one `6N × N` `TerraData`, so the `.terra` format is
  the flat one. `wrap()` / `neighbour()` step across a face's edge onto the next face (turned).
- **`chunk_size` and `face_tiles` are baked in.** The parser trims `face_tiles` to a multiple of
  `chunk_size` (the demo's 216 becomes 208 at `chunk_size: 16`), and the stored `.terra` file
  holds the world at that width. Changing EITHER on a planet whose tiles already exist reads
  them at the wrong size -- bands of missing sea across the globe -- until the world is
  regenerated, and neither invalidates the file on its own: in Tiles mode `sync_field()` passes
  `tiles_x = 0`, which tells `TerraStore` to keep whatever is loaded. Delete the `.terra` and
  re-bake. Change the two together, or pick a `chunk_size` that divides `face_tiles` exactly.
- **Horizon culling.** The far side of a globe is *inside* the camera's frustum, so the
  pipeline's per-view frustum test cannot reject it: without help, half a planet is drawn,
  shadow-mapped and water-simulated behind its own horizon. `TerrainSystem::
  cull_planet_horizon_()` deactivates the SceneObject of every chunk whose centre lies past the
  horizon by more than its own angular width and the height of what stands on it (the exact
  tangent angles, `acos(R/D) + acos(R/(R+height_scale))`), which prunes the chunk *and its
  water children* from the render snapshot and the update walk. The chunk stays loaded, so
  coming back round the limb costs nothing, and the hide threshold sits a little further round
  than the show threshold so a chunk on the limb cannot flicker. Measured on planet_demo:
  1206 drawn meshes → 835, shadow casters 1424 → 1011.
- **Meshing.** Each chunk is meshed in flat face space exactly as a flat chunk, then
  `warp_chunk_to_planet()` bends every vertex onto the globe and carries its normal through the
  map's Jacobian. Chunk objects sit at the planet's centre. A planet keeps every chunk loaded.
- **Merging is off (`max_merge = 1`), and that is not a missed optimisation.** On flat ground a
  merged rectangle is exactly coplanar with the per-tile geometry it borders, so only the
  subdivision differs — a T-junction, whose cracks are sub-pixel. Bent onto a sphere the merged
  quad becomes a **chord**, and its edge leaves the surface its neighbours lie on: at four tiles
  on `planet_demo`'s 122-unit globe it deviates ~1.7 cm, which is a plainly visible seam on a 1 m
  tile at ground level. Raising the quad by its sagitta does not rescue it — that converts the gap
  into a ledge of the same size, which reads just as badly at a grazing angle. A width can be
  solved for against a deviation tolerance, but no tolerance loose enough to be worth merging is
  tight enough to be invisible. Measured before this was settled: a curvature-derived width bought
  ~5% of the shadow triangles and ~11% of load near the surface, and **nothing at all zoomed out**,
  where no chunk draws LOD 0. A planet's cost belongs to its `lods:` field tiers instead. A scene
  can still author `max_merge:` explicitly if it wants to experiment.
- **Water.** Each chunk's sea, lakes and rivers are WaterBodies in **their own frame** (local Z
  out from the planet at the chunk's centre, `WaterBody::local_frame`), so the water system's
  height-field queries, depth probes, flow and buoyancy still hold over a curved surface. Their
  waves come from the world position (triplanar), so neighbouring pieces meet without a seam.
- **Cost, and what actually bounds it.** Every chunk of the globe is built at load --
  `6 · (face_tiles / chunk_size)²` of them, 1536 at the demo's 1024-tile faces and
  `chunk_size: 64` -- and **none of them is ever evicted**, because a planet has no view radius.
  A planet gives each one a collider too (there is no collision radius on a globe: ground solid
  in front of you and not behind is worse than none), so a planet nothing stands on should say
  `collision: false` and `gravity: 0`.

  The number to watch is **resident mesh bytes**, not chunk count: mesh memory scales with the
  globe's TOTAL TILE COUNT, so raising `chunk_size` alongside `face_tiles` to hold the chunk
  count flat does not hold memory flat -- it makes each resident unit bigger instead.
  `TerrainSystem::resident_mesh_bytes()` reports it, the `terrain.mesh_mb` overlay line shows
  it, and a line is logged when a terrain finishes loading so a headless run can read it too.
  Those buffers are host-visible and persistently mapped, so this is system RAM, not VRAM, and
  no budget in the streamer counts bytes -- `max_chunk_jobs_per_frame`, `max_chunks_in_flight`
  and `upload_budget_ms` all pace wall clock or counts.

  **`planet_coarse_builds: true` is what makes a large planet affordable.** With it, a chunk too
  small on screen to draw its native tile geometry is built with only its coarsest `field` tier
  (`ChunkBuildJob::coarse_only`) and promoted to a full build as the camera comes in, with a
  dead band and a dwell so a drifting camera cannot churn it. The threshold is the finest
  `field` tier's own `screen_size` -- the exact size at which the renderer starts asking for
  LOD 0, since `select_lod()` ignores `thresholds[0]` -- so the policy and the renderer agree by
  construction. Water is still built in a coarse pass (it comes from the columns, not the mesh),
  which keeps `chunk_water_hash()` identical across a promotion so the already-baked WaterBody
  children carry over instead of every ocean on the globe blinking. A `collision: true` planet
  is excluded and behaves exactly as before: a collider has to be the chunk's real shape, not a
  quantised stand-in that changes with the camera.

  Measured on `planet_demo`, 1920x1080, `collision: false`:

  | | chunks | resident mesh | terrain load | frame | RSS |
  |---|---|---|---|---|---|
  | `face_tiles: 512`, `chunk_size: 32`, `field 1/4/16`, peak 112 | 1536 | 802 MB | 1.6 s | 14.4 ms | 1.42 GB |
  | `face_tiles: 1024`, `chunk_size: 64`, `field 1/4/16`, peak 256 | 1536 | 3413 MB | 7.7 s | — | — |
  | the same, `field 2/8/32` | 1536 | 2629 MB | 4.0 s | — | 4.78 GB |
  | the same, **+ `planet_coarse_builds`** | 1536 | **590 MB** | 1.8 s | 22.3 ms | 2.38 GB |

  So four times the tiles costs LESS resident mesh than the 512 globe did, for about half again
  the frame time. Two separate levers are visible there and both matter: dropping a `field: 1`
  tier (which is as dense as the native tops and carries its own vertices) is worth 784 MB on
  its own, and the coarse builds are worth another 2 GB.

  Changing `face_tiles` does **not** invalidate the stored `.terra` -- see the `chunk_size`
  trap above, which is the same trap -- so delete the file and re-bake. `TOY_TERRAIN_SAVE=1`
  writes a freshly baked world from a headless run, since saving is otherwise the editor's.
- **Gravity.** The terrain registers a physxcoopa gravity source at its centre — at zero strength
  until every chunk is live, so nothing dropped at the start falls through ground still being
  built. Rigid bodies, buoyancy, `CharacterController` (walking in its own frame, up away from
  the planet), `FootIK` and the orbit `CameraController` follow the field.
- **Camera.** A planet's radius is derived, never authored, so every camera number tuned against
  it is a magic multiple that stops framing the world the moment `face_tiles` changes --
  planet_demo was authored that way, at 4.3 radii out with `min_distance` four units above the
  *datum* on a globe whose mountains stand a third of its radius tall (so full zoom-in was inside
  the terrain). `PlanetCameraFit` ([`planet_camera_fit.h`](planet_camera_fit.h)), on the camera
  object beside its `Camera` and `CameraController`, takes those over: `max_distance` from the
  distance a sphere of radius `R` exactly fills the vertical fov at (the editor viewport's own
  `EditorCamera::frame()` rule), `min_distance` from `R + measured peak + min_clearance`,
  `zoom_speed` from the range divided by a notch count, and the clip planes from the far end of
  the zoom. The scene keeps authoring the rig's **shape** -- `pitch_at_*_distance_deg`,
  `tangent_at_*`, the look button, the smoothing -- and hands over the **scale**.
  It writes all three distances or none, because the zoom-coupled pitch and look tilt both
  normalise the zoom as `(distance - min) / (max - min)`: deriving a subset would silently move
  where an authored `pitch_at_max_distance_deg` is reached. A scene with no planet is a strict
  no-op, and `shadow_distance` / `volumetrics_max_distance` are deliberately not derived (render
  config, and they depend on the atmosphere's thickness, which no camera can see).
- **Sky** is evaluated in the camera's own frame on the planet (`render::PlanetFrameState`: the
  sky's view rays and sun turned so the camera's up is the sky's zenith). The global **fog** is
  plain distance fog on a planet (world Z is no height there): give a planet scene walked on the
  ground a light `fog_density` (and none at all when the whole globe is meant to be in view, as
  in planet_demo -- fog over hundreds of metres would simply erase it). Neither changes a flat
  scene's shaders or values.
- **Shader.** Planet chunks draw with `terrain_styled_planet` (`terrain_styled.glsl` built with
  `TERRAIN_PLANET`): "up" is the per-vertex `uv.y` the warp wrote, and strata have an even grain.
  The flat `terrain_styled` carries none of that code.
- **Editor.** The World Generator's Planet toggle, Add > Planet, and Tile Mode (hover marches the
  view ray through the columns; a brush or selection stays on the face it starts on).

- **Clouds.** The volumetric cloud layer (`clouds: true`) works on a planet: it is a real
  spherical shell about the planet's centre, so it curves to the horizon, the ground occludes
  it, and it is visible from orbit and from the surface alike. The render settings'
  `cloud_altitude`, `cloud_thickness` and `cloud_scale` are **discarded** there -- they are
  absolute metres tuned for an Earth-sized sky, and on a globe a few hundred units across they
  would put the deck radii out in space. `world::planet_cloud_deck()`
  ([`planet_atmosphere.h`](planet_atmosphere.h)) sizes it from the planet instead.

  **To move the deck, use `PlanetAtmosphere`'s four `cloud_*` knobs**, which are proportions for
  the same reason the shell's are -- one set of numbers has to work on a globe of any size:

  ```
  altitude  = max(peak * cloud_altitude_peak_scale,     R * cloud_altitude_radius_scale)
  thickness = max(R    * cloud_thickness_radius_scale,  altitude * cloud_thickness_altitude_scale)
  ```

  The peak term dominates on a world with real relief (planet_demo's mountains stand 112 units
  above a 326-unit datum), so `cloud_altitude_peak_scale` is the knob that raises those clouds;
  the radius term is the floor that keeps an ocean world's deck off the water. The scene's
  *atmosphere* is the `PlanetAtmosphere` with `day_sky_altitude_scale > 0` -- the same marker
  `day_sky_factor()` uses -- so a second one authored as a fog deck does not get a vote. With no
  PlanetAtmosphere at all the defaults apply, so a hand-written `planet: true` + `clouds: true`
  scene still gets a working layer.

  **The deck is kept inside the shell**, squeezed back with a warning if the knobs push it out.
  Not a tidiness rule: `volumetrics_march.frag` bounds its ray by the shell's own outer radius,
  so a deck beyond it gets none of the atmosphere's in-scatter and reads as a layer in front of
  the limb rather than within it. Give it room with `radius_thickness_scale`, not by clipping.

  `cloud_scale` stays underivable from YAML: the field's noise periods are fixed cloud-space
  lengths (13 km), so it is solved for three weather systems around the deck's own
  circumference. At scale 1 a globe whose whole circumference is under a kilometre of cloud
  space reads one constant weather value everywhere and comes out featureless.

  `cloud_type: flat` is NOT supported on a planet -- its layer is an infinite world-Z slab --
  and falls back to the volumetric layer with a warning. Cloud shadows are off there too (the
  shadow map is still a flat XY square around the camera).
- **Atmosphere density is a DEPTH, not a per-metre figure.** `Volume::density` is per unit of
  length, so it is the one number in a shell's profile that does not scale with the globe: the
  thickness and the scale height are derived from the planet, the path a ray takes through them
  grows with it, and an authored density silently rides along meaning something different.
  planet_demo carried `0.0006` from its 326-unit days, which is a limb optical depth of about
  **0.3** -- a haze that measures as nothing against the star field, which is why the shaped
  gradient was never visible on screen. Set `PlanetAtmosphere`'s `limb_optical_depth` instead:

      density = limb_optical_depth / sqrt(2*pi * scale_height * planet_radius)

  the grazing-path integral through an exponential atmosphere. ~2 is a clear bright limb with the
  disc still legible through it; 1 is a thin veil, 4 reads as a gas giant. The limb is
  `sqrt(2*pi*R/H)` times deeper than the view straight down (5.7x on the demo), so a SHORTER
  `scale_height_fraction` sharpens the band and clears the disc, a longer one spreads the glow
  further out. 0 -- the default -- leaves an authored `density` alone, so no existing scene moves.

  Measuring it: take a radial luminance profile about the planet's centre rather than trusting
  the eye. The demo's runs 173 at the limb to 21 (the star floor) by 1.7 radii, over a surface
  that sits at 278 px of a 1080-line frame.

- **The froxel grid gives an orbital atmosphere almost no slices.** `volumetrics_mode: froxel`
  distributes its 64 depth slices EXPONENTIALLY between 0.1 and `volumetrics_max_distance`, which
  is right for fog near the camera and wrong for a shell seen from four radii out: on the demo
  the whole atmosphere in front of the planet lands in **2.5 slices**. Raising
  `volumetrics_max_distance` does not help (it stretches the same 64 slices further) and
  `volumetrics_mode: raymarch` measurably barely does either -- the fix is optical depth, above.
  What `volumetrics_max_distance` IS for is making sure the shell is inside the grid at all:
  keep it past `camera distance + shell outer radius`.

- **The planet frame.** [`env/planet_frame.h`](../env/planet_frame.h) is the one authoritative
  answer to "which way is up here, and how high up is this": `up_at()`, `altitude_at()`,
  `tangent_at()` and `transport()`. A default-constructed `PlanetFrame` **is** the flat Z-up
  world, so a caller asks it unconditionally rather than branching on whether a planet exists --
  the same shape as `MoveOptions::up` in the character motor, which defaults to `+Z` and keeps
  an exact fast path for it. `world::find_planet_frame(scene)` finds the scene's planet;
  `render::PlanetFrameState` is an alias of the same type.

- **Day and night.** The terminator is one shared soft band (`gfx_planet_day()` in
  [`gfx/planet.glsl`](../../assets/shaders/gfx/planet.glsl)), so the atmosphere shell, the
  cloud deck, the star backdrop's daylight fade and the ground all cross into night at the same
  angle. `PlanetAtmosphere`'s `moon_intensity` lights the hemisphere facing away from the sun --
  ground, water and clouds together -- and `night_sky_color` / `day_sky_color` are the two
  colours the cloud deck's ambient blends between across that band. **`moon_intensity` defaults
  to 0, which is a black night side**: without it nothing lights that hemisphere at all, and the
  deck is still there and still occluding the stars at the limb while emitting nothing. The
  moon sits opposite the sun (a full moon over the anti-solar point) and casts no shadows; an
  independently placed moon and a `moon_color` would both need a direction and a colour plumbed
  to the lighting pass and the cloud march, and are out of scope.

**Not on planets yet:** moving platforms under a character, Mount/Rider, ragdoll recovery,
navigation, the first-person camera, cloud shadows (the map is still a flat XY square around
the camera, so it is switched off on a planet), the global height fog (plain distance fog
there), the weather system (its sun path, wind bearing and ground-height plane are all
world-Z), and sky reflections/ambient (they still read the world-oriented sky gradient).
More than one planet per scene is out of scope for now. A LARGE planet is in scope with
`planet_coarse_builds` and a `field` tier no finer than 2 (see Cost above); what is still out of
scope is a planet whose TILE data outgrows memory, since `TerraData` is 24 bytes a tile plus 8
for its splats -- about 200 MB at `face_tiles: 1024`, and 800 MB at 2048. Streaming a planet
(evicting chunks behind you) remains unimplemented; coarse builds bound what a resident globe
costs instead of changing what is resident.

## Heightmap terrain

`HeightmapTerrain` ([`heightmap_terrain.h`](heightmap_terrain.h)) is a plain height grid: a size
and height in metres, a chunk size, and `height_sub` samples per tile edge. Its data is the SAME
`TerraData`/`.terra` file every mode uses (see [Stored tiles](#stored-tiles) and
[What the two modes share](#what-the-two-modes-share)) -- a height plane, two RGBA8 splat planes
painting eight layers, and the tile plane alongside them for biome/climate/water. Demo:
`./build/toyengine heightmap_terrain_demo`.

A terrain with no height data gets it one of three ways, all one-shot (none is privileged over
another, and none re-fires once the field is saved): a `MapGenerator` bake, a hand sculpt in the
editor, or an `import:` block (see [`terra_image.h`](terra_image.h)) naming a heightmap and/or
splat PNGs to decode straight into the field:

```yaml
- type: Terrain
  mode: heightmap
  import:
    height: terrain/height.png     # 16-bit greyscale; any size -- resampled onto this terrain's grid
    layer0: terrain/splat0.png     # optional, RGBA8: layers 0-3's weights
    layer1: terrain/splat1.png     # optional, RGBA8: layers 4-7's weights
```

The import happens in `sync_field()` the first time the field `needs_bake()`; a later save
(or a `MapGenerator` bake landing first) means the key is never consulted again, same as
`generator:`'s rule. `import_height_png()`/`import_layer_png()` resample (bilinear) when the
source PNG isn't the field's exact size, so any heightmap works with any terrain's grid, not
just one authored at a matching resolution. `export_height_png()`/`export_layer_png()` are the
reverse, for pulling a sculpted/painted terrain back out to edit in another tool.

| File | Purpose |
|---|---|
| [`heightmap_terrain.h`](heightmap_terrain.h) | `HeightmapTerrain` (`TerrainComponent` with `mode = Heightmap`): the authored size, height, chunk size, `height_sub`, `import:`, tessellation and streaming budget. Parsing is `parse_heightmap_terrain()` in `heightmap_terrain.cpp`. |
| [`heightmap_terrain_system.h`](heightmap_terrain_system.h) | `HeightmapTerrainSystem` (order **61**) -- chunk objects, the GPU height / splat textures (patched from `TerraData`'s height/layer journals), the layer texture arrays, the heightfield collider, and the per-frame `TerrainFrameBinding`s the renderer reads. |
| [`terra_image.h`](terra_image.h) | `import_height_png()` / `export_height_png()` / `import_layer_png()` / `export_layer_png()` -- a pixel-level PNG bridge, outside the `.terra` format entirely; a caller (the `import:` key, an editor button, a script) decides when to use them. |

Its CPU store, change tracking and the `.terra` codec are `terra_data.h` / `terra_codec.h` /
`terra_store.h`, above -- shared with Tiles mode, not a mode-specific type.

**Drawing.** Every chunk is one instance of one shared flat patch mesh, so all chunks of a terrain
batch into a single instanced draw. The `heightmap_terrain` surface shader
([`heightmap_terrain_surface.glsl`](../../assets/shaders/heightmap_terrain_surface.glsl)) places each
vertex on the grid -- the height texture is the chunks' material displacement map, the terrain's
numbers a slot of the surface world UBO -- and its own tessellation control stages size edges
from the placed points, so chunk borders never crack. A chunk's transform carries only its corner
and its height range (for culling). Splats and the layer arrays are G-buffer set 3
([`render/terrain_gpu.h`](../render/terrain_gpu.h)). Up to 4 heightmap terrains per scene.

**Cost model.** There is nothing to load per chunk -- all chunks share one patch mesh, the height
texture and the material -- so the surface relies on frustum culling plus screen-space tessellation
(a 4-quad base patch, detail added near the camera; `tessellation.max_distance` caps where the
added detail stops and the base patch -- `patch_resolution` quads per chunk side, default 4 --
takes over. 0, the default, means `min(view_distance, 2000 m)`, **not** the unclamped
`view_distance`: a terrain whose `view_distance` runs past 2 km falls back to the base patch
before its streaming edge even without an explicit cap. A cap short of that clamp trades detail
nothing is close enough to see for a saving only where it stays past everywhere the terrain is
still visibly dense -- set it too low and the falloff reads as a ring of coarse ground sweeping
with the camera; `tessellation.edge_pixels` (default 8) is the lever that thins triangles
uniformly across the whole range instead of cliffing detail at one radius).
`view_distance` (default `auto` = the camera's far clip) is a hard cap for close / top-down cameras
or huge terrains: chunk objects beyond it are not spawned (with one chunk of hysteresis).
`shadow_distance` drops far chunks from the shadow passes (`MeshRenderer::shadow_max_distance`),
`cast_shadows` from all of them.
**Streaming budget.** Unlike Tiles mode, chunk spawning and water rebuilds are not jobified --
`HeightmapTerrainSystem::stream_chunks_()` and `refresh_water_()` both run on the owner thread, so
without pacing a teleport across a big terrain (or the first frame after switching INTO heightmap
mode, with every chunk in view missing at once) would spawn and water-fold every one of them in a
single frame. `TerrainComponent::chunk_budget_ms` (default 2 ms/frame) caps how much owner-thread
time either loop spends per frame, nearest-chunk-first, resuming where it left off next frame; at
least one chunk always lands, so the stream can never fully stall. `Engine::apply_warmup_budgets_`
raises this budget behind the loading screen, same as Tiles mode's `upload_budget_ms`, so a fresh
scene load still streams in quickly rather than being paced as if it were steady-state gameplay.
The heightfield collider's `HeightGrid` build (a multi-megabyte copy plus a pyramid scan) runs on a
`coopa::job::JobEngine` job instead of inline, published once `is_complete()`.
**Eviction:** a terrain untouched for `k_idle_frames` (120) frees its height texture and both
layer `TextureArray`s (`evict_()`); going idle this way and then becoming visible again (an
`EnvPresetSwitcher` flipping back to it, say) re-decodes every layer PNG synchronously --
`layer_texture_size` is the lever to shrink that hitch.
**Deactivation** is a separate, immediate path: the per-frame gather
(`scene.get_components<HeightmapTerrain>()`) only sees an active-in-hierarchy terrain, so the
moment one is deactivated (a preset switch, a script, an inspector toggle) it drops out of that
gather and `release_terrain_()` tears down its chunk objects AND its `HeightfieldCollider` in the
same frame -- there is nothing left to go idle. Reactivating pays the same full reload as the
`k_idle_frames` case; the two paths exist for different reasons (idle vs. explicitly hidden) but
land on the same rebuild. Details are the
expensive part: scatter chunks draw from persistent instance buffers the renderer uploads once per
build (`ParticleMeshBatch::static_key`), thin out toward their draw distance (`fade_start`) and pick
mesh LODs per chunk for a single instance at the chunk's nearest point.

**Physics.** A `HeightfieldCollider` (physxcoopa) on a runtime child: a `TriangleMesh` in
height-grid mode -- no stored triangles, everything (contacts, raycasts, sweeps, the character
motor, the navmesh bake) runs on triangles computed from the grid. Rebuilt when edits pause.

**Weather.** Snow cover as any surface; with `deep_snow`, deep snow and its trenches on flat
ground (the cover layer is then off -- the deep snow is the snow); rain darkens and glosses the
ground and fills puddles in hollows (`puddles`); GroundProbe reads the grid directly, so edit mode
is right too.

**Foliage.** ParticleSystem `mode: scatter` + `shape: terrain`
([`particles/terrain_scatter.h`](../particles/terrain_scatter.h)): deterministic per chunk, masked
by layer / slope / height, streamed around the camera, re-scattered where the terrain is edited.

**Editing.** The editor's Terrain Mode (`editor/app/ui/terrain.cpp`, brushes in
`editor/terrain/terrain_brush.h`).

## Not here yet

Caves and overhangs (the height-column model has no room for them, though mapcoopa generates cave
systems), waterfalls as their own effect (a river's step is a steep slope of its surface),
rivers/roads/towns as 3D geometry, loading only the `.terra` chunks near the camera (the file
format allows it; everything is loaded today), and a per-tile tile-set override. A mode-agnostic
water inset (today the terraced mesher still quantises `water_level` to a step and subtracts an
inset at mesh time, see **What differs at runtime** above -- the data itself carries no
mode-specific copy any more) and a keep-warm pass for an evicted heightmap (so switching back
doesn't re-decode every layer texture synchronously) are both open. An editor button for
`terra_image.h`'s import/export (today it is scene-YAML and script only -- see **Heightmap
terrain** above) is also open. Each sits on top of this layer rather than requiring it to change
shape.
