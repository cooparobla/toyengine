# toyproject -- write toyengine projects from Python

A toyengine project is self-contained in its `assets/` tree. Scenes, objects, materials,
meshes, textures, UI, animations and the rest are YAML and PNG the engine loads directly, and
the only thing outside that tree is whatever C++ the project adds in `src/`. So any program that
can write those files can author a game's content -- no compiler, no engine, no bindings.

That is all this package is: a writer for those files, carrying the schemas and the formatting
conventions the committed assets already use.

**It does not talk to the engine.** There are no bindings and no embedded interpreter, and
nothing in `src/` imports it. It emits files; the engine reads them on its next load.

## Using it

```bash
cd python
uv sync                      # creates .venv and installs toyproject (editable) + numpy
uv run toyproject info       # where am I, and what does this project hold
uv run pytest                # the test suite
```

```python
from toyproject import Project, Scene, Obj, Material

project = Project.find()                       # walks up to the nearest *.toy
Material(albedo=(0.85, 0.15, 0.12), roughness=0.3).write(project, "basic/red_plastic")
(Scene("demo", header="Run: ./run.sh demo")
    .add(Obj("ground").transform(scale=(10, 10, 1)).mesh("plane"))
    .add(Obj("prop").transform(position=(0, 0, 1))
         .mesh("cube", material="materials/red_plastic"))
    .add(Obj("camera").transform(position=(0, -10, 6), rotation=(65, 0, 0)).camera())
    .add(Obj("sun").transform(position=(0, 0, 20)).directional_light())
    .write(project, "effects/demo"))
print(project.report())
```

From a project rather than the engine, reach it through the engine symlink every project
already has -- `.libs/toyengine/python` -- which is what `bin/toypy` in a project does for you.

## What is here

| Module | For |
|---|---|
| `toyproject.project` | `Project`: finding a project, and where each of the 16 asset kinds belongs |
| `toyproject.yamlwrite` | the deterministic YAML emitter, `Flow`, and the number formats |
| `toyproject.png` | 8-bit RGBA and 16-bit greyscale PNGs, with no third-party dependency |
| `toyproject.assets` | typed builders for scenes, objects, materials, meshes and textures, and `write_asset` for every other kind |

Typed builders cover the five kinds the generators under `tools/` actually hammer. The other
eleven go through `write_asset`, which puts the file in the right place and emits it the same
way, leaving the document's shape to the caller. Promoting one to a builder is worth doing when
its key order starts mattering.

## Two things it deliberately will not do

**It does not validate component keys.** There is no reflection in the engine -- every key is
parsed by hand in C++ -- so a misspelled key, or one belonging to a component that was renamed,
is **silently ignored** at load. This package cannot see that. The authority on component keys
is [`.claude/skills/create-scene/reference/components.md`](../.claude/skills/create-scene/reference/components.md);
`Obj`'s convenience methods spell the common ones correctly by construction, and
`Obj.component()` passes anything through.

**It does not create a new project's starter assets.** `assets/config.yaml`, the first scene and
the base meshes come from the editor's own `--new-project`, which a project's `.toyenv.sh` runs
through `ensure_assets()`, so there is one definition of what a starter project holds. This
package writes into a project that already exists.

## Why the YAML is emitted by hand

Assets are reviewed as diffs, so output has to be stable, minimally quoted, and laid out the way
the committed assets are. A general-purpose serializer reorders keys, quotes freely and folds
lines, and that diff noise hides real changes.

The number format is an explicit parameter for the same reason, because the tree genuinely
holds more than one:

| Format | Spells `-1.0` as | Reproduces |
|---|---|---|
| `fmt_num` (default) | `-1.0` | scenes, objects, materials, and `tools/gen_water_grid_mesh.py`'s meshes |
| `fmt_g6` | `-1` | `tools/mesh_yaml.py`'s tile meshes |

`tools/mesh_yaml.py`'s docstring claimed its format matched `gen_water_grid_mesh.py`; it did
not, and the files on disk differ accordingly. `tests/test_mesh_matches_committed.py` pins both
down by regenerating `assets/meshes/water/water_grid.yaml` and comparing it byte for byte, which
is what makes it safe to port the existing generators onto this package: adopting it must not
rewrite a single committed asset.
