#!/usr/bin/env python3
"""Generates the dense demo meshes `damage_demo` dents at their authored resolution.

`assets/meshes/primitives/sphere` (425 verts) and `assets/meshes/props/camp_log` (62 verts) are
too coarse for a 0.03-0.6 m dent footprint -- displacing one of their vertices drags a visible
corner rather than crushing a patch of surface. `camp_log_dense` and `sphere_dense` are the same
shapes at a resolution that dents well, generated rather than hand-authored because of their
vertex count. `metal_plate` in damage_demo instead stays on the coarse shared `cube` and is
subdivided at runtime on first impact (src/toyengine/destruct/dent.h's subdivide_for_dent()) -- the
two demo objects together show both ways of getting enough resolution to dent.

Winding for both shapes is derived by hand (not copied from gen_particle_demo_assets.lathe(),
whose last strip's UVs run backwards) and verified against the existing hand-authored assets:
camp_log_dense's side-quad and cap order is byte-for-byte the same pattern camp_log.yaml itself
uses, confirmed by inspection before writing this generator.
"""

import math
import pathlib
import sys

# toyproject (python/) is stdlib-only, so one path entry is all it needs -- no venv and no
# install, which is why this runs under a bare `python3`. See python/README.md.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "python" / "src"))

from toyproject import fmt_g6, perpendicular_tangents, vertex_normals, write_mesh

def _write(path, verts, faces, uvs):
    """Writes one mesh with explicit area-weighted normals and seam-safe tangents.

    `fmt_g6` is the compact float format these meshes were generated with; writing them with
    the other one would rewrite every vertex line without changing the geometry.

    Args:
        path (pathlib.Path): Destination file.
        verts (list): Vertex positions.
        faces (list): Triangles as index lists.
        uvs (list): One UV per vertex.
    """
    normals = vertex_normals(verts, faces)
    tangents = perpendicular_tangents(normals)
    written = write_mesh(path, verts, faces, uvs, normals=normals, tangents=tangents,
                         fmt=fmt_g6)
    print(f"wrote {len(verts)} vertices, {len(faces)} faces to {written}")


def gen_camp_log_dense(path):
    """A bumpy log along +-0.5 on X, 24 segments around x 16 rings along its length.

    Mirrors camp_log.yaml's lathe construction (profile along X, radius in the Y-Z plane, no
    duplicated seam column) at a resolution a dent kernel can crush.

    Args:
        path (pathlib.Path): Destination file.
    """
    seg, rings = 24, 16
    base_radius = 0.1
    verts, uvs = [], []
    for i in range(rings):
        x = -0.5 + i * (1.0 / (rings - 1))
        v = i / (rings - 1)
        for j in range(seg):
            theta = j * 2.0 * math.pi / seg
            # A deterministic organic undulation, not a true cylinder -- the same bumpy-log
            # look camp_log.yaml has, just denser.
            bump = 1.0 + 0.08 * math.sin(3.0 * theta + i * 0.9) + 0.04 * math.sin(5.0 * theta - i * 0.5)
            r = base_radius * bump
            verts.append([round(x, 5), round(r * math.cos(theta), 5), round(r * math.sin(theta), 5)])
            uvs.append([round(j / seg, 5), round(v, 5)])

    left_centre, right_centre = len(verts), len(verts) + 1
    verts.append([-0.5, 0.0, 0.0])
    verts.append([0.5, 0.0, 0.0])
    uvs.append([0.5, 0.0])
    uvs.append([0.5, 1.0])

    def ring_base(i):
        return i * seg

    faces = []
    for i in range(rings - 1):
        for j in range(seg):
            a = ring_base(i) + j
            b = ring_base(i) + (j + 1) % seg
            c = ring_base(i + 1) + (j + 1) % seg
            d = ring_base(i + 1) + j
            faces.append([a, b, c])
            faces.append([a, c, d])
    for j in range(seg):
        faces.append([left_centre, (j + 1) % seg, j])
    base = ring_base(rings - 1)
    for j in range(seg):
        faces.append([right_centre, base + j, base + (j + 1) % seg])

    _write(path, verts, faces, uvs)


def gen_sphere_dense(path):
    """A radius-1 UV sphere, 48 segments around x 32 interior latitude rings, poles along Z.

    Args:
        path (pathlib.Path): Destination file.
    """
    seg, lat_rings = 48, 32
    radius = 1.0
    verts, uvs = [], []
    for i in range(lat_rings):
        phi = (i + 1) / (lat_rings + 1) * math.pi
        v = (i + 1) / (lat_rings + 1)
        for j in range(seg):
            theta = j * 2.0 * math.pi / seg
            x = radius * math.sin(phi) * math.cos(theta)
            y = radius * math.sin(phi) * math.sin(theta)
            z = radius * math.cos(phi)
            verts.append([round(x, 5), round(y, 5), round(z, 5)])
            uvs.append([round(j / seg, 5), round(v, 5)])

    north, south = len(verts), len(verts) + 1
    verts.append([0.0, 0.0, radius])
    verts.append([0.0, 0.0, -radius])
    uvs.append([0.5, 0.0])
    uvs.append([0.5, 1.0])

    def ring_base(i):
        return i * seg

    faces = []
    for j in range(seg):
        faces.append([north, j, (j + 1) % seg])
    for i in range(lat_rings - 1):
        for j in range(seg):
            a = ring_base(i) + j
            b = ring_base(i) + (j + 1) % seg
            c = ring_base(i + 1) + (j + 1) % seg
            d = ring_base(i + 1) + j
            # Winding is reversed relative to camp_log_dense's side quads: this parametrisation
            # (cos on X, sin on Y, pole axis Z) flips the cross-product sign that decided
            # camp_log_dense's (a, b, c) / (a, c, d) order, verified numerically before writing
            # this generator.
            faces.append([a, c, b])
            faces.append([a, d, c])
    base = ring_base(lat_rings - 1)
    for j in range(seg):
        faces.append([south, base + (j + 1) % seg, base + j])

    _write(path, verts, faces, uvs)


if __name__ == "__main__":
    root = pathlib.Path(__file__).parent.parent
    gen_camp_log_dense(root / "assets/meshes/props/camp_log_dense.yaml")
    gen_sphere_dense(root / "assets/meshes/primitives/sphere_dense.yaml")
