"""PNG writing for toyengine textures, heightmaps and masks.

Written with `zlib` and `struct` rather than through an imaging library, which is what the
generators under `tools/` already did -- four of them carried a near-identical copy of this
boilerplate (`gen_prototype_grid.py`, `gen_hair_mask.py`, `gen_instance_paint_demo.py` and
`blender_to_toy.py`), differing only in bit depth and colour type. Keeping it dependency-free
means a project can generate its textures with nothing installed but Python.

Two formats cover what the engine reads:

| Function | PNG | Read by |
|---|---|---|
| `write_rgba8` | 8-bit RGBA (colour type 6) | albedo, normal, metallic-roughness and mask textures, palettes, LUTs |
| `write_grey16` | 16-bit greyscale (colour type 0) | heightmaps, via `src/toyengine/world/png16.h` |

Both compress at level 9 and write no ancillary chunks, so the same input always produces a
byte-identical file -- which is what lets a generator be re-run in a diff-reviewed tree.
"""

import pathlib
import struct
import zlib

_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def _chunk(tag, data):
    """One PNG chunk: big-endian length, tag, payload, CRC32 of tag and payload."""
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def _write(path, ihdr, raw):
    """Writes a complete PNG, creating the parent directory if it is missing."""
    png = _SIGNATURE + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(raw, 9)) \
        + _chunk(b"IEND", b"")
    path = pathlib.Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)
    return path


def write_rgba8(path, width, height, rows):
    """Writes an 8-bit RGBA PNG.

    Args:
        path (str | pathlib.Path): Destination file. Parent directories are created.
        width (int): Pixels per row.
        height (int): Number of rows.
        rows (iterable): One row per scanline, top row first, each `width * 4` bytes of
            RGBA -- `bytes`, `bytearray`, or any iterable of ints in 0..255. A flat iterable
            of `width * height * 4` values is also accepted and split into rows.

    Returns:
        pathlib.Path: The path written.

    Raises:
        ValueError: If a row is not `width * 4` bytes long, or a flat sequence is not
            `width * height * 4` long.
    """
    scanlines = _rows_of(rows, width * 4, height, "RGBA8")
    raw = bytearray()
    for row in scanlines:
        raw.append(0)       # filter type 0 (none), per scanline
        raw += row
    return _write(path, struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0), bytes(raw))


def write_grey16(path, width, height, samples):
    """Writes a 16-bit greyscale PNG, the format `src/toyengine/world/png16.h` reads.

    Args:
        path (str | pathlib.Path): Destination file. Parent directories are created.
        width (int): Samples per row.
        height (int): Number of rows.
        samples (sequence): `width * height` values in 0..65535, row-major with the top row
            first, as one flat sequence or as one sequence per row.

    Returns:
        pathlib.Path: The path written.

    Raises:
        ValueError: If the sample count is not `width * height`, or a value is out of range.
    """
    flat = _flat(samples, width * height, "grey16")
    raw = bytearray()
    for j in range(height):
        raw.append(0)       # filter type 0 (none)
        row = flat[j * width:(j + 1) * width]
        try:
            raw += struct.pack(f">{width}H", *row)
        except struct.error as exc:
            bad = next((v for v in row if not 0 <= int(v) <= 65535), None)
            raise ValueError(
                f"grey16 sample {bad!r} is outside 0..65535 (row {j})") from exc
    return _write(path, struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0), bytes(raw))


def _rows_of(rows, stride, height, what):
    """Normalises row-or-flat pixel input into a list of `height` rows of `stride` bytes."""
    items = list(rows)
    if items and isinstance(items[0], int):
        if len(items) != stride * height:
            raise ValueError(
                f"{what}: got {len(items)} values, expected {stride * height} "
                f"({height} rows x {stride} bytes)")
        return [bytes(items[j * stride:(j + 1) * stride]) for j in range(height)]
    if len(items) != height:
        raise ValueError(f"{what}: got {len(items)} rows, expected {height}")
    out = []
    for index, row in enumerate(items):
        data = bytes(row)
        if len(data) != stride:
            raise ValueError(
                f"{what}: row {index} is {len(data)} bytes, expected {stride}")
        out.append(data)
    return out


def _flat(samples, count, what):
    """Normalises flat-or-nested sample input into one flat list of `count` values."""
    items = list(samples)
    if items and not isinstance(items[0], (int, float)):
        items = [v for row in items for v in row]
    if len(items) != count:
        raise ValueError(f"{what}: got {len(items)} samples, expected {count}")
    return [int(v) for v in items]
