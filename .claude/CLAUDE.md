# toyengine -- notes for agents

## Building and testing during changes

The engine (`toyengine/`) and editor (`editor/`) are static libraries; `libs/` is header-only.
A targeted rebuild is cheap, a broad one is not -- keep both rare and narrow.

| Change | Typical rebuild |
|---|---|
| an engine/editor `.cpp` | ~5 s |
| a test `.cpp` | ~10 s |
| a header `engine.h` / `editor_app.h` pulls in | ~2 min (~40 files) |
| everything, clean | ~7.5 min |

**Use ccache.** Both build trees compile the same engine sources (a game project does
`add_subdirectory(.libs/toyengine)`, so `habitat/build/` has its own copy of all ~101 engine
objects), and the widely-included headers -- `toy_render_config.h`, `light_data.h`,
`toy_render_pipeline.h` -- invalidate nearly every TU when touched. ccache turns the second
compile of an unchanged TU into a copy, which is most of what these rebuilds are.

```bash
sudo apt install -y ccache                      # once
ccache -M 20G                                   # once; the engine churns a lot of objects
cmake -B build -DCMAKE_CXX_COMPILER_LAUNCHER=ccache   # per build dir, including a game's
ccache -s                                       # hit rate; -z to zero the counters
```

Already-configured build dirs need the launcher added to their cache, not a wipe:
`cmake -B build -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` reconfigures in place and keeps every
existing object. A reverted edit, a branch switch, or the same header touched twice then all
come back from the cache instead of recompiling.

**Do NOT try to share one build directory between the engine and a game project.** Each tree
needs its own: `TOY_SHADER_BUILD_DIR` and `GFX_SHADER_OUTPUT_ROOT` point at that tree's
`build/shaders/`, and the runtime resolves `.spv` files there, so two projects sharing a
directory would fight over compiled shaders. ccache is what makes the duplication cheap --
it shares the compiled OBJECTS across every tree while each keeps its own outputs. That is
why `TOY_SHADER_BUILD_DIR` is scoped to `runtime_paths.cpp` instead of being PUBLIC on
`toyengine_engine` (see the comment at its definition): PUBLIC put a per-tree `-D` on every
TU's command line and made them all cache-unique, which defeated sharing entirely.

**Iterate in ONE tree.** `toyengine/build` has the whole test suite, so engine work belongs
there; build a game project only when you actually need to run or profile that game. Building
both for the same change costs two full engine compiles (~101 objects each) for nothing.

Two things that defeat it, both avoidable: `touch`ing headers to force a rebuild (the content
is unchanged, so prefer fixing the real dependency problem), and `__DATE__`/`__TIME__`-style
macros (none here).

**Build only what you will run, always `-j4`** (more jobs has panicked this box):

| You changed | Build |
|---|---|
| `toyengine/` | `cmake --build build --target toyengine_tests -j4` (or `toyengine_engine` to just compile) |
| `editor/` | `cmake --build build --target toyengine_editor_tests -j4` (or `toyengine_editor_lib`) |
| the game / editor app | `--target toyengine` / `--target toyengine_editor` |

- Never `cmake --build build` with no target while iterating -- that builds every executable.
- `.vscode/settings.json` pins `cmake.parallelJobs` to 4 -- do not delete it. The CMake Tools
  default of `0` is one job per core (20 here), and at ~2 GB per compiler that OOM-kills the
  whole VS Code session, not just the build.
- **Measured 2026-10-24 on the 31 GB / 20-core box: the heaviest TU
  (`toyengine/render/toy_render_pipeline.cpp`) peaks at 2.4 GB RSS over ~25 s.** With only a
  2 GB swapfile, `-j20` demands up to ~48 GB and **hard-rebooted the machine twice** -- not a
  graceful OOM kill. `-j6` worst-cases at ~14 GB and leaves ~17-25 GB free; a full
  all-targets build takes ~7.5 min, engine+tests only ~3.5 min. If you must go higher, run
  the build inside a cgroup so an overshoot kills the build and not the session:
  `systemd-run --user --scope -p MemoryMax=20G -p MemorySwapMax=0 cmake --build build -j6 --target ...`
  (`MemorySwapMax=0` is deliberate: fail fast rather than freeze on the tiny swapfile).
