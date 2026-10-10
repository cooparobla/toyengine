"""Generates the styled terrain tile pieces: assets/meshes/terrain/<style>/tile_<style>_<piece>.yaml.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_tile_styles.py`) whenever the styled tile set needs regenerating. Deterministic:
re-running reproduces every file byte for byte.

A styled terrain (toyengine/world/tile_topology.h) assembles each tile from a few authored
**pieces**, chosen and oriented per tile by a neighbourhood classifier. This file writes four
styles of the eleven pieces:

  round -- grass, dirt, moss, clay: a generous plan-view radius and a soft round-over lip.
  soft  -- sand, snow, ash, salt: rounder still; the lip melts into the slope.
  rock  -- stone, rock: tight, firm corners and a short lip.
  flat  -- water, ice: square, i.e. the voxel shape expressed as pieces.

Every piece is authored in ONE canonical frame inside the unit cell [0,1]^3 (Z up) and the
engine bakes it into the eight D4 orientations (tile_types.h's variant_transform()):

  top_inner / top_edge / top_outer
      The +X+Y quadrant [0.5,1]^2 of a tile's top, at z = 1. `top_edge` has its +Y edge exposed
      (inset by LIP_H, where the wall's round-over takes over); `top_outer` has both edges
      exposed and its corner rounded to the convex rim.
  wall_cap_* / wall_*  (continue, convex, concave)
      The right half (x in [0.5,1]) of the +Y wall face. The plan-view profile runs from
      (0.5, 1) to the end state at x = 1: straight on; round a convex corner centred
      (1-R_OUT, 1-R_OUT) to the 45-degree bisector; or fillet a concave corner centred
      (1-R_IN, 1+R_IN) -- material added into the LOWER cell -- to its bisector. Plain walls are
      vertical extrusions of that profile from z = 0 to 1. Caps (the topmost wall cell) stop at
      z = 1 - LIP_V and round over, a quarter ellipse, to the top inset by LIP_H; the concave
      cap also carries the flat top over its half of the fillet.
  wall_cap_concave_taper
      The taller wall's half of an inner corner, at the cell where the other wall's plateau
      ends: plain along its own face (that cliff keeps rising), with a lip along the fillet arc
      that eases in from nothing at the face to full at the corner's midline, meeting the
      other wall's wall_cap_concave there.

### The authoring contract (what the engine relies on)

* Every vertex that lies on a cell boundary plane -- z = 0, z = 1, x = 0.5, x = 1, a corner
  bisector -- lies on it exactly, and adjacent pieces agree on the rim lines (y = 1 - LIP_H and
  the rim arcs). The engine derives **section caps** from a piece's boundary edges on those
  planes, and they only close the seams between different styles if the edges are where they
  claim to be. Jitter therefore moves interior vertices only.
* The concave lip must not reach past the corner it fillets: LIP_H <= (sqrt(2) - 1) * R_IN.
  Past that, the round-over would dip under the neighbouring tops at the corner.
* Proportions assume a cubic cell. The engine keeps a cap's upper half (z >= 0.5) at tile
  proportions on taller steps, so LIP_V <= 0.5.

UVs written here are placeholders: styled chunks carry only their kind in the UV (see
assets/shaders/terrain_styled.frag, which draws the surface detail procedurally in world space).
"""

import math
import pathlib

from mesh_yaml import face_normal, write_mesh

# Pieces go in assets/meshes/terrain/<style>/ (tags `terrain` and the style; referenced by name).
OUTPUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "assets" / "meshes" / "terrain"
"""Where the generated piece meshes are written; resolved relative to this repo, not the caller's cwd."""

OBJECTS_DIR = OUTPUT_DIR.parent.parent / "objects" / "terrain"
"""Where each style's tile-set object asset is written (see write_tile_set_object())."""

