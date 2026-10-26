# Dynamic flowing water over HeightmapTerrain

> **Status: designed, nothing landed.** Additive — a third water *producer*. The existing
> `WaterBody` (planar / mesh-river) and `TileTerrain` chunk-water paths are untouched.
>
> **Difficulty: moderate.** ~2,600 new lines across ~20 files, of which only ~250 modified lines
> touch shipped water code. No new shader, no new render pass, no new buoyancy code, no GPU work.

## Why

Two kinds of water ship today, and neither flows on its own:

- **`WaterBody`** (`toyengine/water/`) — a planar lake/ocean, or a *flowing* river whose current is
  a **load-time bake off an authored mesh** (`water_flow_bake.h`: gravity projected onto each
  triangle, UV `u` on level reaches). Geometry is authored; water never goes anywhere it was not
  drawn.
- **`TileTerrain` chunk water** (`toyengine/world/`) — mapcoopa floods tiles at generation time and
  `TerrainSystem::add_chunk_water_()` spawns child `WaterBody`s fed per-chunk geometry with exact
  authored depths. Runtime editing exists (`tile_water.h`: `flood_water()`, `drain_water()`, a
  resumable `settle_water()`), but it is a **discrete tile flood-fill** — right for blocky and
  terraced worlds, wrong for a smooth one. **It stays as it is.**

`HeightmapTerrain` — the sculptable, continuous landscape — has **no water coupling at all**. Its
surface shader fakes "water gathers in hollows" with a cavity term
(`assets/shaders/heightmap_terrain_surface.glsl:117-126`) and that is the whole of it.

**What this adds:** place a source on a heightmap terrain and water runs downhill, follows gullies,
pools where the ground closes, finds its level, and carries things that float — a real per-cell
simulation stepped every frame, responding to the terrain being sculpted underneath it.

## Why it is cheap

The water module is already *data-in / query-out*, so this is a new producer, not a new water
system:

- `WaterSurfaceQuery` (`water_surface_query.h`) is **completely producer-agnostic**. Buoyancy,
  gameplay and `WaterSystem::sample()` read nothing but
  `WaterVertex{position, flow, depth, turbulence, wave_scale}`.
- `WaterSystem::plan_tiles_()` already takes exactly
  `(local_pos, indices, flow_ws, depth, turbulence, wave_scale, grid_res)` — tiling, LOD chains,
  meshoptimizer, GPU upload, child-object publishing, `apply_material_()` and the `water` surface
  shader need **zero changes**.
- `bake_()` already skips `weld_by_position` for grids (`grid_res >= 1`) and already replaces the
  stage-2 depth raycasts with authored depths. The only genuinely missing input channels are
  **per-corner flow and turbulence**, which today come only from `bake_flow()`.
- The shader already carries depth and turbulence in `uv`, the current in `tangent.xyz`, measures
  real per-pixel depth off the Hi-Z buffer, and draws shore foam, contact foam, refraction and the
  underwater look from that.
- `add_chunk_water_()` (`toyengine/world/terrain_system.cpp:738-764`) is a working precedent for
  "a terrain spawns chunk water bodies and feeds them geometry", content hash included.

## Decisions

| | |
|---|---|
| **Domain** | One field per `HeightmapTerrain`, **sparsely allocated** in 16×16-cell blocks. Pools persist forever. An empty terrain allocates nothing and costs 0 ms. |
| **Solver** | Pipe-model / hydrostatic (Mei et al. 2007): per-cell depth + 4 outflow fluxes driven by surface-height difference, outflow scaled so a cell cannot drain more than it holds. Mass-conserving, unconditionally stable, no momentum advection. |
| **No erosion** | The heightmap is never written. |
| **Coupling** | Buoyancy + gameplay queries only. |
| **Compute** | CPU on the libcoopa `JobEngine`; the solver is a pure, engine-free, unit-testable function. |

---

## 1. Shape

