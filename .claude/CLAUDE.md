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
2. Unit tier when the suite passes: `ctest --test-dir build -L unit -j4` (~4 s).
3. GPU suites only for systems you touched: `ctest --test-dir build -L gpu -R <system> -j4`.
4. The full suite (`ctest --test-dir build -j4`) once, when the work is finished.

- Run long builds and GPU runs in the background and wait for the completion notice -- don't
  poll. Never run two heavy builds or test runs at once.
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