- Compile-check one file without linking anything:
  `make -C build toyengine/water/water_system.cpp.o`, `make -C build/tests engine/gpu/cloth_test.cpp.o`.
- Batch your edits, then build once. Don't rebuild after each small edit, and never after a
  comment- or doc-only change.
- Prefer changes in `.cpp` files. Finish all header edits before building -- a header the
  `Engine`/`EditorApp` headers include rebuilds most test files.
- Never `--target clean`, delete `build/`, or reconfigure "to be safe". A new `.cpp` is picked up
  automatically (GLOB_RECURSE CONFIGURE_DEPENDS).

**Test the narrowest thing first.** The runners are `build/tests/toyengine_tests` and
`build/tests/toyengine_editor_tests`; a suite is the name in a file's `COOPA_TEST_SUITE("...")`.

1. One suite: `./build/tests/toyengine_tests --suite water_waves` (or
   `ctest --test-dir build -R water_waves`).
2. Unit tier when the suite passes: `ctest --test-dir build -L unit -j2` (~4 s).
3. GPU suites only for systems you touched: `ctest --test-dir build -L gpu -R <system> -j2`.
4. The full suite (`ctest --test-dir build -j2`) once, when the work is finished.

   `-j2`, not more: each GPU test constructs a whole Vulkan device, so `ctest -j2` is already
   two of them at once. See **Never wedge the machine**.

- Run long builds and GPU runs in the background and wait for the completion notice -- don't
  poll. See **Never wedge the machine** below for how, and for the one way that is not allowed.
- **Never edit a header while a build is in flight.** Growing a struct mid-build leaves some
  TUs compiled against the old layout and some against the new, and the ODR mismatch surfaces
  as a segfault in an *unrelated* test (e.g. `toyengine_app_config` crashing inside
  `std::map`'s constructor during `ToyRenderConfig`'s ctor, only ~20 KB deep in the stack --
  so not a stack overflow). Diagnose by comparing `stat -c %y` on the header against the
  `.o` files. Batch edits between builds, as the rule above already says.
- `-L gpu` with no `-R` filter sweeps in every `toyengine_editor_*` UI test; several take
  50-90 s each (~20 min total) and none of them exercise engine systems. Filter to what you
  touched, and keep the full sweep for the end.
- Avoid wait-loops whose own command line matches their own pattern:
  `until ! pgrep -f "cmake --build"` matches the waiter itself and spins forever, and
  `pkill -f "cmake --build"` takes out every sibling waiter too. Match `cc1plus` / `-x cmake`,
  or poll for a sentinel line the build appends to its log.
- Headless only, never a visible window: `HEADLESS=1 MAX_FRAMES=120 ./build/toyengine <scene>`.

## Never wedge the machine

**This box has hard-locked three times.** Not an OOM kill -- a frozen desktop and a power
cycle, losing whatever was unsaved. The arithmetic is why: 31 GB of RAM behind a **2 GB**
swapfile, one compiler TU peaking at 2.4 GB, and one GPU test constructing a full Vulkan device
plus a 20-thread JobEngine. Overcommit here does not degrade, it stops.

Four rules. A `PreToolUse` hook (`tools/claude_build_guard.py`, wired in `.claude/settings.json`)
enforces the first three, because this section existed in prose before and prose was not enough.

1. **Never detach a process.** No `nohup`, no `disown`, no `setsid`, no trailing `&`. A detached
   job outlives the session, so neither you nor the harness can stop it -- four orphaned `ctest`
   sweeps stacked up exactly this way and took the machine down. Use the Bash tool's own
   **`run_in_background: true`**: the harness tracks it, notifies you when it exits, and tears it
   down with the session.

2. **One heavy job at a time.** A build, a test run, or an app run -- never two, and never a
   manual `./build/toyengine*` run while `ctest` is going. Before starting one, if you are not
   certain the machine is idle:

   ```bash
   pgrep -x 'cc1plus|ctest|toyengine_tests|toyengine_edito|toyengine'   # anything = wait
   ```

   `pgrep -x` matches the process NAME. **Never `pgrep -f`** for these -- it matches the command
   line of the shell about to run your own command, so it always "finds" a false positive. Same
   self-match trap the wait-loop bullet above warns about.

