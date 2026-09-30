# terrain_smooth_test — Neighbourhood-Aware Smooth Terrain Tiles

## Introduction

`assets/scenes/terrain_test` renders a streamed tile world in which every tile top is the same flat
quad and every cliff face is the same flat quad. `toyengine/world/README.md` claims a smoother tile
set should be a pure data change (`side_mesh:`, or `sides: { top: … }`), and for a *single* shape
that is true. It cannot, however, produce the Animal Crossing: New Horizons read, because that look
is **conditional on the neighbourhood**: a rounded lip belongs only on an exposed plateau edge, a
quarter-round only at a convex corner, a fillet only at a concave one. Dropping a rounded top mesh
into the current mesher applies the lip to *every* column — interior ones included — and a plateau
comes out as a quilt of buns rather than a rounded shelf.

What is actually missing is a **classifier**: a pure function from a column's 3x3 neighbourhood to
which authored shape goes there and at what orientation. That is the "complex tiling operation" this
plan adds, and it is the only genuinely new idea in it. Everything else — the shapes, the surface
atlas, the world configuration — stays data, exactly as the module's README promises.

**Outcome.** A second scene, `assets/scenes/terrain_smooth_test`, generating a terraced archipelago
with rounded cliff shelves, quarter-round convex corners, concave fillets, one-step inclines and a
rimless brighter atlas — driven by the same procedural `mapcoopa` world `terrain_test` uses.
`terrain_test` keeps its blocky look byte for byte, as the A/B reference.

### Scope decisions

Confirmed with the user before drafting:

* **Island / archipelago.** Expose `mapcoopa`'s `ShapeConfig` on the `Terrain` component; author the
  scene as a three-landmass archipelago.
* **New rimless atlas.** A `smooth` style in `tools/gen_terrain_atlas.py`. The existing atlas's
  darkened one-texel rim is what makes a tile read as a block; rounded geometry under a per-tile
  outline still reads as a block.
* **Ramps included.** One-step straight descents become sloped wedges — ACNH inclines.

Explicitly **out of scope**: water as a blended pass (it stays an opaque surface at the waterline),
LOD or greedy face merging, terrain colliders, and rounded river banks beyond what the existing
channel cut already gives.

---

## Implementation Phases

### Phase 1 — The orientation primitive (`toyengine/world/tile_types.h`)

Fourteen authored shapes cover every rotation and chirality only if the library can orient them, so
this comes first. Added beside the existing `face_transform()`:

* `inline constexpr std::size_t k_tile_orientation_count = 8;`
* `const glm::mat4& variant_transform(std::uint8_t orientation)` — the D4 element, applied **in
  canonical space, before `face_transform()`**. Bits 0-1 are quarter turns CCW about +Z through the
  cell centre `(0.5, 0.5, 0.5)`; bit 2 is a mirror across the `x = 0.5` plane. **Mirror first, then
  rotate.** Same function-local-static table technique and the same translate-to-centre / rotate /
  translate-back construction `face_transform()` uses, so the result still spans `[0,1]^3` and can be
  placed by a plain translate.
* `bool orientation_mirrors(std::uint8_t orientation)` — bit 2, so the baker knows to reverse winding.

The non-obvious part, and what the Doxygen block must state: for the Top face a canonical Z-rotation
is an in-plane rotation of the lip; for a lateral, `face_transform()` has carried canonical +Z onto
the outward normal, so the same rotation spins the wall panel in its own plane and the mirror swaps
its left and right ends — which is precisely the axis the wall variants need.

### Phase 2 — Variant vocabulary and classifier (new `toyengine/world/tile_topology.h`)

Pure and header-only, depending on nothing but `tile_types.h`. This is the file the README gains a
row for.

```cpp
enum class TileVariant : std::uint8_t {
    Flat,          // no exposed cardinal edge -- the interior of a plateau
    Edge,          // one exposed edge: a rounded lip on that side
    OuterCorner,   // two adjacent exposed edges: a quarter-round
    Ridge,         // two opposite exposed edges
    Cape,          // three exposed edges
    Pillar,        // four exposed edges
    Ramp,          // sloped wedge, downhill along the oriented -Y

    WallStraight,      // both ends continue along the cliff
    WallConvexEnd,     // one end turns outward, the other continues
    WallConvexBoth,    // a one-tile spur
    WallConcaveEnd,    // one end turns inward, the other continues
    WallConcaveBoth,   // a one-tile notch
    WallConvexConcave, // one of each
    WallRampGore       // the vertical triangle flanking a ramp
};
inline constexpr std::size_t k_tile_variant_count = 14;
```

