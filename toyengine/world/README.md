# toyengine/world

A streamed 3D tile world, built from a [mapcoopa](../../libs/mapcoopa) `MapGraph`.

The map generator produces a 2D world — elevation, biomes, rivers, roads, towns. This module
turns the first three into geometry: the map is sampled into a grid of integer-height columns,
each column contributes only its **exposed sides**, each side is an **authored mesh asset**
rotated into place, and every side in a chunk merges into one GPU mesh built on the job system.

Demo scene: `cplay terrain_test` ([assets/scenes/terrain_test](../../assets/scenes/terrain_test)).

| File | Purpose |
|---|---|
| [`tile_types.h`](tile_types.h) | The vocabulary: `TileFace` (the six sides), `TileKind` (the twelve atlas surfaces), `TileColumn`, `ChunkCoord`, and `face_transform()` — the rotation carrying the one authored side mesh onto each of the six faces. Also `atlas_cell()`, the UV rectangle a kind occupies. |
| [`tile_mesh_library.h`](tile_mesh_library.h) | `TileMeshLibrary` — bakes an authored side mesh into all six orientations once, then `append()`s one into a chunk's buffers with a scale, a translate, a UV remap and an index rebase. Geometry is **copied** out of its asset handles, so a baked library is immutable and safe for concurrent readers. |
| [`terrain_sampler.h`](terrain_sampler.h) | `TerrainParams` (the whole tiling: tile size, height step, chunk size, view radius) and `TerrainSampler` — owns the generated `MapGraph` and answers `sample(tile_x, tile_y) -> TileColumn`. Adds the point-to-cell lookup mapcoopa has no need for: a nearest-site query over a uniform bucket index. Const after `build()`, hence concurrently readable. |
| [`terrain_chunk.h`](terrain_chunk.h) | The mesher. `sample_chunk_columns()` reads a chunk plus a one-tile neighbour skirt; `mesh_chunk_columns()` merges every exposed side into a `ChunkMeshData`. Pure — no Vulkan, no Scene, no AssetManager — which is what lets it run on a worker and be tested with no device. `ChunkBuildJob` bundles one job's inputs and output into a single shared object. |
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

## Three things worth knowing before changing anything here

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

## Cost model

The pipeline does **no per-mesh frustum culling** (see `PixelRenderPipeline::gather_meshes_()`), so
every live chunk is drawn and shadow-cast every frame. `view_radius` is therefore a direct cost
dial: the live set is `(2 * view_radius + 1)²` draw calls. Prefer raising `chunk_size` over
`view_radius` to see further — it buys coverage at a constant draw-call count, at the cost of a
coarser streaming granularity.

`tiles_per_grid_unit` is quadratic in tiles and does not add detail the map does not have; past
the point where a map cell is a handful of blocks wide, all it resolves is
`MapConfig::terrain_roughness`'s fractal displacement.

## Where the tile assets live

The stock tile set is **shared**, not scene-local:

```
assets/meshes/tile_side_flat.yaml     assets/textures/terrain_atlas.png
assets/meshes/tile_side_bevel.yaml    assets/textures/terrain_atlas_mr.png
```

A tile side and a surface atlas are primitives of this system, not demo content the way
`pixel_demo`'s `cube.000.yaml` is — every terrain scene wants the same ones, and keeping them
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
python3 tools/gen_terrain_atlas.py      # assets/textures/terrain_atlas{,_mr}.png
```

Both are deterministic: re-running either reproduces its files byte for byte.

`KINDS` in the atlas generator must stay in `TileKind` order — the C++ side derives a cell's UV
rectangle from the enum value alone, so a kind is appended, never inserted.

## Not here yet

Caves and overhangs (the height-column model has no room for them, though mapcoopa generates cave
systems), water as its own blended pass rather than an opaque surface, LOD or greedy face merging,
rivers/roads/towns as 3D geometry, and terrain colliders. Each sits on top of this layer rather
than requiring it to change shape.
