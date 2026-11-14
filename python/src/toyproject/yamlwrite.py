"""Deterministic YAML emission for toyengine assets.

Assets are written by hand rather than through a YAML library, and that is deliberate: the
engine's assets are read by people and reviewed as diffs, so the output has to be stable,
minimally quoted, and laid out the way the committed assets already are -- a vec3 inline as
`{ x: 0.0, y: 0.0, z: 0.0 }`, a face as `[0, 1, 2]`, everything else in block style. A general
serializer reorders keys, quotes freely and folds long lines, and the diff noise that produces
is the reason this module exists.

## Flow versus block

`emit()` writes mappings and sequences in block style. Wrap a value in `Flow` to put it on one
line instead, which is what the inline vec3s and index lists in committed assets are:

    emit({"position": Flow({"x": 0.0, "y": 0.0, "z": 1.5})})   # position: { x: 0.0, ... }
    emit({"faces": [Flow([0, 1, 2])]})                         # - [0, 1, 2]

An empty mapping or sequence is always inline (`{}` / `[]`), because `children: []` is how the
committed scenes spell an object with no children.

## Number formatting is explicit, and it has to be

There is no single right float format here -- the committed assets use more than one, and a
generator that changes the one it was written with produces a whole-file diff that hides the
real change. So the format is a parameter rather than a global:

| Helper | `-1.0` | `2/3` | Used by |
|---|---|---|---|
| `fmt_num` (default) | `-1.0` | `0.666667` | scenes, objects, materials, and `gen_water_grid_mesh.py`'s meshes |
| `fmt_g6` | `-1` | `0.666667` | `tools/mesh_yaml.py`'s tile meshes |

Both round to six significant digits; they differ only in whether a whole number keeps its
`.0`. Note that `tools/mesh_yaml.py`'s docstring claims its `fmt_vec` matches
`gen_water_grid_mesh.py` -- it does not, and the two files on disk differ accordingly
(`[-1, -1, 0]` against `[-1.0, -1.0, 0.0]`). Pass the format the asset you are rewriting
already uses.
"""

import math
import re

# A plain (unquoted) YAML scalar: no leading/trailing space, no character that would start a
# flow collection, a comment, an anchor or a key, and not a word the reader would decode as
# something other than a string. Anything else is single-quoted.
#
# The reserved list is YAML **1.2**'s, which is what the engine's reader (fkYAML) implements:
# only `true`, `false`, `null` and `~` are special. YAML 1.1 additionally decoded `yes`, `no`,
# `on`, `off`, `y` and `n` as booleans, and reserving those here was a bug -- `y` is a key in
# every single vec3 the engine loads (`{ x: .., y: .., z: .. }`), so quoting it would have
# rewritten every transform in the tree. A number or `~` fails the pattern below in any case.
_PLAIN = re.compile(r"^[A-Za-z_./][A-Za-z0-9_./\- ]*$")
_RESERVED = {
    "true", "True", "TRUE",
    "false", "False", "FALSE",
    "null", "Null", "NULL",
}


class Flow:
    """Marks a mapping or sequence to be emitted inline, on one line.

    Args:
        value (dict | list): What to wrap. Nested `Flow` markers inside it are honoured, so a
            flow sequence of flow maps stays on one line throughout.
    """

    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __repr__(self):
        return f"Flow({self.value!r})"


def fmt_num(value):
    """Formats a number the way most committed assets spell one.

    Floats keep a decimal point (`-1.0`, not `-1`) and are rounded to six significant digits,
    which is what `str(round(v, 6))` produced in the generators this replaces. Integers are
    written as integers, and a bool is written as `true`/`false` rather than as 1/0.

    Args:
        value (int | float | bool): The number.

    Returns:
        str: Its YAML spelling.

    Raises:
        ValueError: If `value` is a NaN or an infinity, neither of which any asset loader here
            accepts -- and which would otherwise be written as the unreadable `nan`/`inf`.
    """
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if not math.isfinite(value):
        raise ValueError(f"{value!r} is not a finite number; no toyengine asset loader reads one")
    rounded = round(float(value), 6)
    # round() leaves -0.0 negative, and "-0.0" in a diff against "0.0" is pure noise.
    if rounded == 0.0:
        rounded = 0.0
    text = repr(rounded)
    return text


def fmt_g6(value):
    """Formats a number in the compact `%.6g` style `tools/mesh_yaml.py` writes.

    Differs from `fmt_num` only in dropping a whole number's decimal point: `-1.0` becomes
    `-1`. Use it when rewriting a mesh that file produced, so the diff stays empty.

    Args:
        value (int | float | bool): The number.

    Returns:
        str: Its YAML spelling.

    Raises:
        ValueError: If `value` is a NaN or an infinity.
    """
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    if not math.isfinite(value):
        raise ValueError(f"{value!r} is not a finite number; no toyengine asset loader reads one")
    return f"{value:.6g}"


