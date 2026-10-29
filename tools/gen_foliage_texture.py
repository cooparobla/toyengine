"""Generates the leaf-card texture graphs/curve_tree instances.

A foliage CARD is the standard way trees are made cheap: one quad carrying a photo-ish cutout of
a leaf cluster, alpha-tested so its silhouette is the leaves rather than the quad. A few hundred
cards read as a canopy at a fraction of the triangles real leaves would cost.

Two files, because the engine takes the mask separately from the colour (`texture_albedo` and
`texture_alpha_mask`): the mask is a scalar and must never be colour-managed as sRGB.

Both are written **RGBA, with coverage in the ALPHA channel**. The shaders sample
`texture(u_alpha_mask, uv).a` (see gfx/surface/shadow_fs.glsl and gbuffer_fs.glsl), so a
greyscale PNG -- which has no alpha channel at all -- reads as 1.0 everywhere and cuts nothing
out: the card renders as a full opaque quad with leaves painted on it.
"""

import math
import pathlib
import random

from PIL import Image, ImageDraw, ImageFilter

OUT = pathlib.Path(__file__).resolve().parent.parent / "assets" / "textures" / "foliage"
SIZE = 256


def leaf(draw: ImageDraw.ImageDraw, cx: float, cy: float, length: float, angle: float,
         fill) -> None:
    """One pointed leaf: two arcs meeting at a tip, drawn as a polygon."""
    pts = []
    for side in (1.0, -1.0):
        steps = 10
        for i in range(steps + 1):
            t = i / steps
            # Width swells in the middle and pinches at both ends -- a leaf, not an ellipse.
            w = math.sin(t * math.pi) ** 0.8 * length * 0.26 * side
            along = (t - 0.5) * length
            x = cx + along * math.cos(angle) - w * math.sin(angle)
            y = cy + along * math.sin(angle) + w * math.cos(angle)
            pts.append((x, y))
    draw.polygon(pts, fill=fill)


def build(seed: int = 7):
    rng = random.Random(seed)
    colour = Image.new("RGB", (SIZE, SIZE), (58, 92, 44))
    mask = Image.new("L", (SIZE, SIZE), 0)
    cd, md = ImageDraw.Draw(colour), ImageDraw.Draw(mask)

    # A cluster: leaves fanned around the lower-centre, so the card has a stem end to attach by
    # and spreads away from it. That is what makes a card look attached rather than floating.
    root = (SIZE * 0.5, SIZE * 0.92)
    for i in range(34):
        spread = rng.uniform(-1.15, 1.15)
        reach = rng.uniform(0.42, 0.95)
        angle = -math.pi / 2 + spread
        dist = SIZE * 0.42 * reach
        cx = root[0] + math.cos(angle) * dist
        cy = root[1] + math.sin(angle) * dist
        length = SIZE * rng.uniform(0.20, 0.34)
        # Lighter toward the outside, so the card has depth without a normal map.
        shade = int(120 + 110 * reach + rng.uniform(-18, 18))
        cd_fill = (int(shade * 0.50), min(255, shade), int(shade * 0.36))
        leaf(cd, cx, cy, length, angle + rng.uniform(-0.5, 0.5), cd_fill)
        leaf(md, cx, cy, length, angle + rng.uniform(-0.5, 0.5), 255)

    # A soft edge on the mask alone: the alpha test then cuts a slightly ragged outline rather
    # than a polygon-crisp one, which is what stops a card reading as a flat sheet.
    mask = mask.filter(ImageFilter.GaussianBlur(1.2))
    colour = colour.filter(ImageFilter.GaussianBlur(0.6))
    return colour, mask


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    colour, mask = build()

    # Coverage goes in ALPHA, in both files. The albedo carries it too so the material works
    # whether the engine reads albedo.a, the mask, or multiplies the two.
    albedo = colour.convert("RGBA")
    albedo.putalpha(mask)
    albedo.save(OUT / "leaf_card.png")

    mask_rgba = Image.new("RGBA", (SIZE, SIZE), (255, 255, 255, 0))
    mask_rgba.putalpha(mask)
    mask_rgba.save(OUT / "leaf_card_mask.png")

    opaque = sum(1 for p in mask.getdata() if p > 115) / (SIZE * SIZE)
    print(f"wrote {OUT/'leaf_card.png'} and its mask ({SIZE}x{SIZE}, "
          f"{opaque:.0%} opaque at the 0.45 cutoff)")
