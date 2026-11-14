# bin/ -- this project's executables

Things you run, as opposed to things the engine loads. New executables belong here.

| | |
|---|---|
| [`toypy`](toypy) | Python with this project's asset API (`toyproject`) importable -- a script, `-c '<code>'`, or a REPL |
| [`toyproject`](toyproject) | that API's command line: `info`, `new-scene` |

The six scripts at the project root -- `build.sh`, `run.sh`, `editor.sh`, `setup.sh`,
`clean.sh`, `package.sh` -- stay where they are. `toyhub` and the hub invoke them by those exact
paths (`tools/toyhub` execs `<project>/build.sh` and friends), so they are part of a project's
interface rather than a free choice of layout.

## The asset API

A toyengine project is self-contained in its `assets/` tree: scenes, objects, materials, meshes,
textures, UI and animations are YAML and PNG the engine reads directly, and the only thing
outside it is whatever C++ the project adds in `src/`. So a program that writes those files can
author the project's whole content, with nothing compiled and no engine running.

```bash
bin/toyproject info
bin/toypy -c 'from toyproject import Project, Scene, Obj
p = Project.find()
Scene("demo").add(Obj("sun").transform(position=(0, 0, 20)).directional_light()).write(p, "effects/demo")
print(p.report())'
```

The environment is uv-managed and lives in the engine checkout
(`.libs/toyengine/python/.venv`), created on first use, so every project on the machine shares
one install. The API and its reference are documented in
[`.libs/toyengine/python/README.md`](../.libs/toyengine/python/README.md).

**It will not catch a misspelled component key.** The engine has no reflection -- every key is
parsed by hand in C++ -- so an unknown key is silently ignored at load. The authority on
component keys is the `create-scene` skill's `reference/components.md` in the engine checkout.
