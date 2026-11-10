#!/usr/bin/env python3
"""Generates uicoopa's gamepad button-prompt sprite sheet: assets/icons/prompts.png
and its sidecar descriptor assets/icons/prompts.yaml.

Sibling script to gen_default_cursors.py/gen_default_icons.py, deliberately
self-contained (its own copy of the handful of geometry primitives it needs)
rather than importing from either -- see gen_default_icons.py's doc for the
reasoning this mirrors: each generator stays an independent, reproducible unit.

Python 3 standard library only. Same convention as the icon/cursor sheets:
each glyph is a boolean coverage function evaluated at cell-local pixel
coordinates (0..CELL, +Y down, matching image space), rendered white-RGB /
coverage-alpha with 4x4 supersampling so a theme can tint any glyph at draw
time, with a transparent gutter around the glyph to avoid bilinear edge bleed.

A separate sheet from icons.png/cursors.png (not merged into either) so
neither of those files' own byte-for-byte reproducibility or descriptor test
is touched by prompt-glyph changes.

Re-running this script must reproduce assets/icons/prompts.png byte-for-byte;
uicoopa's headless test suite (tests/sprite_sheet_test.cpp's
shipped_sheet_descriptors_are_in_bounds_and_disjoint) treats the checked-in
prompts.yaml as the source of truth for what the sheet should contain.

Usage: python3 tools/gen_button_prompts.py
"""

import math
import os
import struct
import zlib

CELL = 32
SUPERSAMPLE = 4
C = CELL / 2.0  # cell center, both axes

PROMPT_LAYOUT = [
    ["prompt_a", "prompt_b", "prompt_x", "prompt_y", "prompt_l", "prompt_r"],
    ["prompt_dpad", "prompt_dpad_h", "prompt_dpad_v", "prompt_start", "prompt_select", "prompt_stick"],
]

# ---------------------------------------------------------------------------
# Geometry primitives -- copied from gen_default_icons.py/gen_default_cursors.py.
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


def in_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) <= r


# ---------------------------------------------------------------------------
# Per-prompt coverage functions. Letterforms are deliberately built from
# straight strokes/rects only (no curves) -- readable at 32px, and consistent
# with the boxy, schematic look every other generated glyph in this repo has.
# ---------------------------------------------------------------------------

_STROKE = 4.0


def _prompt_a(x, y):
    return (in_stroke(x, y, [(9, 25), (16, 7), (23, 25)], _STROKE) or
            in_stroke(x, y, [(12, 18), (20, 18)], _STROKE))


def _prompt_b(x, y):
    return (in_rect(x, y, 9, 7, 12, 25) or       # spine
            in_rect(x, y, 9, 7, 20, 10) or        # top bar
            in_rect(x, y, 17, 7, 20, 15) or       # top loop right edge
            in_rect(x, y, 9, 14, 20, 17) or       # middle bar
            in_rect(x, y, 17, 17, 20, 25) or      # bottom loop right edge
            in_rect(x, y, 9, 22, 20, 25))         # bottom bar


def _prompt_x(x, y):
    return (in_stroke(x, y, [(9, 7), (23, 25)], _STROKE) or
            in_stroke(x, y, [(23, 7), (9, 25)], _STROKE))


def _prompt_y(x, y):
    return (in_stroke(x, y, [(9, 7), (16, 16)], _STROKE) or
            in_stroke(x, y, [(23, 7), (16, 16)], _STROKE) or
            in_stroke(x, y, [(16, 16), (16, 25)], _STROKE))


def _prompt_l(x, y):
    return (in_rect(x, y, 9, 7, 12, 25) or        # spine
            in_rect(x, y, 9, 22, 21, 25))          # bottom bar


