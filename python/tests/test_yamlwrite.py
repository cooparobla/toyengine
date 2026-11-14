"""The YAML emitter: formatting, flow-versus-block, quoting and the number formats."""

import math

import pytest

from toyproject.yamlwrite import (Flow, document, emit, flow, fmt_g6, fmt_num, fmt_str)


def text(value, **kwargs):
    """emit() joined into one string, for readable assertions."""
    return "\n".join(emit(value, **kwargs))


class TestNumbers:
    def test_fmt_num_keeps_a_whole_float_s_decimal_point(self):
        assert fmt_num(-1.0) == "-1.0"
        assert fmt_num(0.0) == "0.0"

    def test_fmt_g6_drops_it(self):
        assert fmt_g6(-1.0) == "-1"
        assert fmt_g6(0.0) == "0"

    def test_both_round_to_six_significant_digits(self):
        assert fmt_num(2 / 3) == "0.666667"
        assert fmt_g6(2 / 3) == "0.666667"

    def test_ints_stay_ints_in_both(self):
        assert fmt_num(7) == "7"
        assert fmt_g6(7) == "7"

    def test_bools_are_not_written_as_numbers(self):
        assert fmt_num(True) == "true"
        assert fmt_g6(False) == "false"

    def test_negative_zero_is_normalised(self):
        # "-0.0" against "0.0" is pure diff noise in a regenerated asset.
        assert fmt_num(-0.0) == "0.0"
        assert fmt_num(-1e-9) == "0.0"

    @pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
    def test_non_finite_numbers_are_refused(self, bad):
        # Writing "nan" produces an asset no loader here reads, and silently.
        with pytest.raises(ValueError, match="finite"):
            fmt_num(bad)
        with pytest.raises(ValueError, match="finite"):
            fmt_g6(bad)


class TestStrings:
    @pytest.mark.parametrize("plain", [
        "plane", "materials/basic/plastic", "props/tree_branch", "Perspective",
        "my scene", "a_b.c",
    ])
    def test_ordinary_scalars_are_left_unquoted(self, plain):
        assert fmt_str(plain) == plain

    @pytest.mark.parametrize("quoted", ["true", "false", "null", "True", "FALSE", "NULL"])
    def test_words_yaml_1_2_decodes_as_non_strings_are_quoted(self, quoted):
        assert fmt_str(quoted) == f"'{quoted}'"

    @pytest.mark.parametrize("plain", ["y", "n", "yes", "no", "on", "off"])
    def test_yaml_1_1_only_booleans_stay_plain(self, plain):
        # fkYAML is YAML 1.2, where these are ordinary strings -- and `y` is a key in every
        # vec3 the engine loads, so quoting it would rewrite every transform in the tree.
        assert fmt_str(plain) == plain

    @pytest.mark.parametrize("quoted", ["", "1.5", "7", "{a}", "[a]", "a: b", "#x", "*x", " x"])
    def test_anything_structural_or_numeric_is_quoted(self, quoted):
        assert fmt_str(quoted).startswith("'")

    def test_an_embedded_quote_is_doubled(self):
        assert fmt_str("it's") == "'it''s'"


class TestFlowAndBlock:
    def test_a_mapping_is_block_by_default(self):
        assert text({"a": 1, "b": 2}) == "a: 1\nb: 2"

    def test_flow_puts_a_mapping_on_one_line_spaced(self):
        assert text({"position": Flow({"x": 0.0, "y": 1.5})}) == "position: { x: 0.0, y: 1.5 }"

    def test_flow_sequences_are_not_spaced(self):
        assert flow([0, 1, 2]) == "[0, 1, 2]"

    def test_empty_collections_are_always_inline(self):
        # `children: []` is how the committed scenes spell a childless object.
        assert text({"children": [], "extra": {}}) == "children: []\nextra: {}"

    def test_a_nested_mapping_indents_two_spaces(self):
        assert text({"outer": {"inner": 1}}) == "outer:\n  inner: 1"

    def test_a_block_sequence_is_indented_under_its_key(self):
        # The layout every committed asset uses; see _child().
        assert text({"items": [1, 2]}) == "items:\n  - 1\n  - 2"

    def test_a_sequence_of_mappings_carries_the_dash_on_the_first_key(self):
        got = text({"components": [{"type": "Transform"}, {"type": "Camera"}]})
        assert got == "components:\n  - type: Transform\n  - type: Camera"

    def test_a_mapping_in_a_sequence_keeps_its_later_keys_aligned(self):
        got = text({"c": [{"type": "Transform", "position": 1}]})
        assert got == "c:\n  - type: Transform\n    position: 1"

    def test_nested_flow_survives_inside_a_sequence(self):
        assert text({"faces": [Flow([0, 1, 2]), Flow([2, 3, 0])]}) \
            == "faces:\n  - [0, 1, 2]\n  - [2, 3, 0]"

    def test_the_number_format_reaches_nested_values(self):
        assert text({"v": Flow([1.0])}, fmt=fmt_g6) == "v: [1]"
        assert text({"v": Flow([1.0])}, fmt=fmt_num) == "v: [1.0]"

    def test_an_unwritable_type_is_refused_rather_than_stringified(self):
        with pytest.raises(TypeError, match="cannot write"):
            text({"bad": object()})


class TestDocument:
    def test_a_header_becomes_a_comment_block(self):
        got = document({"a": 1}, header="Why this exists.\n\nAnd a second paragraph.")
        assert got == ("# Why this exists.\n"
                       "#\n"
                       "# And a second paragraph.\n"
                       "\n"
                       "a: 1\n")

    def test_without_a_header_it_is_just_the_document(self):
        assert document({"a": 1}) == "a: 1\n"

    def test_it_ends_in_exactly_one_newline(self):
        assert document({"a": 1}).endswith("1\n")
        assert not document({"a": 1}).endswith("\n\n")