```
HeightmapTerrainSystem   order 61   HeightmapData is current this frame
HydroSystem              order 62   NEW
  ├─ bed  <- HeightmapData (dirty-rect refresh only)
  ├─ step the sparse pipe-model field on job workers
  ├─ extract per-chunk WaterVertex arrays (budgeted, camera-limited)
  └─ WaterBody::set_surface(...) on runtime child objects
WaterSystem              order 90   bakes the supplied surface: query.build -> plan_tiles_ -> publish
PhysicsSystem            order 100  buoyancy floats on it, unchanged
```

**`k_hydro_system_order = 62`** — after `HeightmapTerrain` (61) so bed data and chunk streaming are
current; before `Water` (90) so a surface published this frame is baked in the *same* frame (no
one-frame lag), and therefore before `Physics` (100) so buoyancy never samples a stale surface.

### New — `toyengine/water/`

| File | Contents |
|---|---|
| `hydro_field.h` / `.cpp` | `HydroField` — the sparse-block pipe-model solver. **glm only, zero engine deps**, in the style of `water_flow_bake.h` and `snow_field.h`, so it unit-tests with no scene, no GPU, no physics. |
| `hydro_mesh.h` / `.cpp` | `build_hydro_surface()` — pure field → `WaterVertex` + indices for one chunk. No engine deps. |
| `hydro_source.h` / `.cpp` | the `TerrainWater` and `WaterSource` components + `register_hydro_components()`. |
| `hydro_system.h` / `.cpp` | `HydroSystem`, `install_hydro_system()`, `find_hydro()`, `k_hydro_system_order`. |

### Modified

| File | Change |
|---|---|
| `water/water_body.h` / `.cpp` | `WaterMode::Field`; `set_surface()`; `surface_vertices` / `surface_indices` |
| `water/water_system.h` / `.cpp` | early branch in `bake_()`; its tail refactored into `finish_bake_()` |
| `water/water_settings.h` / `.cpp` | three tier knobs (§4) |
| `scene/register.cpp` | `TerrainWater` + `WaterSource` parsers, reusing `parse_water_settings()` (:67-99) |
| `core/engine.cpp` | `register_hydro_components()` near :168; `install_hydro_system()` in `prepare_scene_()` with an order comment |
| `core/engine_render_sync.cpp` | the new knobs in `water_settings_for_()` (:399) |
| `editor/schema/component_schema.cpp` / `component_sections.cpp` | two `add({...})` + two `layout(...)` blocks |

---

## 2. `HydroField`

Sparse allocation is **not optional**. `heightmap_terrain_demo` is 1024 m at 1 m texels; a dense
field at 0.5 m cells would be 4.2M cells ≈ 67 MB for a terrain that may hold one pond, and the
per-frame dry-cull sweep would run over all 4.2M cells even when nothing is wet. Sparse blocks make
memory and cost **O(wet area)** — ~180 active blocks ≈ 46k cells ≈ 750 KB.

```cpp
/** @brief Cells per side of one allocated block. */
inline constexpr int k_hydro_block = 16;

/** @brief Tunables for HydroField::step(); mirror TerrainWater's fields. */
struct HydroParams {
    float cell = 1.0f;            ///< Metres per sim cell.
    float min_depth = 0.01f;      ///< Depth (m) under which a cell is culled to exactly dry.
    float seep_rate = 0.005f;     ///< Metres/s absorbed by the ground, under `seep_depth`.
    float seep_depth = 0.05f;     ///< Depth (m) under which seepage applies -- what dries films.
    float evaporate_rate = 0.0f;  ///< Metres/s lost everywhere.
    float friction = 0.0f;        ///< 0..1 flux damping per step; 0 = frictionless pipes.
    float sim_dt = 1.0f / 60.0f;  ///< Fixed substep (s), for frame-rate independence.
    int   max_substeps = 2;
    int   max_blocks = 4096;      ///< Hard cap; the overflow is logged, never silent.
};
```