One flat enum spans tops and walls so the library table stays a single indexable thing; a top variant
is never requested for a lateral face, nor a wall variant for the top.

**The 3x3 window.**

```cpp
struct Neighbourhood {
    std::int32_t steps = 0;            // the column itself
    std::int32_t cardinal[4] = {};     // in k_lateral_faces order
    std::int32_t diagonal[4] = {};     // NE, NW, SE, SW
    bool water = false;
    bool cardinal_water[4] = {};
};
Neighbourhood read_neighbourhood(const ColumnPad&, std::int32_t lx, std::int32_t ly);
```

The existing one-tile pad already covers all eight neighbours, so `ColumnPad` and
`sample_chunk_columns()` are untouched and `sampler_chunks_agree_across_border` still holds as
written.

**Top classification.**

```cpp
struct TilePiece { TileVariant variant; std::uint8_t orientation; };
TilePiece classify_top(const Neighbourhood&, bool ramps_enabled);
```

The cardinal exposure mask (`neighbour.steps < steps`) selects the variant by popcount and adjacency,
and the set bits give the orientation. Written as a **literal 16-entry table** keyed by the mask, so
it can be read and tested rather than re-derived by arithmetic at every review.

The ramp override is tested **before** the mask, and only when `ramps_enabled`. It requires all of:
the column is not water; exactly one cardinal `d` has `steps == own - 1`; both cardinals
perpendicular to `d` are exactly level; the cardinal opposite `d` is `>= own`; the neighbour in `d`
is not water. It then yields `{Ramp, orientation(d)}`, and the lateral face `d` is **not** emitted —
the wedge *is* that face. A run of such columns forms a continuous staircase: each wedge spans
`[h-1, h]` and its downhill neighbour's spans `[h-2, h-1]`.

**Wall classification.**

```cpp
TilePiece classify_wall(TileFace face, const Neighbourhood&, std::int32_t cell,
                        bool ramps_enabled);
```

For each of the wall's two **ends** (the along-wall directions), from the along-wall neighbour `a`
and the diagonal `a + face`, at the cell index being emitted:

| condition | end state |
|---|---|
| `a.steps <= cell` | **convex** — the cliff wraps outward |
| `a.steps > cell` and `diag.steps <= cell` | **continues** |
| `a.steps > cell` and `diag.steps > cell` | **concave** — the cliff turns inward |

The nine combinations collapse onto five variants plus mirror. Classifying per `cell` rather than per
column is deliberate: a tall wall's ends genuinely change with depth.

`WallRampGore` is emitted on the two faces perpendicular to a ramp's downhill direction, for the top
cell only, and **only when** `n.diagonal[p + d] != own - 1`. That test is computable inside the 3x3
window and is what stops a two-lane ramp growing a wall down its middle.

### Phase 3 — Variant-indexed mesh library (`toyengine/world/tile_mesh_library.h`)

`sides_` becomes
`std::array<std::array<std::array<SideGeometry, k_tile_orientation_count>, k_tile_variant_count>, k_tile_face_count>`.

* `bake_side(face, source)` keeps its signature and fills `[face][Flat][0]`, so `bake_canonical()`,
  `is_baked()`, every existing call site and the entire current `world` test group are untouched.
* New `bake_variant(TileFace, TileVariant, const SkinnedMeshSource&)` bakes one shape into all eight
  orientations of its slot, composing `face_transform(face) * variant_transform(o)`. Mirrored
  orientations reverse the index order and flip `tangent.w`; normals and tangents take the matrix
  itself, since a reflection is its own inverse transpose.
* `append()` gains `TileVariant variant = TileVariant::Flat, std::uint8_t orientation = 0` as trailing
  defaulted arguments and **falls back to `[face][Flat][0]` when the requested slot is empty**. No
  fill-down pass, no wasted copies, and a scene that authors only some shapes degrades to flat for
  the rest instead of punching holes in the world.
* `has_variants()` — true once anything beyond `[*][Flat][0]` is baked. This is the gate, so "smooth
  tiles" stays a data decision rather than a new boolean knob.

### Phase 4 — The mesher (`toyengine/world/terrain_chunk.h`)

`mesh_chunk_columns()` keeps its signature and its fixed emission walk; determinism is load bearing
for the job parallelism. Inside the per-column body:

