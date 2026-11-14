"""Scenes: `assets/scenes/<area>/<name>/scene.yaml`.

A scene is a name, optional per-scene settings, and a list of root objects. It is written as a
directory rather than a bare file -- `assets/scenes/terrain/dig_demo/scene.yaml` -- because a
scene usually carries data of its own beside it (a terrain's `.terra`, a painted instance file,
a heightmap PNG), and the engine loads a bare scene name by that layout.

    from toyproject import Project, Scene, Obj

    scene = Scene("my_demo", header="A worked example.\\nRun: ./run.sh my_demo")
    scene.settings = {"loading": Flow({"min_display_time": 4.0})}
    scene.add(Obj("ground").transform(scale=(10, 10, 1)).mesh("plane"))
    scene.add(Obj("sun").transform(position=(0, 0, 20)).directional_light())
    scene.write(Project.find(), "effects/my_demo")

Per-scene `settings:` override `assets/config.yaml` for this scene alone. They are passed
through as given, because the set of sections is the engine's config schema rather than
anything this module models -- see `src/toyengine/core/config.cpp` and the Render settings
table in the editor.
"""

from .. import yamlwrite
from ..yamlwrite import fmt_num


class Scene:
    """A scene asset.

    Args:
        scene_name (str): The scene's own name, which the engine reports and the editor shows.
            Conventionally the same as its directory.
        header (str): Optional comment block for the top of the file. Committed scenes use it
            to say what the scene is for and how to run it, which is worth keeping: a scene
            with no explanation is the hardest asset to pick up later.
        settings (dict): Optional per-scene settings sections.
    """

    def __init__(self, scene_name, header=None, settings=None):
        self.scene_name = scene_name
        self.header = header
        self.settings = dict(settings) if settings else {}
        self.root_objects = []

    def add(self, obj):
        """Appends a root object.

        Args:
            obj (Obj): The object.

        Returns:
            Scene: self.
        """
        self.root_objects.append(obj)
        return self

    def to_text(self, fmt=fmt_num):
        """Renders the whole `scene.yaml`.

        Root objects are separated by a blank line, as the committed scenes are -- with a dozen
        objects in one file it is the only thing that makes the boundaries findable.

        Args:
            fmt (callable): The number format, `fmt_num` or `fmt_g6`.

        Returns:
            str: The file's text, ending in exactly one newline.

        Raises:
            ValueError: If the scene has no root objects. An empty scene loads, but it is
                almost always a generator bug rather than an intention, and the engine would
                show a black frame with no clue why.
        """
        if not self.root_objects:
            raise ValueError(
                f"scene {self.scene_name!r} has no root objects; add at least a camera and a "
                f"light, or the scene loads to a black frame")

        lines = []
        if self.header:
            for line in self.header.rstrip("\n").split("\n"):
                lines.append("# " + line if line.strip() else "#")
        lines.append("scene:")
        lines.append(f"  scene_name: {yamlwrite.fmt_str(self.scene_name)}")
        if self.settings:
            lines.append("  settings:")
            lines += yamlwrite.emit(self.settings, fmt, 4)
        lines.append("  root_objects:")
        for index, obj in enumerate(self.root_objects):
            if index:
                lines.append("")
            lines += yamlwrite.emit([obj.to_yaml()], fmt, 4)
        return "\n".join(lines) + "\n"

    def write(self, project, name=None, fmt=fmt_num):
        """Writes the scene into a project.

        Args:
            project (Project): The project to write into.
            name (str): The scene's path under `assets/scenes`, e.g. `"effects/my_demo"`.
                Defaults to `scene_name`, which puts it at the top of the scenes tree; the
                committed scenes all sit in an area folder instead.
            fmt (callable): The number format.

        Returns:
            pathlib.Path: The `scene.yaml` written.
        """
        return project.write_text(
            project.path_for("scene", name or self.scene_name), self.to_text(fmt))