**Per block**, interior 16×16 authoritative plus a **1-cell apron** so the flux stencil is purely
local:

- `float depth_[(k+2)*(k+2)]` — apron mirrored from neighbours (0 where no block exists).
- `float bed_[(k+2)*(k+2)]` — cached; the apron reads `HeightmapData` **directly**, because bed
  exists everywhere even where no block does, and outflow into a dry neighbour needs its bed height.
- `float flux_[k*k][4]` — L/R/D/U outflow, m³/s.
- `uint32_t content_hash_` — quantized depth, for the republish test (§4). Free: written during
  integrate.
- `int quiet_frames_`.

Blocks live in a stable `std::vector<HydroBlock>` with a free list, indexed by an open-addressed
`block key -> index` table. **Allocation and freeing are serial and iterate keys in sorted order**,
so the block→index mapping is identical on any worker count.

### The step

Five passes, each `run_range(par, block_count, ...)` over the allocated list, every pass writing
**only its own block** — the rule `tests/engine/gpu/water_parallel_test.cpp` enforces:

1. **Guarantee neighbours.** *Serial, cheap.* Any block holding water guarantees its 8 neighbours
   are allocated. This is the trick that makes block birth deterministic and removes it from the
   parallel passes entirely — an outflow always has a real destination, so there is no
   order-dependent "record-then-allocate" phase.
2. **Halo exchange.** Each block reads its neighbours' interiors, writes only its own apron.
3. **Flux.** Per cell, `H = bed + depth`; accelerate each outflow by
   `A*g*dt*(H_self - H_neigh)/cell`, damp by `friction`, clamp at 0, then **scale all four so
   `sum(outflow)*dt <= depth*cell²`**. That scaling is the entire stability argument.
4. **Integrate.** `depth += dt*(inflow - outflow)/cell²`, a boundary cell's inflow being a read-only
   look at the neighbour's flux array. Then `seep_rate` under `seep_depth`, `evaporate_rate`
   everywhere, and `depth < min_depth -> 0` with the lost volume accumulated. Update
   `content_hash_`.
5. **Reap.** A block whose interior is dry and none of whose neighbours hold water increments
   `quiet_frames_`; freed past `k_hydro_free_after = 30`. The hysteresis stops a draining edge
   thrashing the pool and the published chunk meshes.

Substeps: `n = clamp(round(frame_dt / sim_dt), 1, max_substeps)` with the remainder carried, so the
result is frame-rate independent and reproducible.

### API

```cpp
class HydroField {
public:
    HydroField(glm::ivec2 cells, glm::vec2 origin_xy, const HydroParams& params);

    /** @brief Steps the field `dt` seconds. `bed` answers the ground height at a field cell. */
    void step(float dt, const BedFn& bed, const ParallelFor* par = nullptr);

    /** @brief Adds `volume` cubic metres over a disc, spread by area. Negative drains. */
    void add_volume(const glm::vec2& centre_xy, float radius, float volume);

    /** @brief Re-reads cached bed heights for every allocated block overlapping `cells`. */
    void invalidate_bed(const glm::ivec4& cells, const BedFn& bed);

    float depth_at(const glm::vec2& xy) const;    ///< Bilinear; 0 outside every block.
    float surface_at(const glm::vec2& xy) const;  ///< bed + depth.
    glm::vec2 flow_at(const glm::vec2& xy) const; ///< m/s.

    std::size_t block_count() const;
    float total_volume() const;   ///< Live water, m^3 -- the conservation test reads this.
    float volume_in() const;      ///< Everything ever added.
    float volume_lost() const;    ///< Seeped + evaporated + culled + off the terrain edge.
    std::uint64_t version() const;
};
```

`using BedFn = std::function<float(int, int)>;`, and `ParallelFor` is the existing
`water/water_parallel.h` alias — so the solver runs inline with no job engine and
`water_parallel.h`'s thresholds apply unchanged.

