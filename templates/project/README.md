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
| `@TARGET@.toy` | Project file: build target and the engine pin -- `engine.source` + `engine.ref` for a pinned clone, or `engine.link` for a checkout to build instead (an absolute path for a shared one, a project-relative path for a vendored submodule). |
| `assets/` | Scenes, meshes, materials, textures, UI, `config.yaml`. Shadows the engine's own `assets/`, which stays the fallback (shaders, fonts, shared meshes/materials). `assets/shaders/*.vert/.frag` compile with the engine's and gfxcoopa's shader headers on the include path. |
| `src/` | C++ compiled into the game and the editor. Register components with `TOY_MODULE` (see `src/game_module.cpp`). `src/main.cpp` replaces the engine's game `main`. To change engine code, edit (or fork) `.libs/toyengine`. |
| `build/` | Release tree (`build-debug/` for `./build.sh debug`, which turns Vulkan validation on). |
| `.libs/toyengine` | Where the build finds the engine: a clone, or a symlink to `engine.link` (git-ignored; `./setup.sh` recreates it, `./clean.sh --all` deletes it). |
| `engine/` | Only when the engine is **vendored**: this project's own toyengine as a git submodule, committed here and symlinked from `.libs/toyengine`. `./setup.sh` initialises it on a fresh clone. |

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

## The engine

Three modes, all of them the `.toy` file's `engine:` block:

- **linked** (`engine.link` absolute) -- build a shared toyengine checkout directly,
  uncommitted edits included. Engine changes are shared with every other linked project on the
  machine. `toyhub link . <engine dir>`.
- **pinned** (`engine.ref`) -- `./setup.sh` clones the engine at a pushed commit, so the project
  builds the same on any machine. `toyhub unlink .`; re-pin with `toyhub upgrade . [--ref R]`.
- **vendored** (`engine.link` project-relative) -- the project carries its own toyengine as a
  git submodule in `engine/`, committed and pinned in its own history, affecting no other
  project. `toyhub vendor . [--ref R]`.

Each of those is one step: it takes the mode the project is in now apart -- a clone in `.libs/`,
the engine submodule and its `.gitmodules`/index/config entries, a stale symlink -- and wipes
`build/`, whose CMake cache remembers the old engine path. Uncommitted engine work stops the
switch; `--force` throws it away, `--purge` also drops the submodule's clone in
`.git/modules/`.

After cloning a vendored project, `./setup.sh` (or just `./build.sh`) initialises the submodule
for you; `git clone --recurse-submodules` does it up front.
