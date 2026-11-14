"""The PNG writers: structure, determinism and the validation that prevents silent corruption.

Decoded with `zlib` and `struct` rather than an imaging library, so the tests need nothing
installed that the writers themselves do not.
"""

import struct
import zlib

import pytest

from toyproject import png

SIGNATURE = b"\x89PNG\r\n\x1a\n"


def chunks(data):
    """Every chunk in a PNG as (tag, payload), verifying each CRC on the way."""
    assert data[:8] == SIGNATURE
    out, offset = [], 8
    while offset < len(data):
        length = struct.unpack(">I", data[offset:offset + 4])[0]
        tag = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        crc = struct.unpack(">I", data[offset + 8 + length:offset + 12 + length])[0]
        assert crc == zlib.crc32(tag + payload) & 0xFFFFFFFF, f"bad CRC on {tag!r}"
        out.append((tag, payload))
        offset += 12 + length
    return out


def header(data):
    """The IHDR fields: width, height, depth, colour type, and the three zeros."""
    tag, payload = chunks(data)[0]
    assert tag == b"IHDR"
    return struct.unpack(">IIBBBBB", payload)


def scanlines(data, stride):
    """The decompressed IDAT split into rows, with each row's filter byte checked and dropped."""
    raw = zlib.decompress(b"".join(p for t, p in chunks(data) if t == b"IDAT"))
    rows = []
    for offset in range(0, len(raw), stride + 1):
        assert raw[offset] == 0, "only filter type 0 (none) is written"
        rows.append(raw[offset + 1:offset + 1 + stride])
    return rows


class TestRgba8:
    def test_the_header_declares_8_bit_rgba(self, tmp_path):
        path = png.write_rgba8(tmp_path / "x.png", 2, 3, [bytes(8)] * 3)
        width, height, depth, colour, *rest = header(path.read_bytes())
        assert (width, height, depth, colour) == (2, 3, 8, 6)
        assert rest == [0, 0, 0]

    def test_pixels_survive_the_round_trip(self, tmp_path):
        rows = [bytes((1, 2, 3, 4, 5, 6, 7, 8)), bytes((9, 10, 11, 12, 13, 14, 15, 16))]
        path = png.write_rgba8(tmp_path / "x.png", 2, 2, rows)
        assert scanlines(path.read_bytes(), 8) == rows

    def test_a_flat_sequence_is_split_into_rows(self, tmp_path):
        flat = list(range(2 * 2 * 4))
        path = png.write_rgba8(tmp_path / "x.png", 2, 2, flat)
        assert b"".join(scanlines(path.read_bytes(), 8)) == bytes(flat)

    def test_the_ends_with_iend(self, tmp_path):
        path = png.write_rgba8(tmp_path / "x.png", 1, 1, [bytes(4)])
        assert chunks(path.read_bytes())[-1] == (b"IEND", b"")

    def test_parent_directories_are_created(self, tmp_path):
        path = png.write_rgba8(tmp_path / "a/b/c.png", 1, 1, [bytes(4)])
        assert path.is_file()

    def test_a_short_row_is_refused(self, tmp_path):
        with pytest.raises(ValueError, match="row 0 is 3 bytes"):
            png.write_rgba8(tmp_path / "x.png", 2, 1, [bytes(3)])

    def test_a_wrong_row_count_is_refused(self, tmp_path):
        with pytest.raises(ValueError, match="expected 2"):
            png.write_rgba8(tmp_path / "x.png", 1, 2, [bytes(4)])


class TestGrey16:
    def test_the_header_declares_16_bit_greyscale(self, tmp_path):
        path = png.write_grey16(tmp_path / "h.png", 2, 2, [0, 1, 2, 3])
        width, height, depth, colour, *_ = header(path.read_bytes())
        assert (width, height, depth, colour) == (2, 2, 16, 0)

    def test_samples_survive_as_big_endian_shorts(self, tmp_path):
        path = png.write_grey16(tmp_path / "h.png", 2, 1, [0, 65535])
        assert scanlines(path.read_bytes(), 4) == [struct.pack(">2H", 0, 65535)]

    def test_rows_may_be_nested(self, tmp_path):
        flat = png.write_grey16(tmp_path / "a.png", 2, 2, [1, 2, 3, 4]).read_bytes()
        nested = png.write_grey16(tmp_path / "b.png", 2, 2, [[1, 2], [3, 4]]).read_bytes()
        assert flat == nested

    def test_a_wrong_sample_count_is_refused(self, tmp_path):
        with pytest.raises(ValueError, match="expected 4"):
            png.write_grey16(tmp_path / "h.png", 2, 2, [0, 1, 2])

    def test_an_out_of_range_sample_names_itself_and_its_row(self, tmp_path):
        # Wrapping silently would put a hole in a terrain instead of reporting a bug.
        with pytest.raises(ValueError, match="outside 0..65535"):
            png.write_grey16(tmp_path / "h.png", 2, 2, [0, 0, 0, 70000])


def test_the_same_input_produces_a_byte_identical_file(tmp_path):
    # What lets a generator be re-run in a diff-reviewed tree without churn.
    rows = [bytes(range(8)), bytes(range(8, 16))]
    first = png.write_rgba8(tmp_path / "a.png", 2, 2, rows).read_bytes()
    second = png.write_rgba8(tmp_path / "b.png", 2, 2, rows).read_bytes()
    assert first == second
