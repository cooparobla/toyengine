"""Materials: `assets/materials/<group>/<name>.yaml`.

A material is a flat mapping, and all 29 committed ones open with the same four keys in the
same order -- `albedo`, `metallic`, `roughness`, `ao` -- so those are named parameters here and
everything else is passed through. Materials are grouped in folders (`basic/`, `metal/`,
`natural/`, `transparent/`, ...), but a `MeshRenderer` refers to one by name through the tree
(`materials/plastic` finds `materials/basic/plastic.yaml`), so a group is organisation rather
than part of the reference.

## Keys the committed materials use

Passed through as `**keys`, in the order you give them. This list is what is actually on disk,
not the full set the loader accepts -- the `create-material` skill is the authority, and it
covers the surface-shader and transparency rules that decide whether a key does anything:

| Key | For |
|---|---|
| `shader`, `shader_params` | a derived surface shader (`foliage`, `triplanar`, ...) and its four floats |
| `alpha`, `alpha_mode`, `alpha_cutoff` | `BLEND` glass or `CUTOUT` foliage; a BLEND material with alpha < 1 casts no shadow |
| `refraction`, `ior`, `refraction_thickness`, `refraction_tint` | screen-space refraction, which needs `render.transparency_enabled` |
| `emissive`, `emissive_strength` | a material that glows |
| `texture_albedo`, `texture_normal`, `texture_metallic_roughness`, `texture_alpha_mask` | texture maps |
| `texture_displacement`, `displacement_scale` | tessellated displacement |
"""

from .. import yamlwrite
from ..yamlwrite import Flow, fmt_num
from .node import rgb


class Material:
    """A material asset.

    Args:
        albedo (tuple | Flow): Base colour, as `(r, g, b)` or an already-built colour.
        metallic (float): 0 dielectric .. 1 metal.
        roughness (float): 0 mirror .. 1 fully diffuse.
        ao (float): Baked ambient occlusion multiplier; 1.0 is none.
        header (str): Optional comment block for the top of the file. Worth writing: the
            committed materials use it to record *why* a value is what it is, which is the
            thing a bare number cannot carry.
        **keys: Any further material keys, in output order. See the table above.
    """

    def __init__(self, albedo=(0.8, 0.8, 0.8), metallic=0.0, roughness=0.5, ao=1.0,
                 header=None, **keys):
        self.albedo = albedo
        self.metallic = metallic
        self.roughness = roughness
        self.ao = ao
        self.header = header
        self.keys = dict(keys)

    def to_yaml(self):
        """The material as a mapping ready for `yamlwrite.emit`.

        Returns:
            dict: The four core keys in their committed order, then everything else.
        """
        out = {
            "albedo": self.albedo if isinstance(self.albedo, Flow) else rgb(self.albedo),
            "metallic": self.metallic,
            "roughness": self.roughness,
            "ao": self.ao,
        }
        for key, value in self.keys.items():
            # A colour-shaped tuple is spelled as a colour; a plain list (shader_params) stays
            # a flow sequence, which is how the committed materials write it.
            if key.endswith("_tint") or key == "emissive":
                out[key] = value if isinstance(value, Flow) else rgb(value)
            elif isinstance(value, (list, tuple)):
                out[key] = Flow(list(value))
            else:
                out[key] = value
        return out

    def to_text(self, fmt=fmt_num):
        """Renders the whole material file.

        Args:
            fmt (callable): The number format.

        Returns:
            str: The file's text, ending in exactly one newline.
        """
        return yamlwrite.document(self.to_yaml(), fmt, self.header)

    def write(self, project, name, fmt=fmt_num):
        """Writes the material into a project.

        Args:
            project (Project): The project to write into.
            name (str): Its path under `assets/materials`, e.g. `"basic/plastic"`.
            fmt (callable): The number format.

        Returns:
            pathlib.Path: The file written.
        """
        return project.write_text(project.path_for("material", name), self.to_text(fmt))
