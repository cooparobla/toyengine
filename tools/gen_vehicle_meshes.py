"""Generates the low-poly vehicle meshes the `vehicle_demo` scene drives.

Every body is a soup of flat-shaded hexahedra -- boxes, and boxes with their eight corners moved
independently, which is enough for a tapered nose, a sloped bonnet or a bed wall. Flat shading is
deliberate: these read as blocky machines at the demo's top-down camera distance, and per-face
normals keep the silhouette crisp where area-weighted vertex normals would round every corner into
mush.

Wheels are a single UNIT wheel -- axle along X, spanning [-0.5, 0.5], radius 1 in YZ -- that each
prefab scales to its own width and radius, so there is one wheel mesh rather than one per vehicle.

Run from the repository root:

    python3 tools/gen_vehicle_meshes.py
"""

import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))

from mesh_yaml import perpendicular_tangents, write_mesh

OUT_DIR = pathlib.Path(__file__).parent.parent / "assets" / "meshes" / "vehicles"

# The six quads of a hexahedron, as indices into the eight corners below, wound so that every
# face's normal points outward (verified against the cross product of its first two edges).
HEXA_FACES = [
    [0, 3, 2, 1],  # -Z
    [4, 5, 6, 7],  # +Z
    [0, 1, 5, 4],  # -Y
    [1, 2, 6, 5],  # +X
    [2, 3, 7, 6],  # +Y
    [3, 0, 4, 7],  # -X
]


class Soup:
    """Accumulates flat-shaded polygons, duplicating vertices so each face keeps its own normal.

    Attributes:
        verts (list): Vertex positions.
        faces (list): Polygons as index lists.
        uvs (list): One UV per vertex.
        normals (list): One normal per vertex, constant across each face.
    """

    def __init__(self):
        self.verts = []
        self.faces = []
        self.uvs = []
        self.normals = []

    def add_face(self, corners):
        """Appends one polygon with a flat normal from its first two edges.

        Args:
            corners (list): Vertex positions in winding order (3 or more).
        """
        base = len(self.verts)
        a, b, c = corners[0], corners[1], corners[2]
        u = [b[i] - a[i] for i in range(3)]
        v = [c[i] - b[i] for i in range(3)]
        n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
        length = math.sqrt(sum(t * t for t in n)) or 1.0
        n = [t / length for t in n]

        # Box-ish UVs: the two axes the face varies most along, normalised over the face.
        spans = [max(p[i] for p in corners) - min(p[i] for p in corners) for i in range(3)]
        drop = spans.index(min(spans))
        axes = [i for i in range(3) if i != drop]
        lo = [min(p[axes[k]] for p in corners) for k in range(2)]
        size = [max(spans[axes[k]], 1e-6) for k in range(2)]

        for p in corners:
            self.verts.append(list(p))
            self.normals.append(list(n))
            self.uvs.append([(p[axes[0]] - lo[0]) / size[0], (p[axes[1]] - lo[1]) / size[1], 0.0])
        self.faces.append(list(range(base, base + len(corners))))

    def add_hexa(self, corners):
        """Appends a six-faced solid from its eight corners (bottom 0-3 CCW, top 4-7 CCW)."""
        for face in HEXA_FACES:
            self.add_face([corners[i] for i in face])

    def add_box(self, center, half, taper_top=None, shift_top=(0.0, 0.0)):
        """Appends an axis-aligned box, optionally with a differently sized or offset top face.

        Args:
            center (tuple): Box centre (x, y, z).
            half (tuple): Half extents.
            taper_top (tuple): Top face half extents in x and y; the bottom's when omitted.
            shift_top (tuple): Top face offset in x and y -- a sloped nose or a leaning cab.
        """
        cx, cy, cz = center
        hx, hy, hz = half
        tx, ty = taper_top if taper_top else (hx, hy)
        sx, sy = shift_top
        bottom = [
            (cx - hx, cy - hy, cz - hz),
            (cx + hx, cy - hy, cz - hz),
            (cx + hx, cy + hy, cz - hz),
            (cx - hx, cy + hy, cz - hz),
        ]
        top = [
            (cx - tx + sx, cy - ty + sy, cz + hz),
            (cx + tx + sx, cy - ty + sy, cz + hz),
            (cx + tx + sx, cy + ty + sy, cz + hz),
            (cx - tx + sx, cy + ty + sy, cz + hz),
        ]
        self.add_hexa(bottom + top)

    def write(self, name):
        """Writes the soup to assets/meshes/vehicles/<name>.yaml."""
        path = OUT_DIR / f"{name}.yaml"
        write_mesh(path, self.verts, self.faces, self.uvs, self.normals,
                   perpendicular_tangents(self.normals))
        print(f"  {path.relative_to(OUT_DIR.parent.parent.parent)}  "
              f"({len(self.verts)} verts, {len(self.faces)} faces)")


