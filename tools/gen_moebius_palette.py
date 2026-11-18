"""Generates assets/palettes/moebius.png, a 27x1 palette strip for the moebius_demo scene.

Standalone generator, not part of the C++ build -- run it manually whenever the palette needs
regenerating:

    python3 tools/gen_moebius_palette.py

Entry 0 is the ink colour (the 8-bit form of render.outline_color's default
[0.05, 0.04, 0.08]) and entry 1 is a true paper white -- both exact, so a hatched region
quantizes to {ink, paper} as a flat spot-colour stipple instead of drifting toward whatever
palette entry happens to be nearest. The rest are a warm backbone, a set of cool neutrals, and
four muted hue families -- see the COLORS comment for the two constraints that fixes.

Standard library only (zlib + struct), since system python has no numpy/Pillow -- see
tools/gen_prototype_grid.py's docstring for the same convention.
"""

import pathlib
import struct
import zlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "assets" / "palettes" / "moebius.png"

# Muted, but WIDE: Giraud's palettes are low-saturation and still carry a full circle of hues
# -- dusty blues, sage, mauve, terracotta -- rather than one warm ramp.
#
# TWO constraints pull against each other here, and stylize.frag's quantizer (nearest entry by
# plain RGB distance, see quantize_to_palette) is what makes them sharp:
#
#   1. Add too FEW hues and every material snaps onto the warm ramp -- the scene reads as one
#      sand tone no matter what its materials say.
#   2. Add hues too close to the NEUTRAL AXIS and the opposite happens: a pale lilac at
#      saturation 0.11 sits nearer a grey than the warm ramp does, so neutral surfaces and a
#      cream sky get hijacked into lilac and mint. A quantizer has no notion of "this pixel
#      wanted to be grey" -- only distance.
#
# So: the warm ramp stays the backbone and absorbs everything neutral, and every chromatic
# entry is held at saturation >= ~0.30 so it only wins for a pixel genuinely of that hue. Each
# hue family then needs its own light/mid/dark, because a sage in shadow must have a sage to
# land on or it falls back to brown.
#
# Verify after editing -- the two properties are mechanical, so check them rather than eyeball:
#   * sweep neutral greys 240..30; each must land on an entry with saturation < 0.30.
#     Sweep COOL greys (b slightly > r) too, not just r==g==b: this scene's ambient is sky-lit,
#     so its "neutrals" lean cool, and an all-warm backbone leaves them genuinely nearer the
#     teal than the ochre. That is what put a teal tone-ramp on screen.
#   * sweep each material albedo at full/half/0.3 light; each must land in its own hue family
# The deepest tier is EXPECTED to collapse onto the violet-black: unified darks are the look,
# not a failure.
COLORS = [
    # Ink and paper, exact: a hatched region must quantize to {ink, paper} as a flat
    # spot-colour stipple rather than drifting onto whatever entry happens to be nearest.
    (0x0D, 0x0D, 0x14),  # 0  ink -- matches render.outline_color's default exactly
    (0xF2, 0xEE, 0xDF),  # 1  paper
    # Warm backbone -- ground, sky, and anything the scene means as warm-neutral.
    (0xE6, 0xDC, 0xC4),  # 2  light paper
    (0xC9, 0xB8, 0x94),  # 3  sand
    (0xA8, 0x90, 0x78),  # 4  ochre
    (0x7A, 0x6A, 0x66),  # 5  warm mid
    (0x4E, 0x45, 0x56),  # 6  violet shade
    (0x2A, 0x26, 0x34),  # 7  deep violet shade -- where every darkest tone unifies
    # Cool neutrals. Not decorative: this scene's ambient is sky-lit, so its "neutrals" come
    # out with blue slightly above red, and against an all-warm backbone the nearest entry to
    # one is the TEAL. These four close that gap at the values greys actually occupy.
    (0xB4, 0xB6, 0xB8),  # 8  cool neutral pale
    (0x96, 0x95, 0x91),  # 9  cool neutral light
    (0x6B, 0x6E, 0x70),  # 10 cool neutral mid
    (0x3E, 0x39, 0x3A),  # 11 neutral dark
    # Blues -- sky, distance, and the cool side of a warm-lit shadow.
    (0x8F, 0xB4, 0xCE),  # 12 light blue
    (0x4E, 0x7C, 0xA8),  # 13 dusty blue
    (0x38, 0x42, 0x50),  # 14 blue-grey shade
    (0x2F, 0x4A, 0x66),  # 15 slate shadow
    # Teals.
    (0x6F, 0xA8, 0xA0),  # 16 teal
    (0x2F, 0x5E, 0x57),  # 17 deep teal
    # Greens -- vegetation needs its own ramp or foliage reads as rock.
    (0xA3, 0xBC, 0x78),  # 18 light sage
    (0x6E, 0x8F, 0x45),  # 19 sage
    (0x34, 0x48, 0x2A),  # 20 deep green
    # Mauves -- Giraud's favourite shadow family on warm ground.
    (0x9E, 0x7A, 0xA8),  # 21 mauve
    (0x5A, 0x3B, 0x5E),  # 22 plum
    # Warm accents.
    (0xE2, 0xA0, 0x6E),  # 23 apricot
    (0xC9, 0x6A, 0x52),  # 24 salmon -- the one deliberately saturated entry
    (0x8A, 0x5A, 0x3C),  # 25 rust
    (0x4A, 0x34, 0x28),  # 26 deep rust -- clay in shadow, which neutrals would otherwise take
]


def write_png(path: pathlib.Path, pixels: list) -> None:
    """Writes a strip of opaque RGB pixels as a width x 1 PNG."""
    width = len(pixels)
    raw = bytearray()
    raw.append(0)  # filter type: none
    for r, g, b in pixels:
        raw += bytes((r, g, b))

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, 1, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + \
        chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


if __name__ == "__main__":
    write_png(OUT, COLORS)
    print(f"wrote {OUT} ({len(COLORS)}x1)")
