"""Generates the subdivided grid mesh used by the kitchen_sink test scene's water plane (src/tests/fixtures/scenes/).

Standalone generator, not part of the C++ build -- run it manually whenever
assets/meshes/water/water_grid.yaml needs regenerating:

    uv run --project python tools/gen_water_grid_mesh.py

water_surface.glsl's wave displacement moves each vertex independently; a flat single-quad
mesh (like plane, 4 vertices) can only tilt as a rigid whole under that displacement,
which reads as a plane wobbling rather than a rippling surface. This generates a flat
GRID_SIZE x GRID_SIZE quad grid in the same local space and orientation as plane
(-1..1 in X/Y, normal +Z, one UV tile across the whole grid) so the wave actually has
enough vertices to bend into a visible surface, while staying exactly the same mesh YAML
format Mesh::from_node() already parses for every other mesh in this scene (faces are
quads, fan-triangulated on load -- see gfxcoopa/engine/data/mesh.h's own doc).

The file is written by `toyproject.Mesh` (python/), which carries this schema and the float
format that spells a whole number `-1.0` rather than `-1`. `python/tests/
test_mesh_matches_committed.py` regenerates this very mesh and compares it to the committed
file byte for byte, so the shared writer cannot drift from what is on disk here.
"""

import pathlib

from toyproject import Mesh, Project

GRID_SIZE = 12
"""Quads per side. (GRID_SIZE + 1)^2 vertices, GRID_SIZE^2 quad faces. High enough that the
pond's Gerstner waves (wave_length 1.6 on its WaterBody, over this mesh's 2x2-unit local extent
before the object's own Transform.scale) resolve visible crests rather than aliasing into a
jagged few-vertex fold. The pond's WaterBody loads this mesh CPU-side and bakes it (see
src/toyengine/water/water_system.h); the MeshRenderer itself names no mesh."""

ENGINE_DIR = pathlib.Path(__file__).resolve().parent.parent
"""The engine checkout. Passed to Project() explicitly because an engine has no .toy file, so
Project.find() -- which a game project's generator would use -- does not apply here."""


def main():
    n = GRID_SIZE
    verts, normals, uvs, tangents = [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            x = -1.0 + 2.0 * i / n
            y = -1.0 + 2.0 * j / n
            verts.append([round(x, 6), round(y, 6), 0.0])
            normals.append([0.0, 0.0, 1.0])
            uvs.append([round(i / n, 6), round(j / n, 6)])
            tangents.append([1.0, 0.0, 0.0, 1.0])

    def idx(i, j):
        return j * (n + 1) + i

    faces = []
    for j in range(n):
        for i in range(n):
            # CCW winding, matching plane's [0,1,2,3] convention.
            faces.append([idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)])

    project = Project(ENGINE_DIR)
    mesh = Mesh(verts, faces, uvs, normals=normals, tangents=tangents)
    path = mesh.write(project, "water/water_grid")
    print(f"wrote {len(verts)} vertices, {len(faces)} faces to {path}")


if __name__ == "__main__":
    main()