# The tile-set object's layout: one row per tier, pieces in TilePiece order, spaced so each
# unit cell reads on its own. (piece suffix, row, column)
TILE_SET_LAYOUT = [
    ("top_inner", 0, 0), ("top_edge", 0, 1), ("top_outer", 0, 2),
    ("wall_cap_continue", 1, 0), ("wall_cap_convex", 1, 1), ("wall_cap_concave", 1, 2),
    ("wall_cap_concave_taper", 1, 3), ("wall_cap_continue_taper", 1, 4),
    ("wall_continue", 2, 0), ("wall_convex", 2, 1), ("wall_concave", 2, 2),
]
TILE_SET_SPACING = 1.75
TILE_SET_COLOURS = {0: (0.45, 0.72, 0.30), 1: (0.62, 0.43, 0.28), 2: (0.55, 0.56, 0.60)}
"""Display colours per row: turf for tops, soil for the lipped caps, stone for plain walls."""

STYLES = {
    # R_OUT: convex plan radius; R_IN: concave fillet radius; LIP_H / LIP_V: the round-over's
    # horizontal inset and vertical drop; ARC_SEG: segments per 45 degrees of corner;
    # LIP_SEG: segments across the round-over (1 = a chamfer); JITTER: facet displacement of
    # interior wall vertices; FLAT_SHADED: per-face normals.
    "round": dict(R_OUT=0.40, R_IN=0.45, LIP_H=0.18, LIP_V=0.30, ARC_SEG=2, LIP_SEG=3,
                  JITTER=0.0, FLAT_SHADED=False),
    "soft": dict(R_OUT=0.48, R_IN=0.48, LIP_H=0.19, LIP_V=0.42, ARC_SEG=3, LIP_SEG=4,
                 JITTER=0.0, FLAT_SHADED=False),
    "rock": dict(R_OUT=0.14, R_IN=0.16, LIP_H=0.06, LIP_V=0.10, ARC_SEG=2, LIP_SEG=2,
                 JITTER=0.0, FLAT_SHADED=False),
    "flat": dict(R_OUT=0.0, R_IN=0.0, LIP_H=0.0, LIP_V=0.0, ARC_SEG=1, LIP_SEG=1,
                 JITTER=0.0, FLAT_SHADED=True),
}
"""One entry per style; the file prefix is `tile_<name>`."""

EPS = 1e-9


class Mesh:
    """An indexed polygon mesh with explicit per-vertex normals, deduplicating vertices."""

    def __init__(self):
        self.verts = []
        self.normals = []
        self.faces = []
        self._index = {}

    def vertex(self, p, n):
        """Returns the index of vertex `p` with normal `n`, adding it if new."""
        key = tuple(round(c, 7) for c in p) + tuple(round(c, 4) for c in n)
        if key not in self._index:
            self._index[key] = len(self.verts)
            self.verts.append([float(c) for c in p])
            self.normals.append(normalise(n))
        return self._index[key]

    def polygon(self, points, normals, facing):
        """Adds one polygon, wound so its geometric normal agrees with `facing`.

        Degenerate (zero-area) polygons are dropped; duplicate consecutive points collapse.
        """
        ids = []
        for p, n in zip(points, normals):
            i = self.vertex(p, n)
            if not ids or ids[-1] != i:
                ids.append(i)
        if len(ids) > 1 and ids[0] == ids[-1]:
            ids.pop()
        if len(ids) < 3:
            return
        fn = face_normal(self.verts, ids) if len(ids) == 3 else newell(self.verts, ids)
        if math.sqrt(sum(c * c for c in fn)) < 1e-10:
            return
        if sum(fn[i] * facing[i] for i in range(3)) < 0:
            ids.reverse()
        self.faces.append(ids)

    def write(self, path, flat_shaded):
        """Writes the mesh; flat-shaded meshes get one vertex per face corner with the face normal."""
        verts, normals, faces = self.verts, self.normals, self.faces
        if flat_shaded:
            verts, normals, faces = [], [], []
            for face in self.faces:
                fn = normalise(newell(self.verts, face))
                ids = []
                for i in face:
                    ids.append(len(verts))
                    verts.append(self.verts[i])
                    normals.append(fn)
                faces.append(ids)
        uvs = [[v[0], v[1]] for v in verts]
        write_mesh(path, verts, faces, uvs, normals)


def normalise(v):
    """Returns `v` scaled to unit length (or +Z for a zero vector)."""
    length = math.sqrt(sum(c * c for c in v))
    return [c / length for c in v] if length > EPS else [0.0, 0.0, 1.0]