### Sim cell vs terrain texel

Default sim cell = the terrain's own `HeightmapData::cell()`, with `subdivision: 1 | 2 | 4` for
finer water. **Integer powers of two only** — at `subdivision: 1` bed sampling is a direct index
with no interpolation and the dirty-`IRect`→cell mapping is exact; a non-integer ratio makes both
inexact and the two grids disagree at chunk borders. The parser clamps to the nearest power of two
and warns.

---

## 3. Boundary, conservation, and the thin-film trap

- Outflow across the terrain's XY bounds is **deleted** and counted, so a test can assert
  `volume_in == total_volume + volume_lost` exactly.
- **The trap, stated plainly:** `min_depth` too small, or seepage too weak, and a one-molecule film
  creeps outward forever — every block it touches stays allocated, the active set grows without
  bound, and the feature looks exactly like a memory leak. `seep_rate` applied *only under*
  `seep_depth` is the fix: thin films dry in a second or two while pools deeper than `seep_depth`
  persist untouched. Defaults 0.005 m/s under 0.05 m, with `min_depth = 0.01 m`.
- `max_blocks` is a hard cap. On reaching it the field refuses new blocks and **logs what it
  dropped**, following `kMaxEnvSources` (`env/env_field.h`) and `heightmap_terrain.cpp:103-106`. A
  silent cap would read as "it covered everything".

---

## 4. The `WaterBody` / `WaterSystem` seam, and the republish budget

### The seam

The field knows position, depth, flow and turbulence **exactly**, so none of `weld_by_position`,
`bake_flow()`, `probe_depth_()` or the per-triangle normal sum is needed.

```cpp
// water_body.h
enum class WaterMode { Planar, Flowing, Field };

/**
 * @brief Supplies the surface as already-final, already-welded, WORLD-space vertices -- for water
 *        whose producer knows its own depth, current and turbulence (a simulated field). Takes
 *        priority over every other geometry source; the bake skips the weld, the flow bake and the
 *        depth probes outright and is final on its first pass.
 * @param vertices World-space WaterVertex array (position, flow, depth, turbulence, wave_scale).
 * @param indices  Triangle triples into `vertices`.
 */
void set_surface(std::vector<WaterVertex> vertices, std::vector<std::uint32_t> indices);

std::vector<WaterVertex>   surface_vertices;
std::vector<std::uint32_t> surface_indices;
```

In `water_system.cpp`: refactor the tail of `bake_()` (from `query.build()` through `bake_stage`)
into a private `finish_bake_()`, then add an early branch that unpacks the supplied vertices into
the arrays `plan_tiles_()` already expects (`grid_res = -1`, so `build_mesh_tiles()`), calls
`finish_bake_()`, and marks `bake_stage = 2`.

**~120 modified lines.** `plan_tiles_`, `publish_tiles_`, `make_vertices_`, `apply_material_`, the
shader, the tiles, the LOD chain, ripples, `UnderwaterPass` and buoyancy are untouched. The two
guards in `make_vertices_` (`water_system.cpp:647-656`) still apply and are honoured for free: flow
is never exactly zero (stock vertex shaders normalize the tangent) and
`tangent.w = 2 + clamp(wave_scale, 0, 1)`.

Note the input `uvs` that `set_geometry` takes are **not needed at all** here: they exist only for
`bake_flow()`'s UV-`u` fallback, and this path has real flow vectors.

### The republish budget — the central perf risk

A settled pool changes nothing and must cost zero; a live river changes every frame. Four
mechanisms, in order of importance:

1. **Camera-limited publishing.** Only chunks within `WaterSettings::sim_radius` of the focus
   publish geometry. Beyond it the field keeps simulating (cheap) and draws nothing. Matches
   `WaterSystem`'s own idiom; the single biggest lever.
2. **A changed-enough test.** Each block's `content_hash_` is quantized depth (1 cm), written during
   integrate for free. A chunk's hash combines its blocks'. Equal hash → no republish, so **a pool
   at rest costs literally nothing**.
