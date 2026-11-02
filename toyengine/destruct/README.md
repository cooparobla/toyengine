# toyengine/destruct

Breaking a mesh into solid chunks, and accumulating localized damage, on the CPU, with no
Vulkan, scene or physics dependency (`damage_field.h` depends only on physxcoopa's
`damage::ResolvedDamage` TYPE, not on anything scene- or GPU-shaped).

| File | Purpose |
|---|---|
| [`mesh_solid.h`](mesh_solid.h) | `mesh_volume()`, `best_frame()` (the smallest-box frame among world axes / PCA / a candidate per large face), `fit_boxes()` (splits a piece's box into up to `max_boxes` that hug its geometry) and `centre_in_frame()` (turns a piece into a frame and re-centres it on its own bounds) -- shared by `fracture_mesh()` and `slice_mesh()`, both of which turn a cut-off chunk of mesh into a rigid piece and need the same volume, frame and box fit. `FractureBox` -- one box, in the piece's own frame -- is defined here too, for the same reason. |
| [`fracture.h`](fracture.h) | `fracture_mesh()` — Voronoi cell fracture over a mesh's triangles, the classic Blender "Cell Fracture" construction: scatter `pieces` sites through the mesh and give each site everything closer to it than to any other. A cell is an intersection of half-spaces, so a piece is the source mesh clipped by one plane per neighbouring site — `clip_mesh()` is the only geometry operation needed. Each cut leaves an open mouth, capped with a fan over the cut loop, so every piece is a closed solid with a flat new face wherever it broke. `clip_mesh()`'s `out_cap_first_index`/`in_cap_first_index` pair tracks where the cap region is across a whole chain of clips (one cell against several neighbour planes), so `FracturePiece::cap_first_index` names the interior faces even after several cuts. `FractureSettings` controls the site count, seed and irregularity; `FracturePiece` carries the geometry. |
| [`slice.h`](slice.h) | `slice_mesh()` — cuts a closed mesh by one plane into two closed `SlicePiece`s, each already framed and centred (`mesh_solid.h`'s `best_frame()` + `centre_in_frame()`) exactly like a fracture piece. Two `clip_mesh()` calls (`+normal` and `-normal`), nothing else. `SlicePiece::cap_first_index` marks the new cut face for an interior material. Inherits `clip_mesh()`'s concave-mesh limits: a cut through a ring-inside-a-ring fills the inner hole too, and an open shell slices into open shells with no volume (caught by `SliceSettings::min_volume_fraction`). |
| [`damage_field.h`](damage_field.h) | `DamageField` — a local-space grid of 0..1 saturation cells, allocated lazily (~16 KB at the default resolution). `deposit()` saturates a round or capsule-shaped footprint from a resolved blow (physxcoopa's `damage::ResolvedDamage`) and never exceeds 1 per cell, so a spot already cut through stays cut. `saturated_plane()` PCA-fits a plane through the saturated cells (reusing `mesh_solid.h`'s `jacobi_eigenvectors()`) -- how a slash streak that has cut all the way through becomes a cleave plane that follows the actual damage, with a `flatness` the caller should check before trusting it as a real cut. `saturated_bounds()` is the region a localized fracture would break off. |

## Why it is pure

Vertices and indices in, pieces out. That is what lets
[`scene/destruction_system.h`](../scene/destruction_system.h) run fracture on a worker thread
while the scene is live, cache results keyed on mesh + settings so identical props are cut once,
run a slice synchronously off a click with no job involved, and be tested with nothing but two
arrays — no device, no scene, no frame.

The consumer side (when a hit fractures or a plane cuts, how pieces are spawned and cleaned up,
hull-vs-box colliders, what a saturated DamageField triggers) lives with `Destructible` /
`Sliceable` / `Damageable` / `DestructionSystem` in [`../scene/README.md`](../scene/README.md).
Demo: `assets/scenes/physics/destruction_demo`.