def newell(verts, face):
    """The Newell normal of a polygon -- robust for n-gons and near-degenerate fans."""
    n = [0.0, 0.0, 0.0]
    for k in range(len(face)):
        a = verts[face[k]]
        b = verts[face[(k + 1) % len(face)]]
        n[0] += (a[1] - b[1]) * (a[2] + b[2])
        n[1] += (a[2] - b[2]) * (a[0] + b[0])
        n[2] += (a[0] - b[0]) * (a[1] + b[1])
    return n


def jitter(seed_a, seed_b, amplitude):
    """A deterministic pseudo-random offset in [-amplitude, amplitude]."""
    h = math.sin(seed_a * 127.1 + seed_b * 311.7) * 43758.5453
    return (2.0 * (h - math.floor(h)) - 1.0) * amplitude


# ---------------------------------------------------------------------------------------------
# Plan-view profiles
# ---------------------------------------------------------------------------------------------

def profile(end, st):
    """The wall's plan-view profile for one end state, from (0.5, 1) to the end.

    Returns:
        list: (x, y, nx, ny, interior) per sample -- position, outward horizontal normal, and
        whether the sample is strictly inside the piece (eligible for jitter).
    """
    radius = {"continue": 0.0, "convex": st["R_OUT"], "concave": st["R_IN"]}[end]
    arc_seg = st["ARC_SEG"]
    x_end = 1.0 - radius
    out = [(0.5, 1.0, 0.0, 1.0, False)]
    if st["JITTER"] > 0.0 and x_end - 0.75 > 0.05:
        out.append((0.75, 1.0, 0.0, 1.0, True))
    if x_end > 0.5 + EPS:
        out.append((x_end, 1.0, 0.0, 1.0, radius > EPS))
    if radius <= EPS:
        out[-1] = (out[-1][0], out[-1][1], out[-1][2], out[-1][3], False)
        return out
    for i in range(1, arc_seg + 1):
        last = i == arc_seg
        if end == "convex":
            theta = math.radians(90.0 - 45.0 * i / arc_seg)
            cx, cy = 1.0 - radius, 1.0 - radius
            c, s = math.cos(theta), math.sin(theta)
            out.append((cx + radius * c, cy + radius * s, c, s, not last))
        else:
            theta = math.radians(-90.0 + 45.0 * i / arc_seg)
            cx, cy = 1.0 - radius, 1.0 + radius
            c, s = math.cos(theta), math.sin(theta)
            out.append((cx + radius * c, cy + radius * s, -c, -s, not last))
    return out


def levels(cap, st):
    """The wall's cross-section from bottom to top: (inset, z, horizontal weight, vertical weight).

    A plain wall is a vertical extrusion. A cap stops at z = 1 - LIP_V and rounds over -- a
    quarter ellipse with radii LIP_H (in) and LIP_V (up) -- to the top, inset by LIP_H.
    """
    lip_h, lip_v = st["LIP_H"], st["LIP_V"]
    rounded = cap and lip_h > EPS and lip_v > EPS
    wall_top = 1.0 - lip_v if rounded else 1.0
    out = [(0.0, 0.0, 1.0, 0.0, False)]
    if st["JITTER"] > 0.0:
        out.append((0.0, 0.5 * wall_top, 1.0, 0.0, True))
    out.append((0.0, wall_top, 1.0, 0.0, False))
    if rounded:
        for k in range(1, st["LIP_SEG"] + 1):
            phi = 0.5 * math.pi * k / st["LIP_SEG"]
            out.append((lip_h * (1.0 - math.cos(phi)), wall_top + lip_v * math.sin(phi),
                        math.cos(phi) / lip_h, math.sin(phi) / lip_v, False))
    return out


# ---------------------------------------------------------------------------------------------
# Pieces
# ---------------------------------------------------------------------------------------------