3. **`-j4` for builds, `-j2` for ctest, and never a bare `-j`.** A bare `-j` is one job per core
   (20 here). Measured 2026-10-24: `-j20` demanded ~48 GB and hard-rebooted the box twice; `-j6`
   worst-cases at ~14 GB. If another session might be building too, drop to `-j2`.

4. **Clean up before you walk away.** A sweep you started is yours to finish or stop. If a turn
   ends with one running, say so. If you find an orphan from an earlier session, kill it before
   starting anything.

**Why GPU tests are the expensive ones.** Each constructs an `Engine`: a Vulkan device, render
targets, and a 20-worker JobEngine. `ctest -j2` is therefore *two* of those at once, which is
already the ceiling -- adding a build or a manual engine run on top is what crosses it. The
editor UI suites are also slow (50-110 s each), so the full sweep is ~20 minutes; start it once,
at the end, and let it run alone.

**If the guard blocks you**, it is telling you the machine is busy or the command would outlive
the session. Do not work around it with a different spelling -- wait, or use `run_in_background`.
To disable it deliberately, the human edits `.claude/settings.json` or uses `/hooks`.

---

# Where does this change go?

A map from "I need to change X" to the files, in order, and the narrowest build + test command.
The section above is still the authority on *how* to build; this one is about *where*.

## 1. In-game content (no C++, no rebuild)

Scenes, materials, meshes, prefabs, UI, particles, terrain, water, animation are **assets** —
YAML and PNG the engine loads directly. Each has a skill; use it rather than hand-writing:

| Want | Skill |
|---|---|
| a scene, level, or a test scene for a feature | `create-scene` (also holds the full component key reference) |
| a material / look | `create-material` · a texture `create-texture` · a palette or LUT `create-palette-or-lut` |
| a mesh, collider mesh, LODs | `create-mesh` · a reusable prop `create-object` |
| terrain | `create-terrain` (tile/voxel) · `create-heightmap-terrain` (sculptable landscape) |
| water, lakes, rivers, buoyancy | `create-water-body` |
| fog, god rays, light shafts | `create-fog-volume` |
| particles, fire, smoke, rain | `create-particle-effect` |
| HUDs, menus, in-world labels | `create-ui` · restyle with `create-ui-theme` |
| clips, rigs, Animator, IK | `create-animation` |
| a custom surface look GLSL can express but PBR can't | `create-surface-shader` |
| physics friction / bounciness | `create-physics-material` |

**The one rule that crosses into C++:** a *new component key* is not an asset change. It needs
section 2.

## 2. Engine changes

### Adding a component (the full chain)

There is no reflection. Each step is manual, and skipping one fails silently in a different way:

| # | Edit | Skipping it means |
|---|---|---|
| 1 | the component itself in `toyengine/<area>/` (header declares, `.cpp` has bodies) | — |
| 2 | a parser in `toyengine/scene/register.cpp`, or the area's own `*_yaml.cpp` | the key in scene YAML is silently ignored |
| 3 | the system that updates it, at the right `UpdatePhase` order | it parses but never runs |
| 4 | a `ComponentSchema` in `editor/schema/component_schema.cpp`, and -- over ~10 keys -- a layout in `component_sections.cpp` | the inspector shows raw generic widgets, or one flat wall of them |
| 5 | the key table in the `create-scene` skill's `reference/components.md` | the next agent can't find it |
| 6 | a unit test in `tests/engine/unit/` | — |

Update order matters: a kinematic body driven at the default `Behaviour` (200) is a frame stale
to physics (100). See `toyengine/scene/README.md`'s Conventions.

**A game project's own component** goes through `TOY_MODULE()` (`toyengine/core/module.h`) in
its `src/`, never `register.cpp`.

Build: `cmake --build build --target toyengine_tests -j4` · test: `ctest -L unit -j4`.
**Then rebuild `toyengine_editor`** — scenes are played in the editor, and a stale editor
binary silently drops the new component.

### Other engine areas