def unit_wheel(segments=16, tread_depth=0.06):
    """A unit wheel: axle along X in [-0.5, 0.5], radius 1 in YZ, with a treaded rim.

    The rim alternates between full radius and `tread_depth` less, so the tyre catches the light
    as it rolls -- at this scale that read of rotation matters far more than a round profile.

    Args:
        segments (int): Rim segments (even, so the tread alternates cleanly).
        tread_depth (float): How far the recessed tread segments sit inside the radius.

    Returns:
        Soup: The wheel.
    """
    s = Soup()
    ring = []
    for i in range(segments):
        a = 2.0 * math.pi * i / segments
        r = 1.0 if i % 2 == 0 else 1.0 - tread_depth
        ring.append((math.cos(a) * r, math.sin(a) * r))

    for i in range(segments):
        y0, z0 = ring[i]
        y1, z1 = ring[(i + 1) % segments]
        # Tread band, wound outward.
        s.add_face([(-0.5, y0, z0), (-0.5, y1, z1), (0.5, y1, z1), (0.5, y0, z0)])
        # The two sidewalls, each a fan triangle back to its hub centre.
        s.add_face([(0.5, y0, z0), (0.5, y1, z1), (0.5, 0.0, 0.0)])
        s.add_face([(-0.5, y1, z1), (-0.5, y0, z0), (-0.5, 0.0, 0.0)])
    return s


def sports_car():
    """A low wedge: tapered nose, cabin bump, visible sills."""
    s = Soup()
    s.add_box((0.0, 0.0, 0.0), (0.88, 2.05, 0.26), taper_top=(0.80, 1.95))      # main tub
    s.add_box((0.0, 1.35, 0.26), (0.80, 0.70, 0.16), taper_top=(0.62, 0.52),
              shift_top=(0.0, 0.14))                                            # bonnet slope
    s.add_box((0.0, -0.25, 0.26), (0.78, 0.95, 0.40), taper_top=(0.56, 0.62))   # cabin
    s.add_box((0.0, -1.55, 0.26), (0.84, 0.50, 0.14), taper_top=(0.78, 0.44))   # rear deck
    return s


def pickup():
    """Cab plus an open bed with low walls."""
    s = Soup()
    s.add_box((0.0, 0.0, 0.0), (0.95, 2.40, 0.34))                              # chassis slab
    s.add_box((0.0, 0.85, 0.34), (0.90, 1.05, 0.56), taper_top=(0.76, 0.86))    # cab
    s.add_box((0.0, 2.05, 0.20), (0.92, 0.36, 0.26), taper_top=(0.86, 0.30))    # bonnet
    for x, hx in ((-0.84, 0.11), (0.84, 0.11)):                                 # bed walls
        s.add_box((x, -1.30, 0.34), (hx, 1.05, 0.28))
    s.add_box((0.0, -2.30, 0.34), (0.95, 0.11, 0.28))                           # tailgate
    return s


def hauler_cab():
    """The 6x6's forward-control cab, over the front axle."""
    s = Soup()
    s.add_box((0.0, 0.0, 0.0), (1.18, 1.30, 0.86), taper_top=(1.02, 1.18))
    s.add_box((0.0, -0.10, 0.86), (1.00, 1.10, 0.16), taper_top=(0.88, 0.96))   # roof
    s.add_box((0.0, 1.36, -0.30), (1.14, 0.12, 0.44))                           # bumper
    return s