def build_wall(end, cap, st):
    """One wall half: the profile swept through the cross-section, plus the concave cap's top."""
    mesh = Mesh()
    prof = profile(end, st)
    lev = levels(cap, st)

    grid = []
    for (px, py, nx, ny, interior_p) in prof:
        column = []
        for (inset, z, wh, wv, interior_z) in lev:
            offset = inset
            if interior_p and interior_z and st["JITTER"] > 0.0:
                # Interior vertices only: boundary ones must stay on their planes.
                offset += jitter(px + 3.1 * py, z, st["JITTER"])
            p = (px - nx * offset, py - ny * offset, z)
            n = normalise([nx * wh, ny * wh, wv]) if wv > 0.0 else [nx, ny, 0.0]
            column.append((p, n))
        grid.append(column)

    for i in range(len(grid) - 1):
        for j in range(len(lev) - 1):
            a, b = grid[i][j], grid[i + 1][j]
            c, d = grid[i + 1][j + 1], grid[i][j + 1]
            facing = [a[1][k] + b[1][k] + c[1][k] + d[1][k] for k in range(3)]
            mesh.polygon([a[0], b[0], c[0], d[0]], [a[1], b[1], c[1], d[1]], facing)

    if cap and end == "concave" and st["R_IN"] > EPS:
        # The flat top over this half of the fillet, out to the corner (1, 1): bounded by the
        # top_edge quadrant's rim line, the rim arc, the bisector and the cell boundary x = 1.
        # Fanned from (1, 1 - LIP_H), from which the whole region is visible.
        lip_h, r = st["LIP_H"], st["R_IN"]
        up = [0.0, 0.0, 1.0]
        pivot = (1.0, 1.0 - lip_h, 1.0)
        rim = []
        for i in range(0, st["ARC_SEG"] + 1):
            theta = math.radians(-90.0 + 45.0 * i / st["ARC_SEG"])
            rim.append((1.0 - r + (r + lip_h) * math.cos(theta), 1.0 + r + (r + lip_h) * math.sin(theta), 1.0))
        rim.append((1.0, 1.0, 1.0))
        for a, b in zip(rim, rim[1:]):
            mesh.polygon([pivot, a, b], [up, up, up], up)
    return mesh


def build_wall_taper(st):
    """The taller wall's half of an inner corner where the other wall's plateau ends.

    The concave wall half, but lipped only along its fillet arc: the lip scales from nothing
    where the arc leaves the wall's own face (that cliff keeps rising above) to full at the
    corner's midline, where it meets the lower wall's wall_cap_concave. The scale eases in
    cubically, which keeps the rim from ever crossing the face plane y = 1 into the taller
    column; rim points are clamped to it as a guard. The fill covers this half of the fillet's
    top, out to the corner.
    """
    mesh = Mesh()
    prof = profile("concave", st)
    lip_h, lip_v, r = st["LIP_H"], st["LIP_V"], st["R_IN"]
    n_arc = st["ARC_SEG"] if r > EPS else 0
    n_straight = len(prof) - n_arc
    up = [0.0, 0.0, 1.0]

    def scale_at(index):
        if index < n_straight:
            return 0.0
        return ((index - n_straight + 1) / n_arc) ** 3

    grid = []
    for index, (px, py, nx, ny, _interior) in enumerate(prof):
        t = scale_at(index)
        lh, lv = lip_h * t, lip_v * t
        column = [((px, py, 0.0), [nx, ny, 0.0]), ((px, py, 1.0 - lv), [nx, ny, 0.0])]
        for k in range(1, st["LIP_SEG"] + 1):
            phi = 0.5 * math.pi * k / st["LIP_SEG"]
            off = lh * (1.0 - math.cos(phi))
            q = (px - nx * off, max(py - ny * off, 1.0), 1.0 - lv + lv * math.sin(phi))
            n = normalise([nx * math.cos(phi) / max(lip_h, EPS), ny * math.cos(phi) / max(lip_h, EPS),
                           math.sin(phi) / max(lip_v, EPS)]) if t > 0.0 else [nx, ny, 0.0]
            column.append((q, n))
        grid.append(column)

    rows = len(grid[0])
    for i in range(len(grid) - 1):
        for j in range(rows - 1):
            a, b = grid[i][j], grid[i + 1][j]
            c, d = grid[i + 1][j + 1], grid[i][j + 1]
            facing = [a[1][k] + b[1][k] + c[1][k] + d[1][k] for k in range(3)]
            mesh.polygon([a[0], b[0], c[0], d[0]], [a[1], b[1], c[1], d[1]], facing)

    if n_arc:
        # The fill: from the corner (1, 1), fanned over the (tapered) rim, starting where the
        # arc leaves the face and ending on the midline.
        pivot = (1.0, 1.0, 1.0)
        rim = [column[-1][0] for column in grid[n_straight - 1:]]
        for a, b in zip(rim, rim[1:]):
            mesh.polygon([pivot, a, b], [up, up, up], up)
    return mesh


