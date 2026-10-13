# @NAME@

The project is named after its folder.

A [toyengine](https://github.com/cooparobla/toyengine) project.

## Scripts

Every script runs the same on macOS and Linux, takes `--help`, and can be run from any
directory (they resolve the project folder themselves).

```
./build.sh [release|debug] [-j N] [--target T]   # engine into .libs/ on first run, then game + editor
./editor.sh                                      # edit assets (the first open creates assets/)
./run.sh [scene] [--headless] [--frames N]       # play a scene (by name or path)
./package.sh [dev|ship|assets] [out]             # standalone, signed game in build/dist/
./clean.sh [--all]                               # remove build output (--all: .libs/ too)
./setup.sh                                       # re-sync .libs/toyengine with the .toy engine: block
```

`./run.sh` and `./editor.sh` build first if the binary is not there yet, so a fresh clone is
just `./editor.sh`. The first build compiles the whole engine -- several minutes; later ones
only recompile what changed. `build.sh` uses one job per core, capped at one per 3 GB of RAM;
override with `-j N` or `JOBS=N`.

**Requirements.** CMake 3.21+, a C++20 compiler, the Vulkan loader/headers, `glslc`, GLFW,
OpenSSL and zstd. On macOS, `.libs/toyengine/tools/setup_macos.sh` installs all of it through
Homebrew (Vulkan runs on MoltenVK; no `DYLD_*` variables needed). On Debian/Ubuntu:
`sudo apt install cmake g++ pkg-config libvulkan-dev glslc libglfw3-dev libssl-dev libzstd-dev`.
A missing tool is reported with that line by `./setup.sh` and `./build.sh`.

## Layout

| Path | What |
|---|---|
| `@TARGET@.toy` | Project file: build target and the engine pin (`engine.source` + `engine.ref`, or `engine.link` to build against a local toyengine checkout). |
| `assets/` | Scenes, meshes, materials, textures, UI, `config.yaml`. Shadows the engine's own `assets/`, which stays the fallback (shaders, fonts, shared meshes/materials). `assets/shaders/*.vert/.frag` compile with the engine's and gfxcoopa's shader headers on the include path. |
| `src/` | C++ compiled into the game and the editor. Register components with `TOY_MODULE` (see `src/game_module.cpp`). `src/main.cpp` replaces the engine's game `main`. To change engine code, edit (or fork) `.libs/toyengine`. |
| `build/` | Release tree (`build-debug/` for `./build.sh debug`, which turns Vulkan validation on). |
| `.libs/toyengine` | The engine + its libraries (git-ignored; `./setup.sh` recreates it). |

## Environment variables

Read by the game and the editor, on both platforms:

| Variable | Effect |
|---|---|
| `HEADLESS=1` | No visible window, vsync off (`./run.sh --headless`). Pair with `MAX_FRAMES`. |
| `MAX_FRAMES=N` | Exit after N frames (`./run.sh --frames N`). |
| `SCENE=<path>` | Scene to load; wins over the `./run.sh` argument. |
| `CONFIG=<path>` | Load a different config than `assets/config.yaml`. |
| `PROFILE=1` | Per-frame CPU/GPU timings to `output/profile.csv`. |
| `JOBS=N` | Compile jobs for `./build.sh`. |
| `BUILD_DIR=<dir>` | Which build tree `run.sh` / `editor.sh` / `package.sh` use. |
| `NO_BUILD=1` | Never auto-build: a missing binary is an error instead. |

Change the engine pin with `toyhub upgrade . [--ref <ref>]`, or switch to a local checkout with
`toyhub link . <engine dir>` (and back with `toyhub unlink .`).
