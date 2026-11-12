# editor/mesh

The modelling toolkit: an editable polygon mesh and the operations over it. Almost all of it is
**pure functions over `EditMesh`** with no GPU and no scene, which is why this directory carries
the densest unit-test coverage in the editor.

| File | Purpose |
|---|---|
| [`edit_mesh.h`](edit_mesh.h) | `EditMesh` — the editable polygon mesh, and its round trip to the engine's mesh YAML (`mesh_from_node` / `mesh_to_node`). |
| [`mesh_topology.h`](mesh_topology.h) | Flat adjacency, built once per operation. |
| [`mesh_ops.h`](mesh_ops.h) | Selection and modelling operations. |
| [`mesh_loops.h`](mesh_loops.h) | Quad topology as in Blender: edge loops and rings, Loop Cut, Edge Slide. |
| [`mesh_subdivide.h`](mesh_subdivide.h) | Subdivide, Catmull-Clark and friends — quad-first throughout. |
| [`mesh_mirror.h`](mesh_mirror.h) | Mirroring for Edit and Sculpt Mode. |
| [`proportional.h`](proportional.h) | Proportional editing (Blender's **O**): falloff-weighted transform of nearby geometry. |
| [`sculpt.h`](sculpt.h) | Sculpt Mode brushes. |
| [`paint.h`](paint.h) | Vertex Paint and Weight Paint brushes. |
| [`auto_weights.h`](auto_weights.h) | Binds vertex groups to a rig's bones — the editor side of Blender's "With Automatic Weights" (the solver itself is [`toyengine/scene/auto_skin_weights.h`](../../toyengine/scene/auto_skin_weights.h)). |
| [`mesh_bvh.h`](mesh_bvh.h) | A BVH over fan-triangulated faces, for brush and ray queries. |
| [`primitives.h`](primitives.h) | Parametric starting shapes: Z-up, counter-clockwise front faces. |
| [`shader_ball.h`](shader_ball.h) | The material viewer's shader ball — curved and hard-edged surfaces in one shape. |

## Conventions

- **Pure functions, testable without a device.** A new operation belongs here as a free function
  over `EditMesh` (+ a `MeshTopology` if it needs adjacency), with the panel in
  `../app/ui/mesh_tools.cpp` or `sculpt.cpp` doing only input and undo.
- **Z-up, counter-clockwise front faces.** Matching the engine's convention; a primitive that
  gets this wrong renders inside-out.
- **Weights are baked, never generated at runtime.** `auto_weights` writes into the mesh file's
  `weights:`; nothing in the engine generates them while a game runs.
