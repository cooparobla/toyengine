"""Generates the terrain tile atlas used by the terrain_test scene's chunk material.

Standalone generator, not part of the C++ build -- run it manually (`python3
tools/gen_terrain_atlas.py`) whenever assets/textures/terrain_atlas*.png need regenerating.
Requires numpy and Pillow.

These live in the SHARED assets/textures/ alongside assets/meshes/tile_side_*.yaml, for the
reason that file's generator gives: the tile set is a primitive of the tile system, not demo
content. A scene overrides it by putting a file of the same name in its own textures/, which
AssetSource::resolve() tries before the registered search roots.

Two files, matching the glTF convention the rest of this repo uses (see
tools/gen_material_maps.py):

  terrain_atlas.png    -- base color (RGB), authored in sRGB. One 16x16 cell per
      toy::world::TileKind, laid out in a 4x4 grid in enum order. The C++ side computes a
      cell's UV rectangle from the enum value alone (see tile_types.h's atlas_cell()), so
      THE ORDER OF `KINDS` BELOW IS LOAD-BEARING: it must match TileKind exactly, and a new
      kind is appended, never inserted.
  terrain_atlas_mr.png -- metallic-roughness (metallic in B, roughness in G), linear. Water
      and ice are the only smooth surfaces here; everything else is near-fully rough.

Every cell carries two deliberate features:

  * **Per-texel value noise**, seeded per kind, so a face is not a flat colour patch. Small
    amplitude -- enough to read as material grain at the engine's internal resolution, not
    enough to look like compression noise.
  * **A darkened one-texel rim.** Tiles are NEAREST-sampled and cell UVs are inset by half a
    texel (see atlas_cell()), so this rim lands exactly on the edge of every face and gives
    the world its block outlines. It is the single most "Minecraft" thing in the pipeline,
    and it is data: a smoother tile set drops the rim by editing this file, with no C++
    change anywhere.
"""

import pathlib

import numpy as np
from PIL import Image

CELL = 16
"""Edge length of one atlas cell, in texels. Must match k_atlas_cell_texels in tile_types.h."""

COLUMNS = 4
"""Cells per atlas row. Must match k_atlas_columns in tile_types.h."""

ROWS = 4
"""Cells per atlas column. Must match k_atlas_rows in tile_types.h."""

KINDS = [
    # (name, base colour RGB, noise amplitude, rim darkening, roughness)
    ("grass", (92, 148, 62), 18, 0.82, 0.95),
    ("dirt", (122, 88, 58), 16, 0.82, 0.95),
    ("stone", (128, 128, 132), 14, 0.86, 0.90),
    ("sand", (222, 202, 148), 12, 0.88, 0.92),
    ("snow", (238, 242, 248), 8, 0.92, 0.75),
    ("rock", (96, 92, 96), 16, 0.84, 0.88),
    ("water", (70, 130, 180), 10, 0.94, 0.15),
    ("ice", (170, 214, 232), 8, 0.92, 0.25),
    ("moss", (108, 132, 76), 16, 0.84, 0.95),
    ("clay", (176, 128, 90), 14, 0.86, 0.94),
    ("ash", (74, 68, 66), 14, 0.86, 0.92),
    ("salt", (230, 228, 218), 8, 0.92, 0.85),
]
"""One entry per toy::world::TileKind, IN ENUM ORDER. See this module's docstring."""

OUTPUT_DIR = pathlib.Path(__file__).resolve().parent.parent / "assets" / "textures"
"""Where the generated PNGs are written; resolved relative to this repo, not the caller's cwd."""


def build_cell(index, color, noise_amplitude, rim_scale):
    """Renders one atlas cell's base colour.

    Args:
        index (int): The kind's position in KINDS; seeds the noise so a regenerated atlas is
            byte-identical and one kind's appearance never depends on another's.
        color (tuple): Base RGB, 0-255.
        noise_amplitude (int): Peak per-texel deviation added to every channel.
        rim_scale (float): Multiplier applied to the outermost texel ring; < 1 darkens it.

    Returns:
        numpy.ndarray: A (CELL, CELL, 3) uint8 image.
    """
    rng = np.random.default_rng(1000 + index)
    noise = rng.integers(-noise_amplitude, noise_amplitude + 1, size=(CELL, CELL, 1))
    cell = np.clip(np.array(color, dtype=np.int32) + noise, 0, 255)

    # The rim is applied after the noise so the outline stays continuous rather than
    # dissolving into the grain.
    rim = np.ones((CELL, CELL, 1), dtype=np.float32)
    rim[0, :, :] = rim_scale
    rim[-1, :, :] = rim_scale
    rim[:, 0, :] = rim_scale
    rim[:, -1, :] = rim_scale

    return np.clip(cell * rim, 0, 255).astype(np.uint8)


def main():
    """Writes terrain_atlas.png and terrain_atlas_mr.png into OUTPUT_DIR."""
    if len(KINDS) > COLUMNS * ROWS:
        raise SystemExit(f"{len(KINDS)} kinds do not fit in a {COLUMNS}x{ROWS} atlas")

    width = COLUMNS * CELL
    height = ROWS * CELL
    albedo = np.zeros((height, width, 3), dtype=np.uint8)
    # Metallic-roughness starts fully rough and fully dielectric, so the four spare cells
    # never read as chrome if a future kind lands on one before this file is updated.
    mr = np.zeros((height, width, 3), dtype=np.uint8)
    mr[:, :, 1] = 255

    for index, (_name, color, noise_amplitude, rim_scale, roughness) in enumerate(KINDS):
        column = index % COLUMNS
        row = index // COLUMNS
        y0 = row * CELL
        x0 = column * CELL

        albedo[y0:y0 + CELL, x0:x0 + CELL] = build_cell(index, color, noise_amplitude, rim_scale)
        mr[y0:y0 + CELL, x0:x0 + CELL, 1] = int(round(roughness * 255))
        mr[y0:y0 + CELL, x0:x0 + CELL, 2] = 0  # nothing in this world is metal

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    Image.fromarray(albedo, mode="RGB").save(OUTPUT_DIR / "terrain_atlas.png")
    Image.fromarray(mr, mode="RGB").save(OUTPUT_DIR / "terrain_atlas_mr.png")
    print(f"wrote {OUTPUT_DIR / 'terrain_atlas.png'} ({width}x{height}, {len(KINDS)} kinds)")
    print(f"wrote {OUTPUT_DIR / 'terrain_atlas_mr.png'}")


if __name__ == "__main__":
    main()
