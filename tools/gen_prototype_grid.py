"""Generates assets/textures/prototype/prototype_grid.png, the blockout grid behind materials/prototype_grid.yaml.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_prototype_grid.py`) whenever the texture needs regenerating. Standard library only.

One tile is one metre at the material's default tiling (the `triplanar` shader projects it in
world space, so every surface reads its true size): a soft two-tone checker of quarter-metre
cells, thin quarter-metre lines, and a heavier line on the metre boundary. The boundary line is
one texel on each edge, so two tiles side by side meet in a two-texel line.
"""

import pathlib
import struct
import zlib

SIZE = 128
"""Output width/height in texels (one metre at tiling 1)."""

CELLS = 4
"""Checker cells / minor-line divisions per tile edge."""

CELL_LIGHT = (140, 142, 146)
CELL_DARK = (124, 126, 130)
MINOR_LINE = (104, 106, 110)
MAJOR_LINE = (62, 64, 68)

OUTPUT_PATH = pathlib.Path(__file__).resolve().parent.parent / "assets" / "textures" / "prototype" / "prototype_grid.png"


def texel(x: int, y: int) -> tuple:
    """sRGB colour of texel (x, y)."""
    if x in (0, SIZE - 1) or y in (0, SIZE - 1):
        return MAJOR_LINE
    cell = SIZE // CELLS
    if x % cell == 0 or y % cell == 0:
        return MINOR_LINE
    return CELL_LIGHT if (x // cell + y // cell) % 2 == 0 else CELL_DARK


def write_png(path: pathlib.Path, width: int, height: int, rows: list) -> None:
    """Writes 8-bit RGBA rows (each a bytes object of width * 4) as a PNG."""
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + row for row in rows)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def main() -> None:
    rows = [b"".join(bytes(texel(x, y) + (255,)) for x in range(SIZE)) for y in range(SIZE)]
    write_png(OUTPUT_PATH, SIZE, SIZE, rows)
    print(f"Wrote {OUTPUT_PATH} ({SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
