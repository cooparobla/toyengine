#!/usr/bin/env python3
"""Generates the hair_demo's root mask.

A Hair component's `root_mask` is sampled by the SCALP'S UV, on the CPU, as the groom is
scattered: the red channel thins the hair (by rejection, so the distribution stays even) and the
green channel shortens it. This writes a mask with a bald band and a short-fur band, so the demo
shows both channels doing something rather than describing them.

Run: python3 tools/gen_hair_mask.py
"""

import math
import os
import struct
import zlib

SIZE = 256
OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "textures", "hair", "fur_mask.png")


def mask_pixel(u: float, v: float) -> tuple:
    """Red = density, green = length. Blue and alpha are unused and kept at full."""
    # A diagonal band the hair thins out across, with soft edges -- a hard edge would read as a
    # cut rather than as fur thinning.
    band = abs(math.sin((u * 2.2 + v * 1.4) * math.pi))
    density = min(1.0, max(0.0, (band - 0.18) / 0.45))
    # Shorter fur toward the top of the UV, so the two channels are visibly independent.
    length = 0.35 + 0.65 * v
    return (int(density * 255), int(min(1.0, length) * 255), 255, 255)


def write_png(path: str, width: int, height: int, rows: list) -> None:
    raw = b"".join(b"\x00" + bytes(row) for row in rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 9)) +
           chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(png)


def main() -> None:
    rows = []
    for y in range(SIZE):
        row = bytearray()
        for x in range(SIZE):
            r, g, b, a = mask_pixel(x / (SIZE - 1), y / (SIZE - 1))
            row += bytes((r, g, b, a))
        rows.append(row)
    write_png(OUT, SIZE, SIZE, rows)
    print("wrote", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
