#!/usr/bin/env python3
"""Generates uicoopa's default cursor sprite sheet: assets/icons/cursors.png and its
sidecar descriptor assets/icons/cursors.yaml.

Sibling script to gen_default_icons.py, deliberately self-contained (its own copy
of the handful of geometry primitives it needs) rather than importing from that
script, so each generator stays an independent, reproducible unit -- see that
file's own doc for the reasoning this mirrors.

Python 3 standard library only. Same convention as the icon sheet: each cursor is
a boolean coverage function evaluated at cell-local pixel coordinates (0..CELL,
+Y down, matching image space), rendered white-RGB / coverage-alpha with 4x4
supersampling so UITheme::CursorStyle::color can tint any cursor at draw time,
with a transparent gutter around the glyph to avoid bilinear edge bleed (see
gen_default_icons.py's doc for the full rationale -- identical here).

These are a separate sheet from icons.png/icons.yaml (not merged into it) so
that file's own byte-for-byte reproducibility and its descriptor test are
untouched by cursor changes.

Re-running this script must reproduce assets/icons/cursors.png byte-for-byte;
uicoopa's headless test suite (tests/sprite_sheet_test.cpp's
shipped_sheet_descriptors_are_in_bounds_and_disjoint) treats the checked-in
cursors.yaml as the source of truth for what the sheet should contain.

Usage: python3 tools/gen_default_cursors.py
"""

import math
import os
import struct
import zlib

CELL = 32
SUPERSAMPLE = 4
C = CELL / 2.0  # cell center, both axes

CURSOR_LAYOUT = [
    ["cursor_default", "cursor_pointer", "cursor_text", "cursor_disabled"],
]

# ---------------------------------------------------------------------------
# Geometry primitives -- copied from gen_default_icons.py; see that file for
# the full set. Only what these four glyphs actually need.
# ---------------------------------------------------------------------------


def point_in_polygon(px, py, poly):
    """Standard ray-casting point-in-polygon test for a simple (non-self-
    intersecting) polygon given as a list of (x, y) vertices."""
    inside = False
    x1, y1 = poly[-1]
    for x2, y2 in poly:
        if (y1 > py) != (y2 > py):
            x_at_py = (x2 - x1) * (py - y1) / (y2 - y1) + x1
            if px < x_at_py:
                inside = not inside
        x1, y1 = x2, y2
    return inside


def dist_to_segment(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    length2 = dx * dx + dy * dy
    if length2 <= 1e-9:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / length2))
    cx, cy = x1 + t * dx, y1 + t * dy
    return math.hypot(px - cx, py - cy)


def dist_to_polyline(px, py, points, closed=False):
    pts = list(points) + ([points[0]] if closed else [])
    best = float("inf")
    for i in range(len(pts) - 1):
        x1, y1 = pts[i]
        x2, y2 = pts[i + 1]
        best = min(best, dist_to_segment(px, py, x1, y1, x2, y2))
    return best


def in_stroke(px, py, points, thickness, closed=False):
    return dist_to_polyline(px, py, points, closed) <= thickness / 2.0


def in_rect(px, py, x0, y0, x1, y1):
    return x0 <= px <= x1 and y0 <= py <= y1


def in_ring(px, py, cx, cy, r, thickness):
    return abs(math.hypot(px - cx, py - cy) - r) <= thickness / 2.0


# ---------------------------------------------------------------------------
# Per-cursor coverage functions.
# ---------------------------------------------------------------------------

# Classic arrow-pointer silhouette: tip near the cell's top-left, angled shaft,
# a notch, and a tail spur -- the same construction as a typical OS arrow
# cursor glyph. Hotspot (the actual click point) is the tip, (6, 5).
_CURSOR_ARROW = [
    (6, 5), (6, 22), (10.5, 18), (14, 25.5), (17, 24), (13.5, 17), (20, 17),
]


def _icon_cursor_default(x, y):
    return point_in_polygon(x, y, _CURSOR_ARROW)


def _icon_cursor_pointer(x, y):
    # Simplified pointing-hand glyph: a raised index finger, a palm/fist base,
    # and a thumb notch to the side -- reads as a hand silhouette rather than
    # a bare column. Hotspot is the fingertip, top-center of the finger rect.
    finger = in_rect(x, y, 14, 6, 18, 18)
    palm = in_rect(x, y, 10, 18, 23, 26)
    thumb = in_rect(x, y, 6, 20, 11, 24)
    return finger or palm or thumb