def build_wall_continue_taper(st):
    """The lower wall's cap half where its straight run ends against a taller neighbour.

    That neighbour's wall is plain at this height and coplanar with this one, so a full lip
    would end in a step -- the lip's cut-out left standing against the flat face. Here the lip
    scales from full at x = 0.5 (matching the straight cap before it) to nothing at x = 1,
    eased with a smoothstep so it leaves both ends level. Its end profile is then the plain
    wall's square, and the strip of top the lip no longer covers -- top_edge stops at
    y = 1 - LIP_H -- is filled flat out to the tapered rim.
    """
    mesh = Mesh()
    lip_h, lip_v = st["LIP_H"], st["LIP_V"]
    up = [0.0, 0.0, 1.0]
    face = [0.0, 1.0, 0.0]
    if lip_h <= EPS or lip_v <= EPS:
        return build_wall("continue", True, st)
    samples = 4
    grid = []
    for i in range(samples + 1):
        x = 0.5 + 0.5 * i / samples
        s = i / samples
        t = 1.0 - s * s * (3.0 - 2.0 * s)
        lh, lv = lip_h * t, lip_v * t
        column = [((x, 1.0, 0.0), face), ((x, 1.0, 1.0 - lv), face)]
        for k in range(1, st["LIP_SEG"] + 1):
            phi = 0.5 * math.pi * k / st["LIP_SEG"]
            q = (x, 1.0 - lh * (1.0 - math.cos(phi)), 1.0 - lv + lv * math.sin(phi))
            # The full lip's normal, faded toward the face's as the lip thins to an edge.
            lip_n = normalise([0.0, math.cos(phi) / lip_h, math.sin(phi) / lip_v])
            n = normalise([0.0, (1.0 - t) + t * lip_n[1], t * lip_n[2]])
            column.append((q, n))
        grid.append(column)

    rows = len(grid[0])
    for i in range(len(grid) - 1):
        for j in range(rows - 1):
            a, b = grid[i][j], grid[i + 1][j]
            c, d = grid[i + 1][j + 1], grid[i][j + 1]
            facing = [a[1][k] + b[1][k] + c[1][k] + d[1][k] for k in range(3)]
            mesh.polygon([a[0], b[0], c[0], d[0]], [a[1], b[1], c[1], d[1]], facing)

    # The fill: from top_edge's rim line y = 1 - LIP_H out to the tapered rim.
    for col_a, col_b in zip(grid, grid[1:]):
        ra, rb = col_a[-1][0], col_b[-1][0]
        mesh.polygon([(ra[0], 1.0 - lip_h, 1.0), (rb[0], 1.0 - lip_h, 1.0), rb, ra], [up] * 4, up)
    return mesh


def build_top(piece, st):
    """One top quadrant at z = 1, fanned from the cell centre (0.5, 0.5)."""
    mesh = Mesh()
    up = [0.0, 0.0, 1.0]
    lip_h, r = st["LIP_H"], st["R_OUT"]
    centre = (0.5, 0.5, 1.0)
    if piece == "top_inner":
        outline = [(1.0, 0.5, 1.0), (1.0, 1.0, 1.0), (0.5, 1.0, 1.0)]
    elif piece == "top_edge":
        outline = [(1.0, 0.5, 1.0), (1.0, 1.0 - lip_h, 1.0), (0.5, 1.0 - lip_h, 1.0)]
    else:
        # The rim the two convex cap halves end on: radius R_OUT - LIP_H about the corner
        # centre, sampled at exactly their points (ARC_SEG per 45 degrees).
        rim_r = r - lip_h
        outline = [(1.0 - lip_h, 0.5, 1.0)]
        segments = 2 * st["ARC_SEG"]
        for i in range(segments + 1):
            theta = math.radians(90.0 * i / segments)
            outline.append((1.0 - r + rim_r * math.cos(theta), 1.0 - r + rim_r * math.sin(theta), 1.0))
        outline.append((0.5, 1.0 - lip_h, 1.0))
    for a, b in zip(outline, outline[1:]):
        mesh.polygon([centre, a, b], [up, up, up], up)
    return mesh