| Change | Files | README |
|---|---|---|
| terrain (tile or heightmap), snow | `toyengine/world/` | [world](../toyengine/world/README.md) |
| water, waves, buoyancy | `toyengine/water/` | [water](../toyengine/water/README.md) |
| weather, sky, atmosphere (the GLOBAL environment) | `toyengine/weather/` | [weather](../toyengine/weather/README.md) |
| LOCAL environmental influences -- a rotor's downwash, a vent, a sword swing gusting foliage; the `EnvField` every wind reactor samples | `toyengine/env/` | [env](../toyengine/env/README.md) |
| painted object instances -- brush-placed props, instanced rendering, streamed collision | `toyengine/instance/` | [instance](../toyengine/instance/README.md) |
| particles | `toyengine/particles/` | [particles](../toyengine/particles/README.md) |
| save slots, `ISaveable` | `toyengine/save/` | [save](../toyengine/save/README.md) |
| audio, music, buses | `toyengine/audio/` | [audio](../toyengine/audio/README.md) |
| the F3 overlay, `toy::debug::watch()` | `toyengine/debug/` | [debug](../toyengine/debug/README.md) |
| mesh fracture | `toyengine/destruct/` | [destruct](../toyengine/destruct/README.md) |
| engine lifecycle, scene loading, config | `toyengine/core/` | [core](../toyengine/core/README.md) |
| vehicles -- suspension, tyres, drivetrain, steering | `libs/physxcoopa/physxcoopa/vehicle/` (header-only; the `Vehicle` component and `install_vehicle_system()` live in that library too, not in `toyengine/`) | [vehicle](../libs/physxcoopa/physxcoopa/vehicle/README.md) |

## 3. Render changes

See [`toyengine/render/README.md`](../toyengine/render/README.md) and
[`render/passes/README.md`](../toyengine/render/passes/README.md) first — they explain the frame
graph and the startup-fixed-vs-runtime toggle rule, which is the thing most likely to catch you.

| Change | Where |
|---|---|
| **a new config key** | `render/toy_render_config.h` (the field + its doc) → `toy_render_config.cpp` if a `*_quality` tier should set it → `core/config.cpp` (parse) → `editor/schema/settings_schema.cpp` (the inspector row) |
| **a new pass** | `render/passes/` for the pass class, then the matching `build_*_passes_()` and `record_*()` in `toy_render_pipeline.cpp` |
| **a new debug channel** | the `DebugView` enum + `parse_debug_view()` in `toy_render_config.h/.cpp`, and `assets/shaders/debug_view.frag` |
| **a surface shader** | register it in `Engine`'s `rc.surface_shaders.add({...})` block (`core/engine.cpp`), with the GLSL in `assets/shaders/`; the `create-surface-shader` skill covers the asset side |
| **shadows, lights** | `render/local_shadow_atlas.*`, `render/shadow_fit.h` (cascade splits, clipmap radii, the frustum fits), the `*_shadow_*` methods in the pipeline |

**Three traps, all of which look like "the shader is broken":**

1. **A `*_quality` tier goes inert the moment `config.yaml` writes one of its keys.** Explicit
   keys win over the preset, by design. Prove a new tier with a low-vs-ultra pixel diff, not by
   reading the parsed config.
2. **Startup-fixed vs runtime.** A toggle that selects *which image a pass reads* is baked into
   a descriptor at construction and cannot change at runtime; one that only changes
   push-constant contents is free per frame. Getting this wrong gives you a toggle that appears
   to do nothing.
3. **Schema defaults must equal engine defaults.** `tests/editor/unit/render_settings_schema_test.cpp`
   asserts it field by field.

Build: `--target toyengine_tests -j4` (`toy_render_pipeline.cpp` is the heaviest TU — 2.4 GB, ~25 s).
Test: `ctest -L gpu -R <system> -j4`, filtered.

## 4. Shader changes

```bash
cmake --build build --target toyengine_shaders    # ALWAYS, after any .glsl/.frag/.vert edit
```

**Two traps that cost hours each:**

- **`assets/shaders/` is what loads.** `libs/gfxcoopa/assets/shaders/` has same-named files;
  editing only those is a silent no-op for engine shaders. (gfxcoopa's own shaders do live
  there — the shared bodies under `assets/shaders/gfx/` are the ones to watch.)
- **A `.glsl` edit does nothing until the target is rebuilt.** The `.spv` the runtime loads can
  be days stale, and nothing warns you.