3. **A republish rate cap** — `hydro_republish_hz`, per tier.
4. **A per-frame chunk budget** — `hydro_chunks_per_frame`, per tier.

The numbers: a 32 m chunk at 1 m cells is 33×33 ≈ 1.1k corners; the demo's 64 m chunks are 65×65 ≈
4.2k. A `set_surface` bake is the *cheapest* bake in the system, and its expensive half
(`TileBuild`) is already on a job. At 2 chunks/frame that sits comfortably inside
`bake_budget_ms = 1.5`, and because hydro bodies go through `bake_()` they are charged to
`bake_spent_ms_` automatically and queue fairly against ordinary water bakes.

New in `water_settings.h` (+ `from_quality()` + `Engine::water_settings_for_()`):

| | Low | Medium | High | Ultra |
|---|---|---|---|---|
| `hydro_republish_hz` | 6 | 10 | 15 | 20 |
| `hydro_chunks_per_frame` | 1 | 1 | 2 | 3 |
| `hydro_render_step` (render cells per sim cell) | 4 | 2 | 1 | 1 |

**Prove it, don't assume it:**

```bash
HEADLESS=1 NO_INPUT=1 PROFILE=out.csv MAX_FRAMES=900 TOY_VALIDATION=0 ./build/toyengine flow_demo
tools/plot_profile.py out.csv            # cpu.scene.water
TOY_WATER_LOG_BAKES=0.5 HEADLESS=1 MAX_FRAMES=900 ./build/toyengine flow_demo
```

Add `toy::debug::watch()` counters for active blocks, wet cells, published chunks and step ms. A
low-vs-ultra pixel diff is the only acceptable proof the new tier knobs are live — writing any of
them in `config.yaml` makes the whole `water_quality` preset inert (an existing, documented trap).

---

## 5. Mesh extraction

```cpp
/** @brief Per-chunk surface params; mirrors TerrainWater's render fields. */
struct HydroSurfaceParams {
    int   render_step = 1;          ///< Render cells per sim cell (quality tier).
    float render_min_depth = 0.02f; ///< Depth (m) a cell needs before it is drawn.
    float wave_full_depth = 0.5f;   ///< Depth (m) at which waves reach full height.
    float skirt_clearance = 0.15f;  ///< Metres the dry skirt lip drops below its bed.
};

/**
 * @brief Builds one chunk's water surface from `field`. Vertices are world space and already
 *        welded, so WaterSystem's bake needs no weld pass.
 * @param chunk_cell_min Field cell index of the chunk's min corner.
 * @param chunk_cells    Chunk edge in sim cells.
 */
void build_hydro_surface(const HydroField& field, glm::ivec2 chunk_cell_min, int chunk_cells,
                         const HydroSurfaceParams& params,
                         std::vector<WaterVertex>& out_vertices,
                         std::vector<std::uint32_t>& out_indices);
```

Mirrors `build_chunk_water()` (`world/terrain_chunk.cpp:347-470`), the proven shape:

- A quad per render cell whose covered sim cells are wet, plus a **one-cell dry skirt ring** so the
  shoreline hides under the bank. `edge_fade_depth` and `shore_foam_depth` then give the soft
  contact and the foam band with no extra work.
- **Corner values are averaged over the up-to-4 cells sharing the corner, read from the *global*
  field, not clipped to the chunk.** That is what makes two chunks agree bit-exactly at a shared
  corner — the `water_seams` invariant. A per-body difference (wave params) tears a seam;
  everything per-vertex (flow, turbulence, wave_scale) is safe by construction.
- `position.z = bed + depth`; `depth` averaged; `flow = q / (depth * cell)`; `turbulence` from a
  Froude-style `speed / sqrt(g * depth)`; **`wave_scale = smoothstep(0, wave_full_depth, depth)`**
  so a 2 cm film does not wave. `depth_attenuation()` in the shader does the rest.