def write_tile_set_object(name, prefix):
    """Writes objects/tileset_<name>.yaml: the style as one object, a child per piece.

    This is the style as the editor shows it -- toyengine's Objects tab lists it, opening it
    lays every piece out on a grid, and Tab on a piece edits that piece's mesh. It is also what
    a Terrain's `styles:` names (`round: objects/tileset_round`): the terrain finds each piece by
    its child's NAME (the piece suffix) and uses that child's mesh. Duplicating a tile set in
    the editor copies its piece meshes too, so the copy is a new look to edit freely.
    """
    lines = [
        f"# tileset_{name}: the `{name}` terrain tile style -- every piece a styled terrain is built from,",
        "# one child each, named by piece (see toyengine/world/tile_topology.h's TilePiece). A Terrain",
        f"# uses it with `styles: {{ {name}: objects/tileset_{name} }}`; duplicate it in the editor for a new",
        "# look. GENERATED by tools/gen_tile_styles.py; edit that, or a duplicate, not this file.",
        "object:",
        f"  name: tileset_{name}",
        "  tile_set: true",
        "  components:",
        "    - type: Transform",
        "      position: {x: 0, y: 0, z: 0}",
        "  children:",
    ]
    for piece, row, col in TILE_SET_LAYOUT:
        r, g, b = TILE_SET_COLOURS[row]
        lines += [
            f"    - name: {piece}",
            "      components:",
            "        - type: Transform",
            # Turned half a turn so each piece's outward faces (authored toward +Y) face a
            # camera looking from -Y -- the pieces are single-sided -- and shifted back onto
            # its grid cell.
            f"          position: {{x: {col * TILE_SET_SPACING + 1.0:.2f}, y: {-row * TILE_SET_SPACING + 1.0:.2f}, z: 0}}",
            "          rotation: {x: 0, y: 0, z: 180}",
            "        - type: MeshRenderer",
            f"          mesh_path: {prefix}_{piece}",
            f"          material: {{albedo: {{r: {r}, g: {g}, b: {b}}}, metallic: 0.0, roughness: 0.85}}",
            "      children: []",
        ]
    OBJECTS_DIR.mkdir(parents=True, exist_ok=True)
    path = OBJECTS_DIR / f"tileset_{name}.yaml"
    path.write_text("\n".join(lines) + "\n")
    print(f"wrote {path}")


def main():
    """Writes every style's eleven pieces into OUTPUT_DIR and its tile-set object into OBJECTS_DIR."""
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, st in STYLES.items():
        assert st["R_OUT"] <= 0.5 and st["R_IN"] <= 0.5, name
        assert st["LIP_H"] <= (math.sqrt(2.0) - 1.0) * st["R_IN"] + 1e-9, f"{name}: lip overshoots the fillet"
        assert st["R_OUT"] >= st["LIP_H"], f"{name}: convex radius smaller than the lip"
        assert st["LIP_V"] <= 0.5, name
        prefix = f"tile_{name}"
        flat = st["FLAT_SHADED"]
        out = OUTPUT_DIR / name
        out.mkdir(parents=True, exist_ok=True)
        for piece in ("top_inner", "top_edge", "top_outer"):
            build_top(piece, st).write(out / f"{prefix}_{piece}.yaml", flat)
        for cap in (True, False):
            for end in ("continue", "convex", "concave"):
                piece = f"wall_cap_{end}" if cap else f"wall_{end}"
                build_wall(end, cap, st).write(out / f"{prefix}_{piece}.yaml", flat)
        build_wall_taper(st).write(out / f"{prefix}_wall_cap_concave_taper.yaml", flat)
        build_wall_continue_taper(st).write(out / f"{prefix}_wall_cap_continue_taper.yaml", flat)
        write_tile_set_object(name, prefix)


if __name__ == "__main__":
    main()
