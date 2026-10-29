"""Generates the meshes the curve_tree graph instances.

Two assets, both spanning local **+X from 0 to 1** rather than +Z: a curve's point frame puts
+X along the tangent (`toy::curve::frame_from_tangent`), so a limb segment authored along X
follows the path it is instanced on. Authoring it along Z -- which is what a fence post wants --
would stand every segment on end instead.
"""

import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent / "assets" / "meshes" / "props"


def tube(sides: int, r0: float, r1: float, caps: bool = False):
    """An open tube from x=0 (radius r0) to x=1 (radius r1).

    No caps by default, and that is the point. A limb is CHAINED -- each segment's start sits
    where the previous one ended -- so a start cap is a flat disc whose normal points backward
    along the tangent: straight down on an upright trunk, so unlit, and coincident with the
    neighbour it abuts. The result is a dark disc z-fighting the lit tube at every joint, which
    reads as broken dark faces up the trunk. An open tube has no such face to draw.
    """
    verts, norms, uvs, faces = [], [], [], []

    def ring(x, r):
        out = []
        for i in range(sides):
            a = 2.0 * math.pi * i / sides
            out.append((x, r * math.cos(a), r * math.sin(a)))
        return out

    a_ring, b_ring = ring(0.0, r0), ring(1.0, r1)
    for i in range(sides):
        j = (i + 1) % sides
        quad = [a_ring[i], a_ring[j], b_ring[j], b_ring[i]]
        # One normal per side face, from its own geometry: flat shading suits a stylised limb
        # and keeps the asset readable next to the hand-written post beside it.
        ang = 2.0 * math.pi * (i + 0.5) / sides
        n = (0.0, math.cos(ang), math.sin(ang))
        base = len(verts)
        for k, v in enumerate(quad):
            verts.append(v)
            norms.append(n)
            uvs.append([float(k in (1, 2)), float(k in (2, 3))])
        faces.append([base, base + 1, base + 2, base + 3])

    caps_wanted = ((0.0, r0, (-1.0, 0.0, 0.0), True), (1.0, r1, (1.0, 0.0, 0.0), False)) if caps else ()
    for x, r, n, flip in caps_wanted:
        base = len(verts)
        cap = ring(x, r)
        if flip:
            cap = list(reversed(cap))
        for v in cap:
            verts.append(v)
            norms.append(n)
            uvs.append([0.5 + 0.5 * v[1] / max(r, 1e-6), 0.5 + 0.5 * v[2] / max(r, 1e-6)])
        faces.append([base + i for i in range(sides)])

    return verts, norms, uvs, faces


def write(name: str, sides: int, r0: float, r1: float) -> None:
    verts, norms, uvs, faces = tube(sides, r0, r1)
    lines = ["vertices:"]
    lines += [f"  - [{v[0]:.4f}, {v[1]:.4f}, {v[2]:.4f}]" for v in verts]
    lines.append("normals:")
    lines += [f"  - [{n[0]:.4f}, {n[1]:.4f}, {n[2]:.4f}]" for n in norms]
    lines.append("uvs:")
    lines += [f"  - [{u[0]:.4f}, {u[1]:.4f}]" for u in uvs]
    lines.append("faces:")
    lines += ["  - [" + ", ".join(str(i) for i in f) + "]" for f in faces]
    lines.append("colors: []")
    lines.append("weights:")
    lines += ["  - {  }" for _ in verts]
    lines.append("tangents:")
    lines += ["  - [0, 0, 1, 1]" for _ in verts]
    path = ROOT / f"{name}.yaml"
    path.write_text("\n".join(lines) + "\n")
    print(f"{path.name}: {len(verts)} verts, {len(faces)} faces")


def card(planes: int = 3):
    """Crossed quads along +X: a foliage card that reads from any angle.

    Along +X because a curve's point frame puts +X on the tangent, so a card instanced on a
    branch grows ALONG it. One quad would vanish edge-on; three at 60 degrees never do.

    UVs put the texture's stem end (bottom centre of the image) at x=0, so the cluster appears
    to sprout from the branch rather than float beside it.
    """
    verts, norms, uvs, faces = [], [], [], []
    for k in range(planes):
        a = math.pi * k / planes
        # The quad's width axis, swept around +X.
        wx, wy, wz = 0.0, math.cos(a), math.sin(a)
        # Normals on +Z, not perpendicular to the quad and not along the card.
        #
        # Two reasons, and the second is the subtle one. A card is a two-sided sheet
        # (`cull_backfaces: false`), so a FACE normal leaves the back of every quad pointing away
        # from the light, shading black -- half a canopy of dark shards. And Instance on Points
        # spins each card about its local +Z (`random_rotation`), so a normal anywhere else gets
        # swung by that spin and ends up facing the ground on roughly half the cards. +Z is the
        # spin AXIS: it is the one direction the shuffle cannot touch, so every card shades the
        # same however it is turned.
        n = (0.0, 0.0, 1.0)
        quad = [(0.0, -0.5 * wy, -0.5 * wz), (1.0, -0.5 * wy, -0.5 * wz),
                (1.0, 0.5 * wy, 0.5 * wz),   (0.0, 0.5 * wy, 0.5 * wz)]
        quad_uv = [(0.0, 1.0), (0.0, 0.0), (1.0, 0.0), (1.0, 1.0)]
        base = len(verts)
        for v, uv in zip(quad, quad_uv):
            verts.append(v)
            norms.append(n)
            uvs.append(list(uv))
        faces.append([base, base + 1, base + 2, base + 3])
    return verts, norms, uvs, faces


def write_mesh(name: str, data) -> None:
    verts, norms, uvs, faces = data
    lines = ["vertices:"]
    lines += [f"  - [{v[0]:.4f}, {v[1]:.4f}, {v[2]:.4f}]" for v in verts]
    lines.append("normals:")
    lines += [f"  - [{n[0]:.4f}, {n[1]:.4f}, {n[2]:.4f}]" for n in norms]
    lines.append("uvs:")
    lines += [f"  - [{u[0]:.4f}, {u[1]:.4f}]" for u in uvs]
    lines.append("faces:")
    lines += ["  - [" + ", ".join(str(i) for i in f) + "]" for f in faces]
    lines.append("colors: []")
    lines.append("weights:")
    lines += ["  - {  }" for _ in verts]
    lines.append("tangents:")
    lines += ["  - [1, 0, 0, 1]" for _ in verts]
    path = ROOT / f"{name}.yaml"
    path.write_text("\n".join(lines) + "\n")
    print(f"{path.name}: {len(verts)} verts, {len(faces)} faces")


if __name__ == "__main__":
    ROOT.mkdir(parents=True, exist_ok=True)
    # Slightly tapered in itself, so even an untapered run of segments reads as a limb.
    # The trunk: barely tapered in itself, because the CURVE's taper profile does that work and
    # consecutive segments have to meet without a visible step.
    write("procgraph_limb", sides=10, r0=0.5, r1=0.42)
    # A branch or twig: CONICAL, ending in a point. These are not chained -- each is one limb
    # from its parent to its tip -- so a blunt end reads as a snapped-off stick.
    write("procgraph_branch", sides=8, r0=0.5, r1=0.04)
    write_mesh("procgraph_leaf_card", card(planes=3))
