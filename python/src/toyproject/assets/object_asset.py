"""Object assets (prefabs): `assets/objects/<group>/<name>.yaml`.

An object asset is one `Obj` tree under a top-level `object:` key -- the same node model a
scene's `root_objects` hold, which is why a subtree can move between the two unchanged. A scene
instances one by reference, and may override per instance.

    from toyproject import Project, ObjectAsset, Obj

    branch = Obj("TreeBranch").transform(position=(0, 0, 0)).mesh(
        "props/procgraph_branch", material={"albedo": rgb((0.32, 0.23, 0.16)),
                                            "roughness": 0.95})
    ObjectAsset(branch).write(Project.find(), "props/tree_branch")
"""

from .. import yamlwrite
from ..yamlwrite import fmt_num


class ObjectAsset:
    """A reusable object tree.

    Args:
        root (Obj): The asset's root object. Its `name` is the prefab's name as the editor
            shows it, conventionally CamelCase in the committed assets.
        header (str): Optional comment block for the top of the file.
    """

    def __init__(self, root, header=None):
        self.root = root
        self.header = header

    def to_text(self, fmt=fmt_num):
        """Renders the whole object file.

        Args:
            fmt (callable): The number format.

        Returns:
            str: The file's text, ending in exactly one newline.
        """
        return yamlwrite.document({"object": self.root.to_yaml()}, fmt, self.header)

    def write(self, project, name, fmt=fmt_num):
        """Writes the object asset into a project.

        Args:
            project (Project): The project to write into.
            name (str): Its path under `assets/objects`, e.g. `"props/tree_branch"`.
            fmt (callable): The number format.

        Returns:
            pathlib.Path: The file written.
        """
        return project.write_text(project.path_for("object", name), self.to_text(fmt))