* If `!library.has_variants()`, emit exactly as today.
* Otherwise: `read_neighbourhood()`, `classify_top()`, emit the top with that variant and
  orientation; skip the ramp's downhill lateral; for each remaining lateral walk the cells top-down
  as now, calling `classify_wall()` per cell; emit the ramp gores.

`TerrainParams` (in `terrain_sampler.h`) gains one field: `bool ramps_enabled = false`.

### Phase 5 — Component and scene parser

`TerrainComponent` (`toyengine/world/terrain_component.h`) gains:

* World-shape fields, copied into `MapConfig` by `begin_generation()`: `shape` (a
  `coopa::maps::MapShape`, parsed with the existing `map_shape_from_name()`), `continent_count`,
  `continent_size_m`, `irregularity`, `size_variance`, `coast_detail`, `meters_per_grid_unit`,
  `elevation_smoothing_iterations`, `jitter`. Document that `height_scale` (world z, toyengine) and
  `elevation_range_m` (metres, map-internal physical sizes) are different numbers — that is the easy
  conflation here.
* A shape set: `std::string shape_set`, plus
  `std::array<std::string, k_tile_variant_count> variant_meshes` and
  `set_variant_source(TileVariant, AssetHandle<SkinnedMeshSource>)`, mirroring the existing
  `face_meshes` / `set_face_source()` pair exactly.
* `poll_side_meshes()` additionally bakes each loaded variant source. Top variants bake onto
  `TileFace::Top` (and `Bottom` when `emit_bottom`); wall variants bake onto all four laterals.

`toyengine/scene/register.h`'s `"Terrain"` parser gains, in the same `node.contains(...)` style it
already uses throughout:

```yaml
ramps: true
shape_set: tile_smooth          # expands to tile_smooth_<suffix> per variant
shapes:                         # ...with per-variant overrides, like `sides:` today
  outer_corner: my_own_corner
```

`shape_set` expands through **one** static name table — `{"top_flat", Flat}`, `{"top_edge", Edge}`,
`{"top_outer", OuterCorner}`, `{"top_ridge", Ridge}`, `{"top_cape", Cape}`, `{"top_pillar", Pillar}`,
`{"top_ramp", Ramp}`, `{"wall_straight", WallStraight}`, and so on — reused for both `shape_set`
expansion and `shapes:` lookup, so the two can never drift apart.

### Phase 6 — Asset generators (`tools/`)

* **`tools/mesh_yaml.py`** (new). `fmt_vec`, `face_normal`, `vertex_normals` and `write_mesh` moved
  verbatim out of `gen_tile_side_meshes.py`, which then imports them. `tile_side_{flat,bevel}.yaml`
  must still regenerate byte for byte — the README promises that.
* **`tools/gen_tile_smooth_meshes.py`** (new). Writes the fourteen `tile_smooth_*.yaml` shapes into
  `assets/meshes/`, all in the canonical +Z orientation, from three module constants: `ROUND_RADIUS`
  (~0.35 tile widths, the horizontal corner radius), `LIP` (~0.18 of a step, the vertical
  round-over) and `SEGMENTS` (3, the arc tessellation). Each shape is a flat interior panel plus a
  rounded rim strip only on the edges its variant says are exposed — which is what keeps the cost
  bounded. The wall shapes carry the round-over at their own top edge, so a multi-step cliff comes
  out as stacked rounded shelves: the ACNH cliff read, and one wall role instead of two.
* **`tools/gen_terrain_atlas.py`**. Add a `smooth` style writing
  `assets/textures/terrain_atlas_smooth.png` and `_smooth_mr.png`: rim darkening 1.0 (no rim),
  roughly half the grain amplitude, and brighter, more saturated grass / sand / stone. The two
  existing outputs stay byte-identical, and `KINDS` order remains load bearing for both styles.

**Cost.** Today a side is 4 vertices and 2 triangles. A smooth *rim* shape averages ~8 quads, but
`Flat` stays a single quad — and in a terraced world the overwhelming majority of columns are plateau
interior, so the mean lands nearer 3-4 triangles per side than 16. The scene still pulls
`view_radius` back to 2 (25 live chunks instead of 49), because the pipeline does no per-mesh frustum
culling and shadow casting draws everything a second time. This paragraph goes into the README's Cost
model section.

### Phase 7 — The scene (`assets/scenes/terrain_smooth_test/scene.yaml`)

