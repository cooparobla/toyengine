"""Textures and heightmaps: PNGs, written where each kind is loaded from.

Thin helpers over `toyproject.png`, with the path handling that differs between the two:

- **Textures** are project assets under `assets/textures/`, referenced from a material by
  `texture_albedo`, `texture_normal` and friends.
- **Heightmaps** are not. A `HeightmapTerrain`'s PNG lives beside the scene that uses it
  (`assets/scenes/<area>/<name>/terrain/height.png`), because it is that scene's data rather
  than a shared asset -- so `write_heightmap` takes a project-relative path, not an asset name.

The per-texel form is the one the generators already used -- a function of (x, y) rather than a
prepared buffer, which is how a procedural pattern is naturally written:

    write_texture(project, "grid", 256, lambda x, y: (40, 44, 52, 255))
"""

from .. import png


def write_texture(project, name, size, texel, height=None):
    """Writes an 8-bit RGBA texture under `assets/textures`.

    Args:
        project (Project): The project to write into.
        name (str): Its path under `assets/textures`, e.g. `"terrain/grass_albedo"`. A `.png`
            suffix is added when absent.
        size (int): Width in pixels, and height too unless `height` is given.
        texel (callable): `texel(x, y)` returning `(r, g, b)` or `(r, g, b, a)`, each 0..255.
            A 3-tuple is taken as fully opaque.
        height (int): Height in pixels, when it differs from `size`.

    Returns:
        pathlib.Path: The file written.
    """
    rows_high = height if height is not None else size
    rows = _rows(size, rows_high, texel)
    path = png.write_rgba8(project.path_for("texture", name), size, rows_high, rows)
    return project.record(path)


def write_heightmap(project, relative_path, resolution, height_at, max_height):
    """Writes a 16-bit greyscale heightmap, the format `HeightmapTerrain` loads.

    Heights are normalised against `max_height`, which must be the terrain's own `height` key:
    the PNG stores 0..65535 over 0..`max_height`, so a mismatch rescales the whole landscape.
    Samples are clamped into range rather than wrapping, because a height just past the top
    would otherwise appear as a hole at the bottom.

    Args:
        project (Project): The project to write into.
        relative_path (str): Where to write it, relative to the project root -- conventionally
            `assets/scenes/<area>/<name>/terrain/height.png`.
        resolution (int): Samples per edge.
        height_at (callable): `height_at(x, y)` in metres, called with world coordinates
            spanning 0..`max_height`'s terrain extent as `x = size * i / (resolution - 1)`.
        max_height (float): The terrain's `height`, in metres, that 65535 represents.

    Returns:
        pathlib.Path: The file written.

    Raises:
        ValueError: If `max_height` is not positive -- every sample would divide by it.
    """
    if max_height <= 0:
        raise ValueError(f"max_height must be positive, got {max_height!r}")
    samples = []
    for j in range(resolution):
        for i in range(resolution):
            h = height_at(i, j)
            clamped = min(max(h, 0.0), max_height)
            samples.append(int(round(clamped / max_height * 65535.0)))
    target = project.root / relative_path
    return project.record(png.write_grey16(target, resolution, resolution, samples))


def _rows(width, height, texel):
    """Builds RGBA rows from a per-texel function, top row first."""
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            value = texel(x, y)
            if len(value) == 3:
                row += bytes((value[0], value[1], value[2], 255))
            else:
                row += bytes(value)
        rows.append(bytes(row))
    return rows