def _icon_cursor_text(x, y):
    # I-beam: top/bottom serifs plus a vertical stem. Hotspot is the center.
    return (in_rect(x, y, 12, 6, 20, 9) or
            in_rect(x, y, 12, 23, 20, 26) or
            in_rect(x, y, 14.5, 6, 17.5, 26))


def _icon_cursor_disabled(x, y):
    # Not-allowed glyph: a ring with a diagonal slash. Hotspot is the center.
    if in_ring(x, y, C, C, 10.0, 3.0):
        return True
    return in_stroke(x, y, [(9, 9), (23, 23)], 3.0)


CURSOR_FUNCS = {
    "cursor_default": _icon_cursor_default,
    "cursor_pointer": _icon_cursor_pointer,
    "cursor_text": _icon_cursor_text,
    "cursor_disabled": _icon_cursor_disabled,
}


# ---------------------------------------------------------------------------
# Rasterization + PNG encoding -- identical to gen_default_icons.py.
# ---------------------------------------------------------------------------


def rasterize_icon(func):
    """Returns a CELL*CELL bytearray of alpha coverage (0..255), 4x4 supersampled."""
    alpha = bytearray(CELL * CELL)
    samples = SUPERSAMPLE * SUPERSAMPLE
    for ly in range(CELL):
        for lx in range(CELL):
            coverage = 0
            for sy in range(SUPERSAMPLE):
                py = ly + (sy + 0.5) / SUPERSAMPLE
                for sx in range(SUPERSAMPLE):
                    px = lx + (sx + 0.5) / SUPERSAMPLE
                    if func(px, py):
                        coverage += 1
            alpha[ly * CELL + lx] = round(coverage * 255 / samples)
    return alpha


def write_png(path, width, height, rgba):
    """Writes a minimal 8-bit RGBA PNG: signature + IHDR + one IDAT + IEND."""

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    signature = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # color type 6 = RGBA

    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)  # filter type 0 (None) per scanline
        raw.extend(rgba[y * stride:(y + 1) * stride])
    idat = zlib.compress(bytes(raw), 9)

    with open(path, "wb") as f:
        f.write(signature)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def main():
    cols = max(len(row) for row in CURSOR_LAYOUT)
    rows = len(CURSOR_LAYOUT)
    width, height = cols * CELL, rows * CELL

    pixels = bytearray(width * height * 4)
    entries = []

    for row_idx, row in enumerate(CURSOR_LAYOUT):
        for col_idx, name in enumerate(row):
            cell_x, cell_y = col_idx * CELL, row_idx * CELL
            alpha = rasterize_icon(CURSOR_FUNCS[name])
            for ly in range(CELL):
                for lx in range(CELL):
                    idx = ((cell_y + ly) * width + (cell_x + lx)) * 4
                    pixels[idx + 0] = 255
                    pixels[idx + 1] = 255
                    pixels[idx + 2] = 255
                    pixels[idx + 3] = alpha[ly * CELL + lx]
            entries.append((name, cell_x, cell_y, CELL, CELL))

    out_dir = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "assets", "icons")
    os.makedirs(out_dir, exist_ok=True)

    png_path = os.path.join(out_dir, "cursors.png")
    write_png(png_path, width, height, pixels)

    yaml_lines = [
        "# Generated by tools/gen_default_cursors.py -- do not hand-edit; edit the",
        "# generator and re-run instead so the sheet and descriptor stay in sync.",
        "image: cursors.png",
        f"width: {width}",
        f"height: {height}",
        "sprites:",
    ]
    for name, x, y, w, h in entries:
        yaml_lines.append(f"  - {{ name: {name}, x: {x}, y: {y}, w: {w}, h: {h} }}")
    yaml_path = os.path.join(out_dir, "cursors.yaml")
    with open(yaml_path, "w") as f:
        f.write("\n".join(yaml_lines) + "\n")

    print(f"Wrote {width}x{height} {png_path} ({len(entries)} cursors)")
    print(f"Wrote {yaml_path}")


if __name__ == "__main__":
    main()
