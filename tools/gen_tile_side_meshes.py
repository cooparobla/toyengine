"""Generates the terrain tile SIDE meshes used by the terrain_test scene.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_tile_side_meshes.py`) whenever assets/meshes/tile_side_*.yaml need regenerating.

These live in the SHARED assets/meshes/, not inside any one scene, because a tile side is a
primitive of the tile system rather than demo content -- every terrain scene wants the same
ones. A scene that wants its own set drops a file of the same name into its own meshes/ and
wins automatically: AssetSource::resolve() tries the loading scene's directory before the
registered search roots, so overriding a shared tile mesh is a file-placement decision with
no code or config selecting between them.

A terrain tile's geometry is its sides, and each side is an ordinary mesh asset. Every side
mesh is authored in ONE canonical orientation -- the +Z face of a unit cube spanning [0,1]^3,
matching cube.000.yaml's own [0,1]^3 convention -- and toy::world::TileMeshLibrary rotates it
onto the other five faces at load (see toyengine/world/tile_types.h's face_transform()). So
these files describe a *top*, and the engine derives the walls from it.

Two sets are produced, and the second one is the point of the whole indirection:

  tile_side_flat.yaml  -- a single unit quad at z = 1. The Minecraft-blocky default: hard
      edges, four vertices, one face.
  tile_side_bevel.yaml -- the same footprint with a chamfered rim: a raised centre panel and
      four sloped edge quads, with smooth (area-averaged) vertex normals so the chamfer
      catches the light. Drop-in replacement for the flat one -- `side_mesh: tile_side_bevel`
      in scene YAML, no C++ change -- and the starting point for genuinely smooth tiles.

The emitted schema is exactly the Blender-exported mesh YAML every other mesh in this repo
uses (quads, fan-triangulated on load -- see gfxcoopa/engine/data/mesh.h), because these are
loaded through gfxcoopa's ordinary CPU-side mesh path.
"""

import math
import pathlib

OUTPUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "assets" / "meshes"
"""Where the generated YAML is written; resolved relative to this repo, not the caller's cwd."""

BEVEL_INSET = 0.15
"""How far in from the cell edge the raised centre panel starts, in tile widths."""

BEVEL_DROP = 0.12
"""How far below the centre panel the outer rim sits, in tile heights."""


def fmt_vec(v):
    """Formats a vector as a YAML flow sequence, matching tools/gen_water_grid_mesh.py."""
    return "[" + ", ".join(f"{c:.6g}" for c in v) + "]"


def face_normal(verts, face):
    """Returns the unnormalised normal of a polygon, whose length is twice its area.

    Using the unnormalised cross product is what makes the per-vertex averaging below
    area-weighted for free -- the same trick toy::scene::ClothRenderer uses on its sheet.

    Args:
        verts (list): All vertex positions.
        face (list): Vertex indices of one polygon, in winding order.

    Returns:
        list: The (x, y, z) cross product of its first two edges.
    """
    a, b, c = verts[face[0]], verts[face[1]], verts[face[2]]
    u = [b[i] - a[i] for i in range(3)]
    v = [c[i] - a[i] for i in range(3)]
    return [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]


def vertex_normals(verts, faces):
    """Area-weighted per-vertex normals, so a chamfer shades as a curve rather than as facets.

    Args:
        verts (list): Vertex positions.
        faces (list): Polygons as index lists.

    Returns:
        list: One unit normal per vertex; +Z for any vertex no face touches.
    """
    accumulated = [[0.0, 0.0, 0.0] for _ in verts]
    for face in faces:
        n = face_normal(verts, face)
        for index in face:
            for axis in range(3):
                accumulated[index][axis] += n[axis]

    out = []
    for n in accumulated:
        length = math.sqrt(sum(c * c for c in n))
        out.append([c / length for c in n] if length > 1e-9 else [0.0, 0.0, 1.0])
    return out


def write_mesh(path, verts, faces, uvs):
    """Writes one mesh YAML in the repo's standard schema.

    Tangents are +X with handedness +1 throughout: these meshes are parameterised so that U
    runs along +X in the canonical orientation, which makes +X the analytic tangent -- and
    TileMeshLibrary transports it onto the other five faces with the same rotation it applies
    to the normal, so a normal-mapped tile set would shade correctly on every face.

    Args:
        path (pathlib.Path): Destination file.
        verts (list): Vertex positions.
        faces (list): Polygons as index lists.
        uvs (list): One UV per vertex, spanning the cell's [0,1] footprint.
    """
    normals = vertex_normals(verts, faces)

    lines = ["vertices:"]
    lines += [f"  - {fmt_vec(v)}" for v in verts]
    lines.append("normals:")
    lines += [f"  - {fmt_vec(v)}" for v in normals]
    lines.append("uvs:")
    lines += [f"  - {fmt_vec(v)}" for v in uvs]
    lines.append("faces:")
    lines += [f"  - {fmt_vec(f)}" for f in faces]
    lines.append("colors: []")
    lines.append("weights:")
    lines += ["  - {  }" for _ in verts]
    lines.append("tangents:")
    lines += [f"  - {fmt_vec([1.0, 0.0, 0.0, 1.0])}" for _ in verts]

    path.write_text("\n".join(lines) + "\n")
    print(f"wrote {len(verts)} vertices, {len(faces)} faces to {path}")


def build_flat():
    """The default side: one unit quad at the top of the cell, wound CCW seen from +Z."""
    verts = [[0.0, 0.0, 1.0], [1.0, 0.0, 1.0], [1.0, 1.0, 1.0], [0.0, 1.0, 1.0]]
    uvs = [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]]
    faces = [[0, 1, 2, 3]]
    return verts, faces, uvs


def build_bevel():
    """A chamfered side: a raised centre panel ringed by four sloped edge quads."""
    inset = BEVEL_INSET
    outer_z = 1.0 - BEVEL_DROP

    # 0-3 outer ring (at the cell boundary, dropped); 4-7 inner ring (inset, at full height).
    verts = [
        [0.0, 0.0, outer_z], [1.0, 0.0, outer_z], [1.0, 1.0, outer_z], [0.0, 1.0, outer_z],
        [inset, inset, 1.0], [1.0 - inset, inset, 1.0],
        [1.0 - inset, 1.0 - inset, 1.0], [inset, 1.0 - inset, 1.0],
    ]
    uvs = [[v[0], v[1]] for v in verts]  # the UV is the footprint; the chamfer costs no stretch
    faces = [
        [4, 5, 6, 7],  # centre panel
        [0, 1, 5, 4],  # -Y rim
        [1, 2, 6, 5],  # +X rim
        [2, 3, 7, 6],  # +Y rim
        [3, 0, 4, 7],  # -X rim
    ]
    return verts, faces, uvs


def main():
    """Writes tile_side_flat.yaml and tile_side_bevel.yaml into OUTPUT_DIR."""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    write_mesh(OUTPUT_DIR / "tile_side_flat.yaml", *build_flat())
    write_mesh(OUTPUT_DIR / "tile_side_bevel.yaml", *build_bevel())


if __name__ == "__main__":
    main()
