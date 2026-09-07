"""Generates the subdivided grid mesh used by the pixel_demo scene's water plane.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_water_grid_mesh.py`) whenever
assets/scenes/pixel_demo/meshes/water_grid.000.yaml needs regenerating.

water_surface.glsl's wave displacement moves each vertex independently; a flat single-quad
mesh (like plane.000, 4 vertices) can only tilt as a rigid whole under that displacement,
which reads as a plane wobbling rather than a rippling surface. This generates a flat
GRID_SIZE x GRID_SIZE quad grid in the same local space and orientation as plane.000
(-1..1 in X/Y, normal +Z, one UV tile across the whole grid) so the wave actually has
enough vertices to bend into a visible surface, while staying exactly the same mesh YAML
format Mesh::from_node() already parses for every other mesh in this scene (faces are
quads, fan-triangulated on load -- see gfxcoopa/engine/data/mesh.h's own doc).
"""

import pathlib

GRID_SIZE = 12
"""Quads per side. (GRID_SIZE + 1)^2 vertices, GRID_SIZE^2 quad faces. High enough that
water_surface.glsl's sin-wave (spatial frequency k=1.6 rad/world-unit over this mesh's
2x2-unit local extent, before the object's own Transform.scale) resolves multiple visible
crests rather than aliasing into a jagged few-vertex fold."""

OUTPUT_PATH = pathlib.Path(__file__).parent.parent / "assets/scenes/pixel_demo/meshes/water_grid.000.yaml"


def fmt_vec(v):
    return "[" + ", ".join(str(c) for c in v) + "]"


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
            # CCW winding, matching plane.000's [0,1,2,3] convention.
            faces.append([idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)])

    lines = ["vertices:"]
    lines += [f"  - {fmt_vec(v)}" for v in verts]
    lines.append("normals:")
    lines += [f"  - {fmt_vec(v)}" for v in normals]
    lines.append("uvs:")
    lines += [f"  - {fmt_vec(v)}" for v in uvs]
    lines.append("faces:")
    lines += [f"  - {fmt_vec(v)}" for v in faces]
    lines.append("colors: []")
    lines.append("weights:")
    lines += ["  - {  }" for _ in verts]
    lines.append("tangents:")
    lines += [f"  - {fmt_vec(v)}" for v in tangents]

    OUTPUT_PATH.write_text("\n".join(lines) + "\n")
    print(f"wrote {len(verts)} vertices, {len(faces)} faces to {OUTPUT_PATH}")


if __name__ == "__main__":
    main()