- A welded indexed grid over the chunk's corner slots, compacted to the referenced ones.

---

## 6. Components and authoring

The field config goes in a **separate component on the terrain object**, not nested in
`HeightmapTerrain`. `TileTerrain` nests its `water:` block, but most heightmap terrains will have no
water, nesting would grow a component ~40 TUs include, and a separate component gets its own
inspector sections and its own `Add Component > Terrain` entry. It finds its terrain with the
existing `heightmap_at(scene, world)` / `heightmap_views(scene)`
(`world/heightmap_terrain_system.h:198-201`).

```yaml
# On the same object as HeightmapTerrain.
- type: TerrainWater
  subdivision: 1          # sim cells per terrain height texel: 1 | 2 | 4
  min_depth: 0.01         # m -- under this a cell is exactly dry
  seep_rate: 0.005        # m/s absorbed by the ground, under seep_depth
  seep_depth: 0.05        # m -- what dries thin films without draining pools
  evaporate_rate: 0.0     # m/s everywhere
  friction: 0.0           # 0..1 flux damping
  max_substeps: 2
  max_blocks: 4096        # hard cap; the overflow is logged
  render_min_depth: 0.02  # m before a cell is drawn
  wave_full_depth: 0.5    # m at which waves reach full height
  water:                  # the look template, parsed by the shared parse_water_settings()
    clarity: 2.0
    foam_amount: 1.2
    shore_foam_depth: 0.4
    edge_fade_depth: 0.1

# On any object, anywhere.
- type: WaterSource
  rate: 1.5               # m^3/s; NEGATIVE is a drain
  radius: 1.0             # m -- spread by area over the disc
  enabled: true
```

A drain is a negative `rate`, not a second component — one concept, one inspector, and it composes
(a sink stronger than a source dries a basin).

`TerrainWater` holds a whole `water::WaterBody` as the look template, exactly as
`TerrainComponent::water_look` does, so `copy_water_settings()` (`water_body.cpp:41`) applies it to
every published chunk body unchanged.

### `HydroSystem`

```cpp
inline constexpr int k_hydro_system_order = 62;

class HydroSystem : public coopa::scene::ISceneSystem {
public:
    std::string system_name() const override { return "HydroSystem"; }
    bool runs_in_edit_mode() const override { return true; }   // publish only, never step
    void execute(coopa::scene::Scene&, const coopa::scene::FrameContext&) override;

    /** @brief Depth (m), surface height and current at a world XY; false if dry everywhere. */
    bool sample(const glm::vec2& xy, HydroSample& out) const;
    /** @brief Adds `volume` cubic metres at a world point -- a burst, a bucket, a broken pipe. */
    void add_volume(const glm::vec2& xy, float radius, float volume);
};

HydroSystem* install_hydro_system(coopa::scene::Scene&, int order = k_hydro_system_order);
HydroSystem* find_hydro(coopa::scene::Scene&);
```

`execute()`: resolve each `TerrainWater` to its terrain → refresh bed from the dirty rect (§7) →
apply every enabled `WaterSource` → `field.step()` on `ctx.jobs` → pick the chunks to republish
(hash + rate cap + budget + `sim_radius`) → `build_hydro_surface()` → `set_surface()` on the chunk's
runtime child → destroy chunks that went dry. Runs in edit mode **only to publish**, so water is
visible while editing but does not creep while the scene is paused — the rule `SnowSystem::execute`
already follows.

Chunk children mirror `add_chunk_water_()`: a runtime child named `water`, tagged
`toy::scene::RuntimeObject` so the editor document never sees it, parented to the terrain's
transform, `copy_water_settings(cfg.water_look, *body)`, `mode = WaterMode::Field`.

---

## 7. Dynamic terrain

The invalidation primitive already exists and is explicitly multi-consumer — every consumer keeps
its own cursor (`world/heightmap_data.h:217-248`). `HydroSystem` keeps `height_cursor_` per terrain:

