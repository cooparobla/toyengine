# physxcoopa/query (`coopa::physx::query`)

Shape-dispatch helpers behind `PhysicsWorld`'s public query API (`raycast`, `raycast_any`,
`raycast_all`, `shape_cast`, `sphere_cast`, `box_cast`, `capsule_cast`, `overlap_sphere`,
`overlap_box`, `overlap_capsule`, `compute_penetration` — see `world.h`, which owns the actual broadphase
traversal since it alone has access to the trees). Kept separate so `world.h`'s own body isn't a
wall of per-shape-type raycast/overlap code.

| File | Purpose |
|---|---|
| [`queries.h`](queries.h) | `RaycastHit` (carries a `BodyId`, not a `SceneObject*` — `PhysicsWorld` has no `coopa::scene` dependency), `raycast_shape()` (analytic Sphere/Box/Capsule, BVH descent for `TriangleMesh`), `shape_overlaps_sphere()` / `shape_overlaps_obb()` / `shape_overlaps_capsule()`, plus `QueryFilter` (layer mask, triggers, one ignored body, optional predicate -- every query takes one) and `Penetration` (one minimum-translation vector). |
| [`sweep.h`](sweep.h) | Exact pairwise shape casts and penetration: conservative advancement on exact distances for Sphere/Capsule casts (and Box vs round targets), swept SAT for Box vs Box/triangle, BVH-narrowed per-triangle sweeps for meshes, initial-overlap rules (`started_inside`); `convex_penetration()` / `mesh_penetrations()` behind `compute_penetration()`. |

**Queries are only valid between phases, not from inside `on_substep`** — a raycast issued from a
substep callback observes mid-solve, not-yet-integrated state.
