"""Generates assets/textures/prototype/prototype_grid.png, the blockout grid behind materials/prototype_grid.yaml.

Standalone generator, not part of the C++ build -- run it manually whenever the texture needs
regenerating:

    uv run --project python tools/gen_prototype_grid.py

One tile is one metre at the material's default tiling (the `triplanar` shader projects it in
world space, so every surface reads its true size): a soft two-tone checker of quarter-metre
cells, thin quarter-metre lines, and a heavier line on the metre boundary. The boundary line is
one texel on each edge, so two tiles side by side meet in a two-texel line.

The PNG is written by `toyproject.assets.write_texture` (python/), which replaces the copy of
the zlib/struct PNG boilerplate this file used to carry -- four generators had one each.
"""

import pathlib

from toyproject import Project
from toyproject.assets import write_texture

SIZE = 128
"""Output width/height in texels (one metre at tiling 1)."""

CELLS = 4
"""Checker cells / minor-line divisions per tile edge."""

CELL_LIGHT = (140, 142, 146)
CELL_DARK = (124, 126, 130)
MINOR_LINE = (104, 106, 110)
MAJOR_LINE = (62, 64, 68)

ENGINE_DIR = pathlib.Path(__file__).resolve().parent.parent
"""The engine checkout. Passed to Project() explicitly because an engine has no .toy file."""


def texel(x: int, y: int) -> tuple:
    """sRGB colour of texel (x, y)."""
    if x in (0, SIZE - 1) or y in (0, SIZE - 1):
        return MAJOR_LINE
    cell = SIZE // CELLS
    if x % cell == 0 or y % cell == 0:
        return MINOR_LINE
    return CELL_LIGHT if (x // cell + y // cell) % 2 == 0 else CELL_DARK


def main() -> None:
    path = write_texture(Project(ENGINE_DIR), "prototype/prototype_grid", SIZE, texel)
    print(f"Wrote {path} ({SIZE}x{SIZE})")


if __name__ == "__main__":
    main()