def fmt_str(value):
    """Formats a string, quoting it only where a plain scalar would be read as something else.

    Args:
        value (str): The string.

    Returns:
        str: Either the string as-is, or a single-quoted scalar with `'` doubled.
    """
    if value != "" and value not in _RESERVED and _PLAIN.match(value):
        return value
    return "'" + value.replace("'", "''") + "'"


def fmt_scalar(value, fmt=fmt_num):
    """Formats any scalar: `None`, a bool, a number or a string.

    Args:
        value: The scalar.
        fmt (callable): The number format, `fmt_num` or `fmt_g6`.

    Returns:
        str: Its YAML spelling.

    Raises:
        TypeError: If `value` is not a scalar this module knows how to write.
    """
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return fmt(value)
    if isinstance(value, str):
        return fmt_str(value)
    raise TypeError(f"cannot write {type(value).__name__} as a YAML scalar: {value!r}")


def flow(value, fmt=fmt_num):
    """Renders a mapping, sequence or scalar inline, on one line.

    Mappings are spaced as the committed assets spell them (`{ x: 1.0, y: 2.0 }`); sequences
    are not (`[0, 1, 2]`). An empty collection renders as `{}` or `[]`.

    Args:
        value: A dict, list, tuple, `Flow`, or scalar.
        fmt (callable): The number format.

    Returns:
        str: The one-line rendering.
    """
    if isinstance(value, Flow):
        value = value.value
    if isinstance(value, dict):
        if not value:
            return "{}"
        inner = ", ".join(f"{fmt_str(str(k))}: {flow(v, fmt)}" for k, v in value.items())
        return "{ " + inner + " }"
    if isinstance(value, (list, tuple)):
        if not value:
            return "[]"
        return "[" + ", ".join(flow(v, fmt) for v in value) + "]"
    return fmt_scalar(value, fmt)


def emit(value, fmt=fmt_num, indent=0):
    """Renders a value as YAML lines in block style, honouring `Flow` markers.

    Args:
        value: A dict, list, `Flow`, or scalar. A dict keeps its insertion order, which is how
            a generator controls key order in the output.
        fmt (callable): The number format, `fmt_num` or `fmt_g6`.
        indent (int): Spaces of leading indentation for this level.

    Returns:
        list: The lines, without trailing newlines.
    """
    pad = " " * indent
    if isinstance(value, Flow):
        return [pad + flow(value, fmt)]

    if isinstance(value, dict):
        if not value:
            return [pad + "{}"]
        lines = []
        for key, item in value.items():
            label = f"{pad}{fmt_str(str(key))}:"
            lines += _child(label, item, fmt, indent)
        return lines

    if isinstance(value, (list, tuple)):
        if not value:
            return [pad + "[]"]
        lines = []
        for item in value:
            if isinstance(item, (dict, list, tuple)) and not isinstance(item, Flow) and item:
                block = emit(item, fmt, indent + 2)
                # The first line of the child carries the "- ", the rest keep their indent.
                lines.append(pad + "- " + block[0].lstrip())
                lines += block[1:]
            else:
                lines.append(pad + "- " + flow(item, fmt))
        return lines

    return [pad + fmt_scalar(value, fmt)]


def _child(label, item, fmt, indent):
    """One `key:` line plus whatever its value needs, inline or on the lines below it."""
    if isinstance(item, Flow) or not isinstance(item, (dict, list, tuple)):
        return [f"{label} {flow(item, fmt)}"]
    if not item:
        return [f"{label} {'{}' if isinstance(item, dict) else '[]'}"]
    # Both a block sequence and a block mapping are indented two spaces under their key, which
    # is the layout every committed asset uses -- `vertices:` then `  - [..]`, `components:`
    # then `  - type: ..`. A sequence at the parent's own indent is equally valid YAML and
    # reads as a different file in every diff, so it is not an option here.
    return [label] + emit(item, fmt, indent + 2)


def document(value, fmt=fmt_num, header=None):
    """Renders a whole asset file, with an optional leading comment block.

    Args:
        value: The document's top-level value, usually a dict.
        fmt (callable): The number format.
        header (str): Optional comment text. Each line is prefixed with `# `, and blank lines
            become a bare `#`, which is how the committed assets write their preamble.

    Returns:
        str: The file's full text, ending in exactly one newline.
    """
    lines = []
    if header:
        for line in header.rstrip("\n").split("\n"):
            lines.append("# " + line if line.strip() else "#")
        lines.append("")
    lines += emit(value, fmt)
    return "\n".join(lines) + "\n"
