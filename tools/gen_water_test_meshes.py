"""Generates the meshes for the water_test scene (assets/scenes/water_test/meshes/), the
underwater_test pool, and the shared water props (assets/meshes/sphere_low.yaml, barrel.yaml).

Standalone generator, not part of the C++ build -- run it manually
(`python3 tools/gen_water_test_meshes.py`) whenever the scene's terrain or river needs
regenerating. Writes:

  lake_basin.yaml           render mesh of the terrain: a lake basin with a gentle sand beach to
                            the west, a steep bank to the north, a small island, and a hill to the
                            east with a river channel carved down it into the lake.
  lake_basin_collider.yaml  the same surface for the static MeshCollider (a second file on
                            purpose: AssetManager caches one asset type per resolved path, so a
                            gfx Mesh and a physics TriangleMesh cannot share one -- see
                            physics_test's mesh_floor_collider).
  river_water.yaml          the river's water surface: a strip following the river's centreline
                            spline, UV u running downstream (metres), v across. It descends
                            through a gentle upper reach, steep rapids, and a gentle outlet into
                            the lake -- WaterSystem derives the current from exactly this slope.

World convention: Z up, metres. The lake surface sits at z = 0.
"""

import math
import pathlib

OUT_DIR = pathlib.Path(__file__).parent.parent / "assets/scenes/water_test/meshes"

# --- Terrain grid ---
EXTENT = 40.0          # terrain covers [-EXTENT, EXTENT]^2
CELLS = 100            # quads per side (0.8 m)

# --- Lake ---
LAKE_DEPTH = 3.6

# --- River ---
RIVER_CONTROL = [(37.5, 12.0), (32.0, 14.0), (27.0, 11.0), (23.5, 6.8), (21.0, 4.2), (19.6, 3.0)]
CHANNEL_HALF_WIDTH = 2.2   # carved channel half width at the bank top
WATER_HALF_WIDTH = 2.05    # river surface half width (edges tuck under the banks)
# (s, surface z): gentle reach, rapids, gentle outlet. s is normalized arc length.
RIVER_PROFILE = [(0.0, 6.0), (0.28, 5.6), (0.62, 1.6), (0.9, 0.32), (1.0, 0.03)]
RIVER_ALONG = 140          # strip segments along the river
RIVER_ACROSS = 6           # strip segments across


def smoothstep(e0, e1, x):
    t = max(0.0, min(1.0, (x - e0) / (e1 - e0)))
    return t * t * (3.0 - 2.0 * t)


def catmull_rom(points, samples_per_segment=40):
    pts = [points[0]] + points + [points[-1]]
    out = []
    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        for k in range(samples_per_segment):
            t = k / samples_per_segment
            t2, t3 = t * t, t * t * t
            out.append(tuple(0.5 * ((2 * p1[c]) + (-p0[c] + p2[c]) * t + (2 * p0[c] - 5 * p1[c] + 4 * p2[c] - p3[c]) * t2
                                    + (-p0[c] + 3 * p1[c] - 3 * p2[c] + p3[c]) * t3) for c in range(2)))
    out.append(points[-1])
    return out


CENTRELINE = catmull_rom(RIVER_CONTROL)
_ARC = [0.0]
for a, b in zip(CENTRELINE, CENTRELINE[1:]):
    _ARC.append(_ARC[-1] + math.dist(a, b))
RIVER_LENGTH = _ARC[-1]


def profile_z(s):
    """River surface height at normalized arc length s: piecewise-linear, then eased."""
    s = max(0.0, min(1.0, s))
    for (s0, z0), (s1, z1) in zip(RIVER_PROFILE, RIVER_PROFILE[1:]):
        if s <= s1:
            t = (s - s0) / (s1 - s0)
            t = t * t * (3.0 - 2.0 * t)   # ease each reach into the next: no kinks at the joins
            return z0 + (z1 - z0) * t
    return RIVER_PROFILE[-1][1]


def closest_on_river(x, y):
    """(distance to centreline, normalized arc length s) of the closest centreline point."""
    best = (1e9, 0.0)
    for i, (a, b) in enumerate(zip(CENTRELINE, CENTRELINE[1:])):
        ax, ay = a
        dx, dy = b[0] - ax, b[1] - ay
        l2 = dx * dx + dy * dy
        t = 0.0 if l2 < 1e-12 else max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / l2))
        px, py = ax + dx * t, ay + dy * t
        d = math.hypot(x - px, y - py)
        if d < best[0]:
            best = (d, (_ARC[i] + t * math.sqrt(l2)) / RIVER_LENGTH)
    return best


def shore_radius(theta):
    return 18.5 + 2.5 * math.sin(3.0 * theta) + 1.5 * math.cos(5.0 * theta + 1.0)


def shore_slope(theta):
    """Bank steepness by direction: gentle beach west, steep bank north, moderate elsewhere."""
    west = max(0.0, -math.cos(theta)) ** 2
    north = max(0.0, math.sin(theta)) ** 3
    return 0.32 * (1.0 - west) * (1.0 - north) + 0.09 * west + 1.1 * north


