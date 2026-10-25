# tests/

Written against libcoopa's `coopa/testing/test.h`, **one suite per file**. Adding a test file
needs no CMake edit — the directories are globbed.

```
tests/<area>/support/     fixtures and helpers shared by that area's suites (not tests)
tests/<area>/unit/        no Vulkan device, no window: milliseconds.   ctest label `unit`
tests/<area>/gpu/         constructs an Engine (device, pipelines, a scene).  label `gpu`
tests/<area>/extended/    image-quality metrics and perf budgets -- tuned thresholds worth
                          running when working on that system, not on every change.
                          Built and registered only with -DTOY_EXTENDED_TESTS=ON.  label `extended`
```

`<area>` is `engine` or `editor`; each builds one executable
(`build/tests/toyengine_tests`, `build/tests/toyengine_editor_tests`).

## Running the narrowest thing first

```bash
./build/tests/toyengine_tests --suite water_waves   # one suite, directly
ctest --test-dir build -R water_waves               # the same, through ctest
ctest --test-dir build -L unit -j4                  # the whole unit tier (~4 s)
ctest --test-dir build -L gpu -R <system> -j4       # GPU suites for one system
ctest --test-dir build -j4                          # everything, when the work is done
```

Filter the GPU tier. `-L gpu` with no `-R` sweeps in every `toyengine_editor_*` UI suite;
several take 50-90 s each (~20 min total) and none of them exercise engine systems.

## Two rules that bite

**The file stem IS the suite name.** `ctest` runs `--suite <stem>`, so
`foo_test.cpp` must declare `COOPA_TEST_SUITE("foo")`. A mismatch breaks the run; a duplicate
stem breaks the CMake configure.

**GPU suites are resource-capped on purpose.** Each holds a device, every pipeline and a loaded
scene in memory the GPU shares with the CPU. Unbounded, concurrent runs have exhausted a 16 GB
machine into a kernel watchdog panic — hence `PROCESSORS 4` and `SLOT gpu:2`, which holds that
bound machine-wide, across concurrent ctest runs, shells and agents.

## Conventions for render tests

- `window.visible = false` — the window is never mapped, so a test cannot steal the pointer or
  focus of whoever is using the machine.
- `NO_INPUT=1` — the real keyboard and mouse are ignored, so a frame is the same whatever the
  desktop is doing.
- `FIXED_DT=0` — with no time advancing and the temporal resolves off, two captures of an
  unchanged config are byte-identical, which is what lets these tests compare frames with `== 0`
  instead of an invented noise tolerance. Tests that need time to pass opt out explicitly.
- Captures never touch the disk: `Engine::capture_image()` hands back the pixels.

## Not a test

`tests/editor/docs/` builds `toyengine_editor_docs`, the editor manual's screenshot generator.
Same fixtures and runner, but it produces images rather than checking anything, so it is a tool
and is never registered with ctest. See the `editor-user-docs` skill.
