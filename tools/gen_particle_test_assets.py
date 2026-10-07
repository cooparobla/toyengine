#!/usr/bin/env python3
"""Generates the meshes the particles_test scene and its campfire object use.

Run from the repo root:  python3 tools/gen_particle_test_assets.py

Writes assets/meshes/props/:
  camp_log.yaml       a charred log: a slightly irregular 10-sided cylinder along +X, capped
  camp_stone.yaml     a flattened, lumpy low-poly rock (fire ring stones, pebbles)
  particle_ground.yaml a 40 m disc of gently rolling ground, z ~ 0 near the centre
  mossy_mound.yaml    a bumpy dome (r 3 m, h 1.3 m) -- the scatter demo's emitter surface
  mushroom.yaml       a stem and a domed cap, 1 m tall at scale 1 (scattered at ~0.2)
  magic_torus.yaml    a torus in the XY plane (R 1.1, r 0.22) -- the mesh-emission demo

Deterministic: every random perturbation comes from a fixed-seed generator.
"""

import math
import pathlib
import random

from mesh_yaml import perpendicular_tangents, vertex_normals, write_mesh

ROOT = pathlib.Path(__file__).resolve().parent.parent
MESHES = ROOT / "assets" / "meshes" / "props"


def lathe(profile, segments, close_top=True, close_bottom=True, jitter=None, top_z=None):
    """Revolves a (radius, z) profile about +Z.

    Args:
        profile (list): (radius, z) pairs from bottom to top.
        segments (int): Steps around the axis.
        close_top (bool): Fan-cap the last ring to its axis point.
        close_bottom (bool): Fan-cap the first ring.
        jitter (callable): Optional f(ring, seg, (x, y, z)) -> (x, y, z) perturbation.
        top_z (float): Height of the top cap's apex; the last ring's height when None.

    Returns:
        tuple: (verts, faces, uvs) with quads between rings, CCW from outside.
    """
    verts, uvs, faces = [], [], []
    rings = len(profile)
    for ri, (r, z) in enumerate(profile):
        for s in range(segments):
            a = 2.0 * math.pi * s / segments
            p = (r * math.cos(a), r * math.sin(a), z)
            if jitter:
                p = jitter(ri, s, p)
            verts.append(p)
            uvs.append((s / segments, ri / max(rings - 1, 1)))
    for ri in range(rings - 1):
        for s in range(segments):
            a = ri * segments + s
            b = ri * segments + (s + 1) % segments
            c = (ri + 1) * segments + (s + 1) % segments
            d = (ri + 1) * segments + s
            faces.append([a, b, c, d])
    if close_bottom:
        centre = len(verts)
        verts.append((0.0, 0.0, profile[0][1]))
        uvs.append((0.5, 0.0))
        for s in range(segments):
            faces.append([centre, (s + 1) % segments, s])
    if close_top:
        centre = len(verts)
        verts.append((0.0, 0.0, profile[-1][1] if top_z is None else top_z))
        uvs.append((0.5, 1.0))
        base = (rings - 1) * segments
        for s in range(segments):
            faces.append([centre, base + s, base + (s + 1) % segments])
    return verts, faces, uvs


