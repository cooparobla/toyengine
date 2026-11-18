"""Generates assets/luts/moebius.png, a 32x32x32 colour-grading LUT for the moebius_demo scene.

Standalone generator, not part of the C++ build -- run it manually whenever the grade needs
regenerating:

    python3 tools/gen_moebius_lut.py

## Why a LUT rather than a palette

A palette (render.palette) QUANTIZES: every pixel snaps to its nearest entry, so a smooth
region is cut into bands, and the boundary between two bands sweeps across the screen whenever
the image's brightness shifts -- which is every time the camera moves, since auto-exposure
re-meters. That is the "background colour that grows and shrinks as I move" artefact, and it
is inherent to quantization, not a tuning problem: more entries make the bands finer but never
make the boundaries stop moving.

A LUT REMAPS instead. Every input colour maps to its own output colour, so nothing bands and
nothing sweeps, the full 8-bit range survives, and the cost is one texture fetch regardless of
how rich the grade is (a palette costs a linear scan over every entry, per pixel).

Giraud's flatness comes from flat FILLS and linework -- toon banding, the ink contour and the
crosshatch -- not from a small colour count. Those stay on; this file only decides the hues.

## The grade

Operating on DISPLAY-REFERRED colour (stylize.frag applies this after the tonemap and sRGB
encode, before the ink stages), in this order:

  1. a gentle S-curve, for the printed-ink contrast that separates a flat fill from its neighbour
  2. a saturation lift, since ACES has already pulled the midtones toward neutral
  3. split-toning: shadows toward violet-blue, highlights toward warm cream -- the single most
     Moebius move there is, and what keeps a grey from ever reading as a dead grey
  4. a small hue push of greens toward teal, away from videogame green

Standard library only (zlib + struct), since system python has no numpy/Pillow -- see
tools/gen_prototype_grid.py's docstring for the same convention.
"""

import colorsys
import pathlib
import struct
import zlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "assets" / "luts" / "moebius.png"

SIZE = 32                 # cube side; the strip is SIZE*SIZE wide by SIZE tall
CONTRAST = 0.18           # S-curve strength
SATURATION = 1.18           # midtone saturation multiplier
SHADOW_TINT = (0.44, 0.42, 0.62)    # violet-blue
HIGHLIGHT_TINT = (1.00, 0.95, 0.84)  # warm cream
TINT_STRENGTH = 0.22


def s_curve(x: float) -> float:
    """Smooth contrast around 0.5; CONTRAST 0 is the identity."""
    return x + CONTRAST * (x - 0.5) * (1.0 - abs(x - 0.5) * 2.0)


def grade(r: float, g: float, b: float) -> tuple:
    """Maps one display-referred RGB triple through the grade described in the module docstring."""
    r, g, b = (min(1.0, max(0.0, s_curve(c))) for c in (r, g, b))

    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    # Greens drift toward teal. Narrow and gentle: a wide push would swing the warm ground too.
    if 0.20 < h < 0.42:
        h += 0.035 * (1.0 - abs(h - 0.31) / 0.11)
    # Saturation is lifted most in the midtones, where the tonemap flattened it, and least at
    # the ends -- boosting a near-black or a near-white only produces a colour cast.
    midtone = 1.0 - abs(v - 0.5) * 2.0
    s = min(1.0, s * (1.0 + (SATURATION - 1.0) * midtone))
    r, g, b = colorsys.hsv_to_rgb(h, s, v)

    # Split-tone: lerp toward the shadow tint at the dark end and the highlight tint at the
    # bright end, weighted so the midtones -- where the subject lives -- stay closest to true.
    luma = 0.2126 * r + 0.7152 * g + 0.0722 * b
    shadow_w = TINT_STRENGTH * (1.0 - luma) ** 2
    highlight_w = TINT_STRENGTH * luma ** 2
    out = []
    for c, sh, hi in zip((r, g, b), SHADOW_TINT, HIGHLIGHT_TINT):
        c = c * (1.0 - shadow_w) + (c * sh) * shadow_w
        c = c * (1.0 - highlight_w) + (c * hi + (1.0 - c) * (hi - 0.84) * 0.5) * highlight_w
        out.append(min(1.0, max(0.0, c)))
    return tuple(out)


def build_strip(size: int) -> list:
    """Builds the size*size by size strip: slice z runs left to right, x across, y down."""
    rows = []
    for y in range(size):          # green
        row = []
        for z in range(size):      # blue -- one slice per z, laid out along the strip
            for x in range(size):  # red
                r, g, b = grade(x / (size - 1), y / (size - 1), z / (size - 1))
                row.append((round(r * 255), round(g * 255), round(b * 255)))
        rows.append(row)
    return rows


def write_png(path: pathlib.Path, rows: list) -> None:
    """Writes a list of equal-length RGB rows as an opaque PNG."""
    width, height = len(rows[0]), len(rows)
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type: none
        for r, g, b in row:
            raw += bytes((r, g, b))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + \
        chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


if __name__ == "__main__":
    strip = build_strip(SIZE)
    write_png(OUT, strip)
    print(f"wrote {OUT} ({len(strip[0])}x{len(strip)}, cube side {SIZE})")