def hauler_bed():
    """The 6x6's dump body: a deep tub with flared walls."""
    s = Soup()
    s.add_box((0.0, 0.0, 0.0), (1.22, 2.30, 0.20))                              # floor
    for x in (-1.14, 1.14):
        s.add_box((x, 0.0, 0.20), (0.09, 2.30, 0.62), shift_top=(x * 0.06, 0.0))
    s.add_box((0.0, 2.22, 0.20), (1.22, 0.09, 0.70))                            # headboard
    s.add_box((0.0, -2.22, 0.20), (1.22, 0.09, 0.48))                           # tailboard
    return s


def buggy():
    """A seat tub with a roll hoop -- mostly air, which is the point."""
    s = Soup()
    s.add_box((0.0, 0.0, 0.0), (0.86, 1.55, 0.20))                              # floor pan
    s.add_box((0.0, -0.45, 0.20), (0.74, 0.55, 0.34), taper_top=(0.60, 0.46))   # seat tub
    s.add_box((0.0, 1.25, 0.08), (0.46, 0.34, 0.18))                            # nose cone
    for x in (-0.70, 0.70):                                                     # hoop uprights
        s.add_box((x, -0.70, 0.54), (0.07, 0.07, 0.52))
    s.add_box((0.0, -0.70, 1.06), (0.77, 0.07, 0.07))                           # hoop crossbar
    s.add_box((0.0, -0.05, 0.86), (0.07, 0.72, 0.07))                           # hoop brace
    return s


def dozer_body():
    """A boxy tracked hull with an offset cab and an exhaust stack."""
    s = Soup()
    s.add_box((0.0, 0.0, 0.0), (1.00, 2.00, 0.52))                              # hull
    s.add_box((0.0, -0.55, 0.52), (0.78, 0.86, 0.72), taper_top=(0.66, 0.74))   # cab
    s.add_box((0.0, -0.55, 1.24), (0.84, 0.92, 0.08))                           # cab roof
    s.add_box((0.46, 1.05, 0.52), (0.16, 0.16, 0.44))                           # stack
    for x in (-1.16, 1.16):                                                     # track skirts
        s.add_box((x, 0.0, -0.18), (0.18, 2.10, 0.34))
    return s


def dozer_blade():
    """A curved blade, approximated by three leaning plates and two push arms."""
    s = Soup()
    s.add_box((0.0, 0.10, -0.10), (1.55, 0.12, 0.42), shift_top=(0.0, -0.16))   # mouldboard
    s.add_box((0.0, -0.02, 0.32), (1.55, 0.10, 0.22), shift_top=(0.0, -0.10))   # top fold
    s.add_box((0.0, 0.22, -0.48), (1.55, 0.14, 0.10))                           # cutting edge
    for x in (-0.95, 0.95):
        s.add_box((x, -0.62, -0.18), (0.11, 0.62, 0.11))                        # push arm
    return s


def trailer_body():
    """A flat tipper deck with low sides and a drawbar reaching forward to the hitch."""
    s = Soup()
    s.add_box((0.0, 0.0, 0.0), (1.05, 1.60, 0.18))                              # deck
    for x in (-0.97, 0.97):
        s.add_box((x, 0.0, 0.18), (0.08, 1.60, 0.40))
    s.add_box((0.0, -1.52, 0.18), (1.05, 0.08, 0.40))                           # tailboard
    s.add_box((0.0, 1.52, 0.18), (1.05, 0.08, 0.30))                            # headboard
    s.add_box((0.0, 2.05, -0.06), (0.12, 0.48, 0.10))                           # drawbar
    return s


def main():
    """Writes every vehicle mesh."""
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    print("vehicle meshes:")
    unit_wheel().write("wheel")
    unit_wheel(segments=20, tread_depth=0.10).write("wheel_heavy")
    unit_wheel(segments=12, tread_depth=0.14).write("track_roller")
    sports_car().write("sports_car_body")
    pickup().write("pickup_body")
    hauler_cab().write("hauler_cab")
    hauler_bed().write("hauler_bed")
    buggy().write("buggy_frame")
    dozer_body().write("dozer_body")
    dozer_blade().write("dozer_blade")
    trailer_body().write("trailer_body")


if __name__ == "__main__":
    main()