def terrain_height(x, y):
    d = math.hypot(x, y)
    theta = math.atan2(y, x)
    r = shore_radius(theta)
    slope = shore_slope(theta)
    if d < r:
        z = -min(LAKE_DEPTH, (r - d) * slope * 1.2)
        # soften the bottom of the bowl
        z = -LAKE_DEPTH * (1.0 - math.exp(z / LAKE_DEPTH * 1.6)) / (1.0 - math.exp(-1.6))
    else:
        cap = 1.2 + 3.8 * max(0.0, math.sin(theta)) ** 2   # the north bank stands taller
        z = min(cap, (d - r) * slope)

    # Island.
    di = math.hypot(x - 5.0, y + 7.0)
    z = max(z, 1.3 - 0.24 * di * di)

    # East hill, kept off the lake itself.
    if d > r:
        z = max(z, 7.6 * smoothstep(13.0, 33.0, x) * smoothstep(r, r + 6.0, d))

    # River: raise levees where the ground sits below the water, then carve the channel.
    dist, s = closest_on_river(x, y)
    if dist < CHANNEL_HALF_WIDTH + 6.0:
        surf = profile_z(s)
        levee = surf + 0.45 - max(0.0, dist - (CHANNEL_HALF_WIDTH + 1.5)) * 0.9
        if s < 0.97:
            z = max(z, levee)
        carve = surf - 1.1 + (dist / CHANNEL_HALF_WIDTH) ** 2 * 1.6
        if d > r - 0.5:
            # Outside the lake the bed never dips below the lake surface: the river shoals out
            # over a ford at its mouth, and the lake's own (planar, z = 0) water stays buried
            # under it instead of showing through the river as a second layer.
            carve = max(carve, 0.02)
        z = min(z, carve)
    return z


def fmt(v):
    return "[" + ", ".join(f"{c:.5f}" if isinstance(c, float) else str(c) for c in v) + "]"


def write_mesh(path, verts, faces, normals=None, uvs=None, collider=False):
    lines = ["vertices:"]
    lines += [f"  - {fmt(v)}" for v in verts]
    if not collider:
        lines.append("normals:")
        lines += [f"  - {fmt(n)}" for n in normals]
        lines.append("uvs:")
        lines += [f"  - {fmt(u)}" for u in uvs]
    lines.append("faces:")
    lines += [f"  - {fmt(f)}" for f in faces]
    if not collider:
        lines.append("colors: []")
        lines.append("weights:")
        lines += ["  - {  }" for _ in verts]
    path.write_text("\n".join(lines) + "\n")
    print(f"wrote {len(verts)} vertices, {len(faces)} faces to {path}")


def grid_normals(heights, n, step):
    normals = []
    for j in range(n + 1):
        for i in range(n + 1):
            hl = heights[j][max(i - 1, 0)]
            hr = heights[j][min(i + 1, n)]
            hd = heights[max(j - 1, 0)][i]
            hu = heights[min(j + 1, n)][i]
            dx = (hr - hl) / (step * (min(i + 1, n) - max(i - 1, 0)))
            dy = (hu - hd) / (step * (min(j + 1, n) - max(j - 1, 0)))
            nx, ny, nz = -dx, -dy, 1.0
            l = math.sqrt(nx * nx + ny * ny + nz * nz)
            normals.append([nx / l, ny / l, nz / l])
    return normals


def build_basin():
    n = CELLS
    step = 2.0 * EXTENT / n
    heights = [[terrain_height(-EXTENT + i * step, -EXTENT + j * step) for i in range(n + 1)] for j in range(n + 1)]
    verts, uvs = [], []
    for j in range(n + 1):
        for i in range(n + 1):
            verts.append([-EXTENT + i * step, -EXTENT + j * step, heights[j][i]])
            uvs.append([i / n * 8.0, j / n * 8.0])
    normals = grid_normals(heights, n, step)
    faces = []
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            faces.append([a, a + 1, a + n + 2, a + n + 1])
    write_mesh(OUT_DIR / "lake_basin.yaml", verts, faces, normals, uvs)
    write_mesh(OUT_DIR / "lake_basin_collider.yaml", verts, faces, collider=True)


def build_river():
    verts, uvs, normals = [], [], []
    for k in range(RIVER_ALONG + 1):
        s = k / RIVER_ALONG
        target = s * RIVER_LENGTH
        # centreline point and tangent at arc length `target`
        i = 0
        while i < len(_ARC) - 2 and _ARC[i + 1] < target:
            i += 1
        a, b = CENTRELINE[i], CENTRELINE[i + 1]
        seg = max(_ARC[i + 1] - _ARC[i], 1e-9)
        t = (target - _ARC[i]) / seg
        cx, cy = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
        tx, ty = (b[0] - a[0]) / seg, (b[1] - a[1]) / seg
        nx, ny = -ty, tx   # left of downstream
        z = profile_z(s)
        for j in range(RIVER_ACROSS + 1):
            v = j / RIVER_ACROSS
            off = (v * 2.0 - 1.0) * WATER_HALF_WIDTH
            verts.append([cx + nx * off, cy + ny * off, z])
            uvs.append([target, v * 2.0 * WATER_HALF_WIDTH])
            normals.append([0.0, 0.0, 1.0])   # WaterSystem recomputes these from the triangles
    faces = []
    row = RIVER_ACROSS + 1
    for k in range(RIVER_ALONG):
        for j in range(RIVER_ACROSS):
            a = k * row + j
            # CCW seen from above (+Z): downstream x left-to-right.
            faces.append([a, a + row, a + row + 1, a + 1])
    write_mesh(OUT_DIR / "river_water.yaml", verts, faces, normals, uvs)


