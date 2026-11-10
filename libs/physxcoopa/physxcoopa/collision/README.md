# physxcoopa/collision (`coopa::physx::collision`)

Per-shape-pair contact generation. Every pair has an exact analytic or SAT solution. GJK/EPA are
excluded everywhere (see `sat_hull.h`'s own doc for why a convex hull still gets SAT, not EPA).

## Dispatch

```text
generate_contacts(shape_a, shape_b) -> ContactManifold
  Sphere-Sphere    analytic center distance
  Sphere-Box       closest point on OBB                         [narrowphase.h]
  Box-Box          SAT (6 face + 9 edge-cross axes) + Sutherland-Hodgman clipping   [sat.h, clip.h]
  Capsule-Sphere   closest point on the capsule's segment         [segment.h]
  Capsule-Capsule  closest points between two segments           [segment.h]
  Capsule-Box      segment clipped against the box's 6 slabs     [segment.h]
  *-ConvexHull     SAT over face + edge-cross axes (hull-hull, hull-box); closest-point for
                    hull-sphere/hull-capsule                      [sat_hull.h]
  *-TriangleMesh   per-triangle contact + internal-edge correction via adjacency   [mesh_contact.h]
  Mesh-Mesh        not supported
```

With `allow_speculative` (set by `PhysicsWorld` for a fast-moving pair), Sphere-Sphere, Sphere-Box
and Box-Box also return a single speculative point for a pair that is close but not yet touching,
which is what stops fast spheres and boxes tunnelling through thin walls.

`manifold.h`'s `normal` always points from body `a` toward body `b`; every dispatch branch in
`narrowphase.h` is responsible for getting that sign right regardless of which underlying
per-shape function it called (several of those return the opposite convention and get flipped
before returning).

| File | Purpose |
|---|---|
| [`shape.h`](shape.h) | `ShapeType`, `Shape` (world-scale geometry + material/trigger/layer), and the `world_sphere()`/`world_obb()`/`world_capsule()`/`world_bounds()` instancing helpers everything else in this module builds on. |
| [`manifold.h`](manifold.h) | `ContactPoint` (position, penetration, per-axis impulse accumulators, warm-start `feature_id`) and `ContactManifold` (up to 4 points, `normal`, friction/restitution, `is_trigger`). |
| [`contact_event.h`](contact_event.h) | `ContactEvent` — one enter/stay/exit transition, queued by `PhysicsWorld` during the substep loop and drained once per frame by the caller (`PhysicsSystem` fires the actual `Collider` signals from these). |
| [`sat.h`](sat.h) | `generate_box_box_contacts()` — full box-box SAT + face clipping, with `(reference_face, incident_face, clipped_vertex_index)` packed into a stable `feature_id`. |
| [`sat_hull.h`](sat_hull.h) | `PosedHull` (a `ConvexHull` + world pose), `sat_test_hull_hull()`, `generate_hull_hull_contacts()`, `generate_hull_box_contacts()`, `hull_vs_sphere()`, `hull_vs_capsule()`. Face axes use a ONE-SIDED overlap test (`detail::face_overlap_()`) because a hull's individual faces, unlike a box's 3 unique axes, have no antipodal pairing — a two-sided test ties between a face and its antipodal twin and can pick the wrong one as the contact reference. Edge-edge axes stay two-sided (a cross product has no outward side to call). |
| [`clip.h`](clip.h) | Sutherland-Hodgman polygon clipping against a box's or hull's face planes, `sat.h`/`sat_hull.h`'s clipping step. |
| [`segment.h`](segment.h) | `capsule_vs_sphere()`, `capsule_vs_capsule()`, `capsule_vs_box()` — all analytic, no iteration cap that can fail to converge. |
| [`mesh_contact.h`](mesh_contact.h) | Convex-vs-`TriangleMesh` contact generation with internal-edge correction (the classic "box sliding across a tessellated floor catches on the shared edge between coplanar triangles" bug). Sphere/Capsule use a core-plus-radius closest-point test; Box tests every corner against each triangle's plane directly, since a box has no core-radius offset for the closest-point trick to work once it's already overlapping. Only the mesh BVH's candidate triangles for the shape's bounds are tested (`mesh_contact_candidates()`, visited in index order so the result matches a full scan -- `generate_mesh_contacts(..., use_bvh = false)` is that reference scan); `mesh_triangle_contacts()` is the per-triangle step, shared with `PhysicsWorld::compute_penetration()`. A box whose *centre* has ended up behind a face it is directly under is embedded -- neither feature pair can see it -- and gets one recovery contact along that face's normal, as far as the box's own bounds still reach the surface. |
| [`distance.h`](distance.h) | Exact closest points between a segment (a sphere's or capsule's core) and a box (`closest_points_segment_box()`, piecewise-quadratic, not iterative) or a triangle (`closest_points_segment_triangle()`) -- what shape casts advance on. |
| [`narrowphase.h`](narrowphase.h) | `generate_contacts()` — the dispatch table above, plus `sphere_vs_sphere()`/`sphere_vs_box()`. |

## Usage Example

```cpp
#include <physxcoopa/collision/narrowphase.h>

coopa::physx::collision::ContactManifold m;
bool hit = coopa::physx::collision::generate_contacts(
    id_a, shape_a, pos_a, rot_a,
    id_b, shape_b, pos_b, rot_b,
    m);
if (hit) {
    // m.normal, m.points[0..m.count), m.friction, m.restitution, m.is_trigger
}
```
