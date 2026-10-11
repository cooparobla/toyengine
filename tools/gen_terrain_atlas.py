"""Generates the TileTerrain palette atlas: assets/textures/terrain/terrain_atlas{,_mr}.png.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_terrain_atlas.py`) whenever the atlas needs regenerating. Requires numpy and Pillow.

These live in the SHARED assets/textures/terrain/ beside the tile sets (assets/objects/terrain/),
because every TileTerrain wants the same palette whichever tile set it is built from. A scene
overrides it by putting a file of the same name in its own textures/, which
AssetSource::resolve() tries before the registered search roots.

Two files, matching the glTF convention the rest of this repo uses (see
tools/gen_material_maps.py):

  terrain_atlas.png    -- base color (RGB), authored in sRGB. One flat-colour cell per
      toy::world::TileKind, laid out in a 4x4 grid in enum order. THE ORDER OF `KINDS` BELOW IS
      LOAD-BEARING: it must match TileKind exactly, and a new kind is appended, never inserted.
  terrain_atlas_mr.png -- metallic-roughness (metallic in B, roughness in G), linear.

Each cell is a single colour and nothing else. Chunks draw with
assets/shaders/terrain_styled.frag, which reads a kind's colour from the centre of its cell and
adds the surface detail procedurally, in world space and filtered to the pixel footprint -- a
texture cannot do that here (the engine has no mipmaps, and a rounded piece's projected UVs
seam across its lips).
"""

import pathlib

import numpy as np
from PIL import Image

CELL = 16
"""Edge length of one cell, in texels. Every cell is one flat colour, so its size only has to
keep the shader's centre sample well inside the cell."""

COLUMNS = 4
"""Cells per atlas row. Must match k_atlas_columns in tile_types.h."""

ROWS = 4
"""Cells per atlas column. Must match k_atlas_rows in tile_types.h."""

KINDS = [
    # (name, base colour RGB, roughness) -- IN ENUM ORDER.
    ("grass", (118, 184, 76), 0.92),
    ("dirt", (158, 110, 72), 0.94),
    ("stone", (138, 140, 150), 0.86),
    ("sand", (236, 222, 170), 0.90),
    ("snow", (244, 247, 252), 0.70),
    ("rock", (112, 108, 116), 0.86),
    ("water", (78, 150, 205), 0.15),
    ("ice", (182, 224, 240), 0.25),
    ("moss", (112, 160, 84), 0.94),
    ("clay", (190, 138, 96), 0.92),
    ("ash", (96, 90, 88), 0.90),
    ("salt", (238, 236, 226), 0.82),
]
"""One entry per toy::world::TileKind, IN ENUM ORDER. See this module's docstring."""

OUTPUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "assets" / "textures" / "terrain"
"""Where the generated PNGs are written; resolved relative to this repo, not the caller's cwd."""


def main():
    """Writes terrain_atlas.png and terrain_atlas_mr.png into OUTPUT_DIR."""
    if len(KINDS) > COLUMNS * ROWS:
        raise SystemExit(f"{len(KINDS)} kinds do not fit in a {COLUMNS}x{ROWS} atlas")
    width = COLUMNS * CELL
    height = ROWS * CELL
    albedo = np.zeros((height, width, 3), dtype=np.uint8)
    # Metallic-roughness starts fully rough and fully dielectric, so the spare cells never read
    # as chrome if a future kind lands on one before this file is updated.
    mr = np.zeros((height, width, 3), dtype=np.uint8)
    mr[:, :, 1] = 255
    for index, (_name, color, roughness) in enumerate(KINDS):
        y0 = (index // COLUMNS) * CELL
        x0 = (index % COLUMNS) * CELL
        albedo[y0:y0 + CELL, x0:x0 + CELL] = color
        mr[y0:y0 + CELL, x0:x0 + CELL, 1] = int(round(roughness * 255))
        mr[y0:y0 + CELL, x0:x0 + CELL, 2] = 0  # nothing in this world is metal
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    Image.fromarray(albedo, mode="RGB").save(OUTPUT_DIR / "terrain_atlas.png")
    Image.fromarray(mr, mode="RGB").save(OUTPUT_DIR / "terrain_atlas_mr.png")
    print(f"wrote {OUTPUT_DIR / 'terrain_atlas.png'} ({width}x{height}, {len(KINDS)} kinds)")
    print(f"wrote {OUTPUT_DIR / 'terrain_atlas_mr.png'}")


if __name__ == "__main__":
    main()