def write_lathe(path, profile, segments=20):
    """Revolves `profile` [(radius, z)] (bottom to top) around +Z into a closed mesh."""
    verts, normals, uvs, faces = [], [], [], []
    rings = len(profile)
    for k, (rad, z) in enumerate(profile):
        # Profile tangent -> outward normal in the (r, z) plane.
        r0, z0 = profile[max(k - 1, 0)]
        r1, z1 = profile[min(k + 1, rings - 1)]
        tr, tz = r1 - r0, z1 - z0
        l = math.hypot(tr, tz) or 1.0
        nr, nz = tz / l, -tr / l
        for i in range(segments + 1):
            a = 2.0 * math.pi * i / segments
            ca, sa = math.cos(a), math.sin(a)
            verts.append([rad * ca, rad * sa, z])
            normals.append([nr * ca, nr * sa, nz])
            uvs.append([i / segments, k / (rings - 1)])
    row = segments + 1
    for k in range(rings - 1):
        for i in range(segments):
            a = k * row + i
            faces.append([a, a + 1, a + row + 1, a + row])
    write_mesh(path, verts, faces, normals, uvs)


def build_props(out_dir=None):
    out_dir = out_dir or PROPS_DIR
    # Low-poly unit sphere (radius 1) for the ball and the river boulder.
    sphere = [(math.sin(math.pi * k / 12), -math.cos(math.pi * k / 12)) for k in range(13)]
    write_lathe(out_dir / "sphere_low.yaml", sphere, 20)
    # Barrel-ish capsule along Z: radius 0.35, total height 1.3 -- matches a CapsuleCollider
    # with radius 0.35, height 1.3, direction 2.
    r, hh = 0.35, 0.65 - 0.35
    cap = [(r * math.sin(math.pi * 0.5 * k / 6), -hh - r * math.cos(math.pi * 0.5 * k / 6)) for k in range(7)]
    cap += [(r * math.cos(math.pi * 0.5 * k / 6), hh + r * math.sin(math.pi * 0.5 * k / 6)) for k in range(7)]
    write_lathe(out_dir / "barrel.yaml", cap, 20)


POOL_DIR = pathlib.Path(__file__).parent.parent / "assets/scenes/underwater_test/meshes"
PROPS_DIR = pathlib.Path(__file__).parent.parent / "assets/meshes"   # shared by both scenes
POOL_EXTENT = 16.0
POOL_CELLS = 64


def pool_height(x, y):
    """A deep, rounded-square pool: a rocky floor ~6 m down, walls rising to a rim above water."""
    r = (abs(x) ** 4 + abs(y) ** 4) ** 0.25
    floor = -6.0 + 0.45 * math.sin(x * 0.7) * math.cos(y * 0.55) + 0.25 * math.sin(x * 1.9 + y * 1.3)
    # two mounds on the floor
    floor = max(floor, -3.4 - 0.35 * ((x - 4.0) ** 2 + (y + 3.0) ** 2))
    floor = max(floor, -4.2 - 0.25 * ((x + 5.0) ** 2 + (y - 4.0) ** 2))
    rim = 1.2
    t = smoothstep(9.5, 13.0, r)
    return floor * (1.0 - t) + rim * t


def build_pool():
    n = POOL_CELLS
    step = 2.0 * POOL_EXTENT / n
    heights = [[pool_height(-POOL_EXTENT + i * step, -POOL_EXTENT + j * step) for i in range(n + 1)] for j in range(n + 1)]
    verts, uvs = [], []
    for j in range(n + 1):
        for i in range(n + 1):
            verts.append([-POOL_EXTENT + i * step, -POOL_EXTENT + j * step, heights[j][i]])
            uvs.append([i / n * 6.0, j / n * 6.0])
    normals = grid_normals(heights, n, step)
    faces = []
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            faces.append([a, a + 1, a + n + 2, a + n + 1])
    write_mesh(POOL_DIR / "pool_basin.yaml", verts, faces, normals, uvs)
    write_mesh(POOL_DIR / "pool_basin_collider.yaml", verts, faces, collider=True)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    POOL_DIR.mkdir(parents=True, exist_ok=True)
    print(f"river length {RIVER_LENGTH:.2f} m")
    build_basin()
    build_river()
    build_props()
    build_pool()


if __name__ == "__main__":
    main()
