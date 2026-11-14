"""Meshes: `assets/meshes/<group>/<name>.yaml`.

The schema is the one Blender exports and `gfxcoopa`'s `SkinnedMeshSource` loads: parallel
per-vertex arrays (`vertices`, `normals`, `uvs`, `tangents`, `weights`) and `faces` as index
lists. Faces may be triangles, quads or n-gons -- an n-gon is fan-triangulated on load -- so a
generator can emit the topology it actually has.

## This replaces `tools/mesh_yaml.py`, and reproduces it exactly

The two mesh writers in the tree emitted the same key order and layout and differed in one
thing only: how a float is spelled. Both are reachable here, and the choice matters because
rewriting a committed mesh with the other one is a whole-file diff:

| `fmt` | Spells `-1.0` as | Reproduces |
|---|---|---|
| `fmt_num` (default) | `-1.0` | `tools/gen_water_grid_mesh.py` |
| `fmt_g6` | `-1` | `tools/mesh_yaml.py`, and so the tile sets it generated |

`tools/mesh_yaml.py`'s own docstring claimed its format matched `gen_water_grid_mesh.py`. It
did not, and the files on disk differ accordingly -- which is the sort of thing one shared
writer stops happening again.

The empty-skinning placeholder is written as `{  }`, with the two inner spaces both original
writers emitted, so that a regenerated mesh is byte-identical to the committed one rather than
differing in whitespace on every vertex.
"""

import math
import pathlib

from ..yamlwrite import fmt_g6, fmt_num


def face_normal(verts, face):
    """The unnormalised normal of a polygon, whose length is twice its area.

    Leaving it unnormalised is what makes the per-vertex averaging in `vertex_normals`
    area-weighted for free -- the same trick `toy::scene::ClothRenderer` uses on its sheet.

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
    """One unit tangent per normal: +X made perpendicular to it, or +Y where that degenerates.

    A tangent parallel to its normal is a zero vector once the G-buffer orthogonalises it, and
    normalising that is a NaN -- so a normal (anti)parallel to +X falls back to +Y.

    Args:
        normals (list): Unit normals.

    Returns:
        list: One (x, y, z, w) tangent per normal, handedness +1.
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


def write_mesh(path, verts, faces, uvs, normals=None, tangents=None, fmt=fmt_g6,
               project=None):
    """Writes one mesh file in a single call -- the shared writer the generators use.

    This is the function `tools/mesh_yaml.py` used to provide, moved into the library so that
    a generator imports the asset API and nothing else. It defaults to `fmt_g6` because that is
    the format every mesh written through it already carries; `Mesh(...).write_to(...)` is the
    same thing spelled out, and defaults to `fmt_num` like the rest of the API.

    Unlike its predecessor it prints nothing -- a library writing to stdout makes it the
    caller's problem to silence. The generators report their own output.

    Args:
        path (str | pathlib.Path): Destination file. Parent directories are created.
        verts (list): Vertex positions.
        faces (list): Polygons as vertex-index lists, in winding order.
        uvs (list): One UV per vertex.
        normals (list): Optional per-vertex normals; area-weighted ones are computed when
            omitted.
        tangents (list): Optional per-vertex (x, y, z, w) tangents; +X throughout when omitted.
        fmt (callable): The number format.
        project (Project): Optional, for `report()` bookkeeping only.

    Returns:
        pathlib.Path: The file written.

    Raises:
        ValueError: Via `Mesh`, if the arrays are not parallel or a face indexes a missing
            vertex.
    """
    return Mesh(verts, faces, uvs, normals=normals, tangents=tangents).write_to(
        path, fmt, project)


