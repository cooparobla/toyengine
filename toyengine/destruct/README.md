# toyengine/destruct

Breaking a mesh into solid chunks, on the CPU, with no Vulkan, scene or physics dependency.

| File | Purpose |
|---|---|
| [`fracture.h`](fracture.h) | `fracture_mesh()` — Voronoi cell fracture over a mesh's triangles, the classic Blender "Cell Fracture" construction: scatter `pieces` sites through the mesh and give each site everything closer to it than to any other. A cell is an intersection of half-spaces, so a piece is the source mesh clipped by one plane per neighbouring site — `clip_mesh()` is the only geometry operation needed. Each cut leaves an open mouth, capped with a fan over the cut loop, so every piece is a closed solid with a flat new face wherever it broke. `FractureSettings` controls the site count, seed and irregularity; `FracturePiece` carries the geometry and `FractureBox` the oriented best-fit boxes a physics chunk collides as. |

## Why it is pure

Vertices and indices in, pieces out. That is what lets
[`scene/destruction_system.h`](../scene/destruction_system.h) run it on a worker thread while
the scene is live, cache results keyed on mesh + settings so identical props are cut once, and
be tested with nothing but two arrays — no device, no scene, no frame.

The consumer side (when a hit fractures, how chunks are spawned and cleaned up, why chunks
collide as oriented boxes rather than hulls) lives with `Destructible` /
`DestructionSystem` in [`../scene/README.md`](../scene/README.md). Demo:
`assets/scenes/physics/destruction_demo`.
