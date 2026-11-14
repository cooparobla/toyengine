# @TARGET@ -- notes for agents

A toyengine game project. The engine's own rules are loaded alongside this file (`.claude/` is a
symlink into the engine checkout); that file is the authority on **engine** work, and this one on
**project** work. Where they disagree about targets, trees or paths, this file wins -- the engine's
examples are written for its own checkout.

## What is yours and what is the engine's

| | |
|---|---|
| `src/` | this project's C++. Compiled into **both** the game and the editor, so the editor's Play runs your components. A `TOY_MODULE()` here registers at static-init and cannot be dead-stripped. |
| `assets/` | this project's assets. **Shadows** the engine's -- a file of the same name wins, and the engine's stays the fallback for shaders, fonts and shared meshes. |
| `tests/` | this project's suites (see below). |
| `.libs/toyengine` | the engine, a compiled library. **A project cannot replace an engine file from `src/`.** To change engine code, edit the checkout behind this symlink -- and read the engine's CLAUDE.md first. |

`cmake/ToyProject.cmake` in the engine is what wires all of that; read it before changing the
project's `CMakeLists.txt`, which is deliberately five lines long.

## Building

**This project's build tree is its own `build/`, and it must stay that way.** `TOY_SHADER_BUILD_DIR`
and `GFX_SHADER_OUTPUT_ROOT` point into the tree that configured them and the runtime resolves
`.spv` files there, so two projects sharing one directory fight over compiled shaders. ccache is
what makes the duplication cheap -- it shares compiled objects across every tree. Verify it per
tree, in one command:

```bash
grep -m1 CMAKE_CXX_COMPILER_LAUNCHER build/CMakeCache.txt   # want: =ccache
```

If it prints nothing, add the launcher **in place**, never by wiping the tree:
`cmake -B build -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`.

| You changed | Build |
|---|---|
| `src/` | `cmake --build build --target @TARGET@_tests -j4` (or `--target @TARGET@` to just compile) |
| the game | `cmake --build build --target @TARGET@ -j4` |
| the editor | `cmake --build build --target @TARGET@_editor -j4` -- **required before any manual check**, since a stale editor silently drops a new component |
| `assets/shaders/*.glsl\|.frag\|.vert` | `cmake --build build --target @TARGET@_shaders -j4`. A `.glsl` edit does nothing until this runs, and the `.spv` the runtime loads can be days stale. |

- **`-j4`, never a bare `-j`.** A bare `-j` is one job per core; the heaviest engine TU peaks at
  ~2.4 GB, so a high job count does not OOM-kill, it wedges the desktop.
- `./build.sh` picks its own job count -- one per core capped at one per 3 GB of RAM -- so pass
  `./build.sh -j4` or `JOBS=4`.
- Never `cmake --build build` with no target while iterating: that builds every executable.
- A new `.cpp` is picked up automatically (`GLOB_RECURSE CONFIGURE_DEPENDS`). Never delete `build/`
  or reconfigure "to be safe".

## Testing

Suites live in `tests/{unit,gpu,editor_gpu}/<stem>_test.cpp`, written against the engine's
`coopa/testing/test.h`, **one suite per file**. Adding a file needs no CMake edit -- the directories
are globbed -- and the engine's own fixtures are reachable by the include its suites use
(`"engine/support/scene_variant.h"`, `"editor/support/editor_session.h"`).

**The file stem IS the suite name**: `foo_test.cpp` must declare `COOPA_TEST_SUITE("foo")`. A
mismatch breaks the run; a duplicate stem breaks the CMake configure.

```bash
./build/tests/@TARGET@_tests --suite <stem>          # one suite, directly -- always start here
ctest --test-dir build -L unit -j8                   # the unit tier
ctest --test-dir build -L gpu -R <suite> -j8         # GPU suites, filtered to what you touched
ctest --test-dir build -j8                           # everything, once, when the work is done
```

**`-j8` for ctest, and it is not the same unit as a build's `-j4`.** ctest's `-j` is a weight
budget it divides by each test's `PROCESSORS`, and every GPU suite declares `PROCESSORS 4`, so
`-j8` runs 8/4 = *two* GPU tests at once -- the bound `SLOT gpu:2` already enforces machine-wide,
shared with the engine's suites. Anything below `-j8` silently serialises the whole GPU tier.

`ctest` never builds. It runs whatever binary is on disk, and `@TARGET@_tests` does not build
`@TARGET@_editor_tests`, so a suite can pass from an hours-old runner -- compare `stat -c %y` on
the source, the `.o` and the runner before believing a green or a red.

**Stage the work: test at stage boundaries, not after every edit.** Make every edit in a stage
(headers first, and finish all of them -- editing a header mid-build leaves some TUs compiled
against the old layout, and the ODR mismatch surfaces as a segfault in an *unrelated* test), build
once at the narrowest target, test narrowest-first, and only then start the next stage. Never build
or test after a comment-only or doc-only change.

## One heavy job at a time

A build, a test run, or an app run -- never two. Never detach a process (`nohup`, `disown`,
`setsid`, a trailing `&`): a detached job outlives the session so neither you nor the harness can
stop it. Use the Bash tool's `run_in_background: true` instead. A `PreToolUse` hook in the engine
(`tools/claude_build_guard.py`) enforces both, and derives this project's binary names from the
`target:` in its `.toy` file.

## Adding a component

There is no reflection -- every step is manual, and skipping one fails silently in a different way:

1. the component in `src/`, 2. a parser registered from your `TOY_MODULE()` (never the engine's
`register.cpp`), 3. the system that updates it, at the right `UpdatePhase`, 4. a `ComponentSchema`
so the inspector shows real widgets, 5. a test in `tests/unit/`.

Then **rebuild `@TARGET@_editor`**: scenes are played in the editor, and a stale editor binary
drops the new component with no error.

## Seams: how a project reaches into the engine

`src/toyengine/core/module.h` is the entry point -- `TOY_MODULE()` for the common case, or a
`register_module({...})` literal when you need `on_render_config` (it runs *before* every module
body). The three moments are `on_render_config` (surface shaders, render settings), `on_engine_init`
(parsers, hooks -- after the engine's own parsers, before the default scene) and `on_prepare_scene`
(systems, once per scene, after every built-in one, so replacing a builtin is safe here).

The engine publishes named seams for the rest -- terrain shape and streaming policy, world bake,
which way is up, the sky and cloud backdrop, terrain keys it does not model. The engine's CLAUDE.md
has the current table; `nullptr` is always the engine's own default, so a seam costs nothing when
unused.

## Assets

Authoring one asset well is what the engine's `create-*` skills are for; they apply here unchanged,
and their Verify step renders **this** project. Writing many at once -- a level from a seed, a
hundred materials from a table -- is `bin/toypy` and `bin/toyproject`, the Python asset API in the
engine's `python/`. A misspelled component key is silent either way: the authority is the
`create-scene` skill's `reference/components.md`.

```bash
HEADLESS=1 MAX_FRAMES=120 ./build/@TARGET@ <scene>       # headless, never a visible window
./run.sh <scene> --headless --frames 120                 # the same, through the wrapper
```
