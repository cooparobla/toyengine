#!/usr/bin/env python3
"""Generates uicoopa's default icon sprite sheet: assets/icons/icons.png and its
sidecar descriptor assets/icons/icons.yaml.

Python 3 standard library only -- zlib + struct write the PNG directly, no
Pillow and no build-time dependency. Checked in alongside its output so the
sheet is reproducible and anyone can regenerate or extend it: add an entry to
ICON_LAYOUT, write a coverage function for it, add it to ICON_FUNCS, re-run.

Each icon is a boolean coverage function evaluated at cell-local pixel
coordinates (0..CELL, +Y down, matching image space) and rendered white-RGB /
coverage-alpha with 4x4 supersampling, so uicoopa's Graphic::color can tint
any icon freely at draw time -- see uicoopa/assets/shaders/ui_quad.frag's
`texture(tex, uv) * v_color` path. Icons are drawn well inside their cell
(a several-pixel margin on every side), leaving a transparent gutter -- this
is what keeps UiPass's clamp-to-edge-sampled bilinear filtering from ever
bleeding a neighboring icon into this one's edges, without needing a
half-texel UV inset (see uicoopa/render/sprite_sheet.h's pixel_rect_to_uv()).

Re-running this script must reproduce assets/icons/icons.png byte-for-byte;
uicoopa's headless test suite (tests/sprite_sheet_test.cpp's
shipped_sheet_descriptors_are_in_bounds_and_disjoint) treats the checked-in
icons.yaml as the source of truth for what the sheet should contain.

Usage: python3 tools/gen_default_icons.py
"""

import math
import os
import struct
import zlib

CELL = 32
SUPERSAMPLE = 4
C = CELL / 2.0  # cell center, both axes

ICON_LAYOUT = [
    ["arrow_left", "arrow_right", "arrow_up", "arrow_down",
     "chevron_left", "chevron_right", "chevron_up", "chevron_down"],
    ["caret_up", "caret_down", "check", "cross", "plus", "minus", "dot", "circle"],
    ["gear", "search", "menu", "star", "warning", "info", "lock", "folder"],
]

# ---------------------------------------------------------------------------
# Geometry primitives. Every icon function below takes one point (px, py) in
# cell-local pixel space and returns True if that point is "inside" the glyph.
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


def in_circle(px, py, cx, cy, r):
    return math.hypot(px - cx, py - cy) <= r


def in_ring(px, py, cx, cy, r, thickness):
    return abs(math.hypot(px - cx, py - cy) - r) <= thickness / 2.0


def star_points(cx, cy, r_outer, r_inner, spikes=5, rotation_deg=-90.0):
    pts = []
    for i in range(spikes * 2):
        r = r_outer if i % 2 == 0 else r_inner
        angle = math.radians(rotation_deg + i * (360.0 / (spikes * 2)))
        pts.append((cx + r * math.cos(angle), cy + r * math.sin(angle)))
    return pts


# ---------------------------------------------------------------------------
# Per-icon coverage functions.
# ---------------------------------------------------------------------------

# "Chunky arrow" polygon (tail rectangle + head triangle), one per direction.
_ARROW_RIGHT = [(9, 13), (16, 13), (16, 8), (25, 16), (16, 24), (16, 19), (9, 19)]
_ARROW_LEFT  = [(23, 13), (16, 13), (16, 8), (7, 16), (16, 24), (16, 19), (23, 19)]
_ARROW_UP    = [(13, 23), (13, 16), (8, 16), (16, 7), (24, 16), (19, 16), (19, 23)]
_ARROW_DOWN  = [(13, 9), (13, 16), (8, 16), (16, 25), (24, 16), (19, 16), (19, 9)]


def _icon_arrow_left(x, y):  return point_in_polygon(x, y, _ARROW_LEFT)
def _icon_arrow_right(x, y): return point_in_polygon(x, y, _ARROW_RIGHT)
def _icon_arrow_up(x, y):    return point_in_polygon(x, y, _ARROW_UP)
def _icon_arrow_down(x, y):  return point_in_polygon(x, y, _ARROW_DOWN)


_CHEVRON_THICKNESS = 3.5


def _icon_chevron_left(x, y):  return in_stroke(x, y, [(20, 8), (12, 16), (20, 24)], _CHEVRON_THICKNESS)
def _icon_chevron_right(x, y): return in_stroke(x, y, [(12, 8), (20, 16), (12, 24)], _CHEVRON_THICKNESS)
def _icon_chevron_up(x, y):    return in_stroke(x, y, [(8, 20), (16, 12), (24, 20)], _CHEVRON_THICKNESS)
def _icon_chevron_down(x, y):  return in_stroke(x, y, [(8, 12), (16, 20), (24, 12)], _CHEVRON_THICKNESS)


_CARET_THICKNESS = 3.0


def _icon_caret_up(x, y):   return in_stroke(x, y, [(10, 19), (16, 13), (22, 19)], _CARET_THICKNESS)
def _icon_caret_down(x, y): return in_stroke(x, y, [(10, 13), (16, 19), (22, 13)], _CARET_THICKNESS)


