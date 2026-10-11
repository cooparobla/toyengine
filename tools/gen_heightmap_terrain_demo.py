#!/usr/bin/env python3
"""Generates the assets of assets/scenes/terrain/heightmap_terrain_demo.

Requires numpy and Pillow (see tools/gen_terrain_atlas.py for the venv note).

Writes:
  assets/scenes/terrain/heightmap_terrain_demo/terrain/height.png   16-bit heightmap, 1025 x 1025
  assets/scenes/terrain/heightmap_terrain_demo/terrain/splat0.png   layers 0-3 (RGBA)
  assets/scenes/terrain/heightmap_terrain_demo/terrain/splat1.png   layers 4-7
  assets/textures/terrain/layers/<layer>_albedo.png / _normal.png   tileable 512 x 512 layer maps
  assets/textures/terrain/layers/grass_blades.png                   alpha-masked grass card
  assets/meshes/nature/{grass_tuft,pine_trunk,pine_canopy,boulder,bush}.yaml

The terrain (1024 m square, 160 m tall): a green valley with a meandering river and a lake,
ringed by mountains whose upper slopes break into terraced cliffs and carry snow. Layers:
0 grass, 1 dirt, 2 rock (triplanar), 3 cliff (triplanar), 4 sand, 5 snow, 6 forest floor.

Usage:
    python3 tools/gen_heightmap_terrain_demo.py
"""

import math
import pathlib
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from mesh_yaml import perpendicular_tangents, vertex_normals, write_mesh  # noqa: E402

SCENE = ROOT / "assets/scenes/terrain/heightmap_terrain_demo"
LAYERS_DIR = ROOT / "assets/textures/terrain/layers"
MESH_DIR = ROOT / "assets/meshes/nature"

SIZE_M = 1024.0
HEIGHT_M = 160.0
RES = 1025
SPLAT = 1024
WATER_Z = 16.0      # the lake / river surface (m), matched by the scene's WaterBody

rng = np.random.default_rng(1729)


# ------------------------------------------------------------------------------------ noise

