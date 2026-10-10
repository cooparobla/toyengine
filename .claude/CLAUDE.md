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

**Build only what you will run, always `-j4`** (16 GB machine; more jobs has panicked it):

| You changed | Build |
|---|---|
| `toyengine/` | `cmake --build build --target toyengine_tests -j4` (or `toyengine_engine` to just compile) |
| `editor/` | `cmake --build build --target toyengine_editor_tests -j4` (or `toyengine_editor_lib`) |
| the game / editor app | `--target toyengine` / `--target toyengine_editor` |

- Never `cmake --build build` with no target while iterating -- that builds every executable.
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
- Headless only, never a visible window: `HEADLESS=1 MAX_FRAMES=120 ./build/toyengine <scene>`.