class Mesh:
    """A mesh asset.

    Args:
        verts (list): Vertex positions, each a sequence of three numbers.
        faces (list): Polygons as vertex-index lists, in winding order.
        uvs (list): One UV per vertex.
        normals (list): Optional explicit per-vertex normals; `vertex_normals()` is used when
            omitted.
        tangents (list): Optional per-vertex (x, y, z, w) tangents. Omitted means +X with
            handedness +1 throughout, which is correct for a mesh parameterised so that U runs
            along +X. A mesh whose normals can point along +X must pass its own --
            `perpendicular_tangents()` makes them.
        colors (list): Optional per-vertex colours; written as `colors: []` when omitted, as
            every committed mesh has it.

    Raises:
        ValueError: If `uvs` or an explicit `normals`/`tangents` is not one entry per vertex,
            or a face indexes a vertex that does not exist. Both are silent corruption
            otherwise -- the loader reads past the end of an array or shades from garbage.
    """

    def __init__(self, verts, faces, uvs, normals=None, tangents=None, colors=None):
        self.verts = [list(v) for v in verts]
        self.faces = [list(f) for f in faces]
        self.uvs = [list(u) for u in uvs]
        self.normals = [list(n) for n in normals] if normals is not None else None
        self.tangents = [list(t) for t in tangents] if tangents is not None else None
        self.colors = [list(c) for c in colors] if colors is not None else None

        count = len(self.verts)
        for label, array in (("uvs", self.uvs), ("normals", self.normals),
                             ("tangents", self.tangents), ("colors", self.colors)):
            if array is not None and len(array) != count:
                raise ValueError(
                    f"{label} has {len(array)} entries for {count} vertices; the mesh arrays "
                    f"are parallel and the loader reads them by vertex index")
        for index, face in enumerate(self.faces):
            if len(face) < 3:
                raise ValueError(f"face {index} has {len(face)} vertices, needs at least 3")
            for vertex in face:
                if not 0 <= vertex < count:
                    raise ValueError(
                        f"face {index} indexes vertex {vertex}, outside 0..{count - 1}")

    def to_text(self, fmt=fmt_num):
        """Renders the whole mesh file.

        Args:
            fmt (callable): The number format -- `fmt_num` or `fmt_g6`; see the module doc for
                which committed meshes each reproduces.

        Returns:
            str: The file's text, ending in exactly one newline.
        """
        def vec(values):
            return "[" + ", ".join(fmt(c) for c in values) + "]"

        normals = self.normals if self.normals is not None \
            else vertex_normals(self.verts, self.faces)
        tangents = self.tangents if self.tangents is not None \
            else [[1.0, 0.0, 0.0, 1.0]] * len(self.verts)

        lines = ["vertices:"]
        lines += [f"  - {vec(v)}" for v in self.verts]
        lines.append("normals:")
        lines += [f"  - {vec(v)}" for v in normals]
        lines.append("uvs:")
        lines += [f"  - {vec(v)}" for v in self.uvs]
        lines.append("faces:")
        lines += [f"  - {vec(f)}" for f in self.faces]
        if self.colors is None:
            lines.append("colors: []")
        else:
            lines.append("colors:")
            lines += [f"  - {vec(c)}" for c in self.colors]
        lines.append("weights:")
        lines += ["  - {  }" for _ in self.verts]
        lines.append("tangents:")
        lines += [f"  - {vec(t)}" for t in tangents]
        return "\n".join(lines) + "\n"

    def write(self, project, name, fmt=fmt_num):
        """Writes the mesh into a project, under `assets/meshes`.

        Args:
            project (Project): The project to write into.
            name (str): Its path under `assets/meshes`, e.g. `"water/water_grid"`.
            fmt (callable): The number format.

        Returns:
            pathlib.Path: The file written.
        """
        return project.write_text(project.path_for("mesh", name), self.to_text(fmt))

    def write_to(self, path, fmt=fmt_num, project=None):
        """Writes the mesh to a path of your choosing.

        For a mesh that belongs to one scene rather than to the shared library -- a demo's
        terrain, a one-off prop -- which the committed scenes keep beside the scene itself as
        `assets/scenes/<area>/<name>/meshes/<mesh>.yaml`. `write()` is for `assets/meshes`.

        Args:
            path (str | pathlib.Path): Where to write it. Parent directories are created.
            fmt (callable): The number format.
            project (Project): Optional, and only for bookkeeping -- when given, the file is
                recorded in that project's `report()`. The write itself does not need one.

        Returns:
            pathlib.Path: The file written.
        """
        target = pathlib.Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(self.to_text(fmt))
        return project.record(target) if project is not None else target