def _prompt_r(x, y):
    return (in_rect(x, y, 9, 7, 12, 25) or        # spine
            in_rect(x, y, 9, 7, 20, 10) or         # top bar
            in_rect(x, y, 17, 7, 20, 15) or        # top loop right edge
            in_rect(x, y, 9, 14, 20, 17) or        # middle bar
            in_stroke(x, y, [(12, 17), (22, 25)], _STROKE))  # diagonal leg


def _prompt_dpad(x, y):
    # A plus/cross, matching a physical d-pad's silhouette.
    return in_rect(x, y, 13, 7, 19, 25) or in_rect(x, y, 7, 13, 25, 19)


def _prompt_dpad_h(x, y):
    shaft = in_stroke(x, y, [(7, C), (25, C)], 3.0)
    left_arrow = point_in_polygon(x, y, [(7, C), (13, C - 5), (13, C + 5)])
    right_arrow = point_in_polygon(x, y, [(25, C), (19, C - 5), (19, C + 5)])
    return shaft or left_arrow or right_arrow


def _prompt_dpad_v(x, y):
    shaft = in_stroke(x, y, [(C, 7), (C, 25)], 3.0)
    top_arrow = point_in_polygon(x, y, [(C, 7), (C - 5, 13), (C + 5, 13)])
    bottom_arrow = point_in_polygon(x, y, [(C, 25), (C - 5, 19), (C + 5, 19)])
    return shaft or top_arrow or bottom_arrow


def _prompt_start(x, y):
    # A single thick pill/bar -- distinct from select's thinner dash below.
    return in_rect(x, y, 9, 14, 23, 18)


def _prompt_select(x, y):
    # A single thin dash -- distinct from start's thicker pill above.
    return in_rect(x, y, 10, 15, 22, 17)


def _prompt_stick(x, y):
    # A thumbstick: a filled cap inside its gate ring.
    return in_ring(x, y, C, C, 11.0, 2.5) or in_circle(x, y, C, C, 5.0)


PROMPT_FUNCS = {
    "prompt_a": _prompt_a,
    "prompt_b": _prompt_b,
    "prompt_x": _prompt_x,
    "prompt_y": _prompt_y,
    "prompt_l": _prompt_l,
    "prompt_r": _prompt_r,
    "prompt_dpad": _prompt_dpad,
    "prompt_dpad_h": _prompt_dpad_h,
    "prompt_dpad_v": _prompt_dpad_v,
    "prompt_start": _prompt_start,
    "prompt_select": _prompt_select,
    "prompt_stick": _prompt_stick,
}


# ---------------------------------------------------------------------------
# Rasterization + PNG encoding -- identical to gen_default_cursors.py.
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
    cols = max(len(row) for row in PROMPT_LAYOUT)
    rows = len(PROMPT_LAYOUT)
    width, height = cols * CELL, rows * CELL

    pixels = bytearray(width * height * 4)
    entries = []

    for row_idx, row in enumerate(PROMPT_LAYOUT):
        for col_idx, name in enumerate(row):
            cell_x, cell_y = col_idx * CELL, row_idx * CELL
            alpha = rasterize_icon(PROMPT_FUNCS[name])
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

    png_path = os.path.join(out_dir, "prompts.png")
    write_png(png_path, width, height, pixels)

    yaml_lines = [
        "# Generated by tools/gen_button_prompts.py -- do not hand-edit; edit the",
        "# generator and re-run instead so the sheet and descriptor stay in sync.",
        "image: prompts.png",
        f"width: {width}",
        f"height: {height}",
        "sprites:",
    ]
    for name, x, y, w, h in entries:
        yaml_lines.append(f"  - {{ name: {name}, x: {x}, y: {y}, w: {w}, h: {h} }}")
    yaml_path = os.path.join(out_dir, "prompts.yaml")
    with open(yaml_path, "w") as f:
        f.write("\n".join(yaml_lines) + "\n")

    print(f"Wrote {width}x{height} {png_path} ({len(entries)} prompts)")
    print(f"Wrote {yaml_path}")


if __name__ == "__main__":
    main()
