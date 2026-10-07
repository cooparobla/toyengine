# @NAME@

The project is named after its folder.

A [toyengine](https://github.com/cooparobla/toyengine) project.

```
./build.sh            # fetches the engine into .libs/ on first run, builds game + editor
./editor.sh           # edit assets (the first open creates assets/)
./run.sh [scene]      # play; HEADLESS=1 MAX_FRAMES=600 ./run.sh for a windowless run
./package.sh [dev|ship|assets] [out]   # standalone, signed game in build/dist/ (assets: encoded assets/ only)
./clean.sh [--all]    # remove build/ (and .libs/)
./setup.sh            # re-sync .libs/toyengine with the .toy file's engine: block
```

| Path | What |
|---|---|
| `@TARGET@.toy` | Project file: build target and the engine pin (`engine.source` + `engine.ref`, or `engine.link` to build against a local toyengine checkout). |
| `assets/` | Scenes, meshes, materials, textures, UI, `config.yaml`. Shadows the engine's own `assets/`, which stays the fallback (shaders, fonts, shared meshes/materials). `assets/shaders/*.vert/.frag` compile with the engine's and gfxcoopa's shader headers on the include path. |
| `src/` | C++ compiled into the game and the editor. Register components with `TOY_MODULE` (see `src/game_module.cpp`). `src/main.cpp` replaces the engine's game `main`; `src/toyengine/<path>.h` replaces that engine header (keep its API). |
| `.libs/toyengine` | The engine + its libraries (git-ignored; `./setup.sh` recreates it). |

Change the engine pin with `toyhub upgrade . [--ref <ref>]`, or switch to a local checkout with `toyhub link . <engine dir>` (and back with `toyhub unlink .`).
