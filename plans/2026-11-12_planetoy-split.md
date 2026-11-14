# Moving planetary worlds out of toyengine, into planetoy

**Done**, except the editor extension API (below). `tools/check_core_is_flat.sh` is the gate.

## Why

Planetary support worked -- `planet_demo` rendered a 1024-tile-per-face globe with a derived
atmosphere, horizon culling and coarse chunk builds -- but it served exactly one scene, and it got
there by threading a `planet: true` flag through code written for flat terrain. **57 files** under
`src/toyengine` and `src/editor` mentioned planets.

Both modes were the worse for it. The planet mode was itself incomplete -- `weather/` had no
planet path at all (sun path, wind bearing and ground probe are all world-Z), and
`world/terrain_field.h` is flat-XY, so `GroundProbe`, foliage scatter and gameplay height queries
were silently flat on a globe -- while the flat engine carried vocabulary and branches for a shape
it never used.

The decision: **one or the other.** `toyengine` is flat-terrain only and does not know planets
exist. Planetary support is `~/git/planetoy`, its own project on its own engine branch
(`mode: worktree`, branch `planetoy`), supporting only planets.

## What the engine kept: seams, not shapes

Each costs a flat terrain one null compare, and `nullptr` (or `WorldFrame::radius <= 0`) **is**
the flat world -- there is no `FlatChunkShaper` to pay for.

| Seam | Shape |
|---|---|
| `env/world_frame.h` | `WorldFrame{centre, radius}`; `curved()`, `up_at()`, `altitude_at()`, `tangent_at()`, `point_at()`, free `transport()` |
| `world/chunk_shaper.h` | `ChunkShaper`: `chunk_origin`, `shape`, `seam_locks`, `allows_style_lods`, `allows_coarse_builds` |
| `world/tile_grid.h` | `TileGrid`: `neighbour`, `tile_from`, `tile_at`, `clip_brush`, `region_at`, `grid_point`, `raycast`, `collides_everywhere`, `world_bounds` |
| `world/terrain_system.h` | five `protected virtual`s: `stream_()`, `order_chunks_()`, `chunk_job_priority_()`, `relief_of_()`, `after_terrains_()` -- base bodies are the flat ones verbatim |
| `world/terrain_yaml.h` | `set_terrain_parse_hook()` -- ADDS terrain keys the engine does not model |
| `mapgen/world_bake.h` | `set_world_bake_hook()` -- replaces the flat tile walk; `TerrainSampler::cell_at()` is the one thing an arbitrary walk needs |
| `core/engine.h` | `set_world_frame_source()`, `set_sky_state_hook()`, `set_cloud_state_hook()`, `drive_sky_gradient()`, public `device()`/`allocator()` |
| `core/module.h` | `on_engine_init`, `on_prepare_scene`, `on_render_config` -- three moments, because a surface shader must be registered before the pipeline builds its variants |

`TerrainParams::surface_shader` became a terrain parameter rather than a sixth virtual: it is
asked once per chunk object and is a legitimate authoring knob.

## Renames, so nothing planet-shaped was left

`planet_peak_` -> `relief_peak_`; `TerrainComponent::planet_coarse_builds` -> `coarse_builds`
(it pairs with `ChunkShaper::allows_coarse_builds()`, a core seam, and the engine parses it);
`kPlanetOffset` -> `kGroundOffset`; `TerrainParams` lost `planet` and `layout()`, keeping
`face_tiles` as a generic face width -- a shaper is handed nothing but `const TerrainParams&`, so
its grid dimension has to live there.

`MapGeneratorComponent::sphere` **stays** in core: it is a real mapcoopa topology. Only the
`sphere = params.planet` derivation left, and planetoy restores that coupling in
`ensure_planet_map_is_spherical()`, because "a planet's map must be spherical" is planet knowledge.

## The test split

`terrain_chunk_test` and `terrain_chunk_cost_test` instantiated `PlanetChunkShaper` directly,
pinning the planet classes inside the engine -- so the split had to happen *with* the class move.

- The six coarse-build tests **stayed**, driven by a local no-op `CoarseShaper`. They test the
  engine's `coarse_only` machinery, which is gated on `allows_coarse_builds()`; the planet was only
  ever the vehicle.
- `field_lod_level_is_appended_and_works_on_a_planet` split in two: the engine keeps the flat half
  (vertices appended, indices local), planetoy gets `field_lod_level_is_warped_onto_the_globe`.
- `two_planet_chunks_agree_on_their_shared_seam` moved whole.
- `busy_pad` was lifted into `tests/engine/support/terrain_fixtures.h` so both trees share it.

## Four latent project-tooling bugs this surfaced

None were planet-specific: no game project had ever had its own test suites, so none of this had
been exercised.

1. `tools/toyhub:98`, `templates/project/setup.sh:60` and `hub_core.h:264` tested for
   `<root>/toyengine` after the move to `src/` -- so `toyhub new --worktree` could not run at all.