```cpp
const IRect r = data.height_dirty_since(height_cursor_);
if (!r.empty()) field.invalidate_bed(cells_of(r), bed_fn);   // allocated blocks in `r` only
height_cursor_ = data.height_revision();
```

`height_dirty_since` already returns the **whole grid** once the 256-entry journal floor has passed
the cursor, which is correct and bounded here because the refresh only touches *allocated* blocks.
Precedent: `particles/terrain_scatter.cpp:40,154`.

Nothing else is needed. In particular, reading `HeightmapData` directly **sidesteps the collider's
0.25 s debounce** (`heightmap_terrain_system.cpp:512-531`), so water responds to a sculpt stroke on
the frame it lands while physics catches up on its own schedule.

---

## 8. Stages

Each ends at a buildable, testable state. Batch all header edits before each build.

| # | Work | New / mod LOC | Build + test |
|---|---|---|---|
| 1 | `hydro_field.h/.cpp` + `tests/engine/unit/hydro_field_test.cpp` | 450 + 260 | `--target toyengine_tests -j4` · `--suite hydro_field` |
| 2 | `hydro_mesh.h/.cpp` + `tests/engine/unit/hydro_mesh_test.cpp` | 280 + 160 | `--suite hydro_mesh` |
| 3 | The `WaterBody` / `WaterSystem` seam (§4) | ~120 mod | `--suite water_system` + one new assertion |
| 4 | `hydro_source.*`, `hydro_system.*`, `register.cpp`, `engine.cpp` + `tests/engine/unit/hydro_system_test.cpp` | 700 + 200 | `ctest -L unit -j4` |
| 5 | Editor schema + sections | ~90 mod | `--target toyengine_editor_lib`, then `toyengine_editor` · `ctest -L unit -j4` |
| 6 | Demo scene + `tools/gen_flow_demo_heightmap.py` + `tests/engine/gpu/hydro_scene_test.cpp` | 220 | headless run (§9) · `-L gpu -R hydro` |
| 7 | Tier knobs in `water_settings.*` + `water_settings_for_()`; measure | ~60 mod | low-vs-ultra pixel diff + `PROFILE` |
| 8 | Docs + full sweep | — | `ctest --test-dir build -j4` once |

Stage 3 is the only one touching shipped water code, deliberately isolated so a regression there is
unambiguous.

**Shoreline quality** is the thing to watch at stage 6: a per-cell quad grid at 1 m shows
stair-stepped edges. `edge_fade_depth` and `shore_foam_depth` already soften contacts, and
`subdivision` / `hydro_render_step` trade cost for crispness — but if it still reads badly,
marching-squares shoreline fitting inside `build_hydro_surface()` is the follow-up, a change to one
pure function with its own unit test.

---

## 9. Verification

**Unit** (the file stem *is* the suite name):

- `hydro_field` — `water_conserves_mass_until_it_leaves` (closed basin:
  `volume_in == total_volume + volume_lost` to 1e-4); `water_finds_its_level_in_a_bowl`
  (paraboloid bed; at rest, max surface deviation over wet cells < 1 mm, wet area matches the
  analytic contour); `water_runs_downhill_out_of_a_chute` (tilted plane + V gully: wet cells track
  the centreline, flow is downhill, nothing climbs); `a_cliff_does_not_explode` (90° step, 2000
  steps: finite, `depth >= 0`, total never exceeds what was injected);
  `a_bed_change_moves_the_water` (fill, raise the bed under half, water leaves it, mass conserved);
  `blocks_are_freed_when_they_dry`; `one_and_eight_workers_give_identical_fields` (**bit-exact**
  depth arrays — the `water_parallel` bar).
- `hydro_mesh` — `adjacent_chunks_agree_on_shared_corners` (bit-exact position/depth/wave_scale at
  shared corners — the `water_seams` invariant at unit level); `a_dry_field_builds_nothing`;
  `the_skirt_rings_the_wet_area`; `shallow_cells_get_no_waves`.
