"""Shared writer for the Blender-exported mesh YAML schema, used by the tile generators.

Standalone helpers, not part of the C++ build. Imported by tools/gen_tile_side_meshes.py and
tools/gen_tile_styles.py so both emit byte-for-byte the same schema: quads or n-gons (fan-
triangulated on load -- see gfxcoopa's SkinnedMeshSource), one normal / UV / tangent per vertex.
"""

import math


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


def perpendicular_tangents(normals):
    """One unit tangent per normal: +X made perpendicular to it (Gram-Schmidt), or +Y where the
    normal is (anti)parallel to +X -- a tangent parallel to its normal is a zero vector once
    the G-buffer orthogonalises it, and normalising that is a NaN.
    """
    out = []
    for n in normals:
        for axis in ([1.0, 0.0, 0.0], [0.0, 1.0, 0.0]):
            d = sum(axis[i] * n[i] for i in range(3))
            t = [axis[i] - n[i] * d for i in range(3)]
            length = math.sqrt(sum(c * c for c in t))
            if length > 0.2:
                out.append([c / length for c in t] + [1.0])
                break
    return out


def write_mesh(path, verts, faces, uvs, normals=None, tangents=None):
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
        normals (list): Optional explicit per-vertex normals; area-weighted vertex normals
            (vertex_normals()) when omitted.
        tangents (list): Optional per-vertex (x, y, z, w) tangents; +X throughout when omitted
            (see above). A mesh whose normals can point along +X must pass its own --
            perpendicular_tangents() makes them.
    """
    if normals is None:
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
    if tangents is None:
        lines += [f"  - {fmt_vec([1.0, 0.0, 0.0, 1.0])}" for _ in verts]
    else:
        lines += [f"  - {fmt_vec(t)}" for t in tangents]

    path.write_text("\n".join(lines) + "\n")
    print(f"wrote {len(verts)} vertices, {len(faces)} faces to {path}")