A new directory holding `scene.yaml` only: the shape meshes and the atlas are shared primitives of
the tile system, exactly as `terrain_test/` holds nothing else. A header comment in the same voice as
`terrain_test`'s, saying what this scene demonstrates that the other cannot.

The `Terrain` component, on top of `terrain_test`'s structure:

```yaml
# --- World: an archipelago rather than a canvas-filling rectangle ---
shape: archipelago
continent_count: 3
continent_size_m: 1500        # against the default 60 m per grid unit -> ~25 cells across
irregularity: 0.42
coast_detail: 0.18
grid_size: 64
sea_level: 0.34
terrain_roughness: 0.0        # ACNH plateaus are smooth; roughness would speckle them
elevation_smoothing_iterations: 10
river_count: 8

# --- Terracing: few tall steps, not forty short ones ---
height_scale: 26.0
height_step: 3.25             # 8 terrace levels over the full range
soil_depth_steps: 1
max_wall_steps: 8

tiles_per_grid_unit: 4        # 256 tiles per axis
chunk_size: 32
view_radius: 2

ramps: true
shape_set: tile_smooth
material:
  texture_albedo: textures/terrain_atlas_smooth.png
  texture_metallic_roughness: textures/terrain_atlas_smooth_mr.png
```

Reuse `terrain_test`'s `focus_marker` + orbit-camera + `focus_object` rig unchanged: it is the right
control scheme, and `test_terrain_streams_chunks_around_the_camera` already depends on that shape.
The differences are a higher `pitch_deg` (~42) for the ACNH three-quarter view, and a sun nearer
overhead (`direction: {-0.35, -0.30, -0.88}`) with a brighter `EnvironmentLight` — high-key, but
still off-axis enough that terrace steps shadow each other.

`focus_marker`'s start position is the one value that cannot be authored blind: an archipelago
inscribed in the canvas may put the canvas centre in open sea. Start at `(128, 128)` and retune it
from the first capture onto a landmass.

### Phase 8 — Documentation

* `toyengine/world/README.md`: a table row for `tile_topology.h`; a new "Neighbourhood
  classification" section stating the three wall end states and the top mask table; the cost
  paragraph from Phase 6; `gen_tile_smooth_meshes.py` in the generator list; and a fourth entry under
  "Three things worth knowing before changing anything here" — *the classifier is what keeps a
  rounded tile set from quilting*.
* Doxygen blocks on every new public symbol, per the C++ rules: `@brief`, `@param`, `@return`,
  present tense, no history narration.

---

## Files Touched

| File | Change |
|---|---|
| `toyengine/world/tile_types.h` | `variant_transform()`, `orientation_mirrors()`, `k_tile_orientation_count` |
| `toyengine/world/tile_topology.h` | **new** — `TileVariant`, `Neighbourhood`, `classify_top()`, `classify_wall()` |
| `toyengine/world/tile_mesh_library.h` | variant x orientation table, `bake_variant()`, `has_variants()`, `append()` fallback |
| `toyengine/world/terrain_chunk.h` | the classifier path in `mesh_chunk_columns()` |
| `toyengine/world/terrain_sampler.h` | `TerrainParams::ramps_enabled` |
| `toyengine/world/terrain_component.h` | shape-config fields, `shape_set`, variant sources, variant baking |
| `toyengine/scene/register.h` | the new `Terrain` YAML keys |
| `tools/mesh_yaml.py` | **new** — shared writer, extracted from `gen_tile_side_meshes.py` |
| `tools/gen_tile_smooth_meshes.py` | **new** — the fourteen shapes |
| `tools/gen_terrain_atlas.py` | the `smooth` style |
| `assets/scenes/terrain_smooth_test/scene.yaml` | **new** |
| `toyengine/world/README.md` | classifier section, cost model, generator list |
| `test.cpp` | six `world` tests and one `render_terrain` test |

Generated rather than hand-written: `assets/meshes/tile_smooth_*.yaml` (14) and
`assets/textures/terrain_atlas_smooth{,_mr}.png`.

---

## Testing and Validation

### Automated tests

Added to `test.cpp` and registered in the `k_tests` table like every other test. Tests 1-6 join the
`world` group and are device-free, so they run on every save.

1. **`tile_variant_transforms_are_rigid`** — all eight orientations keep the baked cell inside
   `[0,1]^3`; each is orthogonal; mirrored ones have determinant -1; and
   `face_transform x variant_transform` still carries canonical +Z onto the face's outward normal.
   Pins the mirror-then-rotate order so a later reader cannot silently swap it.
