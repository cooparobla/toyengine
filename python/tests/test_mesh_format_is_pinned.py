"""The mesh writer's exact bytes, pinned against a reference `tools/mesh_yaml.py` produced.

`tools/mesh_yaml.py` was the shared mesh writer every generator imported, and
`toyproject.write_mesh` replaced it. Before it was deleted, it was run on a small mesh chosen to
exercise the formatting edges -- a whole number, a tiny exponent, a negative, a repeating
fraction and a quad -- and its output committed beside this test as
`data_mesh_yaml_reference.yaml`.

That file is the contract. The old writer is gone, so it cannot be re-derived: if this test
fails, the format has moved and every mesh the generators produce will differ from the ones
committed in `assets/`.
"""

import pathlib

from toyproject import fmt_g6, write_mesh

REFERENCE = pathlib.Path(__file__).with_name("data_mesh_yaml_reference.yaml")

# The exact input the reference was generated from.
VERTS = [[0.0, 0.0, 0.0], [1.0, 0.0, 2e-05], [1.0, 1.0, -0.5], [0.0, 1.0, 0.33333333]]
FACES = [[0, 1, 2, 3]]
UVS = [[0, 0], [1, 0], [1, 1], [0, 1]]


def test_write_mesh_reproduces_the_retired_writer_byte_for_byte(tmp_path):
    out = tmp_path / "m.yaml"
    write_mesh(out, VERTS, FACES, UVS)
    assert out.read_text() == REFERENCE.read_text()


def test_the_default_format_is_the_compact_one(tmp_path):
    # write_mesh defaults to fmt_g6 because that is what every mesh it writes already carries.
    implicit, explicit = tmp_path / "a.yaml", tmp_path / "b.yaml"
    write_mesh(implicit, VERTS, FACES, UVS)
    write_mesh(explicit, VERTS, FACES, UVS, fmt=fmt_g6)
    assert implicit.read_text() == explicit.read_text()


def test_the_formatting_edges_the_reference_was_chosen_for():
    text = REFERENCE.read_text()
    assert "[0, 0, 0]" in text, "a whole number drops its decimal point"
    assert "2e-05" in text, "a tiny value keeps exponent form, unpadded"
    assert "-0.5" in text, "negatives are plain"
    assert "0.333333" in text, "six significant digits"
    assert "  - {  }" in text, "the empty-skinning placeholder keeps two inner spaces"
    assert "colors: []" in text
