#!/usr/bin/env python3
"""Generates the character-cloth demo's skirt: assets/meshes/animation/jacket.yaml.

A closed tube around the mannequin's hips: **fitted** at the waist, where it rides the skinning
exactly, then **flaring** below, where it simulates and is pushed around by the legs as they
swing. That split is the point of the demo -- a garment is not a sheet, and the interesting part
of clothing is the boundary between the part that is worn and the part that moves.

**Why it starts at the waist and not the collar.** The mannequin's arms hang against its sides:
measured over the walk cycle, the nearest arm surface to the body plane sits at |x| 0.175 m from
z 1.16 upward, while the chest it would have to cover is 0.20 m wide there. No closed tube fits
between the two, so a garment drawn over that chest is a garment the upper arms are modelled
INSIDE -- which is what the first version of this mesh did, and it read on screen as both arms
hanging through the fabric with every collision test still passing. Below z 1.08 the gap opens to
20-30 mm and a tube fits with room to spare.

Every ring here is sized from the body the engine actually draws (tools/gen_mannequin.py's
PART_MESHES after each shape's scale), with a margin on both sides: wide enough to clear the
torso, narrow enough to clear the hanging arms. A garment that starts inside the body is one the
solver spends every frame pushing out, and the max-distance leash fights it the whole way.

Run from the engine root:

    uv run --project python tools/gen_jacket_mesh.py
"""

import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "python" / "src"))

from toyproject.assets.mesh import Mesh  # noqa: E402

#: Vertices around the tube, and rings from collar to hem.
COLUMNS = 24
ROWS = 15

#: The garment's profile: (z, radius_x, radius_y) from the waist down.
#:
#: The x radii carry the tight constraint. Between z 0.76 and 0.96 the hands sweep past at
#: |x| 0.218-0.238 while the pelvis is 0.20 wide, so each ring there has under 2 cm of room on
#: each side -- which is also why this garment runs a thinner `thickness` than a loose sheet
#: would (see the scene). Below z 0.72 the hands are clear and the skirt is free to flare.
PROFILE = [
    (1.08, 0.165, 0.128),   # waist -- worn, and the highest ring that clears the arms
    (1.02, 0.180, 0.134),
    (0.96, 0.212, 0.142),   # hip; the body is 0.199 x 0.103 here, nearest hand 0.281
    (0.90, 0.214, 0.148),   # the tightest ring in x: body 0.200, nearest hand 0.218
    (0.84, 0.212, 0.156),
    (0.78, 0.208, 0.172),   # the legs start swinging wide behind this
    (0.72, 0.224, 0.196),   # the hands end here...
    (0.66, 0.248, 0.222),   # ...so the skirt flares below them
    (0.60, 0.268, 0.244),
    (0.54, 0.285, 0.262),
    (0.48, 0.298, 0.276),
    (0.42, 0.308, 0.287),
    (0.36, 0.316, 0.295),
    (0.30, 0.321, 0.301),
    (0.24, 0.324, 0.305),   # hem, low shin
]
assert len(PROFILE) == ROWS

#: Below this height the garment simulates; above it, it is WORN -- written from the skinned
#: pose every substep, so it sits exactly where this file put it.
#:
#: 0.86 rather than the waist, and that is forced by the body. From the hip up to the waist the
#: mannequin's hands sweep past within about 10 mm of a ring that already has only 8 mm over the
#: pelvis: an 18 mm slot, less than two fabric thicknesses. Fabric given freedom to move inside
#: it eventually ends up on the wrong side of a forearm, and once it is there collision -- doing
#: its job -- pushes it further out. Painted worn, those rings cannot move at all, and the arms
#: sweep past outside them by construction. Below the hands there is room and the skirt is free.
FITTED_UNTIL_Z = 0.86
HEM_MAX_DISTANCE = 0.30
#: The skirt's backstop and drive; the worn hips get neither.
SKIRT_BACKSTOP_RADIUS = 0.08
SKIRT_BACKSTOP_DISTANCE = 0.01
SKIRT_ANIM_DRIVE = 0.25


