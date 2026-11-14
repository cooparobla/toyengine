"""Write toyengine projects and their assets from Python.

A toyengine project is self-contained in its `assets/` tree -- scenes, objects, materials,
meshes, textures, UI, animations and the rest are YAML and PNG the engine loads directly, and
the only thing outside it is whatever C++ the project adds in `src/`. So a program that can
write that tree can author a whole game's content without compiling anything and without
talking to the engine at all. That is all this package is: a writer for those files, with the
schemas and the formatting conventions of the committed assets built in.

**It does not talk to the engine.** No bindings, no embedded interpreter, nothing imports it
from C++. It emits files; the engine reads them on its next load.

    from toyproject import Project, Scene, Obj, Material

    project = Project.find()
    Material(albedo=(0.85, 0.15, 0.12), roughness=0.3).write(project, "basic/red_plastic")
    (Scene("demo", header="Run: ./run.sh demo")
        .add(Obj("ground").transform(scale=(10, 10, 1)).mesh("plane"))
        .add(Obj("prop").transform(position=(0, 0, 1))
             .mesh("cube", material="materials/red_plastic"))
        .add(Obj("camera").transform(position=(0, -10, 6), rotation=(65, 0, 0)).camera())
        .add(Obj("sun").transform(position=(0, 0, 20)).directional_light())
        .write(project, "effects/demo"))
    print(project.report())

## What it will not check for you

There is no reflection in the engine: every component key is parsed by hand in C++. A
misspelled key, or one belonging to a component that has been renamed, is **silently ignored**
at load -- this package cannot see that and does not pretend to. The authority on component
keys is `.claude/skills/create-scene/reference/components.md`; the convenience methods on `Obj`
spell the common ones correctly by construction, and `component()` passes anything through.

## Where to look

| Module | For |
|---|---|
| `toyproject.project` | `Project`: finding a project, and where each asset kind belongs |
| `toyproject.yamlwrite` | the deterministic YAML emitter, `Flow`, and the number formats |
| `toyproject.png` | 8-bit RGBA and 16-bit greyscale PNGs, dependency-free |
| `toyproject.assets` | the typed builders, and `write_asset` for every other kind |
"""

from .assets import (
    Component,
    Material,
    Mesh,
    Obj,
    ObjectAsset,
    Scene,
    face_normal,
    perpendicular_tangents,
    rgb,
    vec3,
    vertex_normals,
    write_asset,
    write_heightmap,
    write_mesh,
    write_texture,
)
from .project import ASSET_DIRS, Project
from .yamlwrite import Flow, flow, fmt_g6, fmt_num

__version__ = "0.1.0"

__all__ = [
    "ASSET_DIRS",
    "Component",
    "Flow",
    "Material",
    "Mesh",
    "Obj",
    "ObjectAsset",
    "Project",
    "Scene",
    "face_normal",
    "flow",
    "fmt_g6",
    "fmt_num",
    "perpendicular_tangents",
    "rgb",
    "vec3",
    "vertex_normals",
    "write_asset",
    "write_heightmap",
    "write_mesh",
    "write_texture",
]
