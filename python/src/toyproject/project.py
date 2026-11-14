"""A toyengine project on disk: where it is, and where each kind of asset belongs.

A project is self-contained in its `assets/` tree -- scenes, materials, meshes, objects,
textures, UI, animations and the rest are YAML and PNG the engine loads directly, and the only
thing outside that tree is whatever C++ the project adds in `src/`. That is what makes writing
one from Python worthwhile: everything this package emits is loadable without compiling
anything.

## Finding the project

A project root is the directory holding a `.toy` file, which is the same rule `toyhub` and the
project's own `.toyenv.sh` use. `Project.find()` walks up from a starting directory to locate
it, so a generator works regardless of the directory it is run from:

    from toyproject import Project
    project = Project.find()            # from the script's own location, or the cwd

The engine checkout is a project root too for these purposes -- it has an `assets/` tree of the
same shape -- but it has no `.toy`, so pass it explicitly with `Project(path)` when generating
the engine's own demo assets, which is what the scripts under `tools/` do.

## What it does NOT do

It does not create a *new* project's starter assets. `assets/config.yaml`, the first scene and
the cube/plane/sphere meshes come from the editor's own `--new-project`, driven by
`ensure_assets()` in a project's `.toyenv.sh`, so that there is one definition of what a
starter project holds. This package writes assets into a project that already exists.
"""

import pathlib

#: Where each asset kind lives under `assets/`, and the suffix its files carry. The keys are
#: the names used throughout this package and by `write_asset()`.
ASSET_DIRS = {
    "scene": ("scenes", ".yaml"),
    "material": ("materials", ".yaml"),
    "mesh": ("meshes", ".yaml"),
    "object": ("objects", ".yaml"),
    "texture": ("textures", ".png"),
    "ui": ("ui", ".yaml"),
    "ui_theme": ("ui/themes", ".yaml"),
    "animation": ("animations", ".yaml"),
    "rig": ("rigs", ".yaml"),
    "graph": ("graphs", ".yaml"),
    "palette": ("palettes", ".png"),
    "physics_material": ("physics_materials", ".yaml"),
    "damage_material": ("damage_materials", ".yaml"),
    "audio": ("audio", ""),
    "font": ("fonts", ""),
    "shader": ("shaders", ""),
}


class Project:
    """A toyengine project's directory layout.

    Args:
        root (str | pathlib.Path): The project root -- the directory holding its `.toy` file
            and its `assets/` tree. An engine checkout is accepted too.

    Raises:
        NotADirectoryError: If `root` is not a directory.
    """

    def __init__(self, root):
        self.root = pathlib.Path(root).resolve()
        if not self.root.is_dir():
            raise NotADirectoryError(f"{self.root} is not a directory")
        self._written = []

    @classmethod
    def find(cls, start=None):
        """Locates the project containing `start` by walking up to the first `.toy` file.

        Args:
            start (str | pathlib.Path): Where to start looking. Defaults to the current
                working directory.

        Returns:
            Project: The project found.

        Raises:
            FileNotFoundError: If no directory from `start` up to the filesystem root holds a
                `.toy` file. The message names where it looked, because the usual cause is
                running a generator from outside any project.
        """
        here = pathlib.Path(start or pathlib.Path.cwd()).resolve()
        if here.is_file():
            here = here.parent
        for candidate in [here, *here.parents]:
            if any(candidate.glob("*.toy")):
                return cls(candidate)
        raise FileNotFoundError(
            f"no .toy project file in {here} or any parent -- pass Project(path) explicitly "
            f"(an engine checkout has no .toy)")

    @property
    def name(self):
        """str: The project's name, which is always its folder's name."""
        return self.root.name

    @property
    def toy_file(self):
        """pathlib.Path | None: The project's `.toy` file, or None for an engine checkout."""
        found = sorted(self.root.glob("*.toy"))
        return found[0] if found else None

    @property
    def assets(self):
        """pathlib.Path: The project's `assets/` tree. Not required to exist yet."""
        return self.root / "assets"

    @property
    def engine_dir(self):
        """pathlib.Path | None: The engine this project builds against.

        `.libs/toyengine`, which `./setup.sh` points at a shared checkout, a worktree or the
        project's own clone. None when it is not set up (or when this *is* the engine).
        """
        linked = self.root / ".libs" / "toyengine"
        if linked.exists():
            return linked.resolve()
        if (self.root / "src" / "toyengine").is_dir():
            return self.root
        return None

    def dir_for(self, kind):
        """The directory an asset kind lives in.

        Args:
            kind (str): A key of `ASSET_DIRS`, e.g. `"material"`.

        Returns:
            pathlib.Path: The directory, which may not exist yet.

        Raises:
            KeyError: If `kind` is not a known asset kind. The message lists the valid ones.
        """
        if kind not in ASSET_DIRS:
            raise KeyError(f"unknown asset kind {kind!r}; known: {', '.join(sorted(ASSET_DIRS))}")
        return self.assets / ASSET_DIRS[kind][0]

    def path_for(self, kind, name):
        """The file an asset of some kind and name is written to.

        A scene is a directory with a `scene.yaml` inside it, as the engine loads it; every
        other kind is a single file. A `name` may contain slashes to nest, which is how the
        committed assets group materials (`basic/plastic`) and props (`props/tree_branch`), and
        it may already carry the kind's suffix.

        Args:
            kind (str): A key of `ASSET_DIRS`.
            name (str): The asset's logical name, without its suffix.

        Returns:
            pathlib.Path: The destination file.

        Raises:
            ValueError: If `name` is empty or would escape the assets tree.
        """
        if not name or name.startswith("/"):
            raise ValueError(f"asset name must be a relative, non-empty path, got {name!r}")
        if ".." in pathlib.PurePosixPath(name).parts:
            raise ValueError(f"asset name must not leave the assets tree, got {name!r}")
        folder, suffix = ASSET_DIRS[kind]
        if kind == "scene":
            return self.assets / folder / name / "scene.yaml"
        target = self.assets / folder / name
        if suffix and target.suffix != suffix:
            target = target.with_name(target.name + suffix)
        return target

    def write_text(self, path, text):
        """Writes text to `path`, creating parents, and records it as written.

        Args:
            path (str | pathlib.Path): Destination, absolute or relative to the project root.
            text (str): The file's contents.

        Returns:
            pathlib.Path: The path written.
        """
        target = pathlib.Path(path)
        if not target.is_absolute():
            target = self.root / target
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)
        self._written.append(target)
        return target

    def record(self, path):
        """Records a path as written by this project, for `written` and `report()`.

        `write_text` does this itself. Call it after writing a file by another route -- a PNG
        through `toyproject.png`, say -- so the run's report still accounts for it.

        Args:
            path (str | pathlib.Path): The path that was written.

        Returns:
            pathlib.Path: The same path, for chaining.
        """
        resolved = pathlib.Path(path)
        self._written.append(resolved)
        return resolved

    @property
    def written(self):
        """list: Every path written through this project, in order, as `pathlib.Path`.

        A copy: appending to it records nothing. Use `record()` for that.
        """
        return list(self._written)

    def report(self):
        """A one-line-per-file summary of what was written, relative to the project root.

        Returns:
            str: The summary, empty when nothing was written.
        """
        lines = []
        for path in self._written:
            try:
                shown = path.relative_to(self.root)
            except ValueError:
                shown = path
            lines.append(f"wrote {shown}")
        return "\n".join(lines)

    def __repr__(self):
        return f"Project({str(self.root)!r})"