Layout: `assets/shaders/*.frag|vert|tesc|tese` are the entry points;
`assets/shaders/gfx/*.glsl` are shared bodies (`*_body.glsl` are included into a pass's entry
point); `assets/shaders/gfx/surface/` is the G-buffer / shadow / transparent backbone that
derived surface shaders hook via `GFX_SURFACE_*` macros.

## 5. Editor changes

Start at [`editor/README.md`](../editor/README.md) (the developer tour) and
[`editor/app/README.md`](../editor/app/README.md) (frame flow and the panel arrangement).

| Kind of change | Go to |
|---|---|
| **a viewport tool** — gizmos, modal G/R/S, camera, picking | [`editor/viewport/`](../editor/viewport/README.md) |
| **a modelling / sculpt / paint operation** | [`editor/mesh/`](../editor/mesh/README.md) — pure functions over `EditMesh`, unit-tested |
| **a terrain brush** | [`editor/terrain/`](../editor/terrain/README.md) — pure functions over the terrain data |
| **an instance-painting brush** | [`editor/instance/`](../editor/instance/instance_brush.h) — pure functions; the mode itself is `editor/app/ui/instance_paint.h` |
| **visual presentation** — editor colours, metrics, chrome | `editor/app/editor_theme.h`, `editor/themes/`, [`editor/ui/`](../editor/ui/README.md) |
| **a component inspector** — rows, widgets, ranges, defaults, sections | [`editor/schema/`](../editor/schema/README.md) ← *this is the one people mean by "upgrade the inspector"*. Keys in `component_schema.cpp`, their grouping in `component_sections.cpp`, hand-written blocks in `sections/` via `register_inspector_section()` |
| **a settings screen** (Render / Project Settings) | `editor/schema/settings_schema.cpp` |
| **a panel's contents** | `editor/app/ui/<panel>.cpp` (+ its `.h` for new members) -- one class per panel |
| **the UI designer** | `editor/app/ui/ui_canvas.cpp`, `editor/ui/ui_canvas_math.h`, `editor/viewport/rect_gizmo.h` |
| **the animation timeline** | `editor/app/ui/timeline.cpp`, [`editor/anim/`](../editor/anim/README.md) |
| **documents, undo, YAML access** | [`editor/core/`](../editor/core/README.md) |
| **build / packaging** | [`editor/build/`](../editor/build/README.md) |
| **the user manual** | the `editor-user-docs` skill; screenshots come from `tests/editor/docs/docs_test.cpp` |

```bash
cmake --build build --target toyengine_editor_lib -j4   # compile-check
cmake --build build --target toyengine_editor -j4       # before ANY manual check
ctest --test-dir build -L unit -j4
ctest --test-dir build -L gpu -R "editor_<area>" -j4    # filtered: these take 50-90 s each
```

**Prefer `editor/<module>/` over `editor/app/`.** A new operation belongs in a pure,
unit-testable function in `mesh/`, `terrain/`, `viewport/` or `core/`, with the panel doing only
input, layout and undo. That is why those directories carry the editor's test coverage and
`app/ui/` carries almost none.

## 6. Hub changes

See [`hub/README.md`](../hub/README.md).

| Change | Where |
|---|---|
| a page, dialog, the log drawer | `hub/hub_app.h` |
| the project list, hub settings, `~/.toyengine/` | `hub/hub_core.h` |
| **what a project operation actually does** | `tools/toyhub` — the shell script the hub drives. The hub must never duplicate project logic. |
| what `toyhub new` creates | `templates/project/` |

```bash
cmake --build build --target toyengine_hub -j4
HEADLESS=1 MAX_FRAMES=30 HUB_SCREENSHOT=/tmp/hub.png HUB_PAGE=projects ./build/toyengine_hub
```

## 7. Running a game headlessly

```bash
HEADLESS=1 MAX_FRAMES=120 ./build/toyengine <scene>
```

Useful env vars: `FIXED_DT` (0 makes captures byte-identical), `NO_INPUT=1`,
`CAPTURE_FRAMES` / `CAPTURE_RING`, `PROFILE`, `TOY_VALIDATION=0` (the Vulkan validation layer
badly skews CPU profiles in dev builds).