def _icon_check(x, y):
    return in_stroke(x, y, [(9, 17), (14, 22), (23, 10)], 3.5)


def _icon_cross(x, y):
    return (in_stroke(x, y, [(10, 10), (22, 22)], 3.5) or
            in_stroke(x, y, [(22, 10), (10, 22)], 3.5))


def _icon_plus(x, y):
    return in_rect(x, y, 9, 13, 23, 19) or in_rect(x, y, 13, 9, 19, 23)


def _icon_minus(x, y):
    return in_rect(x, y, 9, 13, 23, 19)


def _icon_dot(x, y):
    return in_circle(x, y, C, C, 4.0)


def _icon_circle(x, y):
    return in_ring(x, y, C, C, 10.0, 3.0)


def _icon_gear(x, y):
    r = math.hypot(x - C, y - C)
    if 6.0 <= r <= 10.0:
        return True
    if 10.0 < r <= 13.0:
        angle_mod = math.degrees(math.atan2(y - C, x - C)) % 45.0
        return angle_mod <= 12.0 or angle_mod >= 33.0
    return False


def _icon_search(x, y):
    if in_ring(x, y, 13, 13, 6.0, 3.0):
        return True
    return in_stroke(x, y, [(17.5, 17.5), (25.0, 25.0)], 3.5)


def _icon_menu(x, y):
    return (in_rect(x, y, 8, 9, 24, 12) or
            in_rect(x, y, 8, 14.5, 24, 17.5) or
            in_rect(x, y, 8, 20, 24, 23))


_STAR = star_points(C, C, 11.0, 4.6)


def _icon_star(x, y):
    return point_in_polygon(x, y, _STAR)


_WARNING_TRIANGLE = [(16, 7), (26, 25), (6, 25)]


def _icon_warning(x, y):
    if in_stroke(x, y, _WARNING_TRIANGLE, 2.2, closed=True):
        return True
    if in_rect(x, y, 15, 13, 17, 19):
        return True
    return in_circle(x, y, 16, 22, 1.4)


def _icon_info(x, y):
    if in_ring(x, y, C, C, 10.0, 2.4):
        return True
    if in_circle(x, y, 16, 10.5, 1.7):
        return True
    return in_rect(x, y, 15, 14, 17, 22)


def _icon_lock(x, y):
    if in_rect(x, y, 9, 16, 23, 26):
        return True
    return in_ring(x, y, 16, 15, 6.0, 3.0) and y <= 15.0


def _icon_folder(x, y):
    return in_rect(x, y, 6, 13, 26, 25) or in_rect(x, y, 6, 9, 14, 13)


ICON_FUNCS = {
    "arrow_left": _icon_arrow_left, "arrow_right": _icon_arrow_right,
    "arrow_up": _icon_arrow_up, "arrow_down": _icon_arrow_down,
    "chevron_left": _icon_chevron_left, "chevron_right": _icon_chevron_right,
    "chevron_up": _icon_chevron_up, "chevron_down": _icon_chevron_down,
    "caret_up": _icon_caret_up, "caret_down": _icon_caret_down,
    "check": _icon_check, "cross": _icon_cross,
    "plus": _icon_plus, "minus": _icon_minus,
    "dot": _icon_dot, "circle": _icon_circle,
    "gear": _icon_gear, "search": _icon_search, "menu": _icon_menu, "star": _icon_star,
    "warning": _icon_warning, "info": _icon_info, "lock": _icon_lock, "folder": _icon_folder,
}


# ---------------------------------------------------------------------------
# Rasterization + PNG encoding.
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
    cols = max(len(row) for row in ICON_LAYOUT)
    rows = len(ICON_LAYOUT)
    width, height = cols * CELL, rows * CELL

    pixels = bytearray(width * height * 4)
    entries = []

    for row_idx, row in enumerate(ICON_LAYOUT):
        for col_idx, name in enumerate(row):
            cell_x, cell_y = col_idx * CELL, row_idx * CELL
            alpha = rasterize_icon(ICON_FUNCS[name])
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

    png_path = os.path.join(out_dir, "icons.png")
    write_png(png_path, width, height, pixels)

    yaml_lines = [
        "# Generated by tools/gen_default_icons.py -- do not hand-edit; edit the",
        "# generator and re-run instead so the sheet and descriptor stay in sync.",
        "image: icons.png",
        f"width: {width}",
        f"height: {height}",
        "sprites:",
    ]
    for name, x, y, w, h in entries:
        yaml_lines.append(f"  - {{ name: {name}, x: {x}, y: {y}, w: {w}, h: {h} }}")
    yaml_path = os.path.join(out_dir, "icons.yaml")
    with open(yaml_path, "w") as f:
        f.write("\n".join(yaml_lines) + "\n")

    print(f"Wrote {width}x{height} {png_path} ({len(entries)} icons)")
    print(f"Wrote {yaml_path}")


if __name__ == "__main__":
    main()