def flat_shaded(verts, faces, uvs):
    """Unwelds every face so it shades flat -- the faceted, stylized-rock look."""
    out_v, out_f, out_uv, out_n = [], [], [], []
    for face in faces:
        a, b, c = (verts[face[0]], verts[face[1]], verts[face[2]])
        u = [b[i] - a[i] for i in range(3)]
        v = [c[i] - a[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        length = math.sqrt(sum(x * x for x in n)) or 1.0
        n = [x / length for x in n]
        idx = []
        for i in face:
            idx.append(len(out_v))
            out_v.append(verts[i])
            out_uv.append(uvs[i])
            out_n.append(n)
        out_f.append(idx)
    return out_v, out_f, out_uv, out_n


def write(name, verts, faces, uvs, normals=None):
    """write_mesh() with tangents kept perpendicular to the normals (see perpendicular_tangents),
    every polygon written as triangles (the jittered quads here are not planar, so spelling out
    the engine's fan split leaves an importer nothing to choose), and every value at 5 decimals.
    """
    if normals is None:
        normals = vertex_normals(verts, faces)
    tris = []
    for f in faces:
        for k in range(1, len(f) - 1):
            tris.append([f[0], f[k], f[k + 1]])

    # Round to the editor's own 5 decimals (editor/core/yaml_util.h tidy()), so a save from the
    # editor writes back exactly these numbers instead of nudging the last digit.
    def r5(rows):
        return [[round(c, 5) for c in row] for row in rows]

    MESHES.mkdir(parents=True, exist_ok=True)
    write_mesh(MESHES / f"{name}.yaml", r5(verts), tris, r5(uvs), r5(normals), r5(perpendicular_tangents(normals)))


def gen_log():
    """A 1 m log along +X, radius ~0.1, with a lumpy bark and flat cut ends."""
    rng = random.Random(11)
    bumps = [rng.uniform(0.88, 1.1) for _ in range(10)]

    def jitter(ri, s, p):
        k = bumps[s] * (1.0 + 0.04 * math.sin(ri * 1.7 + s))
        return (p[0] * k, p[1] * k, p[2])

    profile = [(0.1, -0.5 + i / 5.0) for i in range(6)]
    verts, faces, uvs = lathe(profile, 10, jitter=jitter)
    # Lay it along +X: (x, y, z) -> (z, y, -x).
    verts = [(z, y, -x) for (x, y, z) in verts]
    write("camp_log", verts, faces, uvs)


def gen_stone():
    """A flattened, lumpy rock about 1 m across at scale 1, flat-shaded."""
    rng = random.Random(5)
    rings, segs = 6, 9
    profile = []
    for i in range(rings + 1):
        phi = math.pi * i / rings - math.pi / 2.0
        profile.append((0.5 * math.cos(phi), 0.32 * math.sin(phi)))
    noise = {(r, s): rng.uniform(0.82, 1.12) for r in range(rings + 1) for s in range(segs)}

    def jitter(ri, s, p):
        k = noise[(ri, s)]
        return (p[0] * k, p[1] * k, p[2] * (0.9 + 0.2 * k))

    verts, faces, uvs = lathe(profile[1:-1], segs, jitter=jitter, top_z=0.3)
    # Sit it on z = 0 (its lowest point is the bottom cap, at the profile's second ring).
    lift = -min(v[2] for v in verts)
    verts = [(x, y, z + lift) for (x, y, z) in verts]
    v, f, uv, n = flat_shaded(verts, faces, uvs)
    write("camp_stone", v, f, uv, n)


def ground_height(x, y):
    """Gentle rolling hills that stay near z = 0 inside the clearing (r < 6)."""
    r = math.hypot(x, y)
    hills = 0.35 * math.sin(x * 0.35) * math.cos(y * 0.28) + 0.2 * math.sin(x * 0.17 + y * 0.23)
    falloff = min(max((r - 6.0) / 6.0, 0.0), 1.0)
    return hills * falloff - 0.02


def gen_ground():
    """A 40 m disc in polar rings, so its rim is round and its centre dense."""
    rings, segs, radius = 22, 48, 20.0
    verts, uvs = [(0.0, 0.0, ground_height(0.0, 0.0))], [(0.5, 0.5)]
    for ri in range(1, rings + 1):
        rr = radius * (ri / rings) ** 1.35
        for s in range(segs):
            a = 2.0 * math.pi * s / segs
            x, y = rr * math.cos(a), rr * math.sin(a)
            verts.append((x, y, ground_height(x, y)))
            uvs.append((x / 4.0, y / 4.0))
    faces = []
    for s in range(segs):
        faces.append([0, 1 + s, 1 + (s + 1) % segs])
    for ri in range(1, rings):
        base, nxt = 1 + (ri - 1) * segs, 1 + ri * segs
        for s in range(segs):
            faces.append([base + s, nxt + s, nxt + (s + 1) % segs, base + (s + 1) % segs])
    write("particle_ground", verts, faces, uvs)


def gen_mound():
    """A bumpy dome, radius 3 m, 1.3 m high, its rim at z = 0 -- smooth-shaded."""
    rng = random.Random(3)
    rings, segs = 10, 32
    phase = [rng.uniform(0.0, 6.28) for _ in range(4)]
    profile = []
    for i in range(rings + 1):
        t = i / rings
        profile.append((3.0 * math.cos(t * math.pi / 2.0), 1.3 * math.sin(t * math.pi / 2.0)))

    def jitter(ri, s, p):
        a = math.atan2(p[1], p[0])
        k = 1.0 + 0.06 * math.sin(a * 3.0 + phase[0]) + 0.04 * math.sin(a * 7.0 + phase[1] + ri)
        z = p[2] * (1.0 + 0.08 * math.sin(a * 2.0 + phase[2])) if ri > 0 else 0.0
        return (p[0] * k, p[1] * k, z)

    verts, faces, uvs = lathe(profile[:-1], segs, close_bottom=False, jitter=jitter, top_z=1.3)
    write("mossy_mound", verts, faces, uvs)


def gen_mushroom():
    """A mushroom 1 m tall at scale 1: a tapered stem under a domed cap with a lip."""
    segs = 12
    stem = [(0.11, 0.0), (0.1, 0.25), (0.085, 0.5), (0.08, 0.62)]
    cap = [(0.06, 0.6), (0.36, 0.58), (0.42, 0.64), (0.38, 0.78), (0.26, 0.92), (0.0001, 0.98)]
    v1, f1, uv1 = lathe(stem, segs, close_top=False)
    v2, f2, uv2 = lathe(cap, segs, close_top=True, close_bottom=True)
    offset = len(v1)
    verts = v1 + v2
    faces = f1 + [[i + offset for i in f] for f in f2]
    uvs = uv1 + uv2
    write("mushroom", verts, faces, uvs)


def gen_torus():
    """A torus in the XY plane: major radius 1.1, minor 0.22."""
    major, minor, segs, sides = 1.1, 0.22, 40, 12
    verts, uvs, normals, faces = [], [], [], []
    for i in range(segs):
        a = 2.0 * math.pi * i / segs
        ca, sa = math.cos(a), math.sin(a)
        for j in range(sides):
            b = 2.0 * math.pi * j / sides
            cb, sb = math.cos(b), math.sin(b)
            verts.append(((major + minor * cb) * ca, (major + minor * cb) * sa, minor * sb))
            normals.append((cb * ca, cb * sa, sb))
            uvs.append((i / segs, j / sides))
    for i in range(segs):
        for j in range(sides):
            a = i * sides + j
            b = ((i + 1) % segs) * sides + j
            c = ((i + 1) % segs) * sides + (j + 1) % sides
            d = i * sides + (j + 1) % sides
            faces.append([a, b, c, d])
    write("magic_torus", verts, faces, uvs, normals)


def main():
    gen_log()
    gen_stone()
    gen_ground()
    gen_mound()
    gen_mushroom()
    gen_torus()
    print("wrote camp_log, camp_stone, particle_ground, mossy_mound, mushroom, magic_torus to", MESHES)


if __name__ == "__main__":
    main()