2. **`tile_top_classification_is_congruent`** — exhaustive over all 16 cardinal masks: the chosen
   variant's exposed-edge count matches the mask popcount, and rotating the mask by a quarter turn
   rotates the returned orientation by exactly one quarter turn.
3. **`tile_wall_classification_covers_every_end_pair`** — exhaustive over the nine end-state pairs:
   each maps to the expected variant, and the two chiral pairs map to mirrored orientations of *one*
   shape rather than to two shapes.
4. **`chunk_smooth_plateau_interior_stays_flat`** — the quilt-free assertion, and the single most
   important test here. A flat 4x4 pad with a **full** variant library still emits exactly 16 tops
   and no walls, and every top is the `Flat` shape. This is what fails if the classifier is bypassed
   or the mask table is wrong.
5. **`chunk_ramp_replaces_the_downhill_wall`** — a hand-built pad with a straight one-step descent
   and level flanks. With `ramps_enabled` the descending columns take `Ramp`, emit no wall on the
   downhill face, and do emit both gores; with it off they take `Flat` plus a one-step wall. Also
   covers the two-lane case emitting no wall between the lanes.
6. **`chunk_smooth_meshing_is_deterministic`** — the existing determinism test's pad re-run against a
   full variant library, byte for byte.
7. **`terrain_smooth_scene_streams_chunks`** (group `render_terrain`) — the shipped
   `terrain_smooth_test/scene.yaml`, world shrunk before the first tick exactly the way
   `test_terrain_streams_chunks_around_the_camera` does it, asserting `is_ready()`,
   `library().has_variants()`, and that every chunk in the view radius becomes live and non-empty.

The six pre-existing `world` tests must pass **unchanged** — that is the contract that
`has_variants()` gating buys, and it is also the `terrain_test` regression check in miniature.

### Commands

```bash
# 1. Regenerate the assets. Deterministic: re-running reproduces them byte for byte.
python3 tools/gen_tile_smooth_meshes.py
python3 tools/gen_terrain_atlas.py

# 2. Build.
cbuild --vulkan

# 3. The classifier, instant and device-free. Check test 4 before looking at any render.
./build/toyengine_tests --group world

# 4. The streaming smoke tests, through a real Vulkan device.
./build/toyengine_tests --group render_terrain

# 5. Documentation.
coopadocs build
```

After step 1, the working tree should show the fourteen new meshes and the two new textures, and
**no change** to `assets/meshes/tile_side_{flat,bevel}.yaml` or
`assets/textures/terrain_atlas{,_mr}.png`. I will not run `git` — the expected diff gets reported and
you check it.

### Manual validation

```bash
cplay terrain_test          # regression: unchanged blocky look (has_variants() is false here)
cplay terrain_smooth_test   # the deliverable
```

In the smooth scene, fly `focus_marker` over a coastline, a plateau corner and an incline. The three
things to look for, in order: no quilting on plateau interiors; quarter-rounds at convex corners and
fillets at concave ones rather than square edges; and inclines that join their neighbours without a
seam or a wall down the middle.

### Captures for review

Per the standing rule on render-artifact review: every `CAPTURE_FRAMES` run **moves the camera at a
realistic flick speed and then stops, holding still for 1-2 s** before the final frame, so what gets
judged is a stopped image. Three shots — a convex plateau corner, a concave notch, and a ramp — plus
the same three from `terrain_test` for the A/B.

---

## Risks

* **Quilting** is the failure mode to watch. If any interior column picks a rim shape, the plateau
  turns into buns. Test 4 is the tripwire; check it before looking at a render.
* **Triangle budget.** There is no per-mesh frustum culling, and shadow casting draws everything
  again. If a render stutters, pull `view_radius` to 1 or `SEGMENTS` to 2 before touching the
  classifier — and say so rather than quietly shipping a coarser set.
* **Ramps eating the cliffs.** In a terraced world, one-step drops *are* the terrace boundaries, so
  `ramps: true` converts every straight one of them into a slope — a rolling-hills read rather than
  scattered ACNH inclines. If that proves too soft, the follow-on is a deterministic per-tile hash
  gate (`ramp_density`) so only some contours ramp. Flagged after the first capture rather than built
  speculatively.
* **`continent_size_m` units.** Metres, against `meters_per_grid_unit` (60 by default) — not grid
  units and not tiles. Getting it wrong yields either one fused landmass or a canvas of open sea; the
  first capture says which.
