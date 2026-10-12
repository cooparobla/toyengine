# toyengine/world

World-scale ground. Two kinds of terrain live here, each its own component:

- **`TileTerrain`** (formerly `Terrain`; both names load) -- a streamed 3D tile world built from a
  [mapcoopa](../../libs/mapcoopa) `MapGraph`. Most of this file.
- **`HeightmapTerrain`** -- a sculptable, paintable heightmap for open worlds, drawn as
  GPU-displaced tessellated chunks. See [Heightmap terrain](#heightmap-terrain).

## TileTerrain

A streamed 3D tile world, built from a [mapcoopa](../../libs/mapcoopa) `MapGraph`.

The map generator produces a 2D world — elevation, biomes, rivers, roads, towns. This module
**bakes** it once into a stored grid of tiles (integer-height columns plus their climate — see
[Stored tiles](#stored-tiles)), and from then on the tiles *are* the terrain: edited in the
editor's Tile Mode or by gameplay, saved in a `.tiles` file. Every column is assembled from a
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
| [`terrain_sampler.h`](terrain_sampler.h) | `TerrainParams` (the whole tiling: tile size, height step, chunk size, view radius) and `TerrainSampler` — owns the generated `MapGraph` and answers `sample(tile_x, tile_y) -> TileColumn`. Adds the point-to-cell lookup mapcoopa has no need for: a nearest-site query over a uniform bucket index. Const after `build()`, hence concurrently readable. |
| [`terrain_chunk.h`](terrain_chunk.h) | The mesher. `sample_chunk_columns()` reads a chunk plus a one-tile neighbour skirt; `mesh_chunk_styled()` assembles its columns into a `ChunkMeshData`; `build_chunk_collision()` turns that into a static collision mesh. Pure — no Vulkan, no Scene, no AssetManager — which is what lets it run on a worker and be tested with no device. `ChunkBuildJob` bundles one job's inputs and outputs into a single shared object. |
| [`terrain_component.h`](terrain_component.h) | `TerrainComponent` — the scene-authored `TileTerrain` component (old name `Terrain`) (tile set, stored tiles, generator keys, tiling, collision, material) plus the long-lived state: the bake, the baked library and the chunk table, and gameplay queries (`tile_at`, `record_at`, `height_at`). Carries no per-frame logic. |
| [`tile_map_data.h`](tile_map_data.h) | `TileRecord` (16 bytes per tile: ground height, surfaces, flags, biome, climate, water depth and flow, region / country), `TileMapData` (the stored grid: edit journal, column raycast, `.tiles` files) and `TileMapStore` (every terrain's tiles, keyed by file, surviving re-parses). |
| [`tile_water.h`](tile_water.h) | Editing the water over the tiles: `flood_water`, `drain_water`, `settle_water` (after ground moves) and the bake's rounding rule -- all keeping water from standing over dry ground. |
| [`terrain_generator.h`](terrain_generator.h) | The `generator:` block: mapcoopa's own config keys over its standard map (`coopa::maps::standard_config()`), only the differences written; older scenes' flat keys still load. |
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

A TileTerrain's tiles live in a `TileMapData` ([`tile_map_data.h`](tile_map_data.h)), one
16-byte `TileRecord` per tile:

| Field | Meaning |
|---|---|
| `steps` | Ground height in whole steps -- under water, the bed |
| `kinds` | Top and side surface kinds (4 bits each) |
| `flags` | Water, Ocean, Coast, River (a channel cuts it), Flowing (its water is a river's); Road, Town reserved |
| `biome` | mapcoopa biome |
| `temperature`, `moisture` | mapcoopa's 0..1 values (temperature is a static yearly mean: 0 polar, 1 equatorial), as bytes |
| `water_depth` | Steps of water over the ground, filling to the terrace `water_level()`; 0 when dry |
| `region`, `country` | mapcoopa ids (`0xFFFF` = none) |
| `flow` | A river's downstream direction, an octant 1..8 |

**Baking.** With no `.tiles` file (or on a Regenerate) the terrain generates the mapcoopa map from
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

**Water.** mapcoopa decides where water is and how high (`coopa/maps/water_field.h`): the sea at
sea level; each lake at its spill level, just under the lowest point of its shore, over a bed at
least `lake_depth_m` deep; each river at its banked surface, under both banks and only ever
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
is where that surface meets the bank -- the tile set's shape, rounded or square -- and the beds are
ordinary ground (sand), so they collide and the water does not. `water:` takes WaterBody keys.

**Files.** `tiles:` names the file (default `terrain/<object>.tiles` beside the scene). A header,
a chunk index, then one deflated block per 32 × 32 storage chunk, fields stored plane by plane
so the terrain's coherence compresses (terrain_demo's 65k tiles: 127 KB). Saving recompresses
only chunks edited since. `TileMapStore` owns every terrain's tiles keyed by file, so re-parsing
the component (the editor does on every change) keeps the live — possibly unsaved — tiles; the
editor writes them with the scene (Ctrl S).

**Editing at runtime.** The same calls the editor's brushes use:

```cpp
auto* terrain = scene.find_first_component<toy::world::TerrainComponent>();
glm::ivec2 t;
if (terrain->tile_at(world_pos, t)) {
    const toy::world::IRect cell{t.x, t.y, t.x + 1, t.y + 1};
    terrain->tiles->at(t.x, t.y).steps -= 1;                     // dig (on dry ground)
    terrain->tiles->mark_dirty(cell);
    toy::world::settle_water(*terrain->tiles, cell);             // water beside it runs in
}
toy::world::flood_water(*terrain->tiles, {t}, 6);                // fill a basin to terrace 6
const toy::world::TileRecord* r = terrain->record_at(world_pos);   // biome, temperature...
```

`mark_dirty()` journals the rect; TerrainSystem re-meshes exactly the chunks it touches (and the
neighbours whose skirt it is), each keeping its old mesh until the new one swaps in, and the chunk
collider follows. Chunk jobs mesh a copy of their columns taken at submit, so edits never race a
running job. Owner thread only.

**Editing in the editor.** Tile Mode (`editor/app/ui/tile_mode.cpp`, tools in
`editor/terrain/tile_brush.h`): square brushes, tile selection and selection edits, undo in the
scene's timeline. See the editor manual's Terrain page.

## Collision

With `collision: true` (the default) every live chunk carries a static `MeshCollider` built from
the very triangles it draws: rounded lips collide round, voxel blocks collide square. The mesh is
welded and its adjacency and BVH built on the chunk's worker (`build_chunk_collision()`), then
handed to the collider by `shared_ptr` (`MeshCollider::set_mesh()`'s runtime overload), with no
asset involved. `physics_material:` names a `physics_materials/<name>.yaml` for all of them.
Bodies resting on terrain the camera streams away from lose their ground with it: keep
`view_radius` large enough to cover where physics happens.

## Cost model

The pipeline frustum-culls every chunk per view (see `ToyRenderPipeline::gather_meshes_()`): the
camera draws only the chunks it can see, and each shadow cascade/face draws only the chunks inside
its own frustum. `view_radius` still sets the resident set — `(2 * view_radius + 1)²` chunks meshed
and held in memory — and chunks behind the camera can still cast shadows into view. Prefer raising
`chunk_size` over `view_radius` to see further — it buys coverage at fewer, larger draws, at the
cost of a coarser streaming granularity.

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
is far lighter, its pieces being single quads. A chunk builds in about 2 ms on a worker (0.1 ms
sampling, 0.4 ms meshing, 1.4 ms weld and optimize), plus its collision mesh's weld, adjacency and
BVH. Two merges keep it there:
- Straight halves, most of any cliff and most of a lip's rings, are collected and stamped as one
  stretched strip per run.
- Tiles with no exposed edge merge into flat rectangles of equal height and kind.

The levers beyond that are the generator's `ARC_SEG` / `LIP_SEG` and `view_radius`.

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

See `assets/scenes/terrain/planet_demo`.

- **Map.** mapcoopa generates the world **on a sphere** (`topology: sphere` — cells over the whole
  globe, no edges, climate by latitude); see its README. The terrain sets that itself.
- **Layout** (`planet.h`, `PlanetLayout`). Six faces of a cube, each `face_tiles` square, bent
  onto the sphere with equal-angle faces (tiles stay near-square everywhere). The radius is
  `2 · face_tiles · tile_size / π`, so a tile in the middle of a face is `tile_size` across.
  The six faces are stored side by side in one `6N × N` `TileMapData`, so the `.tiles` format is
  the flat one. `wrap()` / `neighbour()` step across a face's edge onto the next face (turned).
- **Meshing.** Each chunk is meshed in flat face space exactly as a flat chunk, with merging
  capped at one tile (`max_merge`: a merged quad would cut a chord under the curve), then
  `warp_chunk_to_planet()` bends every vertex onto the globe and carries its normal through the
  map's Jacobian. Chunk objects sit at the planet's centre. A planet keeps every chunk loaded.
- **Water.** Each chunk's sea, lakes and rivers are WaterBodies in **their own frame** (local Z
  out from the planet at the chunk's centre, `WaterBody::local_frame`), so the water system's
  height-field queries, depth probes, flow and buoyancy still hold over a curved surface. Their
  waves come from the world position (triplanar), so neighbouring pieces meet without a seam.
- **Gravity.** The terrain registers a physxcoopa gravity source at its centre — at zero strength
  until every chunk is live, so nothing dropped at the start falls through ground still being
  built. Rigid bodies, buoyancy, `CharacterController` (walking in its own frame, up away from
  the planet), `FootIK` and the orbit `CameraController` follow the field.
- **Sky** is evaluated in the camera's own frame on the planet (`render::PlanetFrameState`: the
  sky's view rays and sun turned so the camera's up is the sky's zenith). The global **fog** is
  plain distance fog on a planet (world Z is no height there): give a planet scene a light
  `fog_density` (planet_demo: 0.0015). Neither changes a flat scene's shaders or values.
- **Shader.** Planet chunks draw with `terrain_styled_planet` (`terrain_styled.glsl` built with
  `TERRAIN_PLANET`): "up" is the per-vertex `uv.y` the warp wrote, and strata have an even grain.
  The flat `terrain_styled` carries none of that code.
- **Editor.** The World Generator's Planet toggle, Add > Planet, and Tile Mode (hover marches the
  view ray through the columns; a brush or selection stays on the face it starts on).

**Not on planets yet:** moving platforms under a character, Mount/Rider, ragdoll recovery,
navigation, the first-person camera, volumetric clouds,
and sky reflections/ambient (they still read the world-oriented sky). Large or streamed planets
and more than one planet per scene are out of scope for now.

## Heightmap terrain

`HeightmapTerrain` ([`heightmap_terrain.h`](heightmap_terrain.h)) is a plain height grid: a size and
height in metres, a chunk size and a resolution, a 16-bit greyscale heightmap PNG and two RGBA8
splat PNGs painting eight layers. Demo: `./build/toyengine heightmap_terrain_demo`.

| File | Purpose |
|---|---|
| [`heightmap_data.h`](heightmap_data.h) | `HeightmapData` -- the CPU source of truth: the height grid (metres), the splats, bilinear sampling / normals / layer weights, a surface raycast, resample on a geometry change, and the **edit journal** (`height_dirty_since(revision)`) every consumer follows. `HeightmapView` is the world-space query handle. |
| [`heightmap_store.h`](heightmap_store.h) | `HeightmapStore` -- the process-wide owner of every terrain's data, keyed by its heightmap file, so re-parsing a component (the editor does on every edit) keeps unsaved strokes. Saves the PNGs; restores an earlier shape exactly when a resize is undone. |
| [`png16.h`](png16.h) | 16-bit greyscale PNG writer (stb's zlib, our chunks) and readers. |
| [`heightmap_terrain_system.h`](heightmap_terrain_system.h) | `HeightmapTerrainSystem` (order **61**) -- chunk objects, the GPU height / splat textures (patched from the journal), the layer texture arrays, the heightfield collider, and the per-frame `TerrainFrameBinding`s the renderer reads. |

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
(a 4-quad base patch, detail added near the camera). `view_distance` (default `auto` = the camera's
far clip) is a hard cap for close / top-down cameras or huge terrains: chunk objects beyond it are
not spawned (with one chunk of hysteresis). `shadow_distance` drops far chunks from the shadow
passes (`MeshRenderer::shadow_max_distance`), `cast_shadows` from all of them. Details are the
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
systems), waterfalls as their own effect (a river's step is a steep slope of its surface), chunk LOD,
rivers/roads/towns as 3D geometry, loading only the `.tiles` chunks near the camera (the file
format allows it; everything is loaded today), and a per-tile tile-set override. Each sits on top of this layer rather
than requiring it to change shape.