def value_noise(n, cells, seed):
    """Smooth value noise on an n x n grid with `cells` lattice cells per side (bicubic)."""
    r = np.random.default_rng(seed).random((cells + 3, cells + 3)).astype(np.float32)
    img = Image.fromarray(r, mode="F").resize((n * (cells + 3) // cells, n * (cells + 3) // cells), Image.BICUBIC)
    a = np.asarray(img)
    off = n // cells
    return a[off:off + n, off:off + n]


def fbm(n, base_cells, octaves, seed, gain=0.5):
    total = np.zeros((n, n), np.float32)
    amp, norm = 1.0, 0.0
    cells = base_cells
    for o in range(octaves):
        total += amp * (value_noise(n, cells, seed + o * 31) * 2.0 - 1.0)
        norm += amp
        amp *= gain
        cells *= 2
    return total / norm


def periodic_noise(n, scale, seed):
    """Tileable noise: white noise filtered in frequency space (periodic by construction)."""
    white = np.random.default_rng(seed).standard_normal((n, n))
    f = np.fft.fftfreq(n)
    fx, fy = np.meshgrid(f, f)
    r = np.sqrt(fx * fx + fy * fy)
    filt = np.exp(-(r * scale) ** 2)
    out = np.real(np.fft.ifft2(np.fft.fft2(white) * filt))
    out -= out.min()
    return (out / max(out.max(), 1e-9)).astype(np.float32)


def periodic_fbm(n, scales, seed):
    total = np.zeros((n, n), np.float32)
    amp, norm = 1.0, 0.0
    for k, s in enumerate(scales):
        total += amp * periodic_noise(n, s, seed + k)
        norm += amp
        amp *= 0.5
    return total / norm


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# ------------------------------------------------------------------------------------ terrain

def build_heights():
    n = RES
    ys, xs = np.mgrid[0:n, 0:n].astype(np.float32) * (SIZE_M / (n - 1))   # [j, i] -> y, x metres

    # Mountain ring: distance from a slightly off-centre valley axis.
    cx, cy = 520.0, 500.0
    d = np.sqrt(((xs - cx) / 470.0) ** 2 + ((ys - cy) / 430.0) ** 2)
    ring = smoothstep(0.55, 1.05, d)

    ridged = 1.0 - np.abs(fbm(n, 6, 6, 11))          # sharp crests
    rolling = fbm(n, 4, 5, 23)
    detail = fbm(n, 24, 4, 37)

    valley = 22.0 + 7.0 * rolling + 2.0 * detail
    mountains = 40.0 + 115.0 * ridged ** 1.6 * (0.6 + 0.4 * rolling) + 4.0 * detail
    h = valley * (1.0 - ring) + mountains * ring

    # Terraced cliffs on the north-east massif.
    massif = smoothstep(0.75, 1.0, d) * smoothstep(300.0, 650.0, xs) * smoothstep(400.0, 800.0, ys)
    step = 9.0
    terraced = np.floor(h / step) * step + step * smoothstep(0.75, 1.0, (h / step) % 1.0)
    h = h * (1.0 - massif) + terraced * massif

    # A meandering river along the valley (y runs south -> north).
    river_x = 470.0 + 90.0 * np.sin(ys / 170.0) + 40.0 * np.sin(ys / 61.0 + 1.3)
    dr = np.abs(xs - river_x)
    channel = smoothstep(26.0, 6.0, dr) * (1.0 - ring)
    h = h * (1.0 - channel) + (WATER_Z - 2.5) * channel
    banks = smoothstep(60.0, 20.0, dr) * (1.0 - ring)
    h = h - banks * np.clip(h - (WATER_Z + 2.0), 0.0, None) * 0.55

    # A lake in the south-west.
    lake = smoothstep(115.0, 60.0, np.sqrt((xs - 330.0) ** 2 + (ys - 250.0) ** 2))
    h = h * (1.0 - lake) + (WATER_Z - 7.0) * lake

    return np.clip(h, 0.0, HEIGHT_M - 1.0).astype(np.float32), xs, ys


def slope_degrees(h):
    cell = SIZE_M / (RES - 1)
    gy, gx = np.gradient(h, cell)
    return np.degrees(np.arctan(np.sqrt(gx * gx + gy * gy)))


def build_splats(h):
    # Splat texels are texel-centred over the terrain; sample the height grid at their centres.
    n = SPLAT
    idx = (np.arange(n) + 0.5) * (RES - 1) / n
    hs = np.asarray(Image.fromarray(h, mode="F").resize((n, n), Image.BILINEAR))
    sl = np.asarray(Image.fromarray(slope_degrees(h).astype(np.float32), mode="F").resize((n, n), Image.BILINEAR))
    patch = fbm(n, 12, 4, 51) * 0.5 + 0.5
    forest_noise = fbm(n, 8, 4, 61) * 0.5 + 0.5

    w = np.zeros((8, n, n), np.float32)
    snow = smoothstep(108.0, 122.0, hs + 10.0 * (patch - 0.5)) * smoothstep(42.0, 30.0, sl)
    cliff = smoothstep(36.0, 46.0, sl)
    rock = smoothstep(24.0, 33.0, sl) * (1.0 - cliff)
    sand = smoothstep(WATER_Z + 3.0, WATER_Z + 0.8, hs) * (1.0 - cliff)
    dirt = smoothstep(0.62, 0.75, patch) * (1.0 - rock - cliff).clip(0) * (1.0 - sand)
    forest = smoothstep(0.58, 0.68, forest_noise) * smoothstep(WATER_Z + 6.0, WATER_Z + 12.0, hs) \
        * smoothstep(80.0, 60.0, hs) * smoothstep(24.0, 16.0, sl)

    w[5] = snow
    w[3] = cliff * (1.0 - snow)
    w[2] = rock * (1.0 - snow)
    w[4] = sand
    w[1] = dirt * (1.0 - snow)
    w[6] = forest * (1.0 - snow) * (1.0 - sand)
    rest = np.clip(1.0 - w.sum(axis=0), 0.0, 1.0)
    w[0] = rest
    w /= np.maximum(w.sum(axis=0), 1e-6)
    q = np.round(w * 255.0).astype(np.int32)
    # Fix rounding so every texel sums to exactly 255 (largest layer takes the remainder).
    fix = 255 - q.sum(axis=0)
    big = q.argmax(axis=0)
    np.put_along_axis(q, big[None], np.take_along_axis(q, big[None], axis=0) + fix[None], axis=0)
    return np.clip(q, 0, 255).astype(np.uint8)


def save_height(h, path):
    v = np.clip(np.round(h / HEIGHT_M * 65535.0), 0, 65535).astype(np.uint16)
    # Row 0 of the image is the north edge (max y): flip.
    Image.fromarray(v[::-1].copy(), mode="I;16").save(path)


def save_splat(q, k, path):
    rgba = np.stack([q[4 * k + c] for c in range(4)], axis=-1)[::-1].copy()
    Image.fromarray(rgba, mode="RGBA").save(path)


# ------------------------------------------------------------------------------------ layer maps

def normal_from_height(hh, strength):
    gx = (np.roll(hh, -1, axis=1) - np.roll(hh, 1, axis=1)) * 0.5 * strength
    gy = (np.roll(hh, -1, axis=0) - np.roll(hh, 1, axis=0)) * 0.5 * strength
    # Image rows run down = -y in tangent space.
    nx, ny, nz = -gx, gy, np.ones_like(hh)
    l = np.sqrt(nx * nx + ny * ny + nz * nz)
    rgb = np.stack([nx / l, ny / l, nz / l], axis=-1) * 0.5 + 0.5
    return (np.clip(rgb, 0, 1) * 255).astype(np.uint8)


def save_rgb(arr, path):
    Image.fromarray(arr.astype(np.uint8), mode="RGB").save(path)


def colorize(t, lo, hi):
    lo = np.array(lo, np.float32)
    hi = np.array(hi, np.float32)
    return lo[None, None, :] * (1.0 - t[..., None]) + hi[None, None, :] * t[..., None]


def layer_maps():
    n = 512
    LAYERS_DIR.mkdir(parents=True, exist_ok=True)
    out = {}

    g = periodic_fbm(n, [60, 20, 6, 2], 101)
    blades = periodic_noise(n, 1.2, 102)
    grass_h = 0.6 * g + 0.4 * blades
    out["grass"] = (colorize(grass_h, (52, 82, 30), (112, 142, 58)), normal_from_height(grass_h * 8.0, 1.0))

    d = periodic_fbm(n, [40, 12, 3], 201)
    pebbles = (periodic_noise(n, 2.5, 202) > 0.72).astype(np.float32)
    dirt_h = 0.7 * d + 0.3 * pebbles
    out["dirt"] = (colorize(dirt_h, (82, 62, 42), (138, 112, 82)), normal_from_height(dirt_h * 10.0, 1.0))

    r = periodic_fbm(n, [80, 30, 10, 3], 301)
    cracks = np.abs(periodic_noise(n, 25, 302) - 0.5) < 0.02
    rock_h = r - 0.25 * cracks
    out["rock"] = (colorize(rock_h, (78, 76, 72), (150, 146, 138)), normal_from_height(rock_h * 14.0, 1.0))

    yy = np.mgrid[0:n, 0:n][0].astype(np.float32)
    strata = 0.5 + 0.5 * np.sin(yy / n * 2 * math.pi * 9 + periodic_noise(n, 30, 401) * 6.0)
    cliff_h = 0.55 * strata + 0.45 * periodic_fbm(n, [50, 15, 4], 402)
    out["cliff"] = (colorize(cliff_h, (92, 80, 70), (168, 150, 128)), normal_from_height(cliff_h * 16.0, 1.0))

    xx = np.mgrid[0:n, 0:n][1].astype(np.float32)
    ripples = 0.5 + 0.5 * np.sin((xx + 25.0 * periodic_noise(n, 40, 501)) / n * 2 * math.pi * 24)
    sand_h = 0.35 * ripples + 0.65 * periodic_fbm(n, [30, 5], 502)
    out["sand"] = (colorize(sand_h, (184, 164, 120), (226, 208, 166)), normal_from_height(sand_h * 5.0, 1.0))

    s = periodic_fbm(n, [60, 15, 3], 601)
    out["snow"] = (colorize(s, (206, 216, 230), (246, 249, 255)), normal_from_height(s * 4.0, 1.0))

    f = periodic_fbm(n, [40, 10, 3], 701)
    needles = periodic_noise(n, 1.0, 702)
    forest_h = 0.6 * f + 0.4 * needles
    out["forest_floor"] = (colorize(forest_h, (46, 44, 26), (96, 86, 50)), normal_from_height(forest_h * 8.0, 1.0))

    for name, (alb, nrm) in out.items():
        save_rgb(alb, LAYERS_DIR / f"{name}_albedo.png")
        save_rgb(nrm, LAYERS_DIR / f"{name}_normal.png")
        print(f"wrote {name} layer maps")

    # The grass card: tapered blades on transparent black.
    w, hgt = 256, 256
    card = np.zeros((hgt, w, 4), np.float32)
    br = np.random.default_rng(803)
    yy, xx = np.mgrid[0:hgt, 0:w].astype(np.float32)
    for _ in range(26):
        x0 = br.uniform(10, w - 10)
        top = br.uniform(0.15, 0.6) * hgt
        lean = br.uniform(-40, 40)
        width = br.uniform(4, 8)
        t = np.clip((hgt - yy) / (hgt - top), 0, 1)          # 0 at the base, 1 at the tip
        cx = x0 + lean * t * t
        inside = (np.abs(xx - cx) < width * (1.0 - t)) & (yy >= top)
        shade = 0.55 + 0.45 * t
        col = np.stack([0.30 * shade, 0.55 * shade, 0.18 * shade], axis=-1)
        card[inside, 0:3] = col[inside]
        card[inside, 3] = 1.0
    Image.fromarray((card * 255).astype(np.uint8), mode="RGBA").save(LAYERS_DIR / "grass_blades.png")
    print("wrote grass_blades.png")


# ------------------------------------------------------------------------------------ meshes

def grass_tuft():
    """Three crossed vertical cards, 1 m wide, 0.6 m tall, base at the origin."""
    verts, faces, uvs = [], [], []
    for k in range(3):
        a = math.pi * k / 3
        dx, dy = 0.5 * math.cos(a), 0.5 * math.sin(a)
        base = len(verts)
        verts += [[-dx, -dy, 0.0], [dx, dy, 0.0], [dx, dy, 0.6], [-dx, -dy, 0.6]]
        uvs += [[0, 1], [1, 1], [1, 0], [0, 0]]
        faces.append([base, base + 1, base + 2, base + 3])
        # The other side as its own face, so both sides are front faces with an up normal (a
        # back face's normal is flipped by the G-buffer, which would shade the card from below).
        b2 = len(verts)
        verts += [[-dx, -dy, 0.0], [dx, dy, 0.0], [dx, dy, 0.6], [-dx, -dy, 0.6]]
        uvs += [[0, 1], [1, 1], [1, 0], [0, 0]]
        faces.append([b2 + 3, b2 + 2, b2 + 1, b2])
    # Up-facing normals light the cards like the ground they grow from.
    normals = [[0.0, 0.0, 1.0] for _ in verts]
    write_mesh(MESH_DIR / "grass_tuft.yaml", verts, faces, uvs, normals)


def cylinder(r0, r1, z0, z1, sides, verts, faces, uvs):
    base = len(verts)
    for k in range(sides):
        a = 2 * math.pi * k / sides
        verts.append([r0 * math.cos(a), r0 * math.sin(a), z0])
        uvs.append([k / sides, 1.0])
    for k in range(sides):
        a = 2 * math.pi * k / sides
        verts.append([r1 * math.cos(a), r1 * math.sin(a), z1])
        uvs.append([k / sides, 0.0])
    for k in range(sides):
        k2 = (k + 1) % sides
        faces.append([base + k, base + k2, base + sides + k2, base + sides + k])


def cone(r, z0, z1, sides, verts, faces, uvs):
    base = len(verts)
    for k in range(sides):
        a = 2 * math.pi * k / sides + 0.3 * z0
        verts.append([r * math.cos(a), r * math.sin(a), z0])
        uvs.append([k / sides, 1.0])
    tip = len(verts)
    verts.append([0.0, 0.0, z1])
    uvs.append([0.5, 0.0])
    centre = len(verts)
    verts.append([0.0, 0.0, z0])
    uvs.append([0.5, 0.5])
    for k in range(sides):
        k2 = (k + 1) % sides
        faces.append([base + k, base + k2, tip])
        faces.append([base + k2, base + k, centre])


def pine():
    """A low-poly pine about 8 m tall: trunk and canopy as two meshes placed together."""
    verts, faces, uvs = [], [], []
    cylinder(0.28, 0.16, -0.3, 3.2, 7, verts, faces, uvs)
    write_mesh(MESH_DIR / "pine_trunk.yaml", verts, faces, uvs)
    verts, faces, uvs = [], [], []
    cone(2.3, 1.6, 5.0, 9, verts, faces, uvs)
    cone(1.8, 3.6, 6.8, 9, verts, faces, uvs)
    cone(1.2, 5.4, 8.4, 9, verts, faces, uvs)
    write_mesh(MESH_DIR / "pine_canopy.yaml", verts, faces, uvs)


def icosphere(subdiv):
    t = (1.0 + 5 ** 0.5) / 2.0
    v = [[-1, t, 0], [1, t, 0], [-1, -t, 0], [1, -t, 0], [0, -1, t], [0, 1, t], [0, -1, -t], [0, 1, -t],
         [t, 0, -1], [t, 0, 1], [-t, 0, -1], [-t, 0, 1]]
    v = [list(np.array(p) / np.linalg.norm(p)) for p in v]
    f = [[0, 11, 5], [0, 5, 1], [0, 1, 7], [0, 7, 10], [0, 10, 11], [1, 5, 9], [5, 11, 4], [11, 10, 2], [10, 7, 6],
         [7, 1, 8], [3, 9, 4], [3, 4, 2], [3, 2, 6], [3, 6, 8], [3, 8, 9], [4, 9, 5], [2, 4, 11], [6, 2, 10],
         [8, 6, 7], [9, 8, 1]]
    for _ in range(subdiv):
        cache = {}
        nf = []

        def mid(a, b):
            key = (min(a, b), max(a, b))
            if key not in cache:
                p = (np.array(v[a]) + np.array(v[b])) * 0.5
                v.append(list(p / np.linalg.norm(p)))
                cache[key] = len(v) - 1
            return cache[key]

        for a, b, c in f:
            ab, bc, ca = mid(a, b), mid(b, c), mid(c, a)
            nf += [[a, ab, ca], [b, bc, ab], [c, ca, bc], [ab, bc, ca]]
        f = nf
    return v, f


def lumpy(name, scale, flatten, seed, lift):
    v, f = icosphere(2)
    r = np.random.default_rng(seed)
    bumps = r.normal(size=(6, 3))
    out = []
    for p in v:
        p = np.array(p)
        k = 1.0 + 0.18 * sum(math.sin(3.0 * float(np.dot(p, b))) for b in bumps) / 6.0
        q = p * k * np.array(scale)
        q[2] = q[2] * flatten + lift
        out.append(list(q))
    uvs = [[0.5 + 0.5 * p[0], 0.5 + 0.5 * p[1]] for p in v]
    normals = vertex_normals(out, f)
    write_mesh(MESH_DIR / f"{name}.yaml", out, f, uvs, normals, perpendicular_tangents(normals))


def main():
    (SCENE / "terrain").mkdir(parents=True, exist_ok=True)
    MESH_DIR.mkdir(parents=True, exist_ok=True)
    h, _, _ = build_heights()
    save_height(h, SCENE / "terrain/height.png")
    q = build_splats(h)
    save_splat(q, 0, SCENE / "terrain/splat0.png")
    save_splat(q, 1, SCENE / "terrain/splat1.png")
    print(f"wrote heightmap ({h.min():.1f} .. {h.max():.1f} m) and splats")
    layer_maps()
    grass_tuft()
    pine()
    lumpy("boulder", (1.0, 0.85, 0.7), 0.8, 901, 0.25)
    lumpy("bush", (0.9, 0.9, 0.7), 0.9, 902, 0.45)


if __name__ == "__main__":
    main()
