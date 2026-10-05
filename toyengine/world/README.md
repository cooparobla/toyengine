# toyengine/world

A streamed 3D tile world, built from a [mapcoopa](../../libs/mapcoopa) `MapGraph`.

The map generator produces a 2D world — elevation, biomes, rivers, roads, towns. This module
turns the first three into geometry: the map is sampled into a grid of integer-height columns,
each column contributes only its **exposed sides**, each side is an **authored mesh asset**
rotated into place, and every side in a chunk merges into one GPU mesh built on the job system.

Two looks from the same machinery, chosen by data:

- **Voxel** — every exposed side is one authored quad. Demo: `cplay terrain_test`
  ([assets/scenes/terrain_test](../../assets/scenes/terrain_test)).
- **Styled** — every tile is assembled from authored *pieces* chosen by its neighbourhood, with
  rounded lips, quarter-round corners and fillets, and each column shaped by its surface kind's
  style (soft rounded grass, firm stone). This is the Animal Crossing read, switched on by a `styles:`
  key. Demo: `cplay terrain_smooth_test`
  ([assets/scenes/terrain_smooth_test](../../assets/scenes/terrain_smooth_test)). See
  [Styled tiles](#styled-tiles).

| File | Purpose |
|---|---|
| [`tile_types.h`](tile_types.h) | The vocabulary: `TileFace` (the six sides), `TileKind` (the twelve atlas surfaces), `TileColumn`, `ChunkCoord`, and `face_transform()` — the rotation carrying the one authored side mesh onto each of the six faces. Also `atlas_cell()`, the UV rectangle a kind occupies. |
| [`tile_topology.h`](tile_topology.h) | The styled-tile classifier: `TilePiece` (the nine pieces a style is made of), `WallEnd`, `classify_quadrant()`, `classify_wall_end()`, and `ColumnPad` (a chunk's columns plus a one-tile neighbour skirt). Pure functions from a column's neighbourhood to a piece and one of eight D4 orientations (`variant_transform()` in `tile_types.h`). |
| [`tile_mesh_library.h`](tile_mesh_library.h) | `TileMeshLibrary` — bakes an authored side mesh into all six orientations once, then `append()`s one into a chunk's buffers with a scale, a translate, a UV remap and an index rebase. For styled tiles it also bakes each style's pieces into all eight orientations and **derives** their section caps (`append_styled()`). Geometry is **copied** out of its asset handles, so a baked library is immutable and safe for concurrent readers. |
| [`terrain_sampler.h`](terrain_sampler.h) | `TerrainParams` (the whole tiling: tile size, height step, chunk size, view radius) and `TerrainSampler` — owns the generated `MapGraph` and answers `sample(tile_x, tile_y) -> TileColumn`. Adds the point-to-cell lookup mapcoopa has no need for: a nearest-site query over a uniform bucket index. Const after `build()`, hence concurrently readable. |
| [`terrain_chunk.h`](terrain_chunk.h) | The mesher. `sample_chunk_columns()` reads a chunk plus a one-tile neighbour skirt; `mesh_chunk_columns()` merges every exposed side into a `ChunkMeshData`; `mesh_chunk_styled()` is the styled counterpart. Pure — no Vulkan, no Scene, no AssetManager — which is what lets it run on a worker and be tested with no device. `ChunkBuildJob` bundles one job's inputs and output into a single shared object. |
| [`terrain_component.h`](terrain_component.h) | `TerrainComponent` — the scene-authored `Terrain` component (seed, map knobs, tiling, side meshes, material) plus the long-lived state: the generator, the sampler, the baked library and the chunk table. Carries no per-frame logic. |
| [`terrain_system.h`](terrain_system.h) | `TerrainSystem` + `install_terrain_system()` — the streaming policy, at order **60**. Decides which chunks exist, submits their meshing jobs, uploads finished ones, and releases the ones the camera has left behind. |

## How a frame moves through it

```
TerrainSystem::execute()            (order 60, owner thread)
├─ begin_generation()  ──────────►  MapGenerator::generate_async()   [job engine, once]
├─ poll_generation()   ──────────►  TerrainSampler::build()          [owner thread, once]
├─ poll_side_meshes()  ──────────►  TileMeshLibrary::bake_*()        [owner thread, once]
├─ want / release chunks around CameraComponent::main()
├─ submit_queued_chunks()  ──────►  ChunkBuildJob::run()             [job engine, per chunk]
│                                     └─ sample_chunk_columns() + mesh_chunk_columns()
└─ upload_finished_chunks()  ────►  Mesh::from_arrays() + assets.create() + add_child()
                                                                      [owner thread]
```

The split is the design. Sampling and merging are nearly all the cost, are pure, and are
independent per chunk — so they run on workers. Creating GPU buffers, publishing assets and
touching the scene tree are all single-owner operations, so they stay on the owner thread.

## Four things worth knowing before changing anything here

1. **A side is a mesh, and that is load-bearing.** Every side mesh is authored once in one
   canonical orientation — the +Z face of a unit cube spanning `[0,1]³` — and the library rotates
   it onto the other five. Replacing the flat quad with a chamfered, sloped or curved patch is a
   YAML key (`side_mesh:`, or `sides: { top: … }`), never a code change. `tile_side_bevel.yaml`
   ships alongside the flat default to keep that claim honest.

2. **The one-tile pad is what prevents seams.** Face exposure at a chunk's edge needs the
   neighbouring chunk's columns, so sampling covers `(chunk_size + 2)²` and meshing walks only the
   interior. Without it every chunk assumes air past its border and walls itself in, and the world
   becomes a grid of visible boxes. `sampler_chunks_agree_across_border` in the `world` test group
   is what holds this.

3. **The two ways to let go of a chunk are different hazards.** A still-running meshing job is
   handled by shared ownership of its `ChunkBuildJob` plus parking the chunk in `Retiring` until
   its `JobHandle` can be closed (every handle must be closed exactly once). A still-in-flight GPU
   read is handled by the AssetManager's own payload grace period, which is why this module does
   no frame counting of its own. See `terrain_system.h`'s file doc.


4. **The classifier is what keeps a rounded tile set from quilting.** A rounded lip belongs on an
   exposed edge only. Stamp a rounded top on every column and a plateau becomes a quilt of buns.
   Styled tiles therefore choose a piece per quadrant and per wall half from the neighbourhood
   (`tile_topology.h`), and a tile with no exposed edge is one flat quad.
   `chunk_styled_plateau_interior_stays_flat` in the `world` group is the tripwire.
## Cost model

The pipeline frustum-culls every chunk per view (see `PixelRenderPipeline::gather_meshes_()`): the
camera draws only the chunks it can see, and each shadow cascade/face draws only the chunks inside
its own frustum. `view_radius` still sets the resident set — `(2 * view_radius + 1)²` chunks meshed
and held in memory — and chunks behind the camera can still cast shadows into view. Prefer raising
`chunk_size` over `view_radius` to see further — it buys coverage at fewer, larger draws, at the
cost of a coarser streaming granularity.

`greedy_merge` (on by default) merges coplanar faces: equal-height, same-kind tops into rectangles,
and each wall's same-kind cells into tall strips spanning neighbouring columns — about 60% fewer
triangles on `terrain_test`. Merged quads carry tile-space UVs, so a greedy chunk renders with the
`terrain` surface shader (`assets/shaders/terrain.frag`), which repeats the atlas cell per tile.
Only flat unit-quad sides merge; a bevelled side keeps the per-tile path.

`tiles_per_grid_unit` is quadratic in tiles and does not add detail the map does not have; past
the point where a map cell is a handful of blocks wide, all it resolves is
`MapConfig::terrain_roughness`'s fractal displacement.

## Styled tiles

A `styles:` key on the `Terrain` component switches every chunk from the voxel meshers to
`mesh_chunk_styled()` and the `terrain_styled` surface shader:

```yaml
styles:            # name -> mesh prefix; pieces load from meshes/<prefix>_<piece>.yaml
  round: tile_round
  rock: tile_rock
kind_styles:       # which style shapes each TileKind's columns (give `default` explicitly)
  default: round
  stone: rock
```

### Pieces

A style is nine meshes, each authored once in one canonical frame of the unit cell `[0,1]^3`
(Z up) and baked into all eight D4 orientations (`variant_transform()`: mirror across
`x = 0.5`, then quarter turns about +Z):

| Piece | Canonical frame | Placed where |
|---|---|---|
| `top_inner`, `top_edge`, `top_outer` | the +X+Y quadrant of the top, `z = 1`; `top_edge` exposed on +Y | each quadrant of a top with an exposed edge, by whether its two cardinals are lower — three shapes cover all 47 blob cases |
| `wall_cap_{continue,convex,concave}` | the right half (`x ∈ [0.5,1]`) of the +Y wall, with the lip's round-over | the topmost cell of a wall |
| `wall_{continue,convex,concave}` | the same half, a vertical extrusion of the plan profile | every cell below |

A wall half's shape is its **end state** at that cell (`WallEnd`): the cliff continues straight
onto the next column, wraps a convex corner, or fillets a concave one. A concave fillet is
material added into the *lower* cell's corner, so each of the two walls meeting there owns a 45°
half.

**One style per column.** The column's top kind picks the style for its top and its whole cliff,
lip to base. An ACNH cliff is one smooth shape all the way down. Its soil band and stone below are
the atlas kinds the triangles carry (up-facing triangles of the lip take the top kind, so grass
rounds over its own edge), not different geometry.

### Section caps (how different styles meet)

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

`chunk_styled_surface_has_no_holes` ray-casts busy random pads with all four shipped styles and
fails on any see-through hole or exposed back face. Run it with `STYLED_RAY_STRESS=1` (120k rays)
after changing a piece, the classifier or the cap rules.

### Surface: `terrain_styled.frag`

Styled chunks carry no texture coordinates beyond their kind: every vertex's UV is the centre of
its kind's atlas cell. The shader reads the kind's colour and roughness there, then draws the
detail procedurally. It uses world-space value noise at a few frequencies, plus specks, strata on
stone and a slight bump from the noise's analytic gradient, with each octave faded out as it
nears the pixel's footprint. In practice that is an analytic mip chain: sharp up close, calm at a
distance, seamless across lips, corners and chunks, and independent of the engine's (absent)
texture mipmapping. Per-kind character is a table in the shader, in `TileKind` order. The smooth
atlas (`terrain_atlas_smooth.png`) is therefore just a palette.

### Cost

On `terrain_smooth_test` a styled 32×32 chunk averages about 11k triangles and 9k vertices. That
compares with about 2.8k triangles voxel per tile and 0.7k greedy. A chunk builds in about 2 ms on
a worker (0.1 ms sampling, 0.4 ms meshing, 1.4 ms weld and optimize). Two merges keep it there:
- Straight halves, most of any cliff and most of a lip's rings, are collected and stamped as one
  stretched strip per run.
- Tiles with no exposed edge merge into flat rectangles, as in the voxel greedy mesher.

The levers beyond that are the generator's `ARC_SEG` / `LIP_SEG` and `view_radius`.

One trap in this code is easy to step into. `append_styled()` runs thousands of times per chunk,
so it must never `reserve(size + n)`: an exact-size reserve defeats the vector's geometric growth
and once made a chunk take 200 ms to mesh.

## Where the tile assets live

The stock tile set is **shared**, not scene-local:

```
assets/meshes/tile_side_flat.yaml     assets/textures/terrain_atlas.png
assets/meshes/tile_side_bevel.yaml    assets/textures/terrain_atlas_mr.png
assets/meshes/tile_<style>_<piece>.yaml   (round, soft, rock, flat -- nine pieces each)
                                      assets/textures/terrain_atlas_smooth{,_mr}.png
```

A tile side and a surface atlas are primitives of this system, not demo content the way
`pixel_demo`'s `pillar.yaml` is — every terrain scene wants the same ones, and keeping them
inside one scene would mean the second terrain scene copies them.

Nothing selects between shared and local. `coopa::asset::AssetSource::resolve()` tries the
loading scene's own directory **first** and the registered search roots (`assets/`) second, so a
scene that wants its own tile set just drops a file of the same name into its own `meshes/` or
`textures/` and wins by precedence. `assets/scenes/terrain_test/` therefore holds nothing but
its `scene.yaml`, and its `side_mesh: tile_side_flat` still resolves.

One consequence worth knowing: `assets/textures/` is now a fallback namespace for *every* scene's
`textures/…` references. Nothing collides today — `pixel_demo` and `material_maps_test` carry
their own `crate_*.png` / `noise_mask.png`, which win locally — but a scene that typos a local
texture name can now resolve to a shared file instead of failing outright.

## Generating the assets

Both are standalone tools, not part of the build:

```bash
python3 tools/gen_tile_side_meshes.py   # assets/meshes/tile_side_{flat,bevel}.yaml
python3 tools/gen_tile_styles.py        # assets/meshes/tile_<style>_<piece>.yaml
python3 tools/gen_terrain_atlas.py      # assets/textures/terrain_atlas{,_smooth}{,_mr}.png (numpy, Pillow)
```

The two mesh generators share `tools/mesh_yaml.py`'s writer. All are deterministic: re-running
one reproduces its files' contents. The PNGs' bytes can still differ across Pillow versions even
when every pixel matches. Each style in `gen_tile_styles.py` is a handful of constants: corner
radii, lip size, segment counts, and optional facet jitter of interior wall vertices. A new style is a new entry there plus a line
under the scene's `styles:`.

`KINDS` in the atlas generator must stay in `TileKind` order — the C++ side derives a cell's UV
rectangle from the enum value alone, so a kind is appended, never inserted.

## Not here yet

Caves and overhangs (the height-column model has no room for them, though mapcoopa generates cave
systems), water as its own blended pass rather than an opaque surface, chunk LOD,
rivers/roads/towns as 3D geometry, and terrain colliders. Each sits on top of this layer rather
than requiring it to change shape.
