"""Writing any asset kind, including the ones with no typed builder here.

Typed builders cover the five kinds the generators actually hammer -- scenes, objects,
materials, meshes and textures. The other eleven in `ASSET_DIRS` (animations, rigs, graphs,
UI panels and themes, palettes, physics and damage materials, audio, fonts, shaders) are
written through `write_asset`, which puts the file in the right place and emits it with the
same deterministic YAML as everything else, leaving the document's shape to the caller.

    write_asset(project, "physics_material", "ice",
                {"friction": 0.05, "bounciness": 0.1},
                header="Slippery, barely bouncy.")

That is not a lesser path: a typed builder adds key spelling and ordering for a schema worth
pinning down, and nothing else. When a kind here starts being generated often enough that its
key order matters, it is worth promoting to a builder of its own; until then this is the honest
interface, because the engine parses every key by hand and this module cannot validate against
that (see `assets/node.py`'s note on there being no reflection).
"""

from .. import yamlwrite
from ..yamlwrite import fmt_num


def write_asset(project, kind, name, data, header=None, fmt=fmt_num):
    """Writes a YAML asset of any kind into a project.

    Args:
        project (Project): The project to write into.
        kind (str): A key of `toyproject.project.ASSET_DIRS`, e.g. `"physics_material"`.
        name (str): Its path under that kind's directory, without the suffix.
        data (dict | list): The document. A dict keeps its insertion order, which is how you
            control key order in the file. Wrap collections in `Flow` for inline rendering.
        header (str): Optional comment block for the top of the file.
        fmt (callable): The number format.

    Returns:
        pathlib.Path: The file written.

    Raises:
        KeyError: If `kind` is not a known asset kind.
        ValueError: If `kind` names a binary kind (`texture`, `palette`, `audio`, `font`) --
            those are not YAML, and a YAML document written to a `.png` would be loaded as a
            corrupt image rather than reported as a mistake. Use `assets.texture` or
            `toyproject.png` instead.
    """
    binary = {"texture", "palette", "audio", "font"}
    if kind in binary:
        raise ValueError(
            f"{kind!r} is not a YAML asset; use toyproject.png (or assets.texture) for it")
    text = yamlwrite.document(data, fmt, header)
    return project.write_text(project.path_for(kind, name), text)
