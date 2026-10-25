# editor/build

**Build > Refresh / Package / Ship.** Compiles the game, stages its assets, and bundles a
relocatable, signed package.

| File | Purpose |
|---|---|
| [`build_pipeline.h`](build_pipeline.h) | The pipeline itself: configure, compile, stage, bundle. |
| [`build_settings.h`](build_settings.h) | Per-project settings in `<project>/build_settings.yaml`, beside the `.toy` and outside `assets/` (it is not a game asset). |
| [`packager.h`](packager.h) | Stages a project's runtime assets into an output folder — Build > Package, and the asset half of a full build. Converts `config.yaml` to `.caml` when asked. |
| [`deps.h`](deps.h) | What a game binary needs at run time that a player's machine won't have: the shared libraries to ship beside it. |
| [`bundle_linux.h`](bundle_linux.h) | A relocatable Linux folder, and a `.tar.gz` for Shipping. |
| [`bundle_macos.h`](bundle_macos.h) | A self-contained, signed macOS `.app`, and a notarized, stapled `.zip` / `.dmg` for Shipping. |

## Conventions

- **A Shipping build is its own build tree** (`<project>/build-ship`), never the dev tree — the
  flavour is a property of the whole tree (`TOY_SHIPPING` is PUBLIC on the engine library, so
  the library and every executable linking it agree).
- **Engine scenes never ship.** The packager stages the *project's* assets; this repo's own
  `assets/scenes/` are test content and are excluded.
- **Relocatable means tested relocated.** `tests/editor/gpu/build_test.cpp` builds a fresh
  project and runs the binary from an unrelated directory — that is what catches an absolute
  path baked into a build. `tests/editor/unit/packager_test.cpp` asserts engine scenes and
  content outside the runtime dirs never ship.