2. `load_tile_piece()` resolved shipped assets against `ROOT_DIR`, which in a project's tree is
   the PROJECT root. Now searches project assets then engine assets, via a
   `TOY_ENGINE_ASSETS_DIR` the project test target defines.
3. Every coopa library generates a `root_directory.h`, and only the engine's defines `SRC_DIR`;
   all sit on equally PUBLIC include paths, so a project's editor test target picked up a coopa
   one. The engine now exports `TOYENGINE_CONFIG_INCLUDE_DIR` and the test function puts it first.
   `scene_variant()` had the same project-vs-engine confusion and now resolves project-first.
4. `toyengine_add_project_tests()` globbed the engine's test support but not the editor's, so no
   project could build an `editor_gpu` suite; and `RuntimeLayout::shader_roots()` gated the build
   tree's `project_shaders` on the *running* project having shader sources -- which an editor GPU
   test, running as a scratch project, does not.

## Verified

- Gate: zero `planet|cube.?sphere` in `src/toyengine`, `src/editor`, `src/tests`. `globe` and
  `sphere` are deliberately not matched -- a lamp's glass globe, the turntable-vs-trackball
  distinction, and mapcoopa's real topology.
- Engine: **239/239, full sweep at `-j8` in 183 s** (161 unit + 74 GPU, both runners), after the
  sweep that touched widely-included headers. The first full run came back 238/239: the
  `shader_roots()` change above had been gated on `is_directory(built/"project_shaders")`, which
  is false in the engine's own tree, so `runtime_layout` lost the root it asserts comes first.
  The condition is `own_shaders || is_directory(...)` -- the project has shaders of its own, OR
  this build compiled some. Only the full tier catches that: the unit tier was green either way
  because the suite is in the engine runner, which the targeted builds had not rebuilt.
- planetoy: 4 unit + `planet_world` (GPU) + `planet_tile_mode` (editor GPU) build and pass.
- planet_demo end to end: `1536 chunks, 591 MB of mesh, 1579 ms, relief peak 256` -- the
  pre-split numbers.

**One inherited flake.** `planet_world` fails about 1 run in 4 under `ctest -j8`, passing 5/5
alone; the check varies (buoyancy settle, lit-hemisphere luminance). It is the concurrency
sensitivity this suite had before the move, and it moved with it. Not yet diagnosed.

## Follow-up: a project made by `toyhub new` had no config.yaml

planetoy rendered visibly unlike toyengine after the split, and the cause was not the split: it
had no `assets/config.yaml`. `src/main.cpp:95` builds exactly ONE path for it --
`<project_root>/assets/config.yaml` -- and `AppConfig::load()` falls back to hardcoded defaults
when it is missing, with no search across asset roots. So planetoy ran on engine defaults while
toyengine used its tuned 239-line config: no TAA, no auto-exposure, no bloom, no contact shadows.

The editor's project path already seeds it (`Project::default_config()`,
`src/editor/app/project.cpp:644`) from the engine's own config, overriding three keys. A project
scaffolded by `toyhub new` never goes through that, and `templates/project/` ships no `assets/`
at all -- which is why habitat, voxlandia and outlandia all carry one and planetoy did not
(voxlandia's diff against toyengine's is exactly those three keys).

Fixed in both places: planetoy got its copy, and `templates/project/setup.sh` now seeds one when
missing, never overwriting a project's own. Verified by capture -- planetoy vs toyengine on the
same scene now agree to **mean luminance 51.08 vs 51.14** with an identical maximum; the ~7% of
pixels that differ are the spinning cloud deck and TAA jitter between two independent runs.

Also ported into this branch: the three stale `<root>/toyengine` checkout tests
(`templates/project/setup.sh:60`, `tools/toyhub:98`, `src/hub/hub_core.h:264`). Those fixes were
made in `~/git/toyengine`'s working tree and never committed, so the worktree -- checked out from
committed state -- did not have them, leaving planetoy's own `./setup.sh` broken.

## Not done: the editor extension API

Removed from the engine's editor and not yet replaceable by a project:

- `Add > Planet` (`EditorApp::create_planet()`) and its two menu items
- the World Generator's Planet / Face Size / Gravity rows, the unrolled sphere preview, and
  `world_generator_planet()`
- the `PlanetAtmosphere` / `PlanetCameraFit` inspector schemas and sections
- the `planet_clouds` material shader entry

The blocker is ordering: `TOY_MODULE`'s `on_engine_init` runs **before** `EditorApp` registers its
own sections, so no project can replace or add one. The API wants a
`register_editor_hook(id, fn)` run at the *end* of `EditorApp`'s constructor, plus
`register_add_menu_item()` and `register_world_shape()` for the generator's rows. Tile Mode and
`frame_all()` need nothing -- they are already `TileGrid` queries, which is why
`planet_tile_mode_test` still passes after being decoupled from the two removed entry points.