- `hydro_system` — `a_source_fills_a_gully_and_publishes_water` (`WaterSystem::sample()` finds water
  in the basin and none on the ridge); `a_disabled_source_emits_nothing`;
  `sculpting_the_terrain_wakes_the_field`; `dry_chunks_are_destroyed`;
  `edit_mode_publishes_but_does_not_step`.
- `water_system` — one added: `a_supplied_surface_bakes_without_physics_or_weld`
  (`bake_stage == 2` on the first bake, `query` valid, no raycasts).
- `buoyancy` — one added: `a_crate_floats_on_simulated_water`, on the existing
  `tests/engine/support/water_fixtures.h` (`BuoyScene`, `add_box()`, `run()`, `com_of()`).

**GPU** — `hydro_scene`: load the demo, simulate and render, assert water pixels appear in the
basin and that the image settles after the camera stops.

**Demo scene** — `assets/scenes/water/flow_demo/`, a sibling of `water_demo` rather than a change to
the 1 km `heightmap_terrain_demo`: a 256 m terrain (`resolution: 257` → 1 m cells, `height: 40`,
`chunk_size: 32`) with a generated heightmap holding a ridge, a V gully and two basins; a
`WaterSource` at the head of the gully; crates to float in the lower basin.

```bash
cmake --build build --target toyengine_tests -j4
HEADLESS=1 NO_INPUT=1 FIXED_DT=0.016 MAX_FRAMES=900 CAPTURE_FRAMES=895-900 ./build/toyengine flow_demo
```

Any capture or clip for review must end with the camera stopping and holding still 1–2 s.

**Docs** — a new section plus file-table rows in `toyengine/water/README.md`; a cross-ref in
`toyengine/world/README.md`; `TerrainWater` + `WaterSource` in
`.claude/skills/create-scene/reference/components.md`; a flowing-water section in the
`create-water-body` skill; a row in `.claude/CLAUDE.md`'s area map.

---

## 10. Not included, and why

| | |
|---|---|
| **Hydraulic erosion** | the same solver plus a sediment field, but it *writes the heightmap* — chunk bounds, the GPU height texture, the PhysX `HeightGrid` and editor undo all churn every step. A separate feature. |
| **Wet / darkened terrain shading** | out of scope, and the natural first follow-on: the field already has per-cell depth and `gfx_wet_apply()` (`assets/shaders/gfx/surface/wetness.glsl`) already exists. It needs the field reaching the GPU, for which `SnowField` → `render/surface_world.h` binding 2 is the exact template. |
| **Spray / splash particles, flow audio** | out of scope. The solver's Froude number and depth drop are the natural drivers. |
| **A GPU tier** | the CPU solver stays well inside budget at these grid sizes, and every consumer (buoyancy, gameplay, mesh extraction) would need a stale readback. Precedent exists if ever needed: `render/passes/gpu_particle_pass` for persistent buffers + readback, `skinning_pass` for the minimal dispatch. |
| **Vertical waterfall faces** | `WaterSurfaceQuery` projects onto XY by construction. A fall is a steep reach, as it already is for rivers. |
| **TileTerrain coupling** | unchanged by design — tile water stays the right tool for blocky and terraced worlds. |
| **Fields spanning terrains** | one field per `HeightmapTerrain`; `kMaxSurfaceTerrains` is 4. |

---

## Risks

1. **Republish cost on a continuously changing river.** Mitigated by camera-limited publishing, the
   quantized-depth hash, a republish rate cap and a per-frame chunk budget — all four in §4. Must be
   *measured*, not assumed.
2. **The thin-film trap** (§3): too small a `min_depth` or too weak seepage and the active set grows
   without bound, looking exactly like a memory leak.
3. **Stair-stepped shorelines** at a 1 m cell (§8), with marching-squares fitting as the fallback.