def ring_weights(z):
    """The bone weights for a ring at height `z`.

    Blended across the joints rather than switched, so the fitted part deforms smoothly instead
    of creasing at a bone boundary.

    Args:
        z (float): Ring height in metres.

    Returns:
        dict: Bone name -> weight, summing to 1.
    """
    def mix(a, b, t):
        return {a: round(1.0 - t, 4), b: round(t, 4)} if 0.0 < t < 1.0 else ({a: 1.0} if t <= 0.0 else {b: 1.0})

    if z >= 1.25:
        return {"chest": 1.0}
    if z >= 1.05:
        return mix("spine", "chest", (z - 1.05) / 0.20)
    if z >= 0.95:
        return mix("pelvis", "spine", (z - 0.95) / 0.10)
    return {"pelvis": 1.0}


def channels_at(z):
    """The cloth paint channels for a ring at height `z`.

    Returns:
        tuple: (max_distance, backstop_distance, backstop_radius, anim_drive).
    """
    if z >= FITTED_UNTIL_Z:
        return (0.0, 0.0, 0.0, 0.0)          # worn: rides the skinning exactly
    # LINEAR from the hip down, not eased in. A skin-driven vertex (max_distance 0) is given
    # inv_mass 0 and skips collision entirely, so a quadratic ramp -- which leaves the first free
    # rings at a tenth of a millimetre of leash -- makes exactly the rings a swinging thigh
    # reaches uncollidable. Every ring the body can reach needs room to be pushed.
    t = min(1.0, max(0.0, (FITTED_UNTIL_Z - z) / (FITTED_UNTIL_Z - PROFILE[-1][0])))
    return (
        round(HEM_MAX_DISTANCE * t, 5),
        SKIRT_BACKSTOP_DISTANCE,
        round(SKIRT_BACKSTOP_RADIUS * t, 5),
        round(SKIRT_ANIM_DRIVE * t, 5),
    )


def build():
    """Builds the jacket mesh with its vertex groups and cloth channels.

    Returns:
        Mesh: The jacket, ready to write.
    """
    verts, uvs, normals, groups = [], [], [], []
    cloth = {name: [] for name in ("max_distance", "backstop_distance", "backstop_radius",
                                   "anim_drive")}
    for r, (z, rx, ry) in enumerate(PROFILE):
        weights = ring_weights(z)
        md, bd, br, ad = channels_at(z)
        for c in range(COLUMNS):
            a = 2.0 * math.pi * c / COLUMNS
            x, y = rx * math.sin(a), -ry * math.cos(a)   # a = 0 is the BACK (-y), as a cape would be
            verts.append([round(x, 5), round(y, 5), z])
            uvs.append([c / COLUMNS, r / (ROWS - 1)])
            # Outward, from the tube's axis: the ellipse's own gradient, not the radial.
            nx, ny = math.sin(a) / rx, -math.cos(a) / ry
            n = math.hypot(nx, ny)
            normals.append([round(nx / n, 5), round(ny / n, 5), 0.0])
            groups.append(dict(weights))
            cloth["max_distance"].append(md)
            cloth["backstop_distance"].append(bd)
            cloth["backstop_radius"].append(br)
            cloth["anim_drive"].append(ad)

    # Quads around and down, wrapping in the column direction so the tube is CLOSED -- a seam of
    # duplicated vertices would open under simulation, which is exactly where a garment must not.
    faces = []
    for r in range(ROWS - 1):
        for c in range(COLUMNS):
            c2 = (c + 1) % COLUMNS
            i00 = r * COLUMNS + c
            i10 = r * COLUMNS + c2
            i01 = (r + 1) * COLUMNS + c
            i11 = (r + 1) * COLUMNS + c2
            faces.append([i00, i01, i11, i10])   # outward-facing

    tangents = [[0.0, 0.0, 1.0, 1.0]] * len(verts)
    return Mesh(verts, faces, uvs, normals=normals, tangents=tangents, groups=groups, cloth=cloth)


def main():
    out = pathlib.Path(__file__).resolve().parent.parent / "assets" / "meshes" / "animation" / "jacket.yaml"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(build().to_text())
    print(f"wrote {out} ({COLUMNS} around x {ROWS} rings = {COLUMNS * ROWS} vertices)")


if __name__ == "__main__":
    main()
