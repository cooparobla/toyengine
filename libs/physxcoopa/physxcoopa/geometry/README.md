# physxcoopa/geometry (`coopa::physx::geometry`)

Pure geometric primitives and their closest-point/intersection math — no `Body`, no material, no
solver state. `collision/` instances these at a body's world pose; `geometry/` itself never
depends on `dynamics/` or `collision/`.

| File | Purpose |
|---|---|
| [`aabb.h`](aabb.h) | `AABB` — `merge()`, `overlaps()`, `contains()`, `expand()` (broadphase fattening), `surface_area()` (SAH cost metric), `transform()` (exact 8-corner refit under rotation). |
| [`ray.h`](ray.h) | `Ray` (origin/direction/max_distance) + `Ray::intersect()` (AABB slab test, used by broadphase traversal) and the exact analytic `ray_vs_sphere()` / `ray_vs_obb()` / `ray_vs_capsule()` / `ray_vs_triangle()` tests `query/queries.h` dispatches to once broadphase has narrowed candidates down. |
| [`sphere.h`](sphere.h) | `Sphere` — center + radius. |
| [`obb.h`](obb.h) | `OBB` — center/half-extents/orientation, `axes()`, `bounds()`, `closest_point_on_obb()`. |
| [`capsule.h`](capsule.h) | `Capsule` — a segment swept by a radius; `closest_point_on_segment()` and `closest_points_segment_segment()` (Ericson's clamped-parametric approach, with the near-parallel-segments branch handled explicitly). |
| [`triangle_mesh.h`](triangle_mesh.h) | `TriangleMesh` — welded vertices/indices, per-triangle normals, `TriangleAdjacency` (per-edge neighbour triangle or `k_no_neighbor` for a mesh boundary). Construction from raw arrays only; `loaders::TriangleMeshLoader` is the only thing that builds one from an asset. Builds a `MeshBVH` over itself at construction. |
| [`mesh_bvh.h`](mesh_bvh.h) | `MeshBVH` — median-split top-down BVH over a mesh's triangles; `query()` (AABB), `raycast()` (Ray) and `sweep()` (a box translating along a ray, for shape casts), all BVH-descent, no full-mesh scan. |
| [`convex_hull.h`](convex_hull.h) | `ConvexHull` — vertices, merged polygon faces with planes, unique edges, volume/centroid; `support()`, `bounds()`. `build_convex_hull()` is a deterministic (no RNG, no random restarts) incremental/QuickHull builder with coplanar-face merging. SAT-only downstream (`collision/sat_hull.h`) — no GJK/EPA anywhere, see this file's own doc for why. |
