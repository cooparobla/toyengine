"""The `toyproject` command: inspect a project, or write a starter scene into it.

Deliberately small. The package is a library for other programs to write projects with, and
this is the thin bit of it worth having on a command line -- `info` to confirm a generator will
find the project you think it will, and `new-scene` as a runnable example of the library that
produces something loadable.

    bin/toyproject info
    bin/toyproject new-scene effects/my_demo
"""

import argparse
import sys

from . import Material, Obj, Project, Scene, vec3
from .project import ASSET_DIRS


def cmd_info(args):
    """Prints where the project is and what its assets tree holds."""
    project = Project(args.path) if args.path else Project.find()
    print(f"project   {project.name}")
    print(f"root      {project.root}")
    print(f"toy file  {project.toy_file or '(none -- an engine checkout)'}")
    print(f"engine    {project.engine_dir or '(not set up; run ./setup.sh)'}")
    print(f"assets    {project.assets}"
          f"{'' if project.assets.is_dir() else '   (does not exist yet)'}")
    if not project.assets.is_dir():
        print("\nA project with no assets/ has not been opened yet: ./editor.sh or ./run.sh "
              "creates the starter tree (config.yaml, a scene and the base meshes).")
        return 0
    print("\nkind                files")
    for kind in sorted(ASSET_DIRS):
        folder = project.dir_for(kind)
        if not folder.is_dir():
            continue
        count = sum(1 for p in folder.rglob("*") if p.is_file())
        print(f"  {kind:<18} {count:>5}  {folder.relative_to(project.root)}")
    return 0


def cmd_new_scene(args):
    """Writes a loadable starter scene: ground, a prop, an orbit camera and a sun."""
    project = Project(args.path) if args.path else Project.find()
    name = args.name
    scene_name = name.rsplit("/", 1)[-1]

    if not args.no_material:
        Material(albedo=(0.85, 0.15, 0.12), roughness=0.3,
                 header=f"Made by `toyproject new-scene {name}`.").write(
                     project, f"basic/{scene_name}_prop")

    scene = Scene(scene_name, header=(
        f"{scene_name} -- made by `toyproject new-scene {name}`.\n"
        f"\n"
        f"Run: ./run.sh {scene_name}"))
    scene.add(Obj("ground")
              .transform(position=(0, 0, 0), rotation=(0, 0, 0), scale=(10, 10, 1))
              .mesh("plane"))
    prop = Obj("prop").transform(position=(0, 0, 1), rotation=(0, 0, 0), scale=(1, 1, 1))
    if args.no_material:
        prop.mesh("cube")
    else:
        prop.mesh("cube", material=f"materials/{scene_name}_prop")
    scene.add(prop)
    scene.add(Obj("camera")
              .transform(position=(0, -10, 6), rotation=(65, 0, 0), scale=(1, 1, 1))
              .camera()
              .component("CameraController", mode="orbit", target=vec3((0, 0, 0.75)),
                         min_distance=2.0, max_distance=40.0))
    scene.add(Obj("sun")
              .transform(position=(0, 0, 20), rotation=(0, 0, 0), scale=(1, 1, 1))
              .directional_light())
    scene.write(project, name)
    print(project.report())
    return 0


def main(argv=None):
    """Runs the command line.

    Args:
        argv (list): Arguments, defaulting to `sys.argv[1:]`.

    Returns:
        int: A process exit status.
    """
    parser = argparse.ArgumentParser(
        prog="toyproject", description=__doc__.split("\n")[0])
    parser.add_argument("--path", help="the project root (default: found from the cwd)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("info", help="where the project is, and what its assets tree holds")

    new_scene = sub.add_parser("new-scene", help="write a loadable starter scene")
    new_scene.add_argument("name", help="path under assets/scenes, e.g. effects/my_demo")
    new_scene.add_argument("--no-material", action="store_true",
                           help="use the engine's default material instead of writing one")

    args = parser.parse_args(argv)
    try:
        if args.command == "info":
            return cmd_info(args)
        return cmd_new_scene(args)
    except (FileNotFoundError, NotADirectoryError, ValueError, KeyError) as exc:
        print(f"toyproject: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
